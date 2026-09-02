#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/FlatJot.h"
#include "core/JotStore.h"
#include "core/LoomError.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// History - the undo log. What changed, when, by whom, and what it looked like before.
//
// WHY THIS IS NOT THE WAL. loom.wal is the durability path and Snapshot::Write TRUNCATES it the
// moment a snapshot lands, because at that point the log is redundant - that is the whole point of
// a checkpoint. It is therefore exactly the wrong place to look for what a jot said last Tuesday.
// This file is append-only and survives snapshots; it is rotated by size, never by checkpoint.
//
// THE BEFORE-IMAGE IS FREE, and that is the reason this is small. Journal's format already
// guarantees that "a put carries the COMPLETE record, so replay is idempotent" - so the previous
// put for an id IS its before-image. Restoring means re-applying an entry that is already sitting
// in the log; nothing has to capture, diff, or pair up before and after states, and JotStore needs
// no new hook. This class is a second IJournalSink and nothing more.
//
// IT KEEPS THE NO-DISK-IN-THE-CRITICAL-SECTION RULE. OnPut is called under the store's write lock,
// the same as Journal's, so it does the same thing: serialize, push onto a queue, return. A
// background thread appends. What it does NOT inherit is the sync policy - an fsync ladder is for
// the durability path, and losing the last few history entries to a power cut costs a person one
// undo, not their data. So the committer here just appends and flushes on a timer.
//
// TWO COPIES, ON PURPOSE. The file is the record; a bounded deque of the most recent entries is
// what the dashboard lists and restores from, so paging history costs no I/O at all. Open() seeds
// that deque from the tail of the file, or history would look empty after every restart. Restoring
// something older than the deque falls back to scanning the file, which is slow and rare and worth
// exactly nothing to optimize.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct HistoryEntry
{
    uint64_t    mnSeq   = 0;
    int64_t     mnAtUS  = 0;        // wall clock at which the change was applied
    bool        mbDelete = false;
    tJotID      mID     = kInvalidJotID;
    std::string msName;             // slug at the time; for a delete, the last one seen
    std::string msEditor;
    std::string msSummary;          // summary, else a clipped first line - for the list only
    std::string msRecord;           // the complete FlatJot JSON. Empty for a delete.
};

struct HistoryConfig
{
    std::string msPath;

    // Rotation is by size and keeps ONE previous generation (loom.history.1). Unbounded growth is
    // the failure mode every "just log it" feature eventually finds; two generations is enough to
    // make the window long and the disk cost bounded.
    size_t  mnMaxBytes = 64u * 1024u * 1024u;

    // How many recent entries stay in RAM for the dashboard. At a few hundred bytes each this is
    // single-digit megabytes at the default.
    size_t  mnMemory = 2000;

    int64_t mnFlushIntervalMS = 250;
};

struct HistoryStats
{
    bool     mbEnabled  = false;
    uint64_t mnEntries  = 0;     // total ever recorded this run plus what was loaded
    uint64_t mnInMemory = 0;
    uint64_t mnBytes    = 0;
};

class History : public IJournalSink
{
public:
    History();
    ~History() override;

    History(const History&)            = delete;
    History& operator=(const History&) = delete;

    // Reads the tail of an existing log into memory, rotates it if it is already oversized, opens
    // for append and starts the committer. A missing file is a first run, not an error.
    std::error_code Open(const HistoryConfig& config);
    void Close();

    // --- IJournalSink. Called under the store's write lock; must stay cheap. ---
    void OnPut(const FlatJot& jot) override;
    void OnDelete(tJotID id) override;

    // Newest first. idFilter of kInvalidJotID means every jot.
    void List(tJotID idFilter, size_t nLimit, size_t nOffset,
              std::vector<HistoryEntry>& outEntries, size_t& outTotal) const;

    // Memory first, then the file. False means no entry with that sequence number survives.
    bool Get(uint64_t nSeq, HistoryEntry& outEntry) const;

    // The put that was in force immediately BEFORE nSeq for the same jot - i.e. what "undo this
    // change" means. False when nSeq is the jot's first appearance and there is nothing behind it.
    bool Previous(uint64_t nSeq, HistoryEntry& outEntry) const;

    HistoryStats Stats() const;

    // Rewrites a history file in place, dropping every entry belonging to one of the given ids.
    //
    // STATIC AND OFFLINE. It is only ever called with the service stopped, by the purge tool, which
    // holds the data lock - so it does not have to reconcile with a running committer, and it MUST
    // NOT be called on a log this process has open. It exists here rather than in Purge because the
    // line format is this class's business, and a second parser for it is a second thing to keep in
    // step. tmp -> rename, so an interrupted purge leaves the original rather than a half-scrubbed
    // file that would be worse than either outcome.
    static std::error_code PurgeFile(const std::string& sPath, const std::vector<tJotID>& vIDs,
                                     size_t& outRemoved, size_t& outKept);

private:
    void CommitterLoop();
    void Append(const HistoryEntry& entry, const std::string& sLine);
    bool ScanFileFor(uint64_t nSeq, HistoryEntry& outEntry) const;
    static bool ParseLine(const std::string& sLine, HistoryEntry& outEntry);
    static std::string Clip(const std::string& s, size_t nMax);

    HistoryConfig             mConfig;
    void*                     mpFile = nullptr;   // FILE*, opaque to keep the header clean

    mutable std::mutex        mMutex;
    std::condition_variable   mQueueCV;
    std::deque<std::string>   mQueue;     // lines awaiting the disk
    std::deque<HistoryEntry>  mRecent;    // bounded, newest at the back
    uint64_t                  mnNextSeq   = 1;
    uint64_t                  mnEntries   = 0;
    uint64_t                  mnBytes     = 0;
    bool                      mbRunning   = false;

    // Last name and editor seen for each id, so a delete - which carries only an id - can still be
    // listed as "deleted <slug>" instead of as a bare number.
    std::unordered_map<tJotID, std::pair<std::string, std::string>> mLastSeen;

    std::thread               mCommitter;
};
