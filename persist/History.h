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
    // Where the write came from, as the server saw it. Empty for anything logged before origins
    // existed, and for writes with no connection behind them - the importer and a replay. For a put
    // this is read straight back out of the record; a delete carries only an id, so it reuses the
    // last origin seen for that jot, exactly as it does for the editor and the caption.
    std::string msOrigin;
    std::string msSummary;          // summary, else a clipped first line - for the list only
    std::string msRecord;           // the complete FlatJot JSON. Empty for a delete.

    // The multi-record operation this entry belonged to, or 0 for an ordinary single write. Every
    // entry a tag merge produces carries the same value, which is what makes "undo that merge" one
    // act instead of forty restores. See IJournalSink::BeginTransaction.
    //
    // THE ID IS THE SEQ OF THE GROUP'S FIRST ENTRY. That needs no counter, no second id space and
    // no extra durability: seq is already unique, already ordered and already recovered from the
    // file on restart. It also means a group is a CONTIGUOUS run of sequence numbers starting at
    // its own id - the store holds its write lock for the whole operation, so nothing else can log
    // anything in between - and that is what lets a lookup stop as soon as the run ends.
    uint64_t    mnTxnID = 0;

    // How many logged mutations this row stands for. List() folds a run of edits to one jot into a
    // single row - see the coalescing note in HistoryConfig - and this is the size of that run. 1
    // means the row is one mutation. Never serialized; it is computed per read.
    size_t      mnCoalesced = 1;
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

    // COALESCING. Every mutation is logged, but a run of edits to ONE jot by ONE editor with no more
    // than this long between them is LISTED as a single point. Saving a jot four times while typing
    // is one act of editing, and showing it as four rows - each captioned with the jot's unchanged
    // summary - made the view unreadable without making anything more recoverable. The individual
    // records are all still in the file and still restorable by seq; this only changes what List()
    // hands back. The row carries the run's NEWEST state, so restoring the row below it is what
    // "undo that editing session" means.
    int64_t mnCoalesceWindowMS = 10 * 60 * 1000;
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

    // Brackets a multi-record operation so every entry it logs carries one transaction id.
    // BeginTransaction is idempotent-ish rather than nestable: a second Begin without an End simply
    // starts a new group, because the alternative - a depth counter - would let one unbalanced
    // caller swallow every later write into a group that never closes.
    void     BeginTransaction() override;
    uint64_t EndTransaction() override;

    // Newest first. idFilter of kInvalidJotID means every jot.
    void List(tJotID idFilter, size_t nLimit, size_t nOffset,
              std::vector<HistoryEntry>& outEntries, size_t& outTotal) const;

    // Memory first, then the file. False means no entry with that sequence number survives.
    bool Get(uint64_t nSeq, HistoryEntry& outEntry) const;

    // The put that was in force immediately BEFORE nSeq for the same jot - i.e. what "undo this
    // change" means. False when nSeq is the jot's first appearance and there is nothing behind it.
    bool Previous(uint64_t nSeq, HistoryEntry& outEntry) const;

    // Every entry belonging to one multi-record operation, OLDEST FIRST. Empty means no such group
    // survives - either it never existed or it has aged out of both generations of the log.
    //
    // Because a group is a contiguous run of seqs beginning at nTxnID, finding its first entry in
    // memory proves the whole group is in memory; only when that first entry is missing does this
    // fall back to the file, and then a group that old is certainly flushed.
    void ListTransaction(uint64_t nTxnID, std::vector<HistoryEntry>& outEntries) const;

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
    // Returns the transaction id an entry with this seq should carry, adopting the seq as the
    // group's id when it is the first entry in an open group. 0 outside a transaction. Called with
    // mMutex held, from the two sink methods, immediately after the seq is allocated.
    uint64_t Locked_StampTransaction(uint64_t nSeq);
    bool ScanFileFor(uint64_t nSeq, HistoryEntry& outEntry) const;
    void ScanFileForTransaction(uint64_t nTxnID, std::vector<HistoryEntry>& outEntries) const;
    bool ScanFileForPrevious(uint64_t nSeq, tJotID id, HistoryEntry& outEntry) const;
    static bool ParseLine(const std::string& sLine, HistoryEntry& outEntry);
    // Reads just the last line of a log file - O(length of that line), not O(file) - so Open() can
    // learn the highest sequence number a generation reached without parsing the whole thing before
    // retiring it.
    static bool ScanLastLine(const std::string& sPath, HistoryEntry& outEntry);
    static std::string Clip(const std::string& s, size_t nMax);

    // Last state seen for each id, so the NEXT entry for it can be captioned with what actually
    // changed rather than the jot's static summary - which usually does not change from edit to edit.
    //
    // msSummary is the RAW summary field and exists only to be diffed. msCaption is the clipped
    // display line, and is what a delete - which carries only an id - is listed with, so it reads
    // "deleted <slug>: <summary>" instead of as a bare number. Comparing the clipped form against a
    // raw one would call the summary edited every time a jot's summary was empty or over the clip
    // length, which is why these are two fields and not one.
    struct LastSeen
    {
        std::string              msName, msEditor, msSummary, msCaption, msOrigin;
        std::vector<std::string> mTags;
        size_t                   mnTextLen = 0;
    };

    // What actually differs from pPrev, as a short line - "text edited", "+due:...", "-todo" -
    // instead of the summary, which typically stays fixed across edits to tags or body. Empty means
    // nothing this coarse a check can see moved; the caller falls back to the plain summary/text clip.
    static std::string DescribeChange(const LastSeen* pPrev, const FlatJot& jot);

    // Re-captions a coalesced row to describe the whole run against the state it started from,
    // rather than leaving it showing only what its newest single mutation did.
    static void Recaption(HistoryEntry& burst, const HistoryEntry& baseline);

    HistoryConfig             mConfig;
    void*                     mpFile = nullptr;   // FILE*, opaque to keep the header clean

    mutable std::mutex        mMutex;
    std::condition_variable   mQueueCV;
    std::deque<std::string>   mQueue;     // lines awaiting the disk
    std::deque<HistoryEntry>  mRecent;    // bounded, newest at the back
    uint64_t                  mnNextSeq   = 1;

    // The group currently open, and the id it was given. mnOpenTxnID is 0 until the group's first
    // entry is logged, because the id IS that entry's seq and it does not exist before then - which
    // also means a transaction that logs nothing consumes no id at all.
    bool                      mbInTxn     = false;
    uint64_t                  mnOpenTxnID = 0;
    uint64_t                  mnEntries   = 0;
    uint64_t                  mnBytes     = 0;
    bool                      mbRunning   = false;

    std::unordered_map<tJotID, LastSeen> mLastSeen;

    std::thread               mCommitter;
};
