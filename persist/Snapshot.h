#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/JotStore.h"
#include "core/LoomError.h"
#include "persist/Journal.h"

#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Snapshot - the periodic full dump that keeps the WAL from growing forever.
//
// WRITE SEQUENCE, and every step is load-bearing:
//
//     1. write every record to <path>.tmp
//     2. fsync the tmp file            - the bytes are on the platter, not in a buffer
//     3. rename tmp over <path>        - THE COMMIT POINT. rename is atomic, so a reader or a
//                                        crash sees either the whole old snapshot or the whole new
//                                        one, never a half-written file
//     4. truncate the WAL              - only now is the log redundant
//
// Doing (4) before (3) loses data on a crash in between. Writing in place instead of via rename
// means a crash mid-write leaves a truncated snapshot AND a truncated log, which is unrecoverable.
// This ordering is the entire reason the format can be dumb line-oriented JSON.
//
// FORMAT: JSONL, one record per line, oldest first. Same codec as the WAL, so there is one
// definition of a serialized jot. Greppable with the tools you already have, and diffable between
// two snapshots - which matters for a store whose contents are memories you might want to inspect
// by hand.
//
// LOAD: read the snapshot, then replay whatever the WAL accumulated after it. Indexes are never
// persisted; they are rebuilt from the records, which is what keeps the on-disk format free to
// evolve without a migration step.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct SnapshotConfig
{
    std::string msPath;      // e.g. <dir>/loom.snapshot
    std::string msWalPath;   // e.g. <dir>/loom.wal
};

namespace SNAPSHOT
{
    // Writes a snapshot and truncates the journal. Journal may be null (no persistence).
    std::error_code Write(const SnapshotConfig& config, const JotStore& store, Journal* pJournal,
                          size_t& outRecords);

    // Loads snapshot then WAL. Missing files are a first run, not an error.
    std::error_code Load(const SnapshotConfig& config, JotStore& store,
                         size_t& outLoaded, size_t& outReplayed, size_t& outDropped);
}
