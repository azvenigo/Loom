#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// TriageClient - Loom's caller for the offline triage service on zserver (POST /triage, v2).
//
// WHAT IT REPLACES. Watcher used to hand anything its regex could not read to `claude -p --model
// haiku`. The run ledger measured what that cost: 6 runs, one jot each, mean $0.0407 and 29.3
// seconds, on 40-58 input tokens. The cost is almost entirely fixed session overhead - a call that
// correctly decides to do nothing costs about the same as one that does real work - so the useful
// question was never "make the prompt cheaper" but "how many of these calls should never have
// happened". Deterministic triage removed most of them; this removes most of the rest, onto a
// Qwen2.5-Coder 14B on a box that is powered on for Plex anyway.
//
// PROPOSAL-ONLY. THE SERVICE NEVER READS OR WRITES LOOM. It holds no Loom credentials and makes no
// outbound call to azzorin; it is handed a jot and answers with proposals, and every mutation
// still goes through Ops::Update on this side, with the history log and undo behind it. That is
// what makes it acceptable for the memory store to depend on a machine that also runs Apache,
// Jellyfin and Plex: the worst a wrong or compromised service can do is propose a bad tag.
//
// LOOM SUPPLIES THE CONTEXT, THE SERVICE DOES NOT GO LOOKING. Two capabilities need store
// knowledge - reusing the existing tag vocabulary instead of inventing a second spelling of one,
// and comparing against possible duplicates. Both travel in the request body. The whole live
// vocabulary is twelve tags, so this costs nothing and keeps the credential boundary intact.
//
// THE MODEL EXTRACTS, DETERMINISTIC CODE DECIDES. Carried over from v1 and non-negotiable: the
// first dry run had Qwen answer "Friday morning" with 2026-09-10, which was a Thursday. The
// service now computes dates in timezone-aware code from extracted slots, and VerifyDue below
// re-checks the service's two machine-readable date fields against each other before Loom acts on
// either. `reason` is prose and is never parsed for control flow - the wire contract says so
// outright.
//
// REFUSAL IS ALWAYS AVAILABLE AND IS ALWAYS SAFE. Every proposal is independently optional. A jot
// the service declines entirely lands exactly where it would have landed if this service did not
// exist. That is the property that makes putting a model in this pipeline unalarming: the model's
// failure mode is the old behaviour.
//
// CALLED ON THE WATCHER'S POLL THREAD, WHICH IS WHY THE CAPS EXIST. The service budgets itself 18
// seconds internally and returns partial results rather than overrunning, but a backlog of jots
// could still stall ingestion for minutes. nMaxPerPoll bounds the work per cycle; jots past the
// cap keep their status:unprocessed tag and are picked up next poll, which costs nothing.
//
// BREAKER IS TIME-BASED, NOT LATCHING, unlike Watcher's. The usual failure here is a box that is
// asleep or rebooting, which heals with no human involved - so it re-probes after a backoff rather
// than staying down until Loom restarts.
//
// POSIX ONLY FOR NOW, matching JotpostStatus and Watcher's exec path: a Windows build compiles,
// and Triage() always reports failure there, which falls through to the old behaviour.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct TriageConfig
{
    // Empty host = feature off. Watcher then behaves exactly as it did before this existed.
    std::string sHost;
    uint16_t    nPort = 7711;

    // Bearer token. Deliberately NOT stored in Loom (the service's own jot says so) and not in the
    // repo - main.cpp reads it from a file via --resolver-token-file so it never appears in a
    // process listing either.
    std::string sToken;

    // The service budgets itself 18s and answers partially rather than overrunning; this is the
    // transport bound around that, with headroom for a cold model load (measured at 10.2s once).
    int nTimeoutMS = 25000;

    // Per poll cycle, not per jot - see the class comment.
    int nMaxPerPoll = 4;

    // How long the breaker stays open after nFailsToOpen consecutive transport failures.
    int nFailBackoffSec = 300;
    int nFailsToOpen    = 2;
};

// One jot the service may compare against for duplicate detection. Loom searches; the service
// only judges - see the class comment on the credential boundary.
struct TriageCandidate
{
    int64_t                  nID = 0;
    std::string              sName;
    std::string              sSummary;
    std::vector<std::string> vTags;
};

// What Loom could not settle for itself and is therefore asking about. Anything left false is
// omitted from the request's `want` array, and the service is required not to run a prompt for it.
struct TriageWants
{
    bool bSummary  = false;
    bool bClassify = false;
    bool bTopics   = false;
    bool bPriority = false;
    bool bDue      = false;
    bool bDedupe   = false;
};

// Every field is independently optional; the bHave* flags say which the service actually answered.
// A default-constructed instance is "the service told us nothing", which is a valid outcome.
struct TriageResult
{
    bool bHaveSummary = false;
    std::string sSummary;

    bool bHaveKind = false;
    std::string sKind;                     // "journal" | "fact" | "task"

    bool bHaveTopics = false;
    std::vector<std::string> vTopics;      // guaranteed by contract to be a subset of what we sent
    std::vector<std::string> vNewTopics;   // proposed additions - Loom does NOT apply these

    bool bHavePriority = false;
    std::string sPriority;                 // "priority:high" | "priority:normal" | "priority:low"

    bool bHaveDue = false;
    std::string sDueTag;                   // "due:2026-09-08t15:00", already in Loom's tag form
    bool bRecurring = false;               // text genuinely stated recurrence - Loom has no
                                           // convention for this yet, so it surfaces for a human

    bool bHaveDuplicate = false;
    int64_t nDuplicateOf = 0;
    std::string sRelation;                 // "duplicate" | "extends" | "unrelated"

    bool bNeedsInput = false;
    std::string sNeedsSummary;             // begins "NEEDS INPUT: "
    std::string sNeedsQuestion;
    std::string sNeedsPrompt;              // ready-to-paste instruction for an interactive agent

    std::string sStatus;                   // "ok" | "ambiguous" | "deferred" | ...
    std::string sReason;                   // prose. Never branched on.
    int         nRetryAfterSec = 0;        // set when sStatus == "deferred"
};

class TriageClient
{
public:
    explicit TriageClient(TriageConfig config) : mConfig(std::move(config)) {}

    TriageClient(const TriageClient&)            = delete;
    TriageClient& operator=(const TriageClient&) = delete;

    bool Configured() const { return !mConfig.sHost.empty() && !mConfig.sToken.empty(); }

    // Reset at the top of each Watcher poll - the cap is per cycle.
    void BeginPoll() { mnUsedThisPoll = 0; }

    // True if a further Triage() this cycle would do real work: budget left, breaker closed, and
    // the feature configured at all. Lets the caller skip building a request it cannot send.
    bool Available() const;

    // nJotID is the jot's own id, which IS its ingestion timestamp (Jot.h) - it becomes the
    // request's now_local, so relative phrases resolve from when the jot was WRITTEN rather than
    // from when zserver happens to answer. (v1's /resolve had no such field and used the service's
    // own clock; that was the single worst thing about the v1 contract.)
    //
    // vTags must be the jot's tags as they stand: the service checks them for an existing due: tag
    // and declines rather than proposing a competing date over one a human set by hand.
    //
    // Returns false on any transport, auth or parse failure. A TRUE return with an empty result is
    // a different thing - the service answered and declined - and the caller is expected to tell
    // those apart, because one means the server is down and the other means the jot genuinely
    // needs a person.
    bool Triage(int64_t nJotID,
                const std::string& sName,
                const std::string& sSummary,
                const std::string& sText,
                const std::vector<std::string>& vTags,
                const TriageWants& wants,
                const std::vector<std::string>& vVocabulary,
                const std::vector<TriageCandidate>& vCandidates,
                TriageResult& out);

    bool     IsBreakerOpen() const;
    int      ConsecutiveFailures() const { return mnConsecutiveFailures; }
    uint64_t Calls() const               { return mnCalls; }
    uint64_t Applied() const             { return mnApplied; }

private:
    bool PostTriage(const std::string& sBody, std::string& outBody) const;

    // Re-checks a due proposal before Loom acts on it: the tag parses, it agrees with the
    // service's own due_local rendering of the same instant, and it is not absurdly far from the
    // jot's own timestamp. Clears bHaveDue rather than throwing - a failed check is a decline.
    static void VerifyDue(TriageResult& result, const std::string& sDueLocal, int64_t nRefUS);

    TriageConfig mConfig;

    // Watcher-poll-thread-only, like Watcher's own loop state. The counters are read by the HTTP
    // thread for /stats, a torn-read-tolerant display value - the same call JotpostStatus makes.
    int      mnUsedThisPoll        = 0;
    int      mnConsecutiveFailures = 0;
    int64_t  mnBreakerOpenedUS     = 0;
    uint64_t mnCalls               = 0;
    uint64_t mnApplied             = 0;
};
