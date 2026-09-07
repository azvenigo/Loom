#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// RunLedger - a record of every triage-agent invocation, for one purpose: making usage visible.
//
// Before this existed, the cost of Watcher deciding to call Haiku was invisible - "how much of my
// plan usage is this costing me" had no answer short of tailing a log by hand. Every field here
// comes straight from `claude -p --output-format json`'s own report (total_cost_usd,
// duration_api_ms, usage.*) - nothing is estimated.
//
// A BOUNDED RING, NOT A LOG. Kept to the most recent kMaxRing runs on purpose: this answers "what
// has been happening lately", not "audit every run since install". An unbounded file here would be
// exactly the kind of clutter this whole effort exists to remove. Lifetime counters are kept
// alongside the ring precisely so trimming old entries does not also erase "how much has this ever
// cost in total".
//
// THE 24-HOUR SPEND FIGURE IS WHAT Watcher'S DAILY BUDGET GUARD READS. It has to survive a restart
// - a spend cap that resets every time the process bounces is not a cap - which is the whole reason
// this is persisted rather than kept only in Watcher's own memory.
//
// Same commit discipline as WatchList/IpAcl: whole-file JSON, tmp -> rename.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct TriageRun
{
    int64_t     mnAtUS                = 0;       // when the run started
    int64_t     mnDurationMS          = 0;       // claude's own duration_api_ms
    double      mfCostUSD             = 0.0;     // claude's own total_cost_usd - a Pro-plan
                                                  // equivalent-cost PROXY, not a bill
    uint64_t    mnInputTokens         = 0;
    uint64_t    mnOutputTokens        = 0;
    uint64_t    mnCacheReadTokens     = 0;
    uint64_t    mnCacheCreationTokens = 0;
    size_t      mnJotsGiven           = 0;       // ids handed to the agent this run
    bool        mbSuccess             = false;
    std::string msSessionID;
    std::string msTrigger;    // short human text: what caused this run ("4 unprocessed jots")
    std::string msFailure;    // reason, when !mbSuccess - empty on success
};

// Aggregated view for /stats and the dashboard. Guardrail state (breaker/budget) is Watcher's own
// runtime state, not the ledger's - Stats() takes it as parameters so this stays a plain report of
// history, not a second place that state has to be kept in sync.
struct TriageStats
{
    bool      mbHasLastRun = false;
    TriageRun mLastRun;

    size_t    mnRuns24h        = 0;
    double    mfCostUSD24h     = 0.0;
    uint64_t  mnInputTokens24h  = 0;
    uint64_t  mnOutputTokens24h = 0;

    size_t    mnRunsLifetime    = 0;
    double    mfCostUSDLifetime = 0.0;

    // Since process start - unlike mnRunsLifetime, this is NOT persisted; it answers "how many
    // times has THIS process called the model", which is what a dashboard reading "this run" next
    // to it (jots/tags/todos added) actually means. Watcher's own in-memory counter, folded in here
    // as a parameter for the same reason bBreakerOpen etc. are - see RunLedger::Stats.
    size_t    mnRunsThisProcess = 0;

    bool      mbBreakerOpen          = false;
    int       mnConsecutiveFailures  = 0;
    bool      mbBudgetExceeded       = false;
};

class RunLedger
{
public:
    // A missing or unreadable file is a first run, not an error - starts empty, same tolerance as
    // WatchList::Load.
    std::error_code Load(const std::string& sPath, std::string& outWarning);

    // Appends, trims the ring to kMaxRing, updates lifetime totals, and persists - one call, so a
    // caller cannot record a run and forget to save it.
    std::error_code Record(const TriageRun& run);

    // bBreakerOpen/nConsecutiveFailures/bBudgetExceeded/nRunsThisProcess are Watcher's live state,
    // folded in here only for the convenience of handing the dashboard one object.
    TriageStats Stats(bool bBreakerOpen, int nConsecutiveFailures, bool bBudgetExceeded,
                      size_t nRunsThisProcess = 0) const;

    // Newest first, for GET /triage/runs.
    std::vector<TriageRun> Recent(size_t nLimit) const;

    // What Watcher's daily-budget guard actually reads. A rolling window measured from now, not
    // calendar-midnight - "24 hours" should mean 24 hours, not "since whenever the clock last
    // crossed a day boundary".
    double Sum24hUSD() const;
    size_t Count24h() const;

    static constexpr size_t kMaxRing = 200;

private:
    std::error_code SaveLocked() const;

    mutable std::mutex    mMutex;
    std::string           msPath;
    std::deque<TriageRun> mRuns;              // newest at the back

    // Survive ring trimming - "how much has this ever cost" must not shrink just because old
    // entries aged out of the ring.
    size_t                mnLifetimeRuns    = 0;
    double                mfLifetimeCostUSD = 0.0;
};
