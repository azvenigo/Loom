#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/Jot.h"
#include "core/LoomError.h"

#include <cstdint>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Purge - erasing a jot from every copy of itself, for the day something sensitive gets written
// down by accident.
//
// DELETE IS NOT PURGE, and that is the whole reason this exists. DELETE /jots/<id> removes a jot
// from RAM and appends a `del` to the WAL - and the text is still sitting in the snapshot, still in
// the WAL above the tombstone, and now also in the history log, which exists precisely so that
// deletions can be undone. Four copies, three of which a delete does not touch. If what you wrote
// down was an API key, "deleted" is not a description of where it is.
//
// IT CANNOT RUN IN THE SERVER, and this is not a policy choice. The snapshot and the WAL are open
// and being appended to; the history committer is holding its own file; and RAM is being served
// from. Rewriting those files underneath all of that has no correct ordering. So the operation is
// split in two:
//
//   1. The dashboard (or POST /purge/request) writes a REQUEST FILE naming the ids, why, and what
//      those jots looked like at the time. It changes nothing else. The request is a statement of
//      intent that survives the restart the purge needs.
//   2. `loom --purge=DIR` does the work with the service stopped. It takes the same data lock the
//      server takes, so if the server is still up it refuses rather than corrupting anything -
//      the interlock is the lock, not a check somebody can forget.
//
// WHY A HUMAN CONFIRMS IN THE MIDDLE. Step 2 is irreversible by construction and is aimed at the
// history log, which is the only thing that could undo it. So the request carries a snapshot of
// what was named - id, slug, and a clipped summary - captured while the jots still existed. That is
// what an operator or an agent reads back to the person before stopping the service: the point of
// the labels is to make "yes, those" answerable AFTER the content is unreachable.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct PurgeLabel
{
    tJotID      mID = kInvalidJotID;
    std::string msName;
    std::string msSummary;   // clipped, for the confirmation step - never the full text
};

struct PurgeRequest
{
    int64_t                 mnCreatedUS = 0;
    std::string             msReason;
    std::string             msRequestedBy;   // the address that asked, for the audit trail
    std::vector<tJotID>     mIDs;
    std::vector<PurgeLabel> mLabels;
};

struct PurgeReport
{
    size_t mnRemovedFromStore    = 0;
    size_t mnSnapshotRecords     = 0;
    size_t mnHistoryDropped      = 0;
    size_t mnHistoryKept         = 0;
    size_t mnWalBytesDiscarded   = 0;
    size_t mnNotFound            = 0;   // ids that were already gone from the live store
};

namespace PURGE
{
    // <dir>/loom.purge-request.json
    std::string RequestPath(const std::string& sDir);

    std::error_code WriteRequest(const std::string& sPath, const PurgeRequest& request);
    bool ReadRequest(const std::string& sPath, PurgeRequest& outRequest, std::string& outError);
    std::error_code ClearRequest(const std::string& sPath);

    // The offline half. THE CALLER MUST ALREADY HOLD THE DATA LOCK for sDir - that is what proves
    // no server is running against it.
    //
    // Order matters and mirrors Snapshot's, for the same reason: the snapshot is rewritten and
    // committed by rename BEFORE the WAL is discarded, so an interruption between the two leaves a
    // consistent store that still contains the secret, rather than a truncated one that has lost
    // unrelated data. Purging twice is harmless; purging half is not.
    std::error_code Run(const std::string& sDir, const PurgeRequest& request,
                        PurgeReport& outReport, std::string& outError);

    // The block of text the dashboard shows and an agent is meant to follow. Kept next to the
    // implementation so the steps cannot drift from what Run actually does.
    std::string AgentInstructions(const std::string& sDir, const PurgeRequest& request);
}
