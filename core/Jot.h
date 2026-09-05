#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Jot - the one record type Loom stores.
//
// The id IS the creation timestamp (microseconds since the Unix epoch). That single decision buys
// three things: a unique key, a meaningful value, and a free chronological index - because ids are
// allocated monotonically, a vector of live ids is already sorted by creation time and a date
// range is two binary searches. See JotStore::NextID for how uniqueness survives a clock that
// repeats or steps backwards.
//
// Everything optional is genuinely optional. A jot typed into ZHotkey is just an id and some text;
// msName, msSummary, tags and links stay empty and are omitted from the wire entirely. The two
// string fields exist for the Claude memory store, where a memory is addressed by a stable slug
// ("user-preferences") rather than a timestamp, and where the frontmatter description is the field
// recall actually matches against - so it is indexed separately and weighted above the body.
//
// mTags and mEditor are interned to integers by the store, so a Jot holds ids, not strings. Sorted
// small vectors beat std::set at these cardinalities (0-4 tags is typical) and make intersection a
// linear merge.
//
// mnSummaryLen / mnBodyLen are DERIVED - term counts kept here purely so BM25 scoring does not
// need a second lookup. They are rebuilt from the text on load and never serialized.
//////////////////////////////////////////////////////////////////////////////////////////////////

using tJotID    = int64_t;   // creation time in microseconds since epoch - IS the unique id
using tTagID    = uint32_t;  // interned by TagRegistry
using tEditorID = uint32_t;  // interned; 0 is always "user"
using tTermID   = uint32_t;  // interned by JotStore's term tables

constexpr tJotID    kInvalidJotID  = 0;
constexpr tTagID    kInvalidTagID  = 0xFFFFFFFFu;
constexpr tEditorID kDefaultEditor = 0;          // "user" - interned first, so always id 0
constexpr tTermID   kInvalidTermID = 0xFFFFFFFFu;

// Which text field a term came from. Summary and body are indexed separately so the ranker can
// weight them differently; see Query.h.
enum class eField : uint8_t
{
    kSummary = 0,
    kBody    = 1,
    kCount   = 2
};


struct Jot
{
    tJotID              mID         = kInvalidJotID;  // creation microseconds, unique, monotonic
    int64_t             mnUpdatedUS = 0;              // 0 == never edited
    tEditorID           mEditor     = kDefaultEditor; // 0 == "user"

    std::string         msName;         // optional stable slug; empty for plain jots
    std::string         msSummary;      // optional; weighted above body when ranking
    std::string         msText;

    std::vector<tTagID> mTags;          // sorted, usually empty
    std::vector<tJotID> mLinks;         // sorted, usually empty

    // Link targets named by slug that did not exist yet at write time. The memory store links
    // liberally to memories not yet written - "a [[name]] that doesn't match an existing memory is
    // fine, it marks something worth writing later" - so an unresolved name is held here rather
    // than rejected, and promoted into mLinks when that name appears.
    std::vector<std::string> mPendingLinks;

    // Derived, never serialized. Term counts for BM25 length normalization.
    uint32_t            mnSummaryLen = 0;
    uint32_t            mnBodyLen    = 0;

    // Derived, never serialized. Dense ordinal assigned by the store, stable for the life of the
    // record. Scoring accumulates into a flat array indexed by this rather than into a hash map
    // keyed by id - see JotStore::Locked_ScoreText for why that mattered.
    uint32_t            mnSlot       = 0;

    bool IsNamed()   const { return !msName.empty(); }
    bool WasEdited() const { return mnUpdatedUS != 0; }

    // The timestamp a caller should treat as "when this last changed".
    int64_t EffectiveUpdatedUS() const { return mnUpdatedUS != 0 ? mnUpdatedUS : mID; }
};


//////////////////////////////////////////////////////////////////////////////////////////////////
// JotInput - what a caller supplies to create or patch a jot.
//
// Strings, not interned ids: interning mutates shared tables, so it happens inside the store under
// its write lock, never in the caller.
//
// Every field is optional, and that is load-bearing on the patch path. std::nullopt means "leave
// this alone"; an engaged-but-empty value means "clear it". Collapsing those two into one empty
// string is the classic way a PATCH silently wipes a field nobody asked it to touch.
//
// mLinks entries are each either a decimal jot id or a slug; the store decides which by trying to
// parse. A slug that resolves becomes a real link, one that does not becomes a pending link.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct JotInput
{
    std::optional<std::string>              msName;
    std::optional<std::string>              msSummary;
    std::optional<std::string>              msText;
    std::optional<std::vector<std::string>> mTags;
    std::optional<std::vector<std::string>> mLinks;    // decimal ids or slugs
    std::optional<std::string>              msEditor;  // empty/absent resolves to "user"

    // When this jot was actually created, in microseconds since the epoch. Absent means now, which
    // is the ordinary case; it exists so content migrated in from somewhere else keeps its real
    // date instead of being stamped with the moment of the migration.
    //
    // THIS BECOMES THE ID, because the id is not merely derived from the creation time - it IS the
    // creation time, and the store has no second field to put this in. See "BACKDATING" in
    // JotStore::Add for why that is the right answer rather than the expedient one, and what it
    // costs. CREATE ONLY: an existing jot's id cannot move without breaking every link that points
    // at it, so Update ignores this and Upsert honours it only when it actually creates.
    std::optional<int64_t>                  mnCreatedUS;

    // Deliberately NOT counting mnCreatedUS: it is ignored on the patch path, so a patch carrying
    // only a creation time still changes nothing and must still be rejected as empty.
    bool Empty() const
    {
        return !msName && !msSummary && !msText && !mTags && !mLinks && !msEditor;
    }
};
