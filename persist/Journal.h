#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/FlatJot.h"
#include "core/JotStore.h"
#include "core/LoomError.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Journal - the write-ahead log and the background thread that commits it.
//
// THE WRITE PATH NEVER TOUCHES DISK. A mutation is applied to RAM under the store's write lock, the
// serialized line is pushed onto this queue inside that same critical section, and the request
// returns. A separate thread drains the queue, appends, and fsyncs on a policy. That is what keeps
// a POST at tens of microseconds instead of at the mercy of the filesystem.
//
// SERIALIZATION HAPPENS IN OnPut, UNDER THE STORE LOCK. That is deliberate and it is the whole
// ordering guarantee: two concurrent updates to the same jot reach the log in the order they were
// applied, because the lock that ordered them is still held when they are enqueued. The cost is
// roughly a microsecond of json::dump inside the critical section, which is the right trade - the
// alternative is sequence numbers and a reordering buffer to fix a problem that need not exist.
//
// FORMAT: JSONL, one operation per line.
//     {"op":"put","jot":{...}}
//     {"op":"del","id":1756661962123456}
// A put carries the COMPLETE record, so replay is idempotent and needs no insert/update
// distinction - the last put for an id wins, which is exactly what re-applying a log should mean.
//
// DURABILITY POLICY is a choice the operator makes, not one this code makes for them:
//     kNever    - rely on the OS. Fastest; loses recent writes if the machine loses power.
//     kInterval - fsync every mnSyncIntervalMS. The default.
//     kAlways   - fsync every batch. Slowest, loses nothing the caller was told succeeded.
//////////////////////////////////////////////////////////////////////////////////////////////////

enum class eSyncPolicy
{
    kNever = 0,
    kInterval,
    kAlways
};

struct JournalConfig
{
    std::string msPath;
    eSyncPolicy mSync            = eSyncPolicy::kInterval;
    int64_t     mnSyncIntervalMS = 200;

    // Back-pressure. If the committer cannot keep up, writers are made to wait rather than letting
    // an unbounded queue eat memory until the process dies - which is the failure mode a naive
    // async log always eventually finds.
    size_t      mnMaxQueue       = 100000;
};

class Journal : public IJournalSink
{
public:
    Journal();
    ~Journal() override;

    Journal(const Journal&)            = delete;
    Journal& operator=(const Journal&) = delete;

    // Opens the log for append and starts the committer thread.
    std::error_code Open(const JournalConfig& config);

    // Drains the queue, fsyncs, and stops the thread. Called on shutdown and before a snapshot
    // swaps the file out from underneath it.
    void Close();

    // --- IJournalSink. Called under the store's write lock; must stay cheap. ---
    void OnPut(const FlatJot& jot) override;
    void OnDelete(tJotID id) override;

    // Blocks until everything queued at the moment of the call has been written and synced. This is
    // what --durable-writes and an explicit snapshot are built on.
    std::error_code Flush();

    // Discards the log and starts a fresh empty one. Only safe immediately after a snapshot has
    // been durably renamed into place - that is the commit point that makes the log redundant.
    std::error_code Truncate();

    void FillStats(PersistStats& outStats) const;

    // Replays a log file into the store. Torn final lines are dropped, not treated as errors: an
    // incomplete last record is the EXPECTED residue of a crash, and refusing to start because of
    // one would turn a clean recovery into an outage. Returns the count applied.
    static std::error_code Replay(const std::string& sPath, JotStore& store,
                                  size_t& outApplied, size_t& outDropped);

private:
    void CommitterLoop();
    std::error_code AppendLocked(const std::string& sLine);
    std::error_code SyncNow();

    JournalConfig            mConfig;
    void*                    mpFile = nullptr;   // FILE*, kept opaque so the header stays clean

    mutable std::mutex       mQueueMutex;
    std::condition_variable  mQueueCV;
    std::condition_variable  mDrainedCV;
    std::deque<std::string>  mQueue;
    bool                     mbRunning  = false;
    bool                     mbDraining = false;

    std::thread              mCommitter;

    std::atomic<uint64_t>    mnAppended{ 0 };
    std::atomic<uint64_t>    mnSynced{ 0 };
    std::atomic<uint64_t>    mnBytes{ 0 };
    std::atomic<uint64_t>    mnSnapshots{ 0 };
    std::atomic<int64_t>     mnLastSnapshotUS{ 0 };

public:
    // Bumped by Snapshot so /stats can report both halves of persistence in one place.
    void NoteSnapshot(int64_t nWhenUS)
    {
        mnSnapshots.fetch_add(1, std::memory_order_relaxed);
        mnLastSnapshotUS.store(nWhenUS, std::memory_order_relaxed);
    }
};
