#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/FlatJot.h"
#include "core/JotStore.h"
#include "core/Ops.h"
#include "core/TagRegistry.h"

#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// JotJson - the wire and on-disk format, and the one place the omit-empty rule is enforced.
//
// THE RULE: nothing optional is ever emitted. A jot typed into ZHotkey serializes as
//
//     {"id":1756661962123456,"text":"Ordered 50ft of ethernet cable today"}
//
// and not as that plus "tags":[], "links":[], "summary":"", "editor":"user", "updated":null. What
// you POST is what you GET, so a round-trip is lossless and the WAL line for a bare jot is barely
// longer than the jots.log line it replaces.
//
// Omitted when absent: name, summary, tags, links, pending. Omitted when never edited: updated.
// Omitted when it resolves to the default: editor - an unspecified author and an author of "user"
// are the same thing by definition, so collapsing them loses nothing.
//
// bVerbose materializes every default for clients that would rather not branch. It is a DISPLAY
// convenience only - never feed a verbose document back as an update and expect identical meaning,
// because an explicit "editor":"user" is indistinguishable from the default.
//
// This lives in codec/ rather than http/ because the journal and the snapshot writer need exactly
// the same serialization, and neither should have to link a web framework to get it. Nothing here
// includes crow.
//////////////////////////////////////////////////////////////////////////////////////////////////

namespace JOTJSON
{
    //--------------------------------------------------------------------------------------------
    // Records
    //--------------------------------------------------------------------------------------------

    std::string ToJson(const FlatJot& jot, bool bVerbose);

    // Parses one serialized record back into a FlatJot. This is the WAL/snapshot replay path, so
    // it is strict about the id and forgiving about everything else - a record written by an older
    // build simply lacks the newer keys, and they come back as defaults rather than as an error.
    bool ParseFlat(const std::string& sJson, FlatJot& outJot, std::string& outError);

    // Parses a create/patch body. Absent keys stay nullopt, which is what makes PATCH leave
    // untouched fields alone rather than clearing them.
    bool ParseInput(const std::string& sBody, JotInput& outInput, std::string& outError);

    //--------------------------------------------------------------------------------------------
    // Responses
    //--------------------------------------------------------------------------------------------

    std::string SearchToJson(const SearchResultSet& results, const NameTables& names, bool bVerbose);
    std::string JotListToJson(const std::vector<Jot>& vJots, const NameTables& names, bool bVerbose);
    std::string TagsToJson(const std::vector<TagStat>& vTags);
    std::string ClustersToJson(const std::vector<TagCluster>& vClusters);
    std::string StatsToJson(const StoreStats& stats, const PersistStats& persist);

    // The write response: the record, plus any non-fatal tag warnings. The warnings ride with the
    // result rather than arriving out of band, because an agent that has to make a second call to
    // discover it just fragmented the vocabulary will not make it.
    std::string MutationToJson(const AddResult& result, const NameTables& names, bool bVerbose);

    std::string ErrorToJson(const std::string& sMessage);
}
