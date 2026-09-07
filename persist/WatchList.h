#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/LoomError.h"

#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// WatchList - the runtime-editable list of files Loom watches for new content to ingest.
//
// THIS CLASS ITSELF STILL HAS NO BACKGROUND THREAD - "has this changed" is only answered when
// Resolve() is called, which is cheap (a stat() per watched path) and keeps this type as easy to
// test and reason about as it always was. Loom now DOES poll it in the background, though: see
// persist/Watcher.h, which calls Resolve() on a timer and reacts. The original worry this class's
// design avoided - a real filesystem watcher's debounce problems against a file that syncs in over
// OneDrive in bursts - is answered by Watcher's rotate-on-ingest design instead: a source marked
// ROTATE is renamed aside as one atomic step of ingesting it, so it can never be read twice and
// there is nothing left for a debounce window to protect against.
//
// PENDING MEANS BOTH SIZE AND MTIME DIFFER FROM THE LAST INGEST, not just mtime. A bare touch or a
// metadata-only change (OneDrive re-syncing the same bytes) must not read as new content. A source
// with rotate=true mostly sidesteps this test anyway - see Watcher.h - but it still applies to any
// rotate=false source, and to the dashboard's manual POST /watch/ingest.
//
// Same shape as core/IpAcl: a small JSON file, loaded at startup, edited at runtime from the
// dashboard, written whole via tmp -> rename so a crash mid-write leaves the previous version
// rather than a truncated one.
//
// TWO FILES, ONE CONCERN EACH:
//   loom.watch.json        - the list of paths, plus an optional "rotate" map keyed by path
//                            (missing entries default true). User-facing config, edited via
//                            PUT /watch for the path list; the rotate flag is not exposed there
//                            yet and is a hand-edit of the file for now - see RotateFor().
//   loom.watch-state.json  - size/mtime/last-ingest-time per path, as of the last successful
//                            ingest. Server-owned bookkeeping, never edited directly.
// Losing the state file is harmless (everything just reads as pending again, and ingest is
// idempotent - see persist/Importer.h), which is why it is kept separate from the config a person
// actually edits.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct WatchStatus
{
    std::string msPath;
    bool        mbExists     = false;
    uint64_t    mnSizeBytes  = 0;    // 0 when !mbExists
    int64_t     mnMtimeUS    = 0;    // 0 when !mbExists
    bool        mbPending    = false;
    int64_t     mnLastIngestUS = 0;  // 0 == never ingested
    bool        mbRotate     = true; // see WatchList::RotateFor
};

class WatchList
{
public:
    // Missing files are not an error - a first run has neither the config nor the state yet. A
    // corrupt config is reported and comes up empty, the same tolerance IpAcl gives a corrupt
    // allow list, rather than refusing to start.
    std::error_code Load(const std::string& sConfigPath, const std::string& sStatePath,
                        std::string& outWarning);

    void Get(std::vector<std::string>& outPaths) const;

    // Trims, drops empties, dedupes (first occurrence wins), and writes the config file.
    std::error_code Set(const std::vector<std::string>& paths, std::string& outError);

    // Stats every configured path now. No side effects - this can be called as often as the
    // dashboard likes.
    std::vector<WatchStatus> Resolve() const;

    // Records that sPath was just ingested at this size/mtime, and persists the state file.
    // sPath need not already be one of the configured paths - it always ends up tracked.
    std::error_code MarkIngested(const std::string& sPath, uint64_t nSizeBytes, int64_t nMtimeUS);

    // Whether Watcher should rotate (rename aside) sPath as part of ingesting it, rather than
    // leaving it in place. Defaults true for any configured path with no explicit entry in the
    // config's "rotate" map - true is right for an inbox-style source like jotpost, where nothing
    // else needs the file to keep existing at that path. Pass false for a source something ELSE
    // still needs to read at that path after Loom has ingested it.
    bool RotateFor(const std::string& sPath) const;

private:
    struct WatchRecord
    {
        uint64_t mnSizeBytes    = 0;
        int64_t  mnMtimeUS      = 0;
        int64_t  mnLastIngestUS = 0;
    };

    std::error_code SaveConfigLocked() const;
    std::error_code SaveStateLocked() const;

    mutable std::shared_mutex mMutex;
    std::string               msConfigPath;
    std::string               msStatePath;
    std::vector<std::string>  mPaths;
    std::unordered_map<std::string, WatchRecord> mState;
    std::unordered_map<std::string, bool>        mRotate;   // absent == true
};
