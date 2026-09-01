#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/Jot.h"

#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// FlatJot - a Jot with every interned id resolved back to a string.
//
// A stored Jot holds tag and editor IDs, which are meaningless outside the process that interned
// them. Anything crossing a boundary - the wire, the WAL, a snapshot file - needs the strings. This
// is that form, and it is deliberately the ONE serialization type: the HTTP layer, the MCP layer,
// the journal and the snapshot all speak FlatJot, so there is exactly one definition of what a jot
// looks like outside RAM.
//
// WHY THIS EXISTS AS A SEPARATE TYPE, and not as "pass the NameTables along":
//
// The journal has to serialize a mutation while the store's write lock is still held, so that WAL
// order is guaranteed to equal apply order. But resolving tag names needs the tag registry, which
// that same lock guards - and std::shared_mutex is not recursive, so a sink that tried to look the
// names up itself would deadlock instantly. Flattening inside the store, where the lock is already
// held, is what makes ordered journaling possible at all.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct FlatJot
{
    tJotID                   mID         = kInvalidJotID;
    int64_t                  mnUpdatedUS = 0;      // 0 == never edited
    std::string              msEditor;             // empty == "user"
    std::string              msName;
    std::string              msSummary;
    std::string              msText;
    std::vector<std::string> mTags;
    std::vector<tJotID>      mLinks;
    std::vector<std::string> mPendingLinks;

    bool IsDefaultEditor() const { return msEditor.empty() || msEditor == "user"; }
};
