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
#include "persist/Purge.h"
#include "persist/SinkFanout.h"
#include "persist/Snapshot.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
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
            "  Every change is also appended to DIR/loom.history, which - unlike the WAL - is never\n"
            "  truncated by a snapshot. That is what GET /history and the dashboard's History view\n"
            "  read, and what a restore re-applies.\n"
            "\n"
            "  --purge=DIR    erase the jots named in DIR/loom.purge-request.json from the snapshot,\n"
            "                 the WAL and the history log. Requires the service to be STOPPED. On\n"
            "                 its own it is a dry run; add --yes to actually erase.\n");
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

    HttpServer server(ops, store, config, bPersist ? &journal : nullptr, snapConfig, acl,
                      &history);
    gpServer = &server;
    std::signal(SIGINT,  OnSignal);
    std::signal(SIGTERM, OnSignal);

    std::printf("loom listening on http://%s:%u  (%zu jots, %s)\n",
                config.msBind.c_str(), static_cast<unsigned>(config.mnPort), store.Size(),
                bPersist ? snapConfig.msWalPath.c_str() : "no persistence");
    if (config.msToken.empty() && config.msBind != "127.0.0.1" && !acl.Enabled())
        std::printf("  WARNING: bound beyond loopback with no --token and no address list\n");
    if (acl.Enabled())
        std::printf("  address list active (loopback always allowed)\n");

    const std::error_code ec = server.Run();
    gpServer = nullptr;

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
