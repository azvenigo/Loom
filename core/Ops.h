#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "JotStore.h"
#include "Query.h"

#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Ops - the operation surface. The only thing a front end is allowed to call.
//
// This layer exists because Loom has more than one front door: REST routes, MCP tools, the
// importer, and the benchmark. Without it, adding MCP means reimplementing every route against
// JotStore and then watching the two surfaces drift apart - one gains a parameter, the other does
// not, and the difference only shows up as an agent behaving differently from curl. With it, MCP
// is a JSON Schema table and a dispatch switch over these same calls.
//
// The division of labour is strict:
//
//   JotStore  owns invariants. It does exactly what it is told and refuses what would corrupt it.
//             No upserts, no "create if missing", no advice.
//   Ops       owns policy. Upsert-by-name, warning text, limit clamping, query parsing, and the
//             decision that a near-duplicate tag is a warning rather than a rejection.
//
// Nothing here throws; everything returns std::error_code, house style.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct OpsConfig
{
    size_t mnDefaultLimit = 50;
    size_t mnMaxLimit     = 500;

    // Past this many live non-reserved tags, every write carries a vocabulary-size warning. Not a
    // hard cap - refusing writes to protect a tag budget would be worse than the problem.
    size_t mnMaxTags      = 200;

    //--------------------------------------------------------------------------------------------
    // DUPLICATE DETECTION ON CREATE. How many existing memories a new one may be told it resembles,
    // and how close it has to be to count.
    //
    // mnDuplicateCandidates of 0 turns the check off entirely.
    //--------------------------------------------------------------------------------------------
    size_t mnDuplicateCandidates = 3;

    // A FRACTION OF THE NEW RECORD'S OWN SCORE, not an absolute one. BM25 scores are unbounded and
    // depend on corpus size, document lengths and how rare the query terms happen to be, so a fixed
    // threshold that means "very similar" in a store of sixty memories means "unrelated" in one of
    // sixty thousand. Scoring the new jot against its own summary gives the ceiling that query can
    // reach in THIS store right now, and everything else is measured against it.
    //
    // 0.5 rather than something tighter because the new jot is also the newest, and the recency
    // multiplier (RANK::kRecencyWeight) gives it up to 35% more than an otherwise identical record
    // written months ago - so an exact duplicate scores around 0.74 here, not 1.0.
    float  mfDuplicateRatio      = 0.5f;
};

// An existing memory that resembles one just created. Advice, exactly like OpWarnings - the write
// has already happened and is not in question.
struct DuplicateCandidate
{
    tJotID      mID = kInvalidJotID;
    std::string msName;
    std::string msSummary;     // summary, else a clipped first line - enough to recognize it by

    // Relevance relative to what the new record itself scored on the same query, so 1.0 means "as
    // good a match for this summary as the new jot is". See OpsConfig::mfDuplicateRatio.
    float       mfSimilarity = 0.0f;
};

// A write's non-fatal advice. These ride back with the new id and are the entire mechanism by
// which the tag vocabulary stays under control: agents self-correct on the next write because they
// were told at the moment it mattered. Nothing here ever fails a request.
struct OpWarnings
{
    std::vector<std::string> mMessages;
    bool Empty() const { return mMessages.empty(); }
};

struct AddResult
{
    Jot        mJot;
    OpWarnings mWarnings;
    bool       mbCreated = false;   // false means an existing named jot was updated instead

    // The patch resolved to the record that was already there, so nothing was written. See
    // MutationResult::mbNoChange.
    bool       mbNoChange = false;

    // Existing memories the new one may duplicate. Only ever populated on a create - an update to a
    // jot that already exists cannot be a duplicate of anything, it IS the record.
    std::vector<DuplicateCandidate> mDuplicates;
};

struct SearchResultSet
{
    std::vector<Jot> mJots;         // already ordered and limited
    std::vector<float> mScores;     // parallel to mJots
    size_t mnMatched   = 0;
    bool   mbTruncated = false;
};


class Ops
{
public:
    Ops(JotStore& store, const OpsConfig& config = OpsConfig());

    //----------------------------------------------------------------------------------------
    // Writes
    //----------------------------------------------------------------------------------------

    // Creates a jot. If the input carries a name that already exists, this is an ERROR
    // (kNameInUse) - use Upsert when overwrite is what you mean. Keeping the two apart is what
    // stops an import from silently replacing a memory that a human wrote.
    //
    // input.mnCreatedUS backdates the jot, which means it becomes the id - see BACKDATING in
    // JotStore::Add. It is validated here rather than in the store because refusing a future date
    // is policy, while moving off a taken id is an invariant; kInvalidArgument for either a future
    // timestamp or one below kCreatedFloorUS.
    std::error_code Add(const JotInput& input, AddResult& outResult);

    // Create-or-update keyed on the slug. This is the memory-store and reconcile path.
    // nExpectUpdatedUS applies only when the jot already exists; pass 0 for last-write-wins.
    //
    // input.mnCreatedUS applies only on the branch that actually creates. Re-running an import is
    // therefore safe: the second pass updates the content and leaves the original date alone,
    // rather than resurrecting a date the jot has since outgrown.
    std::error_code Upsert(const JotInput& input, int64_t nExpectUpdatedUS, AddResult& outResult);

    // input.mnCreatedUS is IGNORED. A jot's id is its creation time, and moving it would strand
    // every link and backlink pointing at the old one; a patch that only carries a creation time
    // is an empty patch and comes back kInvalidArgument.
    std::error_code Update(tJotID id, const JotInput& patch, int64_t nExpectUpdatedUS,
                           AddResult& outResult);

    std::error_code Delete(tJotID id);

    // Re-applies a complete record read back out of the history log. This is the undo path.
    //
    // POLICY, which is why it is here and not a raw LoadFlatBatch at the route:
    //
    //   - The id is kept. That is the entire point - a restore puts the SAME jot back, links to it
    //     survive, and a jot that was deleted comes back at the address other jots still reference.
    //   - `updated` is stamped NOW rather than carried from the record. The restore is a change and
    //     it happened at the moment somebody asked for it; keeping the old stamp would hide it from
    //     "recently changed" and hand every client an expect_updated value that reads as stale.
    //   - It is REFUSED if the slug now belongs to a different jot. LoadFlatBatch is the replay
    //     path, where names are unique by construction, so its name-index insert does not overwrite
    //     - restoring over a taken slug would leave two jots claiming one name with the index
    //     pointing at whichever got there first. outConflictID names the current holder.
    //   - A restore to the version the jot is ALREADY on writes nothing and reports mbNoChange,
    //     exactly as Update does. The History view offers Restore on every row including the newest,
    //     so this is the common case rather than a corner of it.
    //   - nExpectUpdatedUS is the same optimistic-concurrency guard Update() takes: a restore is a
    //     whole-record overwrite, so without it two agents acting on stale copies would have one
    //     silently clobber the other, which is exactly the failure mode expect_updated exists to
    //     turn into a 409 everywhere else. Pass 0 to skip the check.
    std::error_code Restore(const FlatJot& record, int64_t nExpectUpdatedUS, tJotID& outConflictID,
                            AddResult& outResult);

    // outTxnID names the whole rewrite in the history log, so the front ends can offer "undo that
    // merge" as one act instead of asking somebody to restore each affected jot in turn. 0 when
    // there is no history log to record it in.
    std::error_code MergeTags(const std::vector<std::string>& vFrom, const std::string& sTo,
                              size_t& outJotsChanged, uint64_t& outTxnID);

    //----------------------------------------------------------------------------------------
    // Reads
    //----------------------------------------------------------------------------------------

    std::error_code Get(tJotID id, Jot& outJot) const;
    std::error_code GetByName(const std::string& sName, Jot& outJot) const;

    std::error_code Search(const Query& query, SearchResultSet& outResults) const;

    std::error_code Links(tJotID id, size_t nDepth, std::vector<Jot>& outJots) const;

    void ListTags(std::vector<TagStat>& outStats, bool bIncludeReserved = false) const;
    void TagClusters(std::vector<TagCluster>& outClusters) const;

    StoreStats Stats() const { return mStore.GetStats(); }

    //----------------------------------------------------------------------------------------
    // Query construction from loose strings.
    //
    // Lives here rather than in the HTTP layer so that REST query parameters, an MCP tool's JSON
    // arguments, and the benchmark all interpret "30d" and "limit" identically. The moment two
    // front ends parse a range differently, the same question returns different answers depending
    // on which door it came through.
    //----------------------------------------------------------------------------------------
    struct QuerySpec
    {
        std::string              msText;
        std::vector<std::string> mTags;
        std::vector<std::string> mNotTags;
        std::string              msEditor;
        std::string              msName;
        std::string              msSince;     // raw microseconds, "30d", or "2026-08-01"
        std::string              msUntil;
        std::string              msLink;      // decimal jot id
        std::string              msOrder;     // "relevance" | "newest" | "oldest"
        size_t                   mnLimit  = 0;
        size_t                   mnOffset = 0;
        bool                     mbPrefix = false;
    };

    std::error_code BuildQuery(const QuerySpec& spec, Query& outQuery) const;

    // Bounds on a caller-supplied creation time. Public so the front doors can quote them in an
    // error message instead of each inventing its own wording for the same rule.
    static constexpr int64_t kCreatedFutureSlackUS = 60LL * 1000000LL;        // clock skew allowance
    static constexpr int64_t kCreatedFloorUS       = 86400LL * 1000000LL;     // 1970-01-02

    static std::error_code ValidateCreatedUS(int64_t nCreatedUS);

private:
    void CollectWarnings(const std::vector<TagSuggestion>& vSuggestions, OpWarnings& outWarnings) const;

    // Runs a newly created jot's name and summary back through the ranker and reports the existing
    // records that come back close to it.
    //
    // THE MCP SERVER'S FIRST INSTRUCTION IS "search before writing - the thing you are about to
    // record may already be here", and until this existed nothing assisted or checked that, so
    // duplicates accumulated silently and only surfaced when somebody read two of them side by
    // side. The index is already built and this is the query it answers constantly, so the check
    // costs one search per create.
    //
    // IT NEVER BLOCKS THE WRITE, for the same reason the tag-drift suggestions do not: an agent
    // that meant to write a second, distinct memory should not have to argue with the store. It is
    // told what it may not have seen and the record is already there either way.
    void CollectDuplicates(const Jot& created, AddResult& outResult) const;

    JotStore& mStore;
    OpsConfig mConfig;
};
