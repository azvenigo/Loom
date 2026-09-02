#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "JotStore.h"
#include "Query.h"

#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Ops - the operation surface. The only thing a front end is allowed to call.
//
// This layer exists because Loom has more than one front door: REST routes, MCP tools, the
// importer, and the benchmark. Without it, adding MCP means reimplementing every route against
// JotStore and then watching the two surfaces drift apart - one gains a parameter, the other does
// not, and the difference only shows up as an agent behaving differently from curl. With it, MCP
// is a JSON Schema table and a dispatch switch over these same calls.
//
// The division of labour is strict:
//
//   JotStore  owns invariants. It does exactly what it is told and refuses what would corrupt it.
//             No upserts, no "create if missing", no advice.
//   Ops       owns policy. Upsert-by-name, warning text, limit clamping, query parsing, and the
//             decision that a near-duplicate tag is a warning rather than a rejection.
//
// Nothing here throws; everything returns std::error_code, house style.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct OpsConfig
{
    size_t mnDefaultLimit = 50;
    size_t mnMaxLimit     = 500;

    // Past this many live non-reserved tags, every write carries a vocabulary-size warning. Not a
    // hard cap - refusing writes to protect a tag budget would be worse than the problem.
    size_t mnMaxTags      = 200;
};

// A write's non-fatal advice. These ride back with the new id and are the entire mechanism by
// which the tag vocabulary stays under control: agents self-correct on the next write because they
// were told at the moment it mattered. Nothing here ever fails a request.
struct OpWarnings
{
    std::vector<std::string> mMessages;
    bool Empty() const { return mMessages.empty(); }
};

struct AddResult
{
    Jot        mJot;
    OpWarnings mWarnings;
    bool       mbCreated = false;   // false means an existing named jot was updated instead

    // The patch resolved to the record that was already there, so nothing was written. See
    // MutationResult::mbNoChange.
    bool       mbNoChange = false;
};

struct SearchResultSet
{
    std::vector<Jot> mJots;         // already ordered and limited
    std::vector<float> mScores;     // parallel to mJots
    size_t mnMatched   = 0;
    bool   mbTruncated = false;
};


class Ops
{
public:
    Ops(JotStore& store, const OpsConfig& config = OpsConfig());

    //----------------------------------------------------------------------------------------
    // Writes
    //----------------------------------------------------------------------------------------

    // Creates a jot. If the input carries a name that already exists, this is an ERROR
    // (kNameInUse) - use Upsert when overwrite is what you mean. Keeping the two apart is what
    // stops an import from silently replacing a memory that a human wrote.
    std::error_code Add(const JotInput& input, AddResult& outResult);

    // Create-or-update keyed on the slug. This is the memory-store and reconcile path.
    // nExpectUpdatedUS applies only when the jot already exists; pass 0 for last-write-wins.
    std::error_code Upsert(const JotInput& input, int64_t nExpectUpdatedUS, AddResult& outResult);

    std::error_code Update(tJotID id, const JotInput& patch, int64_t nExpectUpdatedUS,
                           AddResult& outResult);

    std::error_code Delete(tJotID id);

    // Re-applies a complete record read back out of the history log. This is the undo path.
    //
    // POLICY, which is why it is here and not a raw LoadFlatBatch at the route:
    //
    //   - The id is kept. That is the entire point - a restore puts the SAME jot back, links to it
    //     survive, and a jot that was deleted comes back at the address other jots still reference.
    //   - `updated` is stamped NOW rather than carried from the record. The restore is a change and
    //     it happened at the moment somebody asked for it; keeping the old stamp would hide it from
    //     "recently changed" and hand every client an expect_updated value that reads as stale.
    //   - It is REFUSED if the slug now belongs to a different jot. LoadFlatBatch is the replay
    //     path, where names are unique by construction, so its name-index insert does not overwrite
    //     - restoring over a taken slug would leave two jots claiming one name with the index
    //     pointing at whichever got there first. outConflictID names the current holder.
    std::error_code Restore(const FlatJot& record, tJotID& outConflictID, AddResult& outResult);

    std::error_code MergeTags(const std::vector<std::string>& vFrom, const std::string& sTo,
                              size_t& outJotsChanged);

    //----------------------------------------------------------------------------------------
    // Reads
    //----------------------------------------------------------------------------------------

    std::error_code Get(tJotID id, Jot& outJot) const;
    std::error_code GetByName(const std::string& sName, Jot& outJot) const;

    std::error_code Search(const Query& query, SearchResultSet& outResults) const;

    std::error_code Links(tJotID id, size_t nDepth, std::vector<Jot>& outJots) const;

    void ListTags(std::vector<TagStat>& outStats, bool bIncludeReserved = false) const;
    void TagClusters(std::vector<TagCluster>& outClusters) const;

    StoreStats Stats() const { return mStore.GetStats(); }

    //----------------------------------------------------------------------------------------
    // Query construction from loose strings.
    //
    // Lives here rather than in the HTTP layer so that REST query parameters, an MCP tool's JSON
    // arguments, and the benchmark all interpret "30d" and "limit" identically. The moment two
    // front ends parse a range differently, the same question returns different answers depending
    // on which door it came through.
    //----------------------------------------------------------------------------------------
    struct QuerySpec
    {
        std::string              msText;
        std::vector<std::string> mTags;
        std::vector<std::string> mNotTags;
        std::string              msEditor;
        std::string              msName;
        std::string              msSince;     // raw microseconds, "30d", or "2026-08-01"
        std::string              msUntil;
        std::string              msLink;      // decimal jot id
        std::string              msOrder;     // "relevance" | "newest" | "oldest"
        size_t                   mnLimit  = 0;
        size_t                   mnOffset = 0;
        bool                     mbPrefix = false;
    };

    std::error_code BuildQuery(const QuerySpec& spec, Query& outQuery) const;

private:
    void CollectWarnings(const std::vector<TagSuggestion>& vSuggestions, OpWarnings& outWarnings) const;

    JotStore& mStore;
    OpsConfig mConfig;
};
