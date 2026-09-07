#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/JotStore.h"
#include "core/Ops.h"
#include "persist/RunLedger.h"
#include "persist/WatchList.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Watcher - the background thread that turns "a watched file grew" into "an agent triaged it",
// with hard ceilings on how often and how much that is allowed to cost.
//
// THE ONLY BACKGROUND THREAD IN LOOM. Everything else in this codebase answers "what is true right
// now" only when asked (see WatchList.h's own header, and core/JotStore's design). This exists
// because the alternative - a human opening a dashboard to trigger ingestion, or a fixed-interval
// cron job that calls the model whether or not there is anything to do - was measured to cost real
// plan usage for nothing: a no-op Haiku call still spends real tokens (mostly cache-creation) just
// to say "nothing to do". See the loom-triage-idle-cost jot for the number. So the trigger has to
// be event-driven, and something has to be the thing doing the noticing.
//
// EACH POLL IS CHEAP UNCONDITIONALLY: a stat() per watched path (WatchList::Resolve, unchanged)
// plus, only if anything is pending, an in-process Ops::Search for status:unprocessed. NEITHER
// TOUCHES THE MODEL. The model is invoked only past that point, and only when every guardrail below
// allows it - which is also why this can safely poll every 15s while a cron-driven model call could
// not.
//
// ROTATE-FIRST INGESTION closes a real hole: the previous design (POST /watch/ingest) stats the
// file AFTER reading it (HttpServer.cpp), so an append landing mid-import gets marked ingested and
// is then never read again. Renaming the file aside as the first step of ingesting it means the
// import always reads a file nothing is still appending to, and - the more important property - a
// line, once successfully imported, physically cannot be read a second time. That is what makes a
// deliberately loom_delete'd jot's resurrection (confirmed live: the same two test jots came back
// after a re-ingest of the file that originally produced them) impossible rather than merely rare.
// See WatchList::RotateFor for the per-path opt-out, and jotpost/main.cpp for why the source can
// always regenerate a fresh file at the same path with no coordination.
//
// GUARDRAILS, IN THE ORDER THEY ARE CHECKED - see MaybeInvokeAgent in the .cpp for the exact logic:
//
//   breaker open           - stop invoking entirely; a human has to fix whatever is failing and
//                             restart the service (breaker state is deliberately NOT persisted -
//                             see below).
//   cooldown since last run - new jots simply wait for the next eligible window.
//   runs in the last hour   - a burst of small triggers cannot flood the model with back-to-back
//                             tiny calls; the daily-spend guard would eventually catch this too, but
//                             this one turns it off far sooner.
//   spend in the last 24h   - the actual bound on runaway cost, read from RunLedger::Sum24hUSD,
//                             which is why THAT is persisted even though breaker state is not: a
//                             spend cap that resets on every restart is not a cap.
//
// THE CIRCUIT BREAKER IS IN-MEMORY ONLY, on purpose. It exists to stop hammering a broken exec path
// (a bad --on-new-jots, a moved script) immediately rather than waiting for the daily budget to
// exhaust - but the daily budget is what actually bounds worst-case spend regardless of restarts,
// so there is no safety gap in NOT persisting the faster, cruder guard. A restart after a config fix
// is also the obvious way to clear it; inventing a second "reset the breaker" control was not worth
// it for a single-operator service.
//
// ALERTS ARE ROLLING, NOT PER-INCIDENT: exactly one jot per category (failure / budget), upserted
// by slug (Ops::Upsert), so a condition that recurs updates the same card instead of spawning a new
// one every cycle. Recovery ADDS status:done and KEEPS todo - the project's existing convention for
// "this happened and was resolved" (see web/Dashboard.h's isDone/toggleDone). A third alert
// category, "needs input" (something ambiguous that needs the human's judgement), is raised by the
// AGENT ITSELF via loom_upsert - see packaging/systemd/loom-triage-prompt.txt - not by this class,
// because deciding what is ambiguous is exactly the judgement call this class does not make.
//
// POSIX ONLY FOR NOW. Process spawning with a timeout is fork/exec/waitpid here; a Windows
// implementation would be CreateProcess + WaitForSingleObject and does not exist yet. Setting
// --on-new-jots on a Windows build is refused with a clear message rather than silently doing
// nothing - see Watcher.cpp.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct WatcherConfig
{
    int    nPollIntervalSec        = 15;
    int    nCooldownSec            = 60;
    int    nMaxRunsPerHour         = 6;
    double fMaxDailyUSD            = 0.50;
    int    nChildTimeoutSec        = 300;
    int    nMaxConsecutiveFailures = 3;
    size_t nMaxIdsPerRun           = 20;

    // Executed with each pending jot id appended as a decimal-string argv entry, argv[0] being
    // this path. Empty means: ingest watched files, never invoke an agent. Relative paths are
    // resolved against the current working directory, same as any other exec.
    std::string sOnNewJots;
};

class Watcher
{
public:
    // store is needed alongside ops for exactly one thing - Flatten(jot, names) to read a jot's
    // tags back out as strings when editing an alert jot's tag list, the same reason HttpServer
    // itself is handed both rather than just Ops. See core/FlatJot.h.
    Watcher(WatchList& watch, Ops& ops, JotStore& store, RunLedger& ledger,
            const WatcherConfig& config);
    ~Watcher();

    Watcher(const Watcher&)            = delete;
    Watcher& operator=(const Watcher&) = delete;

    // No-op if sOnNewJots and the watch list are both irrelevant (nothing configured) - Start()
    // still runs the poll loop either way, since ingestion alone (no agent) is a legitimate mode.
    void Start();

    // Blocks until the loop thread has woken from its current sleep and exited. Safe to call from
    // the same shutdown path as everything else in main.cpp.
    void Stop();

    // Runtime guardrail state, read from the HTTP thread for /stats - see RunLedger::Stats, which
    // takes these as parameters rather than owning them, so history and live state are not two
    // copies of the same fact.
    bool IsBreakerOpen() const { return mbBreakerOpen.load(); }
    int  ConsecutiveFailures() const { return mnConsecutiveFailures.load(); }
    bool IsBudgetExceeded() const { return mbBudgetExceeded.load(); }

    // How many times this process has invoked the triage agent - "calls to Haiku" on the
    // dashboard's health panel. Deliberately NOT the same number as RunLedger's lifetime/24h
    // counts: those survive a restart, this answers "since THIS process started", the same window
    // the store's own since-start counters (jots/tags/todos added) use.
    uint64_t RunsThisProcess() const { return mnRunsThisProcess.load(); }

private:
    void Loop();
    void PollOnce();

    // Rotates and imports every pending configured path. Returns the ids newly imported this cycle
    // - not used for triage targeting (see below) but folded into the trigger description.
    size_t IngestPending();

    // The default path now: mechanical extraction (bracketed tags, a short note's own text as its
    // summary, a relative due date computed off the jot's OWN id) with NO model call at all - see
    // Watcher.cpp. Only what this cannot confidently handle gets tagged `tbd` for a human to
    // decide, rather than spending a Haiku invocation (confirmed live: ~$0.02-0.03 of FIXED
    // per-invocation overhead regardless of content) on jots simple enough not to need it.
    void DeterministicTriage();

    // Guarantees every `tbd` jot is `todo` + `priority:high`, in code rather than trusting a single
    // call site to always remember both - a tbd card that isn't even a todo is invisible on the
    // dashboard, which defeats the entire point of the tag. Cheap (an in-process query plus,
    // usually, zero patches) so it just runs every poll.
    void EnsureNeedsInputVisible();

    // What actually decides whether to call the model: an in-process, cost-free query for
    // status:unprocessed minus status:needs-input (the latter tag means "already flagged for a
    // human, do not keep re-spending tokens re-deciding the same jot is ambiguous every cycle" -
    // see the prompt file). Runs the agent if guardrails allow and anything comes back.
    void MaybeInvokeAgent();

    // Upserts (by slug) the one rolling alert jot for a category, appending a timestamped line and
    // capping history at a handful of entries - see the class comment. bResolve adds status:done
    // and keeps todo without touching the accumulated text, so what happened stays visible.
    void RaiseAlert(const std::string& sSlug, const std::string& sLine);
    void ResolveAlert(const std::string& sSlug);

    WatchList& mWatch;
    Ops&       mOps;
    JotStore&  mStore;
    RunLedger& mLedger;
    WatcherConfig mConfig;

    std::thread             mThread;
    std::atomic<bool>       mbStop{ false };
    std::mutex              mCvMutex;
    std::condition_variable mCv;

    // Guardrail state. Atomics because IsBreakerOpen() etc. are read from the HTTP thread while the
    // loop thread writes them - simple flags, not worth a second mutex for.
    std::atomic<bool>     mbBreakerOpen{ false };
    std::atomic<int>      mnConsecutiveFailures{ 0 };
    std::atomic<bool>     mbBudgetExceeded{ false };
    std::atomic<uint64_t> mnRunsThisProcess{ 0 };

    // Loop-thread-only state, no synchronization needed - never touched off that thread.
    int64_t              mnLastRunUS = 0;
    std::deque<int64_t>  mRunTimesLastHour;
    bool                 mbFailureAlertActive = false;
    bool                 mbBudgetAlertActive  = false;
};
