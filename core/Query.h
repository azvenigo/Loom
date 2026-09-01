#pragma once

#include "Jot.h"

#include <cstdint>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Query - what to look for, and how results come back.
//
// Execution order is the whole performance story, and it runs cheapest-first:
//
//   1. Boolean filters (tags, editor, time range, link) intersect into a candidate set. These are
//      sorted posting-list merges and an mChrono slice - no scoring, no text touched.
//   2. Free text intersects term postings against that candidate set.
//   3. Only the survivors get scored.
//   4. Partial sort to mnLimit, never a full sort.
//
// Doing it the other way round - score everything, then filter - is the standard way a search path
// ends up a thousand times slower than it needs to be.
//
// FIELD WEIGHTING is why summary and body are indexed separately. In the memory store the
// frontmatter description is explicitly the field recall matches against; a hit there means far
// more than the same word buried in the body. kSummaryWeight is what makes ranked search a
// credible replacement for the hand-maintained cluster index files.
//////////////////////////////////////////////////////////////////////////////////////////////////

enum class eOrder : uint8_t
{
    kAuto = 0,   // relevance when there is query text, newest-first otherwise
    kRelevance,
    kNewest,
    kOldest
};

struct Query
{
    std::string              msText;         // free text; empty means "filters only"
    std::vector<std::string> mTags;          // ALL must be present
    std::vector<std::string> mNotTags;       // NONE may be present
    std::string              msEditor;       // exact, empty means any
    std::string              msName;         // exact slug lookup, empty means any

    tJotID                   mnSince   = 0;  // inclusive; 0 means open
    tJotID                   mnUntil   = 0;  // inclusive; 0 means open
    tJotID                   mnLinkedTo = kInvalidJotID;  // jots linked to or from this one

    size_t                   mnLimit  = 50;
    size_t                   mnOffset = 0;
    eOrder                   mOrder   = eOrder::kAuto;

    // Treat the final query term as a prefix. This is the live-as-you-type path for ZHotkey:
    // "data" also matches "database" while the user is still typing, without a separate API.
    bool                     mbPrefixLastTerm = false;

    bool HasText()    const { return !msText.empty(); }
    bool HasFilters() const
    {
        return !mTags.empty() || !mNotTags.empty() || !msEditor.empty() || !msName.empty()
            || mnSince != 0 || mnUntil != 0 || mnLinkedTo != kInvalidJotID;
    }
};


struct SearchHit
{
    tJotID mID     = kInvalidJotID;
    float  mfScore = 0.0f;
};

struct SearchResults
{
    std::vector<SearchHit> mHits;      // already limited and ordered
    size_t                 mnMatched = 0;   // total before offset/limit
    bool                   mbTruncated = false;
};


//////////////////////////////////////////////////////////////////////////////////////////////////
// Ranking constants. Collected here rather than scattered through JotStore.cpp because these are
// the knobs anyone tuning result quality will reach for first.
//////////////////////////////////////////////////////////////////////////////////////////////////
namespace RANK
{
    // Standard BM25 parameters. k1 controls term-frequency saturation, b the strength of length
    // normalization.
    constexpr float kBM25_K1 = 1.2f;
    constexpr float kBM25_B  = 0.75f;

    // A term found in the summary counts this much more than the same term in the body.
    constexpr float kSummaryWeight = 3.0f;

    // A query term that is also one of the jot's tags. Tagging is a deliberate act, so it is strong
    // evidence - stronger per term than the summary, which is prose and may mention anything.
    //
    // Applied PER MATCHING TERM and multiplied by that term's idf, never as a flat per-jot bonus.
    // A flat bonus lets a jot matching one query term via a tag outrank a jot matching every term
    // in its summary; scoring per term keeps query coverage dominant, which is what a reader
    // actually expects.
    constexpr float kTagMatchBoost = 2.0f;

    // Recency: score is multiplied by (1 + kRecencyWeight * exp(-age / kRecencyHalfLifeUS)).
    // Deliberately a multiplier on relevance rather than an additive term, so recency breaks ties
    // between comparable matches instead of burying an old exact hit under new noise.
    constexpr float   kRecencyWeight     = 0.35f;
    constexpr int64_t kRecencyHalfLifeUS = 30LL * 24 * 60 * 60 * 1000000LL;   // 30 days

    // A prefix expansion matches many terms; each one counts for less than an exact hit so that
    // typing more characters monotonically sharpens the result set.
    constexpr float kPrefixPenalty = 0.5f;

    // Cap on how many distinct terms one prefix expands to. Without it, a single letter unions
    // most of the index.
    constexpr size_t kMaxPrefixExpansion = 64;
}
