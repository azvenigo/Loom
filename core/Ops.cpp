// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "Ops.h"

#include "LoomTime.h"
#include "TagRegistry.h"

#include <algorithm>
#include <cstdlib>

Ops::Ops(JotStore& store, const OpsConfig& config)
    : mStore(store)
    , mConfig(config)
{
}

void Ops::CollectWarnings(const std::vector<TagSuggestion>& vSuggestions, OpWarnings& outWarnings) const
{
    for (const TagSuggestion& s : vSuggestions)
    {
        std::string sMsg = "new tag '" + s.msIncoming + "' ";
        if (s.mnDistance == 0)
            sMsg += "looks like a variant of ";
        else
            sMsg += "is " + std::to_string(s.mnDistance)
                  + (s.mnDistance == 1 ? " edit from " : " edits from ");
        sMsg += "'" + s.msExisting + "' (" + std::to_string(s.mnExistingCount) + " uses)";
        outWarnings.mMessages.push_back(std::move(sMsg));
    }

    const size_t nVocab = mStore.GetStats().mnTags;
    if (nVocab > mConfig.mnMaxTags)
    {
        outWarnings.mMessages.push_back(
            "tag vocabulary is " + std::to_string(nVocab) + " tags, over the "
            + std::to_string(mConfig.mnMaxTags) + " you asked to be warned at - consider /tags/similar");
    }
}

//====================================================================================================
// Writes
//====================================================================================================

std::error_code Ops::ValidateCreatedUS(int64_t nCreatedUS)
{
    // A future creation time is refused, and this is the guard that matters most. The id is also
    // the allocator's high-water mark, so a jot dated 2099 would drag mnLastID there with it and
    // every id issued afterwards - for the life of the store - would be a fabricated future
    // timestamp. One typo would permanently break the meaning of every subsequent id, so this is
    // not a matter of taste. A little slack absorbs clock skew between a client and this machine.
    const int64_t nNow = LOOMTIME::NowMicros();
    if (nCreatedUS > nNow + kCreatedFutureSlackUS)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    // Below this is not a date anybody means. It catches the standard migration mistake of passing
    // seconds where microseconds are wanted: a plausible 2026 value in seconds reads as a few
    // minutes past the epoch here, which would otherwise be accepted and silently file the jot
    // under 1970 instead of being reported.
    if (nCreatedUS < kCreatedFloorUS)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    return LoomOK();
}

std::error_code Ops::Add(const JotInput& input, AddResult& outResult)
{
    outResult = AddResult();

    if (input.mnCreatedUS)
    {
        if (std::error_code ec = ValidateCreatedUS(*input.mnCreatedUS))
            return ec;
    }

    MutationResult result;
    if (std::error_code ec = mStore.Add(input, result))
        return ec;

    CollectWarnings(result.mSuggestions, outResult.mWarnings);
    outResult.mJot      = std::move(result.mJot);
    outResult.mbCreated = true;
    return LoomOK();
}

std::error_code Ops::Upsert(const JotInput& input, int64_t nExpectUpdatedUS, AddResult& outResult)
{
    outResult = AddResult();

    // Without a name there is nothing to key an upsert on, so it degrades to a plain create rather
    // than guessing at identity from the text.
    if (!input.msName || input.msName->empty())
        return Add(input, outResult);

    const std::string sName = TagRegistry::Normalize(*input.msName);

    Jot existing;
    if (!mStore.GetByName(sName, existing))
    {
        const std::error_code ec = Add(input, outResult);

        // Lost the race to another writer between the lookup and the insert. The other writer's
        // record is now the one that exists, so fall through to updating it rather than failing a
        // call whose whole contract is "make it so".
        if (LoomErrorOf(ec) == eLoomErr::kNameInUse && mStore.GetByName(sName, existing))
        {
            MutationResult result;
            if (std::error_code ec2 = mStore.Update(existing.mID, input, nExpectUpdatedUS, result))
                return ec2;

            CollectWarnings(result.mSuggestions, outResult.mWarnings);
            outResult.mJot       = std::move(result.mJot);
            outResult.mbCreated  = false;
            outResult.mbNoChange = result.mbNoChange;
            return LoomOK();
        }
        return ec;
    }

    MutationResult result;
    if (std::error_code ec = mStore.Update(existing.mID, input, nExpectUpdatedUS, result))
        return ec;

    CollectWarnings(result.mSuggestions, outResult.mWarnings);
    outResult.mJot       = std::move(result.mJot);
    outResult.mbCreated  = false;
    // Upsert is the memory-store path: an agent re-asserting a memory it already wrote is the
    // single most common no-op in the system, so this flag matters more here than anywhere.
    outResult.mbNoChange = result.mbNoChange;
    return LoomOK();
}

std::error_code Ops::Update(tJotID id, const JotInput& patch, int64_t nExpectUpdatedUS,
                            AddResult& outResult)
{
    outResult = AddResult();

    if (patch.Empty())
        return MakeLoomError(eLoomErr::kInvalidArgument);

    MutationResult result;
    if (std::error_code ec = mStore.Update(id, patch, nExpectUpdatedUS, result))
        return ec;

    CollectWarnings(result.mSuggestions, outResult.mWarnings);
    outResult.mJot       = std::move(result.mJot);
    outResult.mbCreated  = false;
    outResult.mbNoChange = result.mbNoChange;
    return LoomOK();
}

std::error_code Ops::Delete(tJotID id)
{
    return mStore.Remove(id);
}

std::error_code Ops::Restore(const FlatJot& record, int64_t nExpectUpdatedUS, tJotID& outConflictID,
                             AddResult& outResult)
{
    outResult      = AddResult();
    outConflictID  = kInvalidJotID;

    if (record.mID == kInvalidJotID)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    // Same guard Update() applies: if the jot currently exists and has moved since the caller last
    // saw it, refuse rather than silently overwrite. A jot that no longer exists (deleted) has
    // nothing to be stale relative to, so the check is skipped and restoring it proceeds.
    Jot current;
    if (nExpectUpdatedUS != 0 && mStore.Get(record.mID, current)
        && current.EffectiveUpdatedUS() != nExpectUpdatedUS)
        return MakeLoomError(eLoomErr::kConflict);

    // A RESTORE THAT CHANGES NOTHING IS NOT A MUTATION.
    //
    // The same rule Update follows, and here it is not an edge case but the common one: the History
    // view puts a Restore button on EVERY row including the newest, and the newest row is by
    // definition the version the jot is already on. Writing it anyway bumped `updated` - discarding
    // every other agent's expect_updated token for nothing - added a WAL line, and appended a
    // history entry that was a byte-for-byte copy of the one being restored from, differing only in
    // its timestamp. The log is supposed to answer "what changed".
    //
    // The call still succeeds and still returns the record: the caller asked for a state and that is
    // the state. mbNoChange says nothing was written, for anyone who cares to tell the difference.
    FlatJot currentFlat;
    if (mStore.Flatten(record.mID, currentFlat) && JotStore::SameContent(currentFlat, record))
    {
        if (!mStore.Get(record.mID, outResult.mJot))
            return MakeLoomError(eLoomErr::kNotFound);
        outResult.mbCreated  = false;
        outResult.mbNoChange = true;
        return LoomOK();
    }

    // The slug check is a read, so it races a concurrent write in principle. It is still worth
    // doing: the window is microseconds, the alternative is a silently duplicated name that nothing
    // else in the store would ever notice, and the loser of the race gets a name index pointing at
    // the other jot rather than corruption.
    if (!record.msName.empty())
    {
        Jot holder;
        if (mStore.GetByName(record.msName, holder) && holder.mID != record.mID)
        {
            outConflictID = holder.mID;
            return MakeLoomError(eLoomErr::kNameInUse);
        }
    }

    std::vector<FlatJot> vOne(1, record);
    vOne[0].mnUpdatedUS = LOOMTIME::NowMicros();

    size_t nLoaded = 0;
    if (std::error_code ec = mStore.LoadFlatBatch(vOne, nLoaded, /*bJournal*/ true))
        return ec;
    if (nLoaded == 0)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    if (!mStore.Get(record.mID, outResult.mJot))
        return MakeLoomError(eLoomErr::kNotFound);

    outResult.mbCreated = false;
    return LoomOK();
}

std::error_code Ops::MergeTags(const std::vector<std::string>& vFrom, const std::string& sTo,
                               size_t& outJotsChanged)
{
    return mStore.MergeTags(vFrom, sTo, outJotsChanged);
}

//====================================================================================================
// Reads
//====================================================================================================

std::error_code Ops::Get(tJotID id, Jot& outJot) const
{
    if (!mStore.Get(id, outJot))
        return MakeLoomError(eLoomErr::kNotFound);
    return LoomOK();
}

std::error_code Ops::GetByName(const std::string& sName, Jot& outJot) const
{
    if (!mStore.GetByName(sName, outJot))
        return MakeLoomError(eLoomErr::kNotFound);
    return LoomOK();
}

std::error_code Ops::Search(const Query& query, SearchResultSet& outResults) const
{
    outResults = SearchResultSet();

    SearchResults hits;
    mStore.Search(query, hits);

    outResults.mnMatched   = hits.mnMatched;
    outResults.mbTruncated = hits.mbTruncated;

    std::vector<tJotID> vIDs;
    vIDs.reserve(hits.mHits.size());
    outResults.mScores.reserve(hits.mHits.size());
    for (const SearchHit& h : hits.mHits)
    {
        vIDs.push_back(h.mID);
        outResults.mScores.push_back(h.mfScore);
    }

    mStore.GetMany(vIDs, outResults.mJots);

    // GetMany drops ids that vanished between the search and the fetch (another thread deleting
    // one). Rare, but it would silently misalign the parallel score vector, so trim to match.
    if (outResults.mScores.size() != outResults.mJots.size())
        outResults.mScores.resize(outResults.mJots.size());

    return LoomOK();
}

std::error_code Ops::Links(tJotID id, size_t nDepth, std::vector<Jot>& outJots) const
{
    outJots.clear();

    Jot root;
    if (!mStore.Get(id, root))
        return MakeLoomError(eLoomErr::kNotFound);

    std::vector<tJotID> vIDs;
    mStore.Neighborhood(id, nDepth == 0 ? 1 : nDepth, vIDs);
    mStore.GetMany(vIDs, outJots);
    return LoomOK();
}

void Ops::ListTags(std::vector<TagStat>& outStats, bool bIncludeReserved) const
{
    mStore.ListTags(outStats, bIncludeReserved);
}

void Ops::TagClusters(std::vector<TagCluster>& outClusters) const
{
    mStore.TagClusters(outClusters);
}

//====================================================================================================
// Query construction
//====================================================================================================

std::error_code Ops::BuildQuery(const QuerySpec& spec, Query& outQuery) const
{
    outQuery = Query();

    outQuery.msText          = spec.msText;
    outQuery.mTags           = spec.mTags;
    outQuery.mNotTags        = spec.mNotTags;
    outQuery.msEditor        = spec.msEditor;
    outQuery.msName          = spec.msName;
    outQuery.mbPrefixLastTerm = spec.mbPrefix;
    outQuery.mnOffset        = spec.mnOffset;

    const int64_t nNowUS = LOOMTIME::NowMicros();

    if (!spec.msSince.empty() && !LOOMTIME::ParseTimeSpec(spec.msSince, nNowUS, outQuery.mnSince))
        return MakeLoomError(eLoomErr::kInvalidArgument);
    if (!spec.msUntil.empty() && !LOOMTIME::ParseTimeSpec(spec.msUntil, nNowUS, outQuery.mnUntil))
        return MakeLoomError(eLoomErr::kInvalidArgument);

    // An inverted range is a caller mistake, not an empty result - saying so beats silently
    // returning nothing and letting them wonder where their jots went.
    if (outQuery.mnSince != 0 && outQuery.mnUntil != 0 && outQuery.mnSince > outQuery.mnUntil)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    if (!spec.msLink.empty())
    {
        char* pEnd = nullptr;
        const long long nLink = std::strtoll(spec.msLink.c_str(), &pEnd, 10);
        if (pEnd == spec.msLink.c_str() || nLink <= 0)
            return MakeLoomError(eLoomErr::kInvalidArgument);
        outQuery.mnLinkedTo = static_cast<tJotID>(nLink);
    }

    if (spec.msOrder.empty() || spec.msOrder == "auto")
        outQuery.mOrder = eOrder::kAuto;
    else if (spec.msOrder == "relevance")
        outQuery.mOrder = eOrder::kRelevance;
    else if (spec.msOrder == "newest" || spec.msOrder == "recent")
        outQuery.mOrder = eOrder::kNewest;
    else if (spec.msOrder == "oldest")
        outQuery.mOrder = eOrder::kOldest;
    else
        return MakeLoomError(eLoomErr::kInvalidArgument);

    // Clamped rather than rejected: a client asking for 10000 results wants "as many as you'll
    // give me", and failing the whole query over it helps nobody.
    outQuery.mnLimit = spec.mnLimit == 0 ? mConfig.mnDefaultLimit
                                         : std::min(spec.mnLimit, mConfig.mnMaxLimit);

    return LoomOK();
}
