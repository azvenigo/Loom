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
// DELIBERATELY NOT A FILESYSTEM WATCHER. There is no background thread and no debounce logic.
// "Has this file changed" is only answered when something asks (GET /watch), which is cheap - a
// stat() per watched path - and side-steps the debounce problem a real watch would have against a
// file that syncs in over OneDrive in bursts.
//
// PENDING MEANS BOTH SIZE AND MTIME DIFFER FROM THE LAST INGEST, not just mtime. A bare touch or a
// metadata-only change (OneDrive re-syncing the same bytes) must not read as new content.
//
// Same shape as core/IpAcl: a small JSON file, loaded at startup, edited at runtime from the
// dashboard, written whole via tmp -> rename so a crash mid-write leaves the previous version
// rather than a truncated one.
//
// TWO FILES, ONE CONCERN EACH:
//   loom.watch.json        - the list of paths. User-facing config, edited via PUT /watch.
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
};
