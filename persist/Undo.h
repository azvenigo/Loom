#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/LoomError.h"
#include "core/Ops.h"
#include "persist/History.h"

#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Undo - putting a whole multi-record operation back, as one act.
//
// WHY THIS IS NOT IN Ops. Ops is the operation surface over JotStore and knows nothing about the
// history log; History is a journal sink and knows nothing about Ops. Restoring a transaction needs
// both - read the group out of the log, hand each record to Ops::Restore - so it belongs to
// neither, and putting it in either would make one of them depend on the other for one function.
//
// WHY IT IS NOT WRITTEN TWICE. Both front doors need it: POST /history/restore and the loom_restore
// tool. Restoring ONE seq is already spelled out at both, which is tolerable at fifteen lines; a
// group restore is a loop with per-record outcomes, a partial-failure report and two modes, and two
// copies of that would drift within a release.
//
// TWO MODES, and the distinction is the whole feature:
//
//   REAPPLY puts each jot back into the state the transaction left it in. It is the exact analogue
//   of restoring one seq, and on a group that is still the current state it changes nothing.
//
//   UNDO puts each jot back into the state it was in immediately BEFORE the transaction. This is
//   what "undo that tag merge" means, and it is the reason transaction ids exist at all: every
//   individual write a merge makes was always restorable, but nothing recorded that they were one
//   act, so reversing one meant finding and restoring each affected jot by hand.
//
// PARTIAL SUCCESS IS A REAL OUTCOME. Forty jots, and by the time somebody reverses the merge two of
// them may have been edited, renamed or deleted since. Each record is attempted independently and
// reported independently rather than the whole thing failing on the first refusal - a group restore
// that stops halfway with no report is worse than one that finishes and says which two it skipped.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct UndoItem
{
    tJotID      mID       = kInvalidJotID;
    uint64_t    mnFromSeq = 0;          // the logged version that was put back
    std::string msName;
    bool        mbRestored  = false;    // a write happened
    bool        mbNoChange  = false;    // already in that state, so nothing was written
    tJotID      mConflictID = kInvalidJotID;  // the jot that now holds this one's slug
    std::string msError;                // empty unless this record was skipped
};

struct UndoReport
{
    uint64_t              mnTxnID = 0;
    bool                  mbUndo  = false;
    std::vector<UndoItem> mItems;
    size_t                mnRestored  = 0;
    size_t                mnUnchanged = 0;
    size_t                mnFailed    = 0;
};

namespace UNDO
{
    // Restores every record in one transaction. bUndo selects the mode above.
    //
    // sOrigin is stamped onto each restored record, because the restore is a write happening now
    // from there - the same rule the single-seq paths follow. Empty leaves the logged origin alone.
    //
    // kNotFound, with outError set to something a person can act on, when no such group survives in
    // either generation of the log. Anything else is reported per record in the report; the call
    // itself succeeds as long as the group was found, even if every record in it was refused.
    //
    // NO expect_updated. The caller holds one transaction id, not N per-jot revisions, so there is
    // no token to check against and inventing one would be worse than admitting the gap. That makes
    // this the one write path in Loom without the concurrency guard, which is why the report names
    // every jot it touched: after the fact is the only place that information can be offered.
    std::error_code ByTransaction(Ops& ops, History& history, uint64_t nTxnID, bool bUndo,
                                  const std::string& sOrigin, UndoReport& outReport,
                                  std::string& outError);
}
