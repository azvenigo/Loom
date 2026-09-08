// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// loom - the service entry point.
//
// Startup order matters: load the snapshot and replay the log BEFORE the journal starts appending,
// or replay would re-log everything it just read and the WAL would double on every restart.
//
// Arg parsing is minimal and swaps to CLP::CLI_Parser when the ZLibraries submodule lands.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "core/IpAcl.h"
#include "core/JotStore.h"
#include "core/LoomTime.h"
#include "core/Ops.h"
#include "http/HttpServer.h"
#include "persist/DataLock.h"
#include "persist/History.h"
#include "persist/Importer.h"
#include "persist/Journal.h"
#include "persist/JotpostStatus.h"
#include "persist/Purge.h"
#include "persist/RunLedger.h"
#include "persist/SinkFanout.h"
#include "persist/Snapshot.h"
#include "persist/TriageClient.h"
#include "persist/WatchList.h"
#include "persist/Watcher.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{
    HttpServer* gpServer = nullptr;

    void OnSignal(int)
    {
        // Only async-signal-safe work here: flip the flag inside the server and let Run() unwind.
        if (gpServer)
            gpServer->Stop();
    }

    const char* ArgStr(int argc, char** argv, const char* pName, const char* pDefault)
    {
        const size_t nLen = std::strlen(pName);
        for (int i = 1; i < argc; ++i)
        {
            if (std::strncmp(argv[i], pName, nLen) == 0 && argv[i][nLen] == '=')
                return argv[i] + nLen + 1;
        }
        return pDefault;
    }

    bool ArgFlag(int argc, char** argv, const char* pName)
    {
        for (int i = 1; i < argc; ++i)
        {
            if (std::strcmp(argv[i], pName) == 0)
                return true;
        }
        return false;
    }

    // A handful of records so /jots, /tags and /tags/similar all have something to show. The
    // near-duplicate tags are deliberate - they demonstrate the write-time warning and give
    // /tags/similar a real cluster to find.
    void Seed(Ops& ops)
    {
        struct SeedJot
        {
            const char* pText;
            const char* pName;
            const char* pSummary;
            const char* pTags;
            const char* pEditor;
        };

        static const SeedJot kSeeds[] = {
            { "Ordered 50ft of ethernet cable today", nullptr, nullptr, nullptr, nullptr },
            { "Watered the office plants and topped off the coffee supply.",
              nullptr, nullptr, "errands", "user" },
            { "Fixed the flaky retry logic in the integration test suite.",
              nullptr, nullptr, "ci", "user" },
            { "The nightly build keeps timing out on the integration suite - looks like a leak.",
              nullptr, nullptr, "cicd", "user" },
            { "Staging deploys automatically on merge to main. Production requires a manual "
              "approval step and runs the smoke tests first.",
              "deploy-notes", "how staging and production deploys work", "infra", "claude" },
            { "Backups run nightly at 02:00 and are verified with a checksum before rotation.",
              nullptr, nullptr, "infrastructure", "user" },
            { "Prefer clear names over clever one-liners. Keep functions small. Write tests for "
              "anything with a tricky invariant.",
              "project-conventions", "coding style and review conventions for this project",
              "type:reference", "claude" },
            { "Loom keeps every jot in RAM with an inverted index over summary and body, ranked "
              "with BM25 plus a recency multiplier.",
              "loom-design", "how Loom indexes and ranks jots", "loom", "codex" },
            { "Query cost is O(matched postings). Postings carry slot, tf and doclen so scoring "
              "is a linear streaming scan with no random access.",
              nullptr, nullptr, "loom", "codex" },
        };

        for (const SeedJot& s : kSeeds)
        {
            JotInput in;
            in.msText = s.pText;
            if (s.pName)    in.msName    = s.pName;
            if (s.pSummary) in.msSummary = s.pSummary;
            if (s.pTags)    in.mTags     = std::vector<std::string>{ s.pTags };
            if (s.pEditor)  in.msEditor  = s.pEditor;

            AddResult result;
            if (std::error_code ec = ops.Add(in, result))
                std::printf("  seed failed: %s\n", ec.message().c_str());
        }
    }
}


namespace
{
    //--------------------------------------------------------------------------------------------
    // --purge=DIR. The destructive half of the purge, run with the service stopped.
    //
    // THE DATA LOCK IS THE INTERLOCK. This takes the same lock the server holds for its whole life,
    // so "is Loom still running against this directory" is answered by the OS rather than by a
    // check somebody could race or skip. A refusal here is the guard working.
    //
    // A BARE --purge IS A DRY RUN. It says exactly what would go and changes nothing; --yes is what
    // actually erases. That makes a mistyped or half-remembered command harmless, and it gives the
    // person doing the confirming something to read that came from the tool rather than from the
    // request file they are being asked to trust.
    //--------------------------------------------------------------------------------------------
    int RunPurge(const std::string& sDir, bool bConfirmed)
    {
        DataLock lock;
        if (!lock.Acquire(sDir + "/loom.lock"))
        {
            std::printf("loom is still running against %s\n", sDir.c_str());
            std::printf("  stop the service before purging - the snapshot, the WAL and the history\n"
                        "  log are all open and being appended to.\n");
            return 1;
        }

        const std::string sPath = PURGE::RequestPath(sDir);
        PurgeRequest request;
        std::string sError;
        if (!PURGE::ReadRequest(sPath, request, sError))
        {
            std::printf("%s\n", sError.c_str());
            std::printf("  a purge is requested from the dashboard (History view) or with\n"
                        "  POST /purge/request, which writes the file this reads.\n");
            return 1;
        }

        std::printf("purge request from %s, created %s\n",
                    request.msRequestedBy.empty() ? "(unknown)" : request.msRequestedBy.c_str(),
                    LOOMTIME::FormatUS(request.mnCreatedUS).c_str());
        if (!request.msReason.empty())
            std::printf("  reason: %s\n", request.msReason.c_str());
        std::printf("  %zu jot(s):\n", request.mIDs.size());
        for (const PurgeLabel& label : request.mLabels)
        {
            std::printf("    %lld  %-28s %s\n",
                        static_cast<long long>(label.mID),
                        label.msName.empty() ? "(unnamed)" : label.msName.c_str(),
                        label.msSummary.c_str());
        }

        if (!bConfirmed)
        {
            std::printf("\nDRY RUN - nothing has been changed.\n");
            std::printf("  Re-run with --yes to erase these from the snapshot, the WAL and the\n"
                        "  history log. That cannot be undone: the undo log is one of the things\n"
                        "  being scrubbed.\n");
            return 0;
        }

        PurgeReport report;
        if (std::error_code ec = PURGE::Run(sDir, request, report, sError))
        {
            std::printf("purge FAILED: %s\n", sError.empty() ? ec.message().c_str() : sError.c_str());
            std::printf("  the request file is left in place so this can be retried.\n");
            return 1;
        }

        PURGE::ClearRequest(sPath);

        std::printf("\npurged.\n");
        std::printf("  removed from the store   %zu\n", report.mnRemovedFromStore);
        if (report.mnNotFound)
            std::printf("  already absent           %zu\n", report.mnNotFound);
        std::printf("  snapshot rewritten       %zu records\n", report.mnSnapshotRecords);
        std::printf("  WAL discarded            %zu bytes\n", report.mnWalBytesDiscarded);
        std::printf("  history entries dropped  %zu (kept %zu)\n",
                    report.mnHistoryDropped, report.mnHistoryKept);
        std::printf("\nStart the service again. Backups of this directory, and anything that already\n"
                    "read these jots over the API, are out of scope and still need handling by hand.\n");
        return 0;
    }
}


int main(int argc, char** argv)
{
    if (ArgFlag(argc, argv, "--help") || ArgFlag(argc, argv, "-h"))
    {
        std::printf(
            "loom - in-RAM jot service\n\n"
            "  --bind=ADDR    interface to listen on   (default 127.0.0.1)\n"
            "  --port=N       port                     (default 7700)\n"
            "  --threads=N    worker threads           (default: hardware concurrency)\n"
            "  --token=SECRET require 'Authorization: Bearer SECRET' on every route but /health\n"
            "  --data=DIR     persist to DIR/loom.wal + DIR/loom.snapshot (default ./loom-data)\n"
            "  --no-persist   RAM only; a restart starts empty\n"
            "  --sync=MODE    never | interval | always            (default interval)\n"
            "  --seed         populate a few sample jots, only if the store loads empty\n"
            "\n"
            "  The address allow list lives in DIR/loom.acl.json and is edited from the dashboard\n"
            "  (shield icon, bottom left) or over PUT /acl. Loopback is always allowed, so a bad\n"
            "  list can always be repaired from the machine itself.\n"
            "\n"
            "  DIR/loom.watch.json lists files Loom watches for new content (edited from the\n"
            "  dashboard or over PUT /watch). A file whose size and mtime have both changed since\n"
            "  it was last pulled in shows up on the dashboard's Needs Attention card; POST\n"
            "  /watch/ingest actually imports it. Nothing is watched in the background - this is\n"
            "  only checked when asked.\n"
            "\n"
            "  Every change is also appended to DIR/loom.history, which - unlike the WAL - is never\n"
            "  truncated by a snapshot. That is what GET /history and the dashboard's History view\n"
            "  read, and what a restore re-applies.\n"
            "\n"
            "  --purge=DIR    erase the jots named in DIR/loom.purge-request.json from the snapshot,\n"
            "                 the WAL and the history log. Requires the service to be STOPPED. On\n"
            "                 its own it is a dry run; add --yes to actually erase.\n"
            "\n"
            "  --on-new-jots=CMD   run CMD, with each pending status:unprocessed jot id appended as\n"
            "                      an argument, whenever there is triage work AND every guardrail\n"
            "                      below allows it. Unset (default): watched files still poll and\n"
            "                      ingest in the background, nothing ever invokes a command.\n"
            "  --watch-interval=N  seconds between background polls of watched files (default 15).\n"
            "  --triage-cooldown=N seconds between --on-new-jots invocations (default 600). Each\n"
            "                      invocation has a fixed session-overhead cost regardless of\n"
            "                      batch size, so a longer cooldown lets more jots share it.\n"
            "  --triage-max-hourly=N  max --on-new-jots invocations per rolling hour (default 6).\n"
            "  --triage-daily-usd=F   stop invoking once the rolling 24h cost (as reported by the\n"
            "                         command's own accounting) reaches this many dollars (default\n"
            "                         0.50). See DIR/loom.triage-runs.json and GET /stats.\n"
            "  --triage-timeout=N     kill --on-new-jots if it runs longer than N seconds (300).\n"
            "  --triage-max-ids=N     cap how many jot ids one invocation is given (default 20).\n"
            "  --triage-max-failures=N  consecutive failures before auto-triage pauses itself until\n"
            "                           loom is restarted (default 3). See persist/Watcher.h.\n"
            "\n"
            "  --jotpost-host=HOST  health-check a jotpost instance (jotpost/main.cpp) at HOST:PORT\n"
            "                       with a cached TCP connect, surfaced as GET /stats' \"jotpost\"\n"
            "                       block and on the dashboard's Health page. Unset (default): no\n"
            "                       check, no block.\n"
            "  --jotpost-port=N     port for the above (default 7701).\n"
            "\n"
            "  --resolver-host=HOST  ask the offline triage service (persist/TriageClient.h) for\n"
            "                        whatever the built-in rules could not settle - summary, kind,\n"
            "                        topical tags, priority, a due phrase the regex cannot read -\n"
            "                        before falling back to tagging a jot `tbd` for you. Unset\n"
            "                        (default): no calls, unchanged behaviour.\n"
            "  --resolver-port=N     port for the above (default 7711).\n"
            "  --resolver-token-file=PATH  file holding the resolver's bearer token. A FILE, not a\n"
            "                        --resolver-token=SECRET flag, so the token never shows up in\n"
            "                        `ps` output or a shell history.\n"
            "  --resolver-timeout=N  seconds to wait on one resolve call (default 20). This waits on\n"
            "                        model inference, not just a socket.\n"
            "  --resolver-max-per-poll=N  most jots to resolve in one watch cycle (default 4), so a\n"
            "                        backlog cannot stall background ingestion. The rest wait for\n"
            "                        the next poll.\n");
        return 0;
    }

    if (const char* pPurge = ArgStr(argc, argv, "--purge", nullptr))
    {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        return RunPurge(pPurge, ArgFlag(argc, argv, "--yes"));
    }

    // stdout is block-buffered when redirected to a file, so a service launched from a script or a
    // unit file would show nothing at all until it exited. Unbuffered costs nothing here - this
    // process prints a handful of lines at startup and shutdown, not per request.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    HttpConfig config;
    config.msBind    = ArgStr(argc, argv, "--bind", "127.0.0.1");
    config.mnPort    = static_cast<uint16_t>(std::atoi(ArgStr(argc, argv, "--port", "7700")));
    config.mnThreads = static_cast<size_t>(std::atoi(ArgStr(argc, argv, "--threads", "0")));
    config.msToken   = ArgStr(argc, argv, "--token", "");

    JotStore   store;
    Ops        ops(store);
    Journal    journal;
    History    history;
    SinkFanout sinks;
    IpAcl      acl;
    WatchList  watch;
    RunLedger  triageLedger;

    const bool bPersist = !ArgFlag(argc, argv, "--no-persist");

    SnapshotConfig snapConfig;
    DataLock       dataLock;
    if (bPersist)
    {
        const std::string sDir = ArgStr(argc, argv, "--data", "loom-data");
        std::error_code ecDir;
        std::filesystem::create_directories(sDir, ecDir);

        // Claim the directory BEFORE reading it. Loom has no cross-process locking anywhere else,
        // so a second instance here would interleave writes into loom.wal silently. Taking it
        // ahead of the load also means the loser exits without having read a thing.
        if (!dataLock.Acquire(sDir + "/loom.lock"))
        {
            std::printf("loom is already running against %s\n", sDir.c_str());
            std::printf("  one instance per data directory - use --data to point elsewhere, or\n"
                        "  --no-persist for a throwaway instance.\n");
            return 1;
        }

        snapConfig.msPath    = sDir + "/loom.snapshot";
        snapConfig.msWalPath = sDir + "/loom.wal";

        // Loaded here rather than inside the server so a bad list is reported at startup, on the
        // console, where somebody can see it - not on the first refused request.
        std::string sAclWarning;
        acl.Load(sDir + "/loom.acl.json", sAclWarning);
        if (!sAclWarning.empty())
            std::printf("  %s\n", sAclWarning.c_str());

        std::string sWatchWarning;
        watch.Load(sDir + "/loom.watch.json", sDir + "/loom.watch-state.json", sWatchWarning);
        if (!sWatchWarning.empty())
            std::printf("  %s\n", sWatchWarning.c_str());

        // Loaded here rather than lazily so the 24h spend guard reads real history from the very
        // first poll - see RunLedger.h on why that figure must survive a restart to mean anything.
        std::string sLedgerWarning;
        triageLedger.Load(sDir + "/loom.triage-runs.json", sLedgerWarning);
        if (!sLedgerWarning.empty())
            std::printf("  %s\n", sLedgerWarning.c_str());

        // Load BEFORE opening the journal. Replaying with the sink already attached would re-log
        // every record it just read, doubling the WAL on each restart.
        size_t nLoaded = 0, nReplayed = 0, nDropped = 0;
        if (std::error_code ec = SNAPSHOT::Load(snapConfig, store, nLoaded, nReplayed, nDropped))
            std::printf("  load failed: %s\n", ec.message().c_str());

        std::printf("loaded %zu from snapshot, replayed %zu from log", nLoaded, nReplayed);
        if (nDropped)
            std::printf(", dropped %zu torn/unparseable", nDropped);
        std::printf("\n");

        JournalConfig jcfg;
        jcfg.msPath = snapConfig.msWalPath;
        const std::string sSync = ArgStr(argc, argv, "--sync", "interval");
        jcfg.mSync = (sSync == "always") ? eSyncPolicy::kAlways
                   : (sSync == "never")  ? eSyncPolicy::kNever
                                         : eSyncPolicy::kInterval;

        if (std::error_code ec = journal.Open(jcfg))
        {
            std::printf("  journal failed to open: %s - running RAM only\n", ec.message().c_str());
        }
        else
        {
            sinks.Add(&journal);

            // The undo log is opened AFTER the WAL and added to the fan-out second, so the
            // durability path always sees a mutation first. A history failure is reported and then
            // ignored: it costs undo, not data, and refusing to start over it would be the tail
            // wagging the dog.
            HistoryConfig hcfg;
            hcfg.msPath = sDir + "/loom.history";
            if (std::error_code ecHist = history.Open(hcfg))
                std::printf("  history log failed to open: %s - undo unavailable\n",
                            ecHist.message().c_str());
            else
                sinks.Add(&history);

            store.SetJournalSink(&sinks);
        }
    }

    // Import BEFORE seeding, and after the journal is open so imported records are durable.
    // Idempotent - the id is derived from the entry timestamp, so re-running skips what is there.
    if (const char* pImport = ArgStr(argc, argv, "--import", nullptr))
    {
        ImportStats st;
        if (std::error_code ec = IMPORT::JotsLog(pImport, store, "user", st))
        {
            std::printf("  import failed: %s (%s)\n", ec.message().c_str(), pImport);
        }
        else
        {
            std::printf("imported %zu of %zu lines from %s\n", st.mnImported, st.mnLines, pImport);
            if (st.mnSkipped)   std::printf("  %zu already present\n", st.mnSkipped);
            if (st.mnBumped)    std::printf("  %zu same-second collisions bumped\n", st.mnBumped);
            if (st.mnMalformed) std::printf("  %zu malformed lines\n", st.mnMalformed);
            if (st.mnImported)
                std::printf("  span %s .. %s\n",
                            LOOMTIME::FormatUS(st.mnOldestUS).c_str(),
                            LOOMTIME::FormatUS(st.mnNewestUS).c_str());
        }
    }

    // Only seed a store that came up empty, so --seed can stay in a launch script without
    // duplicating the sample records on every restart.
    if (ArgFlag(argc, argv, "--seed") && store.Size() == 0)
    {
        Seed(ops);
        std::printf("seeded %zu jots\n", store.Size());
    }

    // The local due-date resolver, if one is configured. Constructed before the Watcher because
    // the Watcher borrows it; null means "not configured", the same nullable-optional-feature
    // shape jotpost's status probe uses below. See persist/TriageClient.h.
    TriageConfig resolverConfig;
    resolverConfig.sHost       = ArgStr(argc, argv, "--resolver-host", "");
    resolverConfig.nPort       = static_cast<uint16_t>(
                                     std::atoi(ArgStr(argc, argv, "--resolver-port", "7711")));
    resolverConfig.nTimeoutMS  = std::atoi(ArgStr(argc, argv, "--resolver-timeout", "20")) * 1000;
    resolverConfig.nMaxPerPoll = std::atoi(ArgStr(argc, argv, "--resolver-max-per-poll", "4"));

    std::unique_ptr<TriageClient> pTriage;
    if (!resolverConfig.sHost.empty())
    {
        // Read from a file rather than a flag so the secret is never in `ps` or a shell history -
        // and trim, because a token file written by an editor almost always ends in a newline and
        // a stray \r or \n in an Authorization header is a 400 that looks like a wrong token.
        const char* pTokenFile = ArgStr(argc, argv, "--resolver-token-file", "");
        if (pTokenFile && *pTokenFile)
        {
            std::ifstream tokenIn(pTokenFile, std::ios::binary);
            if (tokenIn)
            {
                std::string sToken((std::istreambuf_iterator<char>(tokenIn)),
                                   std::istreambuf_iterator<char>());
                const size_t nEnd = sToken.find_last_not_of(" \t\r\n");
                resolverConfig.sToken = (nEnd == std::string::npos) ? "" : sToken.substr(0, nEnd + 1);
            }
        }

        // Refuse loudly rather than starting a resolver that will 401 on every call - an
        // unreachable token file is a config mistake, and silently degrading to `tbd` would look
        // exactly like the resolver simply declining everything.
        if (resolverConfig.sToken.empty())
        {
            std::fprintf(stderr,
                         "loom: --resolver-host set but no token could be read from "
                         "--resolver-token-file; refusing to start the resolver.\n");
            return 1;
        }
        pTriage = std::make_unique<TriageClient>(resolverConfig);
    }

    WatcherConfig watcherConfig;
    watcherConfig.nPollIntervalSec        = std::atoi(ArgStr(argc, argv, "--watch-interval", "15"));
    // 60s measured as a real problem, not a theoretical one: each `claude -p` invocation pays a
    // fixed ~$0.02-0.03 of session/cache overhead regardless of how many jots it processes (its
    // own system prompt and tool schemas, not Loom's content), so a jot arriving alone every
    // minute means paying that fixed cost once PER JOT instead of once per batch. 10 minutes lets
    // jots that arrive close together share one invocation's overhead instead of each buying its
    // own - same-session turnaround, not real-time, which is the right trade for background triage.
    watcherConfig.nCooldownSec            = std::atoi(ArgStr(argc, argv, "--triage-cooldown", "600"));
    watcherConfig.nMaxRunsPerHour         = std::atoi(ArgStr(argc, argv, "--triage-max-hourly", "6"));
    watcherConfig.fMaxDailyUSD            = std::atof(ArgStr(argc, argv, "--triage-daily-usd", "0.50"));
    watcherConfig.nChildTimeoutSec        = std::atoi(ArgStr(argc, argv, "--triage-timeout", "300"));
    watcherConfig.nMaxIdsPerRun           = static_cast<size_t>(
                                                std::atoi(ArgStr(argc, argv, "--triage-max-ids", "20")));
    watcherConfig.nMaxConsecutiveFailures = std::atoi(ArgStr(argc, argv, "--triage-max-failures", "3"));
    watcherConfig.sOnNewJots              = ArgStr(argc, argv, "--on-new-jots", "");

    // Started unconditionally, even with no --on-new-jots: background ingestion of watched files
    // (rotate-and-import) is useful on its own, with or without an agent to hand new jots to.
    Watcher watcher(watch, ops, store, triageLedger, watcherConfig, pTriage.get());
    watcher.Start();

    // Unset host = feature off: no probing, and /stats simply omits the "jotpost" block (same
    // nullable-optional-feature pattern watcher/ledger already use). See persist/JotpostStatus.h.
    JotpostConfig jotpostConfig;
    jotpostConfig.sHost = ArgStr(argc, argv, "--jotpost-host", "");
    jotpostConfig.nPort = static_cast<uint16_t>(std::atoi(ArgStr(argc, argv, "--jotpost-port", "7701")));
    std::unique_ptr<JotpostStatus> pJotpostStatus;
    if (!jotpostConfig.sHost.empty())
        pJotpostStatus = std::make_unique<JotpostStatus>(jotpostConfig);

    HttpServer server(ops, store, config, bPersist ? &journal : nullptr, snapConfig, acl,
                      &history, watch, &watcher, &triageLedger, pJotpostStatus.get());
    gpServer = &server;
    // C5039 ("potentially throwing function passed to an extern C API") is /Wall noise on every
    // signal handler ever registered this way, not a real hazard here - OnSignal only flips an
    // atomic and cannot throw. Suppressed for exactly these two lines rather than project-wide.
    // MSVC-only pragma - gcc/clang have no C5039 and treat the unrecognized pragma itself as a
    // -Wunknown-pragmas error under -Werror, so it is guarded rather than seen at all off MSVC.
#ifdef _MSC_VER
    #pragma warning(suppress : 5039)
#endif
    std::signal(SIGINT,  OnSignal);
#ifdef _MSC_VER
    #pragma warning(suppress : 5039)
#endif
    std::signal(SIGTERM, OnSignal);

    std::printf("loom listening on http://%s:%u  (%zu jots, %s)\n",
                config.msBind.c_str(), static_cast<unsigned>(config.mnPort), store.Size(),
                bPersist ? snapConfig.msWalPath.c_str() : "no persistence");
    // A wildcard bind is not an address anyone can type. Print the one that actually reaches this
    // machine, because that is what gets pasted into another machine's agent config.
    if (server.AdvertisedOrigin() != "http://" + config.msBind + ":" + std::to_string(config.mnPort))
        std::printf("  reachable at %s\n", server.AdvertisedOrigin().c_str());
    if (config.msToken.empty() && config.msBind != "127.0.0.1" && !acl.Enabled())
        std::printf("  WARNING: bound beyond loopback with no --token and no address list\n");
    if (acl.Enabled())
        std::printf("  address list active (loopback always allowed)\n");
    std::printf("  watching every %ds; %s\n", watcherConfig.nPollIntervalSec,
                watcherConfig.sOnNewJots.empty()
                    ? "no --on-new-jots configured, ingest-only"
                    : ("auto-triage via '" + watcherConfig.sOnNewJots + "', capped at $" +
                       std::to_string(watcherConfig.fMaxDailyUSD) + "/24h").c_str());
    if (pJotpostStatus)
        std::printf("  health-checking jotpost at %s:%u\n",
                    jotpostConfig.sHost.c_str(), static_cast<unsigned>(jotpostConfig.nPort));
    // Worth a line of its own: "the offline service is configured" and "the offline service is
    // actually answering" are different facts, and without this the only symptom of a mistyped
    // host is jots quietly going to `tbd` exactly as they did before - which looks like nothing
    // being wrong at all.
    if (pTriage)
        std::printf("  offline triage via %s:%u, up to %d jot(s) per poll\n",
                    resolverConfig.sHost.c_str(), static_cast<unsigned>(resolverConfig.nPort),
                    resolverConfig.nMaxPerPoll);

    const std::error_code ec = server.Run();
    gpServer = nullptr;

    // Stopped before the snapshot, not after: Watcher can still be mid-poll (or, rarely, waiting
    // out a child's timeout) when Run() returns, and Stop() blocks until that settles - joining it
    // first means the snapshot below sees a store nothing else is still writing to.
    watcher.Stop();

    // Snapshot on the way out. A clean shutdown should leave a small log and a current snapshot,
    // so the next start is fast and the WAL does not grow across restarts.
    if (bPersist)
    {
        size_t nWritten = 0;
        if (std::error_code ecSnap = SNAPSHOT::Write(snapConfig, store, &journal, nWritten))
            std::printf("  final snapshot failed: %s\n", ecSnap.message().c_str());
        else
            std::printf("snapshot wrote %zu records\n", nWritten);
        journal.Close();
        // After the journal, so anything still queued in either has already been handed over.
        history.Close();
    }

    if (ec)
    {
        std::printf("loom exited: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("loom stopped\n");
    return 0;
}
