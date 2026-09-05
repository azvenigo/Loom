// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/Undo.h"

#include "codec/JotJson.h"

namespace UNDO
{
    std::error_code ByTransaction(Ops& ops, History& history, uint64_t nTxnID, bool bUndo,
                                  const std::string& sOrigin, UndoReport& outReport,
                                  std::string& outError)
    {
        outReport = UndoReport();
        outReport.mnTxnID = nTxnID;
        outReport.mbUndo  = bUndo;
        outError.clear();

        if (nTxnID == 0)
        {
            outError = "not a transaction id";
            return MakeLoomError(eLoomErr::kInvalidArgument);
        }

        std::vector<HistoryEntry> vGroup;
        history.ListTransaction(nTxnID, vGroup);
        if (vGroup.empty())
        {
            outError = "no operation " + std::to_string(nTxnID) + " in the history log - it may "
                       "have aged out";
            return MakeLoomError(eLoomErr::kNotFound);
        }

        for (const HistoryEntry& logged : vGroup)
        {
            UndoItem item;
            item.mID    = logged.mID;
            item.msName = logged.msName;

            // What this record should be put back to. In reapply mode it is the entry itself; in
            // undo mode it is the version in force immediately before it, which is exactly what
            // Previous() already answers for a single restore.
            HistoryEntry target = logged;
            if (bUndo && !history.Previous(logged.mnSeq, target))
            {
                // The jot did not exist before the transaction, so undoing it would mean DELETING
                // it - a destructive act nobody asked for when they said "put things back". It is
                // reported instead. A tag merge cannot produce this (it only rewrites jots that
                // already carried the tag), but a future multi-record operation that creates
                // records can, and silently erasing them would be the worst possible default.
                item.msError = "created by that operation, so there is no earlier version to put "
                               "back - delete it by hand if that is what you want";
                ++outReport.mnFailed;
                outReport.mItems.push_back(std::move(item));
                continue;
            }

            item.mnFromSeq = target.mnSeq;

            FlatJot record;
            std::string sParseError;
            if (target.msRecord.empty()
                || !JOTJSON::ParseFlat(target.msRecord, record, sParseError))
            {
                item.msError = target.msRecord.empty()
                             ? std::string("that version is a delete, with no record to restore")
                             : "unreadable history entry: " + sParseError;
                ++outReport.mnFailed;
                outReport.mItems.push_back(std::move(item));
                continue;
            }

            if (!sOrigin.empty())
                record.msOrigin = sOrigin;

            tJotID    conflictID = kInvalidJotID;
            AddResult result;
            const std::error_code ec = ops.Restore(record, /*nExpectUpdatedUS*/ 0, conflictID,
                                                   result);
            if (ec)
            {
                item.mConflictID = conflictID;
                item.msError = (conflictID != kInvalidJotID)
                    ? "the slug '" + record.msName + "' now belongs to jot "
                        + std::to_string(conflictID)
                    : ec.message();
                ++outReport.mnFailed;
            }
            else if (result.mbNoChange)
            {
                item.mbNoChange = true;
                ++outReport.mnUnchanged;
            }
            else
            {
                item.mbRestored = true;
                ++outReport.mnRestored;
            }

            outReport.mItems.push_back(std::move(item));
        }

        return LoomOK();
    }
}
