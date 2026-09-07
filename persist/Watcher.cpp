// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/Watcher.h"

#include "core/LoomTime.h"
#include "core/Query.h"
#include "persist/Importer.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <regex>
#include <sstream>

#ifndef _WIN32
    #include <csignal>
    #include <poll.h>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

using json = nlohmann::json;

namespace
{
    int64_t NowUS()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string Lower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    std::string TrimWs(const std::string& s)
    {
        size_t a = 0, b = s.size();
        while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
        return s.substr(a, b - a);
    }

    // Every [bracketed] token, trimmed, plus the text with them removed. "[todo] walk the dog" ->
    // ({"todo"}, "walk the dog") - the same convention loom-triage-prompt.txt already documented
    // for an LLM to follow by hand; this is the same rule run mechanically for the common case.
    std::vector<std::string> ExtractBrackets(const std::string& sText, std::string& outClean)
    {
        static const std::regex re(R"(\[([^\]]+)\])");
        std::vector<std::string> vOut;
        for (auto it = std::sregex_iterator(sText.begin(), sText.end(), re);
             it != std::sregex_iterator(); ++it)
            vOut.push_back(TrimWs((*it)[1].str()));
        outClean = TrimWs(std::regex_replace(sText, re, ""));
        return vOut;
    }

    // "due in 2 minutes", "remind me in 3 days", "due in an hour" (word numbers not handled -
    // falls through to no match, same as anything else this simple regex does not recognize).
    // Deliberately narrow: this is meant to catch the common phrasing cheaply, not parse English.
    bool ParseRelativeOffset(const std::string& sText, int64_t& outOffsetUS)
    {
        static const std::regex re(R"(\b(\d+)\s*(minute|min|hour|hr|day)s?\b)",
                                   std::regex::icase);
        std::smatch m;
        if (!std::regex_search(sText, m, re))
            return false;

        const long nAmount = std::strtol(m[1].str().c_str(), nullptr, 10);
        const std::string sUnit = Lower(m[2].str());
        int64_t nUnitUS = 60LL * 1000000;
        if (sUnit == "hour" || sUnit == "hr") nUnitUS = 3600LL * 1000000;
        else if (sUnit == "day")              nUnitUS = 86400LL * 1000000;
        outOffsetUS = static_cast<int64_t>(nAmount) * nUnitUS;
        return true;
    }

    // due:<local-datetime>, Loom's existing convention (Dashboard.h's dueOf/setDue) - lowercase t,
    // since tag normalization lowercases everything anyway (TagRegistry::Normalize).
    std::string FormatDueLocal(int64_t nUS)
    {
        const std::time_t t = static_cast<std::time_t>(nUS / 1000000);
        std::tm tmLocal{};
#ifdef _WIN32
        localtime_s(&tmLocal, &t);
#else
        localtime_r(&t, &tmLocal);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dt%H:%M", &tmLocal);
        return buf;
    }

    std::string UtcStamp()
    {
        const std::time_t t = std::time(nullptr);
        std::tm tmUtc{};
#ifdef _WIN32
        gmtime_s(&tmUtc, &t);
#else
        gmtime_r(&t, &tmUtc);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%dT%H%M%SZ", &tmUtc);
        return buf;
    }

    //--------------------------------------------------------------------------------------------
    // Child process execution with a hard wall-clock timeout. POSIX only - see the class comment
    // in Watcher.h for why a Windows equivalent (CreateProcess + WaitForSingleObject) is deferred.
    //--------------------------------------------------------------------------------------------
    struct ChildResult
    {
        bool        bLaunchFailed = false;
        bool        bTimedOut     = false;
        int         nExitCode     = -1;
        std::string sStdout;
    };

#ifndef _WIN32
    ChildResult RunChildWithTimeout(const std::vector<std::string>& vArgs, int nTimeoutSec)
    {
        ChildResult result;
        if (vArgs.empty())
        {
            result.bLaunchFailed = true;
            return result;
        }

        int pipeFds[2];
        if (::pipe(pipeFds) != 0)
        {
            result.bLaunchFailed = true;
            return result;
        }

        const pid_t pid = ::fork();
        if (pid < 0)
        {
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);
            result.bLaunchFailed = true;
            return result;
        }

        if (pid == 0)
        {
            // Child. Only stdout is redirected - stderr is left inherited (the loom process's own,
            // which under systemd is the journal) so a diagnostic from a broken script or a bad
            // `claude` invocation is visible in `journalctl -u loom` rather than swallowed.
            ::dup2(pipeFds[1], STDOUT_FILENO);
            ::close(pipeFds[0]);
            ::close(pipeFds[1]);

            std::vector<char*> vArgv;
            vArgv.reserve(vArgs.size() + 1);
            for (const std::string& s : vArgs)
                vArgv.push_back(const_cast<char*>(s.c_str()));
            vArgv.push_back(nullptr);

            ::execvp(vArgv[0], vArgv.data());
            // execvp only returns on failure. _exit, not exit - this is a forked child and must
            // not run any of the parent's atexit handlers (flushing the parent's open files twice,
            // closing sockets the parent still owns, etc).
            ::_exit(127);
        }

        // Parent.
        ::close(pipeFds[1]);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(nTimeoutSec);
        bool bChildClosedPipe = false;

        while (true)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                break;
            const int nRemainingMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            struct pollfd pfd{};
            pfd.fd     = pipeFds[0];
            pfd.events = POLLIN;
            const int nReady = ::poll(&pfd, 1, nRemainingMs > 0 ? nRemainingMs : 0);
            if (nReady < 0)
            {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (nReady == 0)
                continue;   // poll's own timeout slice elapsed; the outer deadline check decides

            if (pfd.revents & (POLLIN | POLLHUP | POLLERR))
            {
                char buf[4096];
                const ssize_t nRead = ::read(pipeFds[0], buf, sizeof(buf));
                if (nRead > 0)
                {
                    result.sStdout.append(buf, static_cast<size_t>(nRead));
                    continue;
                }
                // nRead == 0: EOF - the child closed its write end, whether by exiting normally or
                // by execve itself failing.
                bChildClosedPipe = true;
                break;
            }
        }
        ::close(pipeFds[0]);

        if (!bChildClosedPipe)
        {
            // Deadline hit with the pipe still open - presumed hung. SIGTERM, a short grace
            // period, then SIGKILL. There is no blocking "waitpid with a timeout" in POSIX, so the
            // grace period is polled in slices - cheap, and this path only runs when something is
            // already wrong.
            ::kill(pid, SIGTERM);
            bool bReaped = false;
            for (int i = 0; i < 50 && !bReaped; ++i)   // 50 * 100ms = 5s grace
            {
                int status = 0;
                if (::waitpid(pid, &status, WNOHANG) == pid)
                    bReaped = true;
                else
                    ::usleep(100000);
            }
            if (!bReaped)
            {
                ::kill(pid, SIGKILL);
                int status = 0;
                ::waitpid(pid, &status, 0);
            }
            result.bTimedOut = true;
            return result;
        }

        int status = 0;
        ::waitpid(pid, &status, 0);
        if (WIFEXITED(status))
            result.nExitCode = WEXITSTATUS(status);
        return result;
    }
#else
    ChildResult RunChildWithTimeout(const std::vector<std::string>&, int)
    {
        ChildResult result;
        result.bLaunchFailed = true;
        return result;
    }
#endif

    //--------------------------------------------------------------------------------------------
    // What `claude -p --output-format json` reports about itself. Nothing here is estimated - see
    // RunLedger.h.
    //--------------------------------------------------------------------------------------------
    struct ParsedRunOutput
    {
        bool        bParsed  = false;
        bool        bIsError = true;
        double      fCostUSD = 0.0;
        int64_t     nDurationMS = 0;
        uint64_t    nInputTokens = 0, nOutputTokens = 0;
        uint64_t    nCacheReadTokens = 0, nCacheCreationTokens = 0;
        std::string sSessionID;
        std::string sResultText;
    };

    ParsedRunOutput ParseClaudeJson(const std::string& sStdout)
    {
        ParsedRunOutput out;
        json j = json::parse(sStdout, nullptr, false);
        if (j.is_discarded() || !j.is_object())
            return out;   // bParsed stays false - callers treat that as a failed run

        out.bParsed     = true;
        out.bIsError    = j.value("is_error", true);
        out.fCostUSD    = j.value("total_cost_usd", 0.0);
        out.nDurationMS = j.value("duration_api_ms", (int64_t)0);
        out.sSessionID  = j.value("session_id", std::string());
        out.sResultText = j.value("result", std::string());

        if (j.contains("usage") && j["usage"].is_object())
        {
            const auto& u            = j["usage"];
            out.nInputTokens         = u.value("input_tokens", (uint64_t)0);
            out.nOutputTokens        = u.value("output_tokens", (uint64_t)0);
            out.nCacheReadTokens     = u.value("cache_read_input_tokens", (uint64_t)0);
            out.nCacheCreationTokens = u.value("cache_creation_input_tokens", (uint64_t)0);
        }
        return out;
    }
}


Watcher::Watcher(WatchList& watch, Ops& ops, JotStore& store, RunLedger& ledger,
                 const WatcherConfig& config)
    : mWatch(watch), mOps(ops), mStore(store), mLedger(ledger), mConfig(config)
{
}

Watcher::~Watcher()
{
    Stop();
}

void Watcher::Start()
{
    if (mThread.joinable())
        return;
    mbStop = false;
    mThread = std::thread(&Watcher::Loop, this);
}

void Watcher::Stop()
{
    mbStop = true;
    mCv.notify_all();
    if (mThread.joinable())
        mThread.join();
}

void Watcher::Loop()
{
    while (!mbStop.load())
    {
        PollOnce();

        std::unique_lock lock(mCvMutex);
        mCv.wait_for(lock, std::chrono::seconds(mConfig.nPollIntervalSec),
                    [this] { return mbStop.load(); });
    }
}

void Watcher::PollOnce()
{
    IngestPending();
    DeterministicTriage();
    MaybeInvokeAgent();   // dormant with no --on-new-jots, and typically finds nothing left to do
                          // either way now - DeterministicTriage resolves every status:unprocessed
                          // jot one way or another (processed, or tagged tbd) before this runs.
    EnsureNeedsInputVisible();
}

void Watcher::DeterministicTriage()
{
    Query query;
    query.mTags   = { "status:unprocessed" };
    query.mnLimit = 200;   // free - no model involved, no reason to be stingy

    SearchResultSet results;
    if (mOps.Search(query, results) || results.mJots.empty())
        return;

    // The existing vocabulary, for reusing a tag rather than minting "webserver" alongside an
    // existing "WebServer" - the same rule the prompt gave an LLM, run here without one.
    std::vector<TagStat> vVocab;
    mOps.ListTags(vVocab, false);

    NameTables names;
    mStore.SnapshotNames(names);

    for (const Jot& jot : results.mJots)
    {
        const FlatJot flat = Flatten(jot, names);

        std::string sClean;
        const std::vector<std::string> vBrackets = ExtractBrackets(flat.msText, sClean);

        std::vector<std::string> vTags;
        for (const std::string& t : flat.mTags)
            if (t != "status:unprocessed")
                vTags.push_back(t);

        // Confident means: either the jot told us its own structure ([todo], [tag]), or it is
        // short enough that its own text needs no interpretation to serve as a summary. Long,
        // unstructured prose is the case this cannot safely guess at - see the tbd branch below.
        const bool bConfident = !vBrackets.empty() || flat.msText.size() <= 150;

        bool bTodo = false;
        for (const std::string& sRaw : vBrackets)
        {
            const std::string sLower = Lower(sRaw);
            if (sLower == "todo") { bTodo = true; continue; }

            std::string sChosen = sLower;
            for (const TagStat& v : vVocab)
                if (Lower(v.msTag) == sLower) { sChosen = v.msTag; break; }
            if (std::find(vTags.begin(), vTags.end(), sChosen) == vTags.end())
                vTags.push_back(sChosen);
        }
        if (bTodo && std::find(vTags.begin(), vTags.end(), "todo") == vTags.end())
            vTags.push_back("todo");

        // Exact, not approximate: the offset applies to the JOT'S OWN id, which IS its ingestion
        // timestamp (Jot.h) - no notion of "now" is needed, so this does not care how long the
        // jot sat pending before this poll happened to run.
        int64_t nOffsetUS = 0;
        if (bTodo && ParseRelativeOffset(flat.msText, nOffsetUS))
        {
            vTags.erase(std::remove_if(vTags.begin(), vTags.end(),
                        [](const std::string& t) { return t.rfind("due:", 0) == 0; }),
                       vTags.end());
            vTags.push_back("due:" + FormatDueLocal(jot.mID + nOffsetUS));
        }

        JotInput patch;
        if (!bConfident)
        {
            // tbd IMPLIES todo, always, in code rather than relying on a human or a prompt to
            // remember both - EnsureNeedsInputVisible enforces this too, as a backstop for a tbd
            // tag applied any other way (by hand, say), but setting it here is the normal path.
            if (std::find(vTags.begin(), vTags.end(), "todo") == vTags.end())
                vTags.push_back("todo");
            vTags.push_back("tbd");
        }
        else if (!sClean.empty())
        {
            patch.msSummary = sClean.size() > 100 ? sClean.substr(0, 100) : sClean;
        }
        // else: e.g. a jot that was just "[todo]" with nothing else - nothing to summarize,
        // leave the summary field untouched.

        patch.mTags = vTags;
        AddResult result;
        mOps.Update(jot.mID, patch, 0, result);   // best-effort; retried next poll if this fails
    }
}

void Watcher::EnsureNeedsInputVisible()
{
    Query query;
    query.mTags   = { "tbd" };
    query.mnLimit = 100;

    SearchResultSet results;
    if (mOps.Search(query, results))
        return;

    NameTables names;
    mStore.SnapshotNames(names);

    for (const Jot& jot : results.mJots)
    {
        const FlatJot flat = Flatten(jot, names);
        const bool bHasTodo = std::find(flat.mTags.begin(), flat.mTags.end(), "todo")
                             != flat.mTags.end();
        const bool bHasHighPriority = std::find(flat.mTags.begin(), flat.mTags.end(), "priority:high")
                                      != flat.mTags.end();
        if (bHasTodo && bHasHighPriority)
            continue;

        std::vector<std::string> vTags = flat.mTags;
        // Drop any other priority rather than have two priority: tags fight over which one the
        // dashboard reads - a needs-input jot is high priority, full stop.
        vTags.erase(std::remove_if(vTags.begin(), vTags.end(),
                    [](const std::string& t) { return t.rfind("priority:", 0) == 0; }),
                   vTags.end());
        if (!bHasTodo)
            vTags.push_back("todo");
        vTags.push_back("priority:high");

        JotInput patch;
        patch.mTags = vTags;
        AddResult result;
        mOps.Update(jot.mID, patch, 0, result);   // best-effort; retried next poll if this fails
    }
}

size_t Watcher::IngestPending()
{
    size_t nTotalImported = 0;

    for (const WatchStatus& s : mWatch.Resolve())
    {
        if (!s.mbExists)
            continue;

        // A rotate=true source is drained unconditionally whenever it has any bytes at all, not
        // gated on WatchList's own pending test - that test exists to answer "should GET /watch
        // show this as needing attention", a different question from "is there anything here to
        // take". Once a rotate source is always emptied on ingest, "pending" and "has bytes" are
        // the same fact anyway; asking the pending test too would only add a way for stale state
        // (a lost loom.watch-state.json, say - see WatchList.h) to leave real content stranded.
        const bool bShouldIngest = s.mbRotate ? (s.mnSizeBytes > 0) : s.mbPending;
        if (!bShouldIngest)
            continue;

        std::string sImportPath = s.msPath;
        std::string sArchivedPath;

        if (s.mbRotate)
        {
            const std::filesystem::path srcPath(s.msPath);
            const std::filesystem::path archiveDir = srcPath.parent_path() / "ingested";

            std::error_code ecDir;
            std::filesystem::create_directories(archiveDir, ecDir);
            if (ecDir)
            {
                RaiseAlert("triage-alert-failure",
                          "could not create " + archiveDir.string() + ": " + ecDir.message());
                continue;
            }

            const std::filesystem::path dest =
                archiveDir / (srcPath.filename().string() + "." + UtcStamp());

            std::error_code ecMove;
            std::filesystem::rename(srcPath, dest, ecMove);
            if (ecMove)
            {
                // A second source hitting the exact same UTC second would collide here - rare
                // enough (one attempt per poll cycle, one file) that surfacing it as a failure
                // alert to retry next cycle is the right amount of handling, not a uniquifier.
                RaiseAlert("triage-alert-failure",
                          "could not archive " + s.msPath + " -> " + dest.string() + ": " +
                          ecMove.message());
                continue;
            }
            sImportPath   = dest.string();
            sArchivedPath = dest.string();
        }

        ImportStats st;
        const std::error_code ecImport = IMPORT::JotsLog(sImportPath, mStore, "user", st);
        if (ecImport)
        {
            RaiseAlert("triage-alert-failure",
                      "ingest of " + sImportPath + " failed: " + ecImport.message() +
                      (sArchivedPath.empty() ? "" : " (content preserved at " + sArchivedPath + ")"));
            continue;
        }

        nTotalImported += st.mnImported;

        if (st.mnMalformed > 0)
        {
            // The content is never recoverable from inside Loom once this happens - Importer
            // counts malformed lines but never captures their text (persist/Importer.cpp) - so the
            // alert has to point at the one place the bytes still exist.
            std::ostringstream oss;
            oss << st.mnMalformed << " malformed line(s) in "
                << (sArchivedPath.empty() ? sImportPath : sArchivedPath)
                << " were not imported - inspect the file by hand";
            RaiseAlert("triage-alert-failure", oss.str());
        }

        if (s.mbRotate)
        {
            // Recorded purely for GET /watch's lastIngestUS / dashboard display - a rotated
            // source's live path no longer holds the bytes that were just measured, so there is no
            // meaningful size/mtime to remember here the way the non-rotate branch below does.
            mWatch.MarkIngested(s.msPath, 0, 0);
        }
        else
        {
            for (const WatchStatus& fresh : mWatch.Resolve())
            {
                if (fresh.msPath == s.msPath && fresh.mbExists)
                {
                    mWatch.MarkIngested(s.msPath, fresh.mnSizeBytes, fresh.mnMtimeUS);
                    break;
                }
            }
        }
    }

    return nTotalImported;
}

void Watcher::MaybeInvokeAgent()
{
    // In practice this finds nothing, now: DeterministicTriage (see Watcher.h) resolves every
    // status:unprocessed jot before this ever runs, one way or another - normal processing, or
    // tagged tbd. Left in place (dormant with no --on-new-jots configured) rather than removed, in
    // case a future backlog of tbd jots is ever worth handing to an agent in a deliberate batch.
    Query query;
    query.mTags    = { "status:unprocessed" };
    query.mNotTags = { "tbd" };
    query.mOrder   = eOrder::kOldest;
    query.mnLimit  = mConfig.nMaxIdsPerRun;

    SearchResultSet results;
    if (std::error_code ec = mOps.Search(query, results))
        return;   // transient search failure; try again next cycle
    if (results.mJots.empty())
        return;   // nothing to do - no model call, full stop

    if (mConfig.sOnNewJots.empty())
        return;   // ingest-only mode - configured with no agent to hand this to

    // ---- Guardrails, in the order documented on the class ----

    if (mbBreakerOpen.load())
        return;   // already alerted; stays open until the service is restarted - see Watcher.h

    const int64_t nNowUS = NowUS();

    if (mnLastRunUS != 0 && (nNowUS - mnLastRunUS) < static_cast<int64_t>(mConfig.nCooldownSec) * 1000000)
        return;   // new jots simply wait for the next eligible window

    while (!mRunTimesLastHour.empty() && (nNowUS - mRunTimesLastHour.front()) > 3600LL * 1000000)
        mRunTimesLastHour.pop_front();
    if (static_cast<int>(mRunTimesLastHour.size()) >= mConfig.nMaxRunsPerHour)
    {
        mbBudgetExceeded = true;
        if (!mbBudgetAlertActive)
        {
            RaiseAlert("triage-alert-budget",
                      std::to_string(mRunTimesLastHour.size()) + " runs in the last hour reached "
                      "the configured limit of " + std::to_string(mConfig.nMaxRunsPerHour) +
                      " - waiting for the window to roll forward");
            mbBudgetAlertActive = true;
        }
        return;
    }

    const double fSpent24h = mLedger.Sum24hUSD();
    if (fSpent24h >= mConfig.fMaxDailyUSD)
    {
        mbBudgetExceeded = true;
        if (!mbBudgetAlertActive)
        {
            std::ostringstream oss;
            oss << "24h spend $" << fSpent24h << " has reached the configured daily limit of $"
                << mConfig.fMaxDailyUSD << " - no further triage runs until it rolls off";
            RaiseAlert("triage-alert-budget", oss.str());
            mbBudgetAlertActive = true;
        }
        return;
    }

    // Under budget - clear a previously-active budget alert rather than leaving a stale "waiting"
    // card around once spend has rolled back off.
    if (mbBudgetAlertActive)
    {
        ResolveAlert("triage-alert-budget");
        mbBudgetAlertActive = false;
    }
    mbBudgetExceeded = false;

    // ---- All clear - invoke ----

    std::vector<std::string> vArgs;
    vArgs.push_back(mConfig.sOnNewJots);
    for (const Jot& jot : results.mJots)
        vArgs.push_back(std::to_string(jot.mID));

    mnLastRunUS = nNowUS;
    mRunTimesLastHour.push_back(nNowUS);
    mnRunsThisProcess.fetch_add(1, std::memory_order_relaxed);

    const ChildResult child = RunChildWithTimeout(vArgs, mConfig.nChildTimeoutSec);

    TriageRun run;
    run.mnAtUS      = nNowUS;
    run.mnJotsGiven = results.mJots.size();
    run.msTrigger   = std::to_string(results.mJots.size()) + " unprocessed jot" +
                      (results.mJots.size() == 1 ? "" : "s");

    if (child.bLaunchFailed)
    {
        run.mbSuccess = false;
        run.msFailure = "could not launch '" + mConfig.sOnNewJots + "'";
    }
    else if (child.bTimedOut)
    {
        run.mbSuccess = false;
        run.msFailure = "timed out after " + std::to_string(mConfig.nChildTimeoutSec) + "s";
    }
    else
    {
        const ParsedRunOutput parsed = ParseClaudeJson(child.sStdout);
        if (!parsed.bParsed)
        {
            run.mbSuccess = false;
            run.msFailure = "exit " + std::to_string(child.nExitCode) +
                            ", output was not the expected JSON";
        }
        else
        {
            run.mfCostUSD             = parsed.fCostUSD;
            run.mnDurationMS          = parsed.nDurationMS;
            run.mnInputTokens         = parsed.nInputTokens;
            run.mnOutputTokens        = parsed.nOutputTokens;
            run.mnCacheReadTokens     = parsed.nCacheReadTokens;
            run.mnCacheCreationTokens = parsed.nCacheCreationTokens;
            run.msSessionID           = parsed.sSessionID;

            if (child.nExitCode != 0 || parsed.bIsError)
            {
                run.mbSuccess = false;
                run.msFailure = "exit " + std::to_string(child.nExitCode) +
                                (parsed.sResultText.empty() ? "" : ": " + parsed.sResultText);
            }
            else
            {
                run.mbSuccess = true;
            }
        }
    }

    mLedger.Record(run);

    if (run.mbSuccess)
    {
        mnConsecutiveFailures = 0;
        if (mbFailureAlertActive)
        {
            ResolveAlert("triage-alert-failure");
            mbFailureAlertActive = false;
        }
    }
    else
    {
        const int nFailures = mnConsecutiveFailures.fetch_add(1) + 1;
        if (nFailures >= mConfig.nMaxConsecutiveFailures)
        {
            mbBreakerOpen = true;
            RaiseAlert("triage-alert-failure",
                      std::to_string(nFailures) + " consecutive failures (latest: " +
                      run.msFailure + ") - auto-triage is now PAUSED until loom is restarted "
                      "after this is fixed");
            mbFailureAlertActive = true;
        }
    }
}

void Watcher::RaiseAlert(const std::string& sSlug, const std::string& sLine)
{
    Jot existing;
    const bool bHadExisting = !mOps.GetByName(sSlug, existing);

    // Cap history at 5 entries, newest first - this is a card to glance at, not a growing log.
    // Anything older either self-resolved or is captured by whatever's newest here.
    std::vector<std::string> vLines;
    if (bHadExisting)
    {
        std::istringstream iss(existing.msText);
        std::string sOne;
        while (std::getline(iss, sOne))
            vLines.push_back(sOne);
    }
    vLines.insert(vLines.begin(), LOOMTIME::FormatUS(NowUS()) + "  " + sLine);
    if (vLines.size() > 5)
        vLines.resize(5);

    std::ostringstream oss;
    for (size_t i = 0; i < vLines.size(); ++i)
    {
        if (i) oss << "\n";
        oss << vLines[i];
    }

    // A generic diagnostic prompt rather than none at all - "the card itself should have the
    // prompt necessary to address it" applies here too: a broken exec path or an exhausted budget
    // is exactly the kind of thing worth handing to an agent to investigate (read the journal,
    // check the on-new-jots path, check RunLedger) rather than a human debugging it cold.
    oss << "\n\n---\nTo investigate: read journalctl -u loom (or wherever this instance logs), "
           "check the --on-new-jots command still exists and runs by hand, and check GET /stats "
           "for the current triage/budget numbers. Paste this whole card to an agent for help.";

    JotInput in;
    in.msName    = sSlug;
    in.msSummary = sSlug == "triage-alert-budget"
                       ? "loom-triage has hit a spend guardrail"
                       : "loom-triage needs attention";
    in.msText    = oss.str();
    in.mTags     = std::vector<std::string>{ "todo", "priority:high", "type:prompt" };
    in.msEditor  = "loom-watcher";

    AddResult result;
    mOps.Upsert(in, 0, result);   // best-effort - a failure to write the alert has nowhere further
                                  // to escalate to, so it is simply not retried until the next trip
}

void Watcher::ResolveAlert(const std::string& sSlug)
{
    Jot existing;
    if (mOps.GetByName(sSlug, existing))
        return;   // nothing to resolve

    NameTables names;
    mStore.SnapshotNames(names);
    const FlatJot flat = Flatten(existing, names);

    if (std::find(flat.mTags.begin(), flat.mTags.end(), "status:done") != flat.mTags.end())
        return;   // already resolved

    std::vector<std::string> vTags = flat.mTags;
    vTags.push_back("status:done");   // ADDS, keeps todo - see class comment and
                                      // web/Dashboard.h's isDone/toggleDone convention

    JotInput in;
    in.msName   = sSlug;
    in.msText   = flat.msText + "\n\nresolved " + LOOMTIME::FormatUS(NowUS());
    in.mTags    = vTags;
    in.msEditor = "loom-watcher";

    AddResult result;
    mOps.Upsert(in, 0, result);
}
