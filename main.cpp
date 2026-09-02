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
#include "persist/Importer.h"
#include "persist/Journal.h"
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
            "  list can always be repaired from the machine itself.\n");
        return 0;
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

    JotStore store;
    Ops      ops(store);
    Journal  journal;
    IpAcl    acl;

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
            std::printf("  journal failed to open: %s - running RAM only\n", ec.message().c_str());
        else
            store.SetJournalSink(&journal);
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

    HttpServer server(ops, store, config, bPersist ? &journal : nullptr, snapConfig, acl);
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
    }

    if (ec)
    {
        std::printf("loom exited: %s\n", ec.message().c_str());
        return 1;
    }

    std::printf("loom stopped\n");
    return 0;
}
