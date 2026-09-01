#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/JotStore.h"
#include "core/LoomError.h"

#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Importer - ZHotkey's jots.log into Loom.
//
// The existing log is append-only JSONL written by ZHotkey's WriteToLogFile:
//
//     {"ts":"2026-04-02 22:59:22","entry":"Watered the office plants today. "}
//
// Two fields, no id, no tags, no edit history. That is the entire prior art, and it is the corpus
// this service exists to replace.
//
// TIMESTAMPS ARE PRESERVED, and that is the whole point of the exercise. A jot's id IS its creation
// time, so importing sets the id from "ts" rather than allocating a new one - the history keeps its
// real shape, an "oldest first" query returns 2025 entries first, and a date-range filter over the
// imported past actually works. Allocating fresh ids would silently collapse two years of history
// into one afternoon.
//
// THREE HAZARDS, all of them real in this file:
//
//   1. LOCAL TIME, NO ZONE. ZHotkey wrote DateTime.Now, so "2026-04-02 22:59:22" is local wall
//      clock with no offset recorded. It is interpreted as local time and converted to UTC via
//      mktime, which is the only reading that puts an entry back on the day it was written.
//      Treating it as UTC instead would shift every entry by the machine's offset.
//
//   2. SECOND RESOLUTION. Ids are microseconds and must be unique. Two entries written in the same
//      second would collide, so a collision walks forward by one microsecond. Deterministic given
//      the file order, which is what keeps re-import idempotent.
//
//   3. RE-IMPORT. Because ids are derived from the content of the file rather than allocated, the
//      same line always maps to the same id - so a second import skips what is already there
//      instead of duplicating the whole log. Safe to leave in a startup script.
//
// Trailing whitespace in "entry" is preserved, not trimmed. These are somebody's notes; the
// importer's job is to move them, not to tidy them.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct ImportStats
{
    size_t mnLines     = 0;   // lines read
    size_t mnImported  = 0;   // new jots created
    size_t mnSkipped   = 0;   // already present from a previous import
    size_t mnMalformed = 0;   // unparseable, reported rather than silently dropped
    size_t mnBumped    = 0;   // same-second collisions resolved by walking forward
    int64_t mnOldestUS = 0;
    int64_t mnNewestUS = 0;
};

namespace IMPORT
{
    // sEditor is who gets credit; empty resolves to "user", which is correct for this file.
    std::error_code JotsLog(const std::string& sPath, JotStore& store,
                            const std::string& sEditor, ImportStats& outStats);

    // Exposed for testing: "2026-04-02 22:59:22" as LOCAL time -> microseconds since epoch UTC.
    bool ParseLocalTimestamp(const std::string& sTS, int64_t& outUS);
}
