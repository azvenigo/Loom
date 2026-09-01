// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// persisttest - assertions over the durability layer.
//
// Separate from coretest because it needs the codec and journal objects, which are compiled at a
// relaxed warning level (vendored nlohmann does not survive /Wall /WX). Keeping it apart is what
// lets core stay strict.
//
// The properties asserted here are the ones that are invisible until the day they matter:
//
//   - a hard kill loses nothing that was flushed
//   - a torn final line is DROPPED, not treated as corruption that blocks startup
//   - replay is idempotent, so restarting twice does not double the store
//   - a delete replays in the right order relative to the puts around it
//   - the snapshot commit point (tmp -> fsync -> rename -> truncate) never loses records
//
// Every case here was reproduced by hand against the running server first; these exist so they
// stay fixed.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "codec/JotJson.h"
#include "core/JotStore.h"
#include "core/Ops.h"
#include "persist/Journal.h"
#include "persist/Snapshot.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int gnChecks = 0;
    int gnFailed = 0;

    void Check(bool bCondition, const char* pWhat)
    {
        ++gnChecks;
        std::printf("  %s  %s\n", bCondition ? "ok  " : "FAIL", pWhat);
        if (!bCondition)
            ++gnFailed;
    }

    void Section(const char* pName) { std::printf("\n[%s]\n", pName); }

    std::string gsDir;

    SnapshotConfig Paths()
    {
        SnapshotConfig c;
        c.msPath    = gsDir + "/loom.snapshot";
        c.msWalPath = gsDir + "/loom.wal";
        return c;
    }

    void Reset()
    {
        std::error_code ec;
        std::filesystem::remove_all(gsDir, ec);
        std::filesystem::create_directories(gsDir, ec);
    }

    JotInput Text(const std::string& s)
    {
        JotInput in;
        in.msText = s;
        return in;
    }

    size_t CountLines(const std::string& sPath)
    {
        FILE* p = std::fopen(sPath.c_str(), "rb");
        if (!p)
            return 0;
        size_t n = 0;
        int ch;
        while ((ch = std::fgetc(p)) != EOF)
        {
            if (ch == '\n')
                ++n;
        }
        std::fclose(p);
        return n;
    }

    //--------------------------------------------------------------------------------------------

    void TestJournalReplay()
    {
        Section("journal replay");
        Reset();

        tJotID nKeep = 0, nDoomed = 0;
        {
            JotStore store;
            Ops      ops(store);
            Journal  journal;

            JournalConfig cfg;
            cfg.msPath = Paths().msWalPath;
            cfg.mSync  = eSyncPolicy::kAlways;
            Check(!journal.Open(cfg), "journal opens");
            store.SetJournalSink(&journal);

            AddResult a, b;
            JotInput named = Text("the surviving record");
            named.msName = "keeper";
            named.mTags  = std::vector<std::string>{ "durability" };
            ops.Add(named, a);
            nKeep = a.mJot.mID;

            ops.Add(Text("this one gets deleted"), b);
            nDoomed = b.mJot.mID;

            // An edit after the create: replay must end at the LAST value, not the first.
            AddResult upd;
            ops.Update(nKeep, Text("edited before the crash"), 0, upd);

            ops.Delete(nDoomed);

            Check(!journal.Flush(), "flush succeeds");
            journal.Close();
        }

        // Fresh process, so to speak.
        {
            JotStore store;
            size_t nApplied = 0, nDropped = 0;
            Check(!Journal::Replay(Paths().msWalPath, store, nApplied, nDropped), "replay succeeds");
            Check(nDropped == 0, "nothing dropped from an intact log");
            Check(store.Size() == 1, "the deleted record did not come back");

            Jot jot;
            Check(store.GetByName("keeper", jot), "named record recovered by slug");
            Check(jot.msText == "edited before the crash", "the LAST write won, not the first");
            Check(jot.mID == nKeep, "id preserved across replay");
            Check(jot.mTags.size() == 1, "tags survived interning round-trip");
        }
    }

    void TestTornFinalLine()
    {
        Section("torn final line");
        Reset();

        {
            JotStore store;
            Ops      ops(store);
            Journal  journal;

            JournalConfig cfg;
            cfg.msPath = Paths().msWalPath;
            cfg.mSync  = eSyncPolicy::kAlways;
            journal.Open(cfg);
            store.SetJournalSink(&journal);

            AddResult r;
            ops.Add(Text("a complete record"), r);
            journal.Flush();
            journal.Close();
        }

        // Exactly what a power cut mid-write leaves behind: a partial line, no newline.
        {
            FILE* p = std::fopen(Paths().msWalPath.c_str(), "ab");
            const char* pTorn = "{\"op\":\"put\",\"jot\":{\"id\":424242,\"text\":\"never fini";
            std::fwrite(pTorn, 1, std::strlen(pTorn), p);
            std::fclose(p);
        }

        {
            JotStore store;
            size_t nApplied = 0, nDropped = 0;
            const std::error_code ec = Journal::Replay(Paths().msWalPath, store, nApplied, nDropped);

            // The important half: a torn tail is EXPECTED crash residue. Refusing to start over one
            // would turn a clean recovery into an outage.
            Check(!ec, "replay does not fail on a torn tail");
            Check(nDropped == 1, "the torn line is counted as dropped");
            Check(store.Size() == 1, "the intact record before it still loaded");

            Jot jot;
            Check(!store.Get(424242, jot), "the torn record is not present");
        }
    }

    void TestReplayIdempotent()
    {
        Section("replay idempotence");
        Reset();

        {
            JotStore store;
            Ops      ops(store);
            Journal  journal;
            JournalConfig cfg;
            cfg.msPath = Paths().msWalPath;
            cfg.mSync  = eSyncPolicy::kAlways;
            journal.Open(cfg);
            store.SetJournalSink(&journal);

            for (int i = 0; i < 20; ++i)
            {
                AddResult r;
                ops.Add(Text("record " + std::to_string(i)), r);
            }
            journal.Flush();
            journal.Close();
        }

        // Replaying the same log twice into the same store must not duplicate anything - which is
        // what makes a put carry the whole record rather than a delta.
        JotStore store;
        size_t nApplied = 0, nDropped = 0;
        Journal::Replay(Paths().msWalPath, store, nApplied, nDropped);
        const size_t nFirst = store.Size();
        Journal::Replay(Paths().msWalPath, store, nApplied, nDropped);

        Check(nFirst == 20, "first replay loaded everything");
        Check(store.Size() == nFirst, "second replay changed nothing");
    }

    void TestSnapshotCycle()
    {
        Section("snapshot cycle");
        Reset();

        const SnapshotConfig paths = Paths();

        {
            JotStore store;
            Ops      ops(store);
            Journal  journal;
            JournalConfig cfg;
            cfg.msPath = paths.msWalPath;
            cfg.mSync  = eSyncPolicy::kAlways;
            journal.Open(cfg);
            store.SetJournalSink(&journal);

            for (int i = 0; i < 10; ++i)
            {
                JotInput in = Text("snapshot record " + std::to_string(i));
                if (i % 3 == 0)
                    in.msName = "snap-" + std::to_string(i);
                AddResult r;
                ops.Add(in, r);
            }

            journal.Flush();
            Check(CountLines(paths.msWalPath) == 10, "log holds every mutation before the snapshot");

            size_t nWritten = 0;
            Check(!SNAPSHOT::Write(paths, store, &journal, nWritten), "snapshot writes");
            Check(nWritten == 10, "every live record made it into the snapshot");
            Check(CountLines(paths.msPath) == 10, "snapshot file has one line per record");

            // Truncation is the LAST step, after the rename commit point.
            Check(CountLines(paths.msWalPath) == 0, "log truncated once the snapshot is durable");

            // Writes after the snapshot land in the fresh log.
            AddResult after;
            ops.Add(Text("written after the snapshot"), after);
            journal.Flush();
            Check(CountLines(paths.msWalPath) == 1, "post-snapshot write goes to the new log");
            journal.Close();
        }

        {
            JotStore store;
            size_t nLoaded = 0, nReplayed = 0, nDropped = 0;
            Check(!SNAPSHOT::Load(paths, store, nLoaded, nReplayed, nDropped), "load succeeds");
            Check(nLoaded == 10, "snapshot half restored");
            Check(nReplayed == 1, "log half replayed on top");
            Check(store.Size() == 11, "both halves present, nothing double-counted");

            Jot jot;
            Check(store.GetByName("snap-0", jot), "named record survived the snapshot round-trip");
        }
    }

    void TestNoPersistIsSilent()
    {
        Section("no sink attached");

        // The store must work identically with no journal - that is the --no-persist path, and the
        // path every unit test in coretest runs on.
        JotStore store;
        Ops      ops(store);

        AddResult r;
        Check(!ops.Add(Text("no journal attached"), r), "add works with no sink");
        Check(!ops.Delete(r.mJot.mID), "delete works with no sink");
        Check(store.Size() == 0, "store is consistent");
    }
}


int main(int argc, char** argv)
{
    gsDir = (argc > 1) ? argv[1] : "persisttest-data";
    std::printf("persisttest - Loom phase 2   (scratch dir: %s)\n", gsDir.c_str());

    TestJournalReplay();
    TestTornFinalLine();
    TestReplayIdempotent();
    TestSnapshotCycle();
    TestNoPersistIsSilent();

    std::error_code ec;
    std::filesystem::remove_all(gsDir, ec);

    std::printf("\n%d checks, %d failed\n", gnChecks, gnFailed);
    return gnFailed == 0 ? 0 : 1;
}
