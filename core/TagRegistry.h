#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "Interner.h"
#include "Jot.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// TagRegistry - interning, usage stats, and the machinery for keeping the tag vocabulary honest.
//
// "Keep it relevant and under control" is the stated goal, and it fails silently when three agents
// write independently: nobody is wrong, and the vocabulary still drifts into meds/med/medication.
// A rule in a document does not fix that because no agent reads it at write time. So the control
// lives here and surfaces in the WRITE RESPONSE - Suggest() is called on every create, and its
// output rides back with the new id as a non-fatal warning. Nothing is ever rejected; agents
// self-correct on the next write because they were told, at the moment it mattered.
//
// RESERVED TAGS. A tag containing ':' is structural, not vocabulary - "type:user", "status:done",
// "asserted:2026-08-18" carry memory-store frontmatter that has nowhere better to live. They are
// excluded from similarity clustering and from the vocabulary budget, because otherwise every
// asserted: date would look like a near-duplicate of every other one and drown the real signal.
//
// Refcounts are maintained by JotStore as jots gain and lose tags. A tag whose count falls to zero
// keeps its id (ids are never reused) but drops out of listings.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct TagStat
{
    std::string msTag;
    uint32_t    mnCount   = 0;   // live jots carrying this tag
    tJotID      mFirstUS  = 0;   // first time it was applied
    tJotID      mLastUS   = 0;   // most recent
    bool        mbReserved = false;
};

// One group of tags that look like variants of each other.
struct TagCluster
{
    std::vector<std::string> mMembers;   // most-used first
};

// A single "this looks like something you already have" note.
struct TagSuggestion
{
    std::string msIncoming;
    std::string msExisting;
    uint32_t    mnExistingCount = 0;
    uint32_t    mnDistance      = 0;   // 0 == prefix/plural relationship rather than an edit
};


class TagRegistry
{
public:
    // trim, ASCII lowercase, runs of whitespace and underscores to '-', collapse repeated '-',
    // strip leading/trailing '-'. Returns empty for a tag that normalizes to nothing.
    static std::string Normalize(std::string_view sTag);

    static bool IsReserved(std::string_view sNormalizedTag);

    // Interning. Intern() creates on demand; Find() never does.
    tTagID Intern(std::string_view sNormalizedTag);
    tTagID Find(std::string_view sNormalizedTag) const;
    const std::string& Name(tTagID id) const { return mNames.Value(id); }
    bool IsValid(tTagID id) const { return mNames.IsValid(id); }
    size_t Count() const { return mNames.Count(); }

    // Refcounting, driven by JotStore as tags are applied and removed.
    void AddRef(tTagID id, int64_t nWhenUS);
    void Release(tTagID id);
    uint32_t RefCount(tTagID id) const;

    // Live tags only (count > 0), most-used first. bIncludeReserved defaults false because the
    // interesting question is almost always "what does my vocabulary look like".
    void List(std::vector<TagStat>& outStats, bool bIncludeReserved = false) const;

    // How many live, non-reserved tags exist. This is the number --max-tags is measured against.
    size_t VocabularySize() const;

    // Near-duplicates of a tag about to be created. Empty when the tag already exists, since an
    // exact hit is not a suggestion. Only considers tags with a live refcount.
    void Suggest(std::string_view sNormalizedTag, std::vector<TagSuggestion>& outSuggestions) const;

    // Every group of mutually-similar live tags. Backs GET /tags/similar. O(n^2) over the
    // vocabulary, which is fine at any size a human would tolerate reading the output of.
    void Clusters(std::vector<TagCluster>& outClusters) const;

    void Clear();

private:
    // Levenshtein with an early bail once the distance exceeds nMax. The bail is what makes the
    // all-pairs scan in Clusters() cheap - most pairs fail on the length check alone.
    static uint32_t BoundedEditDistance(std::string_view a, std::string_view b, uint32_t nMax);

    // True when b is a is a plural/prefix variant of a ("meds"/"med", "network"/"networking").
    static bool IsMorphologicalVariant(std::string_view a, std::string_view b);

    struct Entry
    {
        uint32_t mnCount    = 0;
        int64_t  mFirstUS   = 0;
        int64_t  mLastUS    = 0;
        bool     mbReserved = false;
    };

    Interner           mNames;
    std::vector<Entry> mEntries;   // parallel to mNames ids
};
