// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "JotStore.h"

#include "LoomTime.h"
#include "Tokenizer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace
{
    constexpr size_t kFieldCount = static_cast<size_t>(eField::kCount);

    // Insert into a sorted vector, ignoring a duplicate.
    inline void SortedInsert(std::vector<tJotID>& v, tJotID id)
    {
        const auto it = std::lower_bound(v.begin(), v.end(), id);
        if (it == v.end() || *it != id)
            v.insert(it, id);
    }

    inline void SortedErase(std::vector<tJotID>& v, tJotID id)
    {
        const auto it = std::lower_bound(v.begin(), v.end(), id);
        if (it != v.end() && *it == id)
            v.erase(it);
    }

    inline bool SortedContains(const std::vector<tJotID>& v, tJotID id)
    {
        return std::binary_search(v.begin(), v.end(), id);
    }

    // Intersects b into a, both sorted. Linear merge, no allocation.
    void IntersectInto(std::vector<tJotID>& a, const std::vector<tJotID>& b)
    {
        size_t nWrite = 0;
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size())
        {
            if (a[i] < b[j])       ++i;
            else if (b[j] < a[i])  ++j;
            else                   { a[nWrite++] = a[i]; ++i; ++j; }
        }
        a.resize(nWrite);
    }
}


JotStore::JotStore(const StoreConfig& config)
    : mConfig(config)
{
    // "user" must be editor id 0 - the record's default, and the value omitted from the wire. It
    // is interned first and unconditionally so that invariant holds even for an empty store.
    mEditors.Intern("user");
    mEditorPostings.resize(mEditors.Count());
}

//====================================================================================================
// Slot allocation
//
// A slot is a small dense handle for a live jot. Ids are 64-bit timestamps and therefore terrible
// array indices; slots exist so the scoring loop can index a flat float array instead of hashing an
// id for every posting it visits. Slots are recycled on delete, so the array stays sized to the live
// record count rather than to everything ever written.
//====================================================================================================

uint32_t JotStore::Locked_AllocSlot(tJotID id)
{
    if (!mFreeSlots.empty())
    {
        const uint32_t nSlot = mFreeSlots.back();
        mFreeSlots.pop_back();
        mSlotToID[nSlot] = id;
        return nSlot;
    }

    mSlotToID.push_back(id);
    return static_cast<uint32_t>(mSlotToID.size() - 1);
}

void JotStore::Locked_FreeSlot(uint32_t nSlot)
{
    if (nSlot >= mSlotToID.size() || mSlotToID[nSlot] == kInvalidJotID)
        return;
    mSlotToID[nSlot] = kInvalidJotID;
    mFreeSlots.push_back(nSlot);
}

//====================================================================================================
// ID allocation
//====================================================================================================

tJotID JotStore::Locked_NextID()
{
    const int64_t nNow = LOOMTIME::NowMicros();
    int64_t nPrev = mnLastID.load(std::memory_order_relaxed);
    int64_t nNext = 0;

    // The id is a timestamp, so it must stay unique and monotonic even when the wall clock does
    // not. Two writes inside the same microsecond, an NTP step backwards, or a VM resuming all land
    // here and all resolve the same way: take the next value after the last one issued.
    do
    {
        nNext = std::max(nNow, nPrev + 1);
    }
    while (!mnLastID.compare_exchange_weak(nPrev, nNext, std::memory_order_relaxed));

    return nNext;
}

//====================================================================================================
// Index maintenance
//
// Update is implemented as UnindexContent -> mutate -> IndexContent rather than as a fine-grained
// diff. It costs more per edit, and it buys the guarantee that the indexes cannot drift out of
// agreement with the records - which is the failure mode that produces queries silently missing
// jots you can see. Edits are rare by design; correctness here is worth far more than the cycles.
//====================================================================================================

void JotStore::Locked_AddPosting(tTermID term, eField field, const Posting& posting)
{
    auto& vLists = mPostings[static_cast<size_t>(field)];
    if (term >= vLists.size())
        vLists.resize(term + 1);

    tPostings& list = vLists[term];
    const auto it = std::lower_bound(list.begin(), list.end(), posting.mID,
        [](const Posting& p, tJotID v) { return p.mID < v; });

    if (it != list.end() && it->mID == posting.mID)
        *it = posting;
    else
        list.insert(it, posting);
}

void JotStore::Locked_RemovePosting(tTermID term, eField field, tJotID id)
{
    auto& vLists = mPostings[static_cast<size_t>(field)];
    if (term >= vLists.size())
        return;

    tPostings& list = vLists[term];
    const auto it = std::lower_bound(list.begin(), list.end(), id,
        [](const Posting& p, tJotID v) { return p.mID < v; });

    if (it != list.end() && it->mID == id)
        list.erase(it);
}

void JotStore::Locked_InsertTermLex(tTermID term)
{
    const std::string& sTerm = mTerms.Value(term);
    const auto it = std::lower_bound(mTermsByLex.begin(), mTermsByLex.end(), sTerm,
        [this](tTermID a, const std::string& b) { return mTerms.Value(a) < b; });

    if (it != mTermsByLex.end() && mTerms.Value(*it) == sTerm)
        return;

    mTermsByLex.insert(it, term);
}

void JotStore::Locked_IndexField(Jot& jot, const std::string& sText, eField field, uint32_t& outLen)
{
    outLen = 0;
    if (sText.empty())
        return;

    std::vector<std::string> vTerms;
    // Stopwords are dropped from the body but kept in the summary. A summary is one curated
    // sentence - throwing half of it away costs more than the postings it saves.
    if (field == eField::kBody)
        TOK::Tokenize(sText, vTerms);
    else
        TOK::TokenizeKeepStopwords(sText, vTerms);

    if (vTerms.empty())
        return;

    outLen = static_cast<uint32_t>(vTerms.size());
    mnTotalLen[static_cast<size_t>(field)] += vTerms.size();

    // Collapse to term frequencies before touching the index, so a word appearing five times
    // costs one posting insert rather than five.
    std::sort(vTerms.begin(), vTerms.end());
    size_t i = 0;
    while (i < vTerms.size())
    {
        size_t j = i;
        while (j < vTerms.size() && vTerms[j] == vTerms[i])
            ++j;

        const bool     bIsNew = (mTerms.Find(vTerms[i]) == kInvalidStrID);
        const tTermID  term   = mTerms.Intern(vTerms[i]);
        if (bIsNew)
            Locked_InsertTermLex(term);

        Posting posting;
        posting.mID      = jot.mID;
        posting.mnSlot   = jot.mnSlot;
        posting.mnTF     = static_cast<uint32_t>(j - i);
        posting.mnDocLen = outLen;
        Locked_AddPosting(term, field, posting);
        i = j;
    }
}

void JotStore::Locked_UnindexField(tJotID id, const std::string& sText, eField field)
{
    if (sText.empty())
        return;

    std::vector<std::string> vTerms;
    if (field == eField::kBody)
        TOK::Tokenize(sText, vTerms);
    else
        TOK::TokenizeKeepStopwords(sText, vTerms);

    if (vTerms.empty())
        return;

    const size_t nIdx = static_cast<size_t>(field);
    mnTotalLen[nIdx] = (mnTotalLen[nIdx] >= vTerms.size()) ? mnTotalLen[nIdx] - vTerms.size() : 0;

    std::sort(vTerms.begin(), vTerms.end());
    vTerms.erase(std::unique(vTerms.begin(), vTerms.end()), vTerms.end());

    for (const std::string& sTerm : vTerms)
    {
        const uint32_t term = mTerms.Find(sTerm);
        if (term != kInvalidStrID)
            Locked_RemovePosting(term, field, id);
    }

    // Term ids and mTermsByLex are deliberately NOT pruned when the last posting goes. Ids must
    // stay stable for the life of the process, and a term that appeared once will very likely
    // appear again. An empty posting list costs 24 bytes and answers "no hits" immediately.
}

void JotStore::Locked_IndexJot(Jot& jot)
{
    Locked_IndexField(jot, jot.msSummary, eField::kSummary, jot.mnSummaryLen);
    Locked_IndexField(jot, jot.msText,    eField::kBody,    jot.mnBodyLen);

    for (tTagID tag : jot.mTags)
    {
        if (tag >= mTagPostings.size())
            mTagPostings.resize(tag + 1);
        SortedInsert(mTagPostings[tag], jot.mID);
        mTags.AddRef(tag, jot.EffectiveUpdatedUS());
    }

    if (jot.mEditor >= mEditorPostings.size())
        mEditorPostings.resize(jot.mEditor + 1);
    SortedInsert(mEditorPostings[jot.mEditor], jot.mID);

    Locked_AddBacklinks(jot.mID, jot.mLinks);

    for (const std::string& sPending : jot.mPendingLinks)
        SortedInsert(mPendingLinks[sPending], jot.mID);
}

void JotStore::Locked_UnindexJot(const Jot& jot)
{
    Locked_UnindexField(jot.mID, jot.msSummary, eField::kSummary);
    Locked_UnindexField(jot.mID, jot.msText,    eField::kBody);

    for (tTagID tag : jot.mTags)
    {
        if (tag < mTagPostings.size())
            SortedErase(mTagPostings[tag], jot.mID);
        mTags.Release(tag);
    }

    if (jot.mEditor < mEditorPostings.size())
        SortedErase(mEditorPostings[jot.mEditor], jot.mID);

    Locked_RemoveBacklinks(jot.mID, jot.mLinks);

    for (const std::string& sPending : jot.mPendingLinks)
    {
        const auto it = mPendingLinks.find(sPending);
        if (it != mPendingLinks.end())
        {
            SortedErase(it->second, jot.mID);
            if (it->second.empty())
                mPendingLinks.erase(it);
        }
    }
}

void JotStore::Locked_AddBacklinks(tJotID nFrom, const std::vector<tJotID>& vTo)
{
    for (tJotID target : vTo)
        SortedInsert(mBacklinks[target], nFrom);
}

void JotStore::Locked_RemoveBacklinks(tJotID nFrom, const std::vector<tJotID>& vTo)
{
    for (tJotID target : vTo)
    {
        const auto it = mBacklinks.find(target);
        if (it != mBacklinks.end())
        {
            SortedErase(it->second, nFrom);
            if (it->second.empty())
                mBacklinks.erase(it);
        }
    }
}

//====================================================================================================
// Links
//====================================================================================================

void JotStore::Locked_ResolveLinks(const std::vector<std::string>& vSpecs, tJotID nSelfID,
                                   std::vector<tJotID>& outLinks, std::vector<std::string>& outPending)
{
    outLinks.clear();
    outPending.clear();

    for (const std::string& sSpec : vSpecs)
    {
        if (sSpec.empty())
            continue;

        // A run of digits long enough to be an id is treated as one. Slugs never look like this -
        // they are kebab-case words - so the ambiguity is theoretical.
        bool bAllDigits = true;
        for (char c : sSpec)
        {
            if (c < '0' || c > '9') { bAllDigits = false; break; }
        }

        if (bAllDigits && sSpec.size() >= 10)
        {
            const tJotID target = std::strtoll(sSpec.c_str(), nullptr, 10);
            if (target != nSelfID && mJots.count(target) != 0)
                SortedInsert(outLinks, target);
            continue;
        }

        const auto it = mNameIndex.find(sSpec);
        if (it != mNameIndex.end())
        {
            if (it->second != nSelfID)
                SortedInsert(outLinks, it->second);
        }
        else
        {
            // Unresolved slugs are parked, not rejected. The memory store links liberally to
            // memories not yet written; a dangling [[name]] marks intent, and dropping it would
            // quietly destroy that.
            if (std::find(outPending.begin(), outPending.end(), sSpec) == outPending.end())
                outPending.push_back(sSpec);
        }
    }
}

void JotStore::Locked_PromotePendingLinks(const std::string& sName, tJotID nTargetID)
{
    const auto it = mPendingLinks.find(sName);
    if (it == mPendingLinks.end())
        return;

    const std::vector<tJotID> vWaiting = it->second;
    mPendingLinks.erase(it);

    for (tJotID waiter : vWaiting)
    {
        const auto jt = mJots.find(waiter);
        if (jt == mJots.end())
            continue;

        Jot& jot = jt->second;
        auto pending = std::find(jot.mPendingLinks.begin(), jot.mPendingLinks.end(), sName);
        if (pending != jot.mPendingLinks.end())
            jot.mPendingLinks.erase(pending);

        if (waiter != nTargetID)
        {
            SortedInsert(jot.mLinks, nTargetID);
            SortedInsert(mBacklinks[nTargetID], waiter);
        }
    }
}

//====================================================================================================
// Validation and field application
//====================================================================================================

std::error_code JotStore::Locked_Validate(const JotInput& input) const
{
    if (input.msText && input.msText->size() > mConfig.mnMaxTextBytes)
        return MakeLoomError(eLoomErr::kTooLarge);
    if (input.msSummary && input.msSummary->size() > mConfig.mnMaxSummaryBytes)
        return MakeLoomError(eLoomErr::kTooLarge);
    if (input.msName && input.msName->size() > mConfig.mnMaxNameBytes)
        return MakeLoomError(eLoomErr::kTooLarge);
    if (input.mTags && input.mTags->size() > mConfig.mnMaxTags)
        return MakeLoomError(eLoomErr::kTooLarge);
    if (input.mLinks && input.mLinks->size() > mConfig.mnMaxLinks)
        return MakeLoomError(eLoomErr::kTooLarge);

    return LoomOK();
}

std::error_code JotStore::Locked_Apply(Jot& jot, const JotInput& input, bool bCreating,
                                       std::vector<TagSuggestion>& outSuggestions)
{
    (void)bCreating;

    if (input.msName)
        jot.msName = TagRegistry::Normalize(*input.msName);   // slugs and tags share a shape
    if (input.msSummary)
        jot.msSummary = *input.msSummary;
    if (input.msText)
        jot.msText = *input.msText;

    if (input.msEditor)
    {
        const std::string sEditor = TOK::Fold(*input.msEditor);
        jot.mEditor = sEditor.empty() ? kDefaultEditor : mEditors.Intern(sEditor);
        if (jot.mEditor >= mEditorPostings.size())
            mEditorPostings.resize(jot.mEditor + 1);
    }

    if (input.mTags)
    {
        jot.mTags.clear();
        for (const std::string& sRaw : *input.mTags)
        {
            const std::string sTag = TagRegistry::Normalize(sRaw);
            if (sTag.empty())
                continue;

            // Ask before interning. Once the tag exists in the registry it is no longer "new", so
            // the suggestion would vanish - and the whole mechanism depends on the warning
            // reaching the agent on the write that introduces the drift.
            std::vector<TagSuggestion> vFor;
            mTags.Suggest(sTag, vFor);
            for (TagSuggestion& s : vFor)
                outSuggestions.push_back(std::move(s));

            const tTagID id = mTags.Intern(sTag);
            if (id == kInvalidTagID)
                continue;
            if (id >= mTagPostings.size())
                mTagPostings.resize(id + 1);

            if (std::find(jot.mTags.begin(), jot.mTags.end(), id) == jot.mTags.end())
                jot.mTags.push_back(id);
        }
        std::sort(jot.mTags.begin(), jot.mTags.end());
    }

    if (input.mLinks)
        Locked_ResolveLinks(*input.mLinks, jot.mID, jot.mLinks, jot.mPendingLinks);

    return LoomOK();
}

//====================================================================================================
// Mutations
//====================================================================================================

std::error_code JotStore::Add(const JotInput& input, MutationResult& outResult)
{
    outResult = MutationResult();

    std::unique_lock lock(mLock);

    if (std::error_code ec = Locked_Validate(input))
        return ec;

    // A jot with no text, no summary and no name is not a memory, it is an accident.
    const bool bHasContent = (input.msText && !input.msText->empty())
                          || (input.msSummary && !input.msSummary->empty())
                          || (input.msName && !input.msName->empty());
    if (!bHasContent)
        return MakeLoomError(eLoomErr::kEmptyJot);

    std::string sName;
    if (input.msName)
    {
        sName = TagRegistry::Normalize(*input.msName);
        if (!sName.empty() && mNameIndex.count(sName) != 0)
            return MakeLoomError(eLoomErr::kNameInUse);
    }

    Jot jot;
    jot.mID = Locked_NextID();

    if (std::error_code ec = Locked_Apply(jot, input, true, outResult.mSuggestions))
        return ec;

    const tJotID id = jot.mID;
    auto [it, bInserted] = mJots.emplace(id, std::move(jot));
    if (!bInserted)
        return MakeLoomError(eLoomErr::kConflict);   // cannot happen: ids are unique by construction

    Jot& stored = it->second;

    stored.mnSlot = Locked_AllocSlot(id);   // must precede indexing - postings carry the slot
    Locked_IndexJot(stored);
    mChrono.push_back(id);   // ids are monotonic, so appending keeps mChrono sorted
    if (!stored.msName.empty())
    {
        mNameIndex.emplace(stored.msName, id);
        Locked_PromotePendingLinks(stored.msName, id);
    }

    Locked_JournalPut(stored);

    ++mnMutations;
    outResult.mJot      = stored;
    outResult.mbCreated = true;
    return LoomOK();
}

std::error_code JotStore::Update(tJotID id, const JotInput& patch, int64_t nExpectUpdatedUS,
                                 MutationResult& outResult)
{
    outResult = MutationResult();

    std::unique_lock lock(mLock);

    const auto it = mJots.find(id);
    if (it == mJots.end())
        return MakeLoomError(eLoomErr::kNotFound);

    Jot& jot = it->second;

    // Optimistic concurrency. Two agents editing the same memory from stale copies is the exact
    // scenario this service invites, so the default has to be "tell me", not "last one wins".
    if (nExpectUpdatedUS != 0 && jot.EffectiveUpdatedUS() != nExpectUpdatedUS)
        return MakeLoomError(eLoomErr::kConflict);

    if (std::error_code ec = Locked_Validate(patch))
        return ec;

    // Everything that can fail is checked before a single index is touched.
    if (patch.msName)
    {
        const std::string sNewName = TagRegistry::Normalize(*patch.msName);
        if (!sNewName.empty() && sNewName != jot.msName && mNameIndex.count(sNewName) != 0)
            return MakeLoomError(eLoomErr::kNameInUse);
    }

    const std::string sOldName = jot.msName;

    // Kept so the applied result can be compared against what was already there. See the no-change
    // check below - this copy is the price of not writing history nobody made.
    const Jot before = jot;

    Locked_UnindexJot(jot);
    if (std::error_code ec = Locked_Apply(jot, patch, false, outResult.mSuggestions))
    {
        // Locked_Apply cannot fail once validation has passed, but if that ever changes, leaving
        // the jot unindexed would be silent corruption. Re-index before returning.
        Locked_IndexJot(jot);
        return ec;
    }

    // A PATCH THAT CHANGES NOTHING IS NOT A MUTATION.
    //
    // Front ends send the whole record, or a whole tag array, rather than a diff - the dashboard's
    // Save, its snooze buttons, and any agent re-asserting a state it already believes in. Treating
    // those as writes had three costs, and the third is what made it visible: it bumped `updated`,
    // so every other agent's expect_updated token was invalidated for no reason; it put a line in
    // the WAL; and it put a point in the history log, which is meant to answer "what changed" and
    // was instead answering "what was submitted". Eight of the first fourteen entries recorded on
    // the live store changed no field at all.
    //
    // The record is still returned and the call still succeeds: the caller asked for a state and
    // that is the state. Only the write is skipped, and mbNoChange says so for anyone who cares.
    if (Locked_SameContent(before, jot))
    {
        Locked_IndexJot(jot);
        outResult.mJot       = jot;
        outResult.mbCreated  = false;
        outResult.mbNoChange = true;
        return LoomOK();
    }

    jot.mnUpdatedUS = LOOMTIME::NowMicros();
    Locked_IndexJot(jot);

    if (sOldName != jot.msName)
    {
        if (!sOldName.empty())
            mNameIndex.erase(sOldName);
        if (!jot.msName.empty())
        {
            mNameIndex.emplace(jot.msName, id);
            Locked_PromotePendingLinks(jot.msName, id);
        }
    }

    Locked_JournalPut(jot);

    ++mnMutations;
    outResult.mJot      = jot;
    outResult.mbCreated = false;
    return LoomOK();
}

std::error_code JotStore::Remove(tJotID id)
{
    std::unique_lock lock(mLock);

    const auto it = mJots.find(id);
    if (it == mJots.end())
        return MakeLoomError(eLoomErr::kNotFound);

    Jot& jot = it->second;

    Locked_UnindexJot(jot);

    if (!jot.msName.empty())
        mNameIndex.erase(jot.msName);

    SortedErase(mChrono, id);

    // Anything that linked TO this jot now has a dangling reference. Demote it back to a pending
    // link keyed by the dead jot's slug if it had one, so re-creating the memory restores the
    // graph; otherwise just drop the edge.
    const auto back = mBacklinks.find(id);
    if (back != mBacklinks.end())
    {
        const std::vector<tJotID> vSources = back->second;
        const std::string sName = jot.msName;
        mBacklinks.erase(back);

        for (tJotID source : vSources)
        {
            const auto st = mJots.find(source);
            if (st == mJots.end())
                continue;

            SortedErase(st->second.mLinks, id);
            if (!sName.empty())
            {
                st->second.mPendingLinks.push_back(sName);
                SortedInsert(mPendingLinks[sName], source);
            }
        }
    }

    if (mpJournal)
        mpJournal->OnDelete(id);

    Locked_FreeSlot(jot.mnSlot);
    mJots.erase(it);
    ++mnMutations;
    return LoomOK();
}

std::error_code JotStore::MergeTags(const std::vector<std::string>& vFrom, const std::string& sTo,
                                    size_t& outJotsChanged)
{
    outJotsChanged = 0;

    const std::string sTarget = TagRegistry::Normalize(sTo);
    if (sTarget.empty() || vFrom.empty())
        return MakeLoomError(eLoomErr::kInvalidArgument);

    std::unique_lock lock(mLock);

    std::vector<tTagID> vSourceIDs;
    for (const std::string& sRaw : vFrom)
    {
        const tTagID id = mTags.Find(TagRegistry::Normalize(sRaw));
        if (id != kInvalidTagID)
            vSourceIDs.push_back(id);
    }
    if (vSourceIDs.empty())
        return MakeLoomError(eLoomErr::kNotFound);

    const tTagID nTargetID = mTags.Intern(sTarget);
    if (nTargetID >= mTagPostings.size())
        mTagPostings.resize(nTargetID + 1);

    // Collect first, mutate second. Rewriting tags reindexes each jot, which reorders the very
    // posting lists being iterated.
    std::vector<tJotID> vAffected;
    for (tTagID src : vSourceIDs)
    {
        if (src == nTargetID || src >= mTagPostings.size())
            continue;
        for (tJotID id : mTagPostings[src])
            SortedInsert(vAffected, id);
    }

    for (tJotID id : vAffected)
    {
        const auto it = mJots.find(id);
        if (it == mJots.end())
            continue;

        Jot& jot = it->second;
        Locked_UnindexJot(jot);

        std::vector<tTagID> vNew;
        vNew.reserve(jot.mTags.size());
        for (tTagID tag : jot.mTags)
        {
            const bool bIsSource = std::find(vSourceIDs.begin(), vSourceIDs.end(), tag) != vSourceIDs.end();
            const tTagID mapped = bIsSource ? nTargetID : tag;
            if (std::find(vNew.begin(), vNew.end(), mapped) == vNew.end())
                vNew.push_back(mapped);
        }
        std::sort(vNew.begin(), vNew.end());
        jot.mTags = std::move(vNew);
        jot.mnUpdatedUS = LOOMTIME::NowMicros();

        Locked_IndexJot(jot);
        Locked_JournalPut(jot);
        ++outJotsChanged;
    }

    ++mnMutations;
    return LoomOK();
}

//====================================================================================================
// Bulk load
//====================================================================================================

std::error_code JotStore::LoadFlatBatch(std::vector<FlatJot>& vFlat, size_t& outLoaded, bool bJournal)
{
    outLoaded = 0;

    // Interning mutates the tag and editor tables, so it has to happen under the write lock. That
    // is the whole reason this is a store method rather than a free conversion function the caller
    // could run beforehand.
    std::unique_lock lock(mLock);

    std::vector<Jot> vJots;
    vJots.reserve(vFlat.size());

    for (FlatJot& flat : vFlat)
    {
        Jot jot;
        jot.mID          = flat.mID;
        jot.mnUpdatedUS  = flat.mnUpdatedUS;
        jot.msName       = flat.msName;
        jot.msSummary    = std::move(flat.msSummary);
        jot.msText       = std::move(flat.msText);
        jot.mLinks       = std::move(flat.mLinks);
        jot.mPendingLinks = std::move(flat.mPendingLinks);

        const std::string sEditor = TOK::Fold(flat.msEditor);
        jot.mEditor = sEditor.empty() ? kDefaultEditor : mEditors.Intern(sEditor);
        if (jot.mEditor >= mEditorPostings.size())
            mEditorPostings.resize(jot.mEditor + 1);

        for (const std::string& sRaw : flat.mTags)
        {
            const std::string sTag = TagRegistry::Normalize(sRaw);
            if (sTag.empty())
                continue;
            const tTagID id = mTags.Intern(sTag);
            if (id == kInvalidTagID)
                continue;
            if (id >= mTagPostings.size())
                mTagPostings.resize(id + 1);
            if (std::find(jot.mTags.begin(), jot.mTags.end(), id) == jot.mTags.end())
                jot.mTags.push_back(id);
        }
        std::sort(jot.mTags.begin(), jot.mTags.end());

        vJots.push_back(std::move(jot));
    }

    return Locked_LoadBatch(vJots, outLoaded, bJournal);
}

std::error_code JotStore::LoadBatch(std::vector<Jot>& vJots, size_t& outLoaded)
{
    std::unique_lock lock(mLock);
    return Locked_LoadBatch(vJots, outLoaded, false);
}

std::error_code JotStore::Locked_LoadBatch(std::vector<Jot>& vJots, size_t& outLoaded, bool bJournal)
{
    outLoaded = 0;

    for (Jot& incoming : vJots)
    {
        if (incoming.mID == kInvalidJotID)
            continue;

        // A replayed WAL legitimately contains several versions of the same jot; the later one
        // wins, which is what makes replay idempotent.
        const auto existing = mJots.find(incoming.mID);
        if (existing != mJots.end())
        {
            Locked_UnindexJot(existing->second);
            if (!existing->second.msName.empty())
                mNameIndex.erase(existing->second.msName);
            Locked_FreeSlot(existing->second.mnSlot);
            mJots.erase(existing);
            SortedErase(mChrono, incoming.mID);
        }

        const tJotID id = incoming.mID;
        auto [it, bInserted] = mJots.emplace(id, std::move(incoming));
        if (!bInserted)
            continue;

        Jot& stored = it->second;
        stored.mnSlot = Locked_AllocSlot(id);
        Locked_IndexJot(stored);
        SortedInsert(mChrono, id);

        if (!stored.msName.empty())
        {
            mNameIndex.emplace(stored.msName, id);
            Locked_PromotePendingLinks(stored.msName, id);
        }

        // Keep the allocator ahead of every id ever loaded, or a restart would start reissuing ids
        // that already exist.
        int64_t nPrev = mnLastID.load(std::memory_order_relaxed);
        while (nPrev < id && !mnLastID.compare_exchange_weak(nPrev, id, std::memory_order_relaxed))
        {
        }

        if (bJournal)
            Locked_JournalPut(stored);

        ++outLoaded;
    }

    return LoomOK();
}

//====================================================================================================
// Reads
//====================================================================================================

bool JotStore::Get(tJotID id, Jot& outJot) const
{
    std::shared_lock lock(mLock);
    const auto it = mJots.find(id);
    if (it == mJots.end())
        return false;
    outJot = it->second;
    return true;
}

bool JotStore::GetByName(const std::string& sName, Jot& outJot) const
{
    const std::string sNorm = TagRegistry::Normalize(sName);

    std::shared_lock lock(mLock);
    const auto it = mNameIndex.find(sNorm);
    if (it == mNameIndex.end())
        return false;

    const auto jt = mJots.find(it->second);
    if (jt == mJots.end())
        return false;

    outJot = jt->second;
    return true;
}

void JotStore::GetMany(const std::vector<tJotID>& vIDs, std::vector<Jot>& outJots) const
{
    outJots.clear();
    outJots.reserve(vIDs.size());

    std::shared_lock lock(mLock);
    for (tJotID id : vIDs)
    {
        const auto it = mJots.find(id);
        if (it != mJots.end())
            outJots.push_back(it->second);
    }
}

size_t JotStore::Size() const
{
    std::shared_lock lock(mLock);
    return mJots.size();
}

StoreStats JotStore::GetStats() const
{
    std::shared_lock lock(mLock);

    StoreStats s;
    s.mnJots         = mJots.size();
    s.mnNamed        = mNameIndex.size();
    s.mnTags         = mTags.VocabularySize();
    s.mnTerms        = mTerms.Count();
    s.mnPendingLinks = mPendingLinks.size();
    s.mnEditors      = mEditors.Count();
    s.mnMutations    = mnMutations;
    if (!mChrono.empty())
    {
        s.mnOldestUS = mChrono.front();
        s.mnNewestUS = mChrono.back();
    }
    return s;
}

void JotStore::Locked_Flatten(const Jot& jot, FlatJot& outFlat) const
{
    outFlat.mID          = jot.mID;
    outFlat.mnUpdatedUS  = jot.mnUpdatedUS;
    outFlat.msName       = jot.msName;
    outFlat.msSummary    = jot.msSummary;
    outFlat.msText       = jot.msText;
    outFlat.mLinks       = jot.mLinks;
    outFlat.mPendingLinks = jot.mPendingLinks;

    // The default editor stays empty rather than becoming the literal "user", so the omit-empty
    // rule in the codec has one thing to test instead of two.
    outFlat.msEditor = (jot.mEditor == kDefaultEditor) ? std::string() : mEditors.Value(jot.mEditor);

    outFlat.mTags.clear();
    outFlat.mTags.reserve(jot.mTags.size());
    for (tTagID id : jot.mTags)
        outFlat.mTags.push_back(mTags.Name(id));
}

// Journals one record while the caller still holds the write lock. Every mutation path routes
// through here so no path can forget, and so ordering is guaranteed by the lock rather than by
// discipline.
bool JotStore::Locked_SameContent(const Jot& a, const Jot& b)
{
    // mTags and mLinks are kept sorted by Locked_Apply, so element-wise equality is the right
    // comparison and not an accident of insertion order. mPendingLinks is not sorted, but it is
    // derived from the text in a stable order, so the same text yields the same vector.
    return a.mEditor      == b.mEditor
        && a.msName       == b.msName
        && a.msSummary    == b.msSummary
        && a.msText       == b.msText
        && a.mTags        == b.mTags
        && a.mLinks       == b.mLinks
        && a.mPendingLinks == b.mPendingLinks;
}

void JotStore::Locked_JournalPut(const Jot& jot)
{
    if (!mpJournal)
        return;
    FlatJot flat;
    Locked_Flatten(jot, flat);
    mpJournal->OnPut(flat);
}

bool JotStore::Flatten(tJotID id, FlatJot& outFlat) const
{
    std::shared_lock lock(mLock);
    const auto it = mJots.find(id);
    if (it == mJots.end())
        return false;
    Locked_Flatten(it->second, outFlat);
    return true;
}

void JotStore::FlattenAll(std::vector<FlatJot>& outFlat) const
{
    std::shared_lock lock(mLock);

    outFlat.clear();
    outFlat.reserve(mJots.size());

    // Walk mChrono rather than mJots so the file comes out oldest-first. A snapshot that is
    // ordered is greppable, diffable between versions, and replays in creation order.
    for (tJotID id : mChrono)
    {
        const auto it = mJots.find(id);
        if (it == mJots.end())
            continue;
        FlatJot flat;
        Locked_Flatten(it->second, flat);
        outFlat.push_back(std::move(flat));
    }
}

void JotStore::SnapshotNames(NameTables& outTables) const
{
    std::shared_lock lock(mLock);

    outTables.mTags.resize(mTags.Count());
    for (uint32_t i = 0; i < mTags.Count(); ++i)
        outTables.mTags[i] = mTags.Name(i);

    outTables.mEditors.resize(mEditors.Count());
    for (uint32_t i = 0; i < mEditors.Count(); ++i)
        outTables.mEditors[i] = mEditors.Value(i);
}

void JotStore::ListTags(std::vector<TagStat>& outStats, bool bIncludeReserved) const
{
    std::shared_lock lock(mLock);
    mTags.List(outStats, bIncludeReserved);
}

void JotStore::TagClusters(std::vector<TagCluster>& outClusters) const
{
    std::shared_lock lock(mLock);
    mTags.Clusters(outClusters);
}

void JotStore::SuggestForTags(const std::vector<std::string>& vTags,
                              std::vector<TagSuggestion>& outSuggestions) const
{
    outSuggestions.clear();

    std::shared_lock lock(mLock);
    for (const std::string& sRaw : vTags)
    {
        const std::string sTag = TagRegistry::Normalize(sRaw);
        if (sTag.empty())
            continue;

        std::vector<TagSuggestion> vFor;
        mTags.Suggest(sTag, vFor);
        for (TagSuggestion& s : vFor)
            outSuggestions.push_back(std::move(s));
    }
}

void JotStore::Neighborhood(tJotID id, size_t nDepth, std::vector<tJotID>& outIDs) const
{
    outIDs.clear();
    if (nDepth == 0)
        return;

    std::shared_lock lock(mLock);
    if (mJots.count(id) == 0)
        return;

    std::vector<tJotID> vSeen{ id };
    std::vector<tJotID> vFrontier{ id };

    // Breadth-first so nearer jots come out first - a caller truncating the list keeps the most
    // relevant half rather than an arbitrary one.
    for (size_t nHop = 0; nHop < nDepth && !vFrontier.empty(); ++nHop)
    {
        std::vector<tJotID> vNext;

        for (tJotID current : vFrontier)
        {
            const auto it = mJots.find(current);
            if (it != mJots.end())
            {
                for (tJotID target : it->second.mLinks)
                {
                    if (!SortedContains(vSeen, target))
                    {
                        SortedInsert(vSeen, target);
                        vNext.push_back(target);
                        outIDs.push_back(target);
                    }
                }
            }

            const auto back = mBacklinks.find(current);
            if (back != mBacklinks.end())
            {
                for (tJotID source : back->second)
                {
                    if (!SortedContains(vSeen, source))
                    {
                        SortedInsert(vSeen, source);
                        vNext.push_back(source);
                        outIDs.push_back(source);
                    }
                }
            }
        }

        vFrontier.swap(vNext);
    }
}

//====================================================================================================
// Search
//====================================================================================================

void JotStore::Locked_ExpandPrefix(std::string_view sPrefix, std::vector<tTermID>& outTerms) const
{
    outTerms.clear();
    if (sPrefix.empty())
        return;

    const auto begin = std::lower_bound(mTermsByLex.begin(), mTermsByLex.end(), sPrefix,
        [this](tTermID a, std::string_view b) { return std::string_view(mTerms.Value(a)) < b; });

    for (auto it = begin; it != mTermsByLex.end(); ++it)
    {
        const std::string& sTerm = mTerms.Value(*it);
        if (sTerm.size() < sPrefix.size() || sTerm.compare(0, sPrefix.size(), sPrefix) != 0)
            break;   // the list is sorted, so the first miss ends the range

        outTerms.push_back(*it);
        if (outTerms.size() >= RANK::kMaxPrefixExpansion)
            break;
    }
}

bool JotStore::Locked_BuildCandidates(const Query& q, std::vector<tJotID>& outCandidates,
                                      bool& outbUnbounded) const
{
    outCandidates.clear();
    outbUnbounded = true;

    // Positive filters only. Negatives and the exact time bounds are applied later as a predicate
    // over survivors, so correctness never depends on this narrowing being complete.
    for (const std::string& sRaw : q.mTags)
    {
        const tTagID tag = mTags.Find(TagRegistry::Normalize(sRaw));
        if (tag == kInvalidTagID || tag >= mTagPostings.size() || mTagPostings[tag].empty())
            return false;   // an AND term nothing carries - the whole query is empty

        if (outbUnbounded)
        {
            outCandidates  = mTagPostings[tag];
            outbUnbounded  = false;
        }
        else
        {
            IntersectInto(outCandidates, mTagPostings[tag]);
        }

        if (outCandidates.empty())
            return false;
    }

    if (!q.msEditor.empty())
    {
        const uint32_t editor = mEditors.Find(TOK::Fold(q.msEditor));
        if (editor == kInvalidStrID || editor >= mEditorPostings.size() || mEditorPostings[editor].empty())
            return false;

        if (outbUnbounded)
        {
            outCandidates = mEditorPostings[editor];
            outbUnbounded = false;
        }
        else
        {
            IntersectInto(outCandidates, mEditorPostings[editor]);
        }

        if (outCandidates.empty())
            return false;
    }

    if (q.mnLinkedTo != kInvalidJotID)
    {
        std::vector<tJotID> vLinked;
        const auto it = mJots.find(q.mnLinkedTo);
        if (it != mJots.end())
        {
            for (tJotID target : it->second.mLinks)
                SortedInsert(vLinked, target);
        }
        const auto back = mBacklinks.find(q.mnLinkedTo);
        if (back != mBacklinks.end())
        {
            for (tJotID source : back->second)
                SortedInsert(vLinked, source);
        }

        if (vLinked.empty())
            return false;

        if (outbUnbounded)
        {
            outCandidates = std::move(vLinked);
            outbUnbounded = false;
        }
        else
        {
            IntersectInto(outCandidates, vLinked);
        }

        if (outCandidates.empty())
            return false;
    }

    // With no other positive filter, a time range is itself the cheapest possible narrowing: two
    // binary searches over mChrono and the slice between them.
    if (outbUnbounded && (q.mnSince != 0 || q.mnUntil != 0))
    {
        const auto begin = q.mnSince != 0
            ? std::lower_bound(mChrono.begin(), mChrono.end(), q.mnSince)
            : mChrono.begin();
        const auto end = q.mnUntil != 0
            ? std::upper_bound(mChrono.begin(), mChrono.end(), q.mnUntil)
            : mChrono.end();

        if (begin >= end)
            return false;

        outCandidates.assign(begin, end);
        outbUnbounded = false;
    }

    return true;
}

float JotStore::Locked_RecencyMultiplier(tJotID id, int64_t nNowUS) const
{
    const int64_t nAge = nNowUS > id ? nNowUS - id : 0;
    const float fDecay = std::exp(-static_cast<float>(nAge) / static_cast<float>(RANK::kRecencyHalfLifeUS));
    return 1.0f + RANK::kRecencyWeight * fDecay;
}

void JotStore::Locked_ScoreText(const Query& q, const std::vector<tJotID>& vCandidates,
                                bool bUnbounded, std::vector<SearchHit>& outHits) const
{
    outHits.clear();

    // Reused across queries on this thread. Sizing it per call would memset the whole slot space
    // for every query - which at 100k records is a 400 KB clear to produce twenty results. Instead
    // it grows monotonically and only the slots actually touched are reset afterwards, so the cost
    // is proportional to hits rather than to corpus size.
    struct Scratch
    {
        std::vector<float>    mScores;
        std::vector<uint32_t> mTouched;
    };
    thread_local Scratch tScratch;

    if (tScratch.mScores.size() < mSlotToID.size())
        tScratch.mScores.resize(mSlotToID.size(), 0.0f);
    tScratch.mTouched.clear();

    std::vector<float>&    vScores  = tScratch.mScores;
    std::vector<uint32_t>& vTouched = tScratch.mTouched;

    // Scores are strictly positive, so zero doubles as "not yet seen" and saves a second array.
    const auto Accumulate = [&](uint32_t nSlot, float fDelta)
    {
        if (vScores[nSlot] == 0.0f)
            vTouched.push_back(nSlot);
        vScores[nSlot] += fDelta;
    };

    std::vector<std::string> vQueryTerms;
    TOK::TokenizeKeepStopwords(q.msText, vQueryTerms);
    if (vQueryTerms.empty())
        return;

    const float fN = static_cast<float>(std::max<size_t>(mJots.size(), 1));

    float fAvgLen[kFieldCount];
    for (size_t f = 0; f < kFieldCount; ++f)
        fAvgLen[f] = std::max(1.0f, static_cast<float>(mnTotalLen[f]) / fN);

    // Query terms that are also tags. A deliberate tag is far stronger evidence than the same word
    // occurring in prose, so this is scored separately below.
    std::vector<tTagID> vQueryTags;
    for (const std::string& sTerm : vQueryTerms)
    {
        const tTagID tag = mTags.Find(sTerm);
        if (tag != kInvalidTagID)
            vQueryTags.push_back(tag);
    }

    for (size_t nTermIdx = 0; nTermIdx < vQueryTerms.size(); ++nTermIdx)
    {
        const bool bIsLast   = (nTermIdx + 1 == vQueryTerms.size());
        const bool bAsPrefix = q.mbPrefixLastTerm && bIsLast;

        std::vector<tTermID> vTerms;
        if (bAsPrefix)
        {
            Locked_ExpandPrefix(vQueryTerms[nTermIdx], vTerms);
        }
        else
        {
            const uint32_t term = mTerms.Find(vQueryTerms[nTermIdx]);
            if (term != kInvalidStrID)
                vTerms.push_back(term);
        }

        for (tTermID term : vTerms)
        {
            const float fExpansionWeight = bAsPrefix ? RANK::kPrefixPenalty : 1.0f;

            for (size_t f = 0; f < kFieldCount; ++f)
            {
                const auto& vLists = mPostings[f];
                if (term >= vLists.size())
                    continue;

                const tPostings& list = vLists[term];
                if (list.empty())
                    continue;

                const float fDF  = static_cast<float>(list.size());
                const float fIDF = std::log(1.0f + (fN - fDF + 0.5f) / (fDF + 0.5f));
                const float fFieldWeight =
                    (static_cast<eField>(f) == eField::kSummary) ? RANK::kSummaryWeight : 1.0f;

                // Linear streaming scan. Everything BM25 needs is in the posting, so this touches
                // no other cache line - which is the entire point of the Posting layout.
                for (const Posting& p : list)
                {
                    if (!bUnbounded && !SortedContains(vCandidates, p.mID))
                        continue;

                    const float fDocLen = static_cast<float>(p.mnDocLen);
                    const float fTF     = static_cast<float>(p.mnTF);
                    const float fNorm = (fTF * (RANK::kBM25_K1 + 1.0f))
                        / (fTF + RANK::kBM25_K1 * (1.0f - RANK::kBM25_B
                            + RANK::kBM25_B * fDocLen / fAvgLen[f]));

                    Accumulate(p.mnSlot, fIDF * fNorm * fFieldWeight * fExpansionWeight);
                }
            }
        }
    }

    // Tag matches are scored per query term, against the same idf as any other evidence, and NOT
    // as a flat bonus. A flat bonus lets a jot matching one query term via a tag outrank a jot
    // matching every term in its summary - which is exactly backwards. Scoring per term means
    // covering more of the query always wins, while a tag still counts for more than prose.
    //
    // This also seeds jots with no text hit at all: outScores[id] default-constructs to zero, so a
    // jot tagged with the query term is found even when it never says the word.
    for (tTagID tag : vQueryTags)
    {
        if (tag >= mTagPostings.size())
            continue;

        const std::vector<tJotID>& list = mTagPostings[tag];
        if (list.empty())
            continue;

        const float fDF  = static_cast<float>(list.size());
        const float fIDF = std::log(1.0f + (fN - fDF + 0.5f) / (fDF + 0.5f));

        for (tJotID id : list)
        {
            if (!bUnbounded && !SortedContains(vCandidates, id))
                continue;

            const auto jt = mJots.find(id);
            if (jt != mJots.end())
                Accumulate(jt->second.mnSlot, fIDF * RANK::kTagMatchBoost);
        }
    }

    // Harvest, then reset only what was touched so the scratch is clean for the next query.
    outHits.reserve(vTouched.size());
    for (uint32_t nSlot : vTouched)
    {
        const tJotID id = nSlot < mSlotToID.size() ? mSlotToID[nSlot] : kInvalidJotID;
        if (id != kInvalidJotID)
            outHits.push_back(SearchHit{ id, vScores[nSlot] });
        vScores[nSlot] = 0.0f;
    }
}

void JotStore::Search(const Query& q, SearchResults& outResults) const
{
    outResults.mHits.clear();
    outResults.mnMatched   = 0;
    outResults.mbTruncated = false;

    std::shared_lock lock(mLock);

    // An exact slug is a key lookup, not a search.
    if (!q.msName.empty())
    {
        const auto it = mNameIndex.find(TagRegistry::Normalize(q.msName));
        if (it != mNameIndex.end())
        {
            outResults.mHits.push_back(SearchHit{ it->second, 1.0f });
            outResults.mnMatched = 1;
        }
        return;
    }

    std::vector<tJotID> vCandidates;
    bool bUnbounded = true;
    if (!Locked_BuildCandidates(q, vCandidates, bUnbounded))
        return;

    // Resolve the negative tags once rather than per jot.
    std::vector<tTagID> vNotTags;
    for (const std::string& sRaw : q.mNotTags)
    {
        const tTagID tag = mTags.Find(TagRegistry::Normalize(sRaw));
        if (tag != kInvalidTagID)
            vNotTags.push_back(tag);
    }

    // The exact predicate. Candidate building above is only a narrowing optimization; this is what
    // actually decides membership, so a bug there can cost speed but never correctness.
    const auto Passes = [&](const Jot& jot) -> bool
    {
        if (q.mnSince != 0 && jot.mID < q.mnSince)
            return false;
        if (q.mnUntil != 0 && jot.mID > q.mnUntil)
            return false;

        for (tTagID tag : vNotTags)
        {
            if (std::find(jot.mTags.begin(), jot.mTags.end(), tag) != jot.mTags.end())
                return false;
        }
        return true;
    };

    const int64_t nNowUS = LOOMTIME::NowMicros();
    std::vector<SearchHit> vHits;

    if (q.HasText())
    {
        std::vector<SearchHit> vScored;
        Locked_ScoreText(q, vCandidates, bUnbounded, vScored);

        vHits.reserve(vScored.size());
        for (const SearchHit& hit : vScored)
        {
            const auto jt = mJots.find(hit.mID);
            if (jt == mJots.end() || !Passes(jt->second))
                continue;
            vHits.push_back(SearchHit{ hit.mID,
                                       hit.mfScore * Locked_RecencyMultiplier(hit.mID, nNowUS) });
        }
    }
    else
    {
        const std::vector<tJotID>& vSource = bUnbounded ? mChrono : vCandidates;
        vHits.reserve(vSource.size());
        for (tJotID id : vSource)
        {
            const auto jt = mJots.find(id);
            if (jt == mJots.end() || !Passes(jt->second))
                continue;
            vHits.push_back(SearchHit{ id, 0.0f });
        }
    }

    outResults.mnMatched = vHits.size();

    eOrder order = q.mOrder;
    if (order == eOrder::kAuto)
        order = q.HasText() ? eOrder::kRelevance : eOrder::kNewest;

    const auto Less = [order](const SearchHit& a, const SearchHit& b) -> bool
    {
        switch (order)
        {
        case eOrder::kOldest:
            return a.mID < b.mID;
        case eOrder::kNewest:
            return a.mID > b.mID;
        case eOrder::kRelevance:
        default:
            if (a.mfScore != b.mfScore)
                return a.mfScore > b.mfScore;
            return a.mID > b.mID;   // newer breaks a scoring tie
        }
    };

    const size_t nWanted = q.mnOffset + (q.mnLimit == 0 ? vHits.size() : q.mnLimit);

    // Partial sort: the caller wants the top N, and fully ordering a large result set to then throw
    // most of it away is the easiest performance mistake to make here.
    if (nWanted < vHits.size())
    {
        std::partial_sort(vHits.begin(), vHits.begin() + static_cast<ptrdiff_t>(nWanted),
                          vHits.end(), Less);
        vHits.resize(nWanted);
        outResults.mbTruncated = true;
    }
    else
    {
        std::sort(vHits.begin(), vHits.end(), Less);
    }

    if (q.mnOffset >= vHits.size())
        return;

    outResults.mHits.assign(vHits.begin() + static_cast<ptrdiff_t>(q.mnOffset), vHits.end());
}

//====================================================================================================
// Rebuild
//====================================================================================================

void JotStore::RebuildIndexes()
{
    std::unique_lock lock(mLock);

    for (size_t f = 0; f < kFieldCount; ++f)
    {
        mPostings[f].clear();
        mnTotalLen[f] = 0;
    }
    mTermsByLex.clear();
    mTerms.Clear();
    mTagPostings.clear();
    mEditorPostings.clear();
    mBacklinks.clear();
    mPendingLinks.clear();
    mNameIndex.clear();
    mChrono.clear();
    mSlotToID.clear();
    mFreeSlots.clear();

    // Tag ids live inside the records, so the registry keeps its interning and only its refcounts
    // are rebuilt. Clearing it here would invalidate every Jot::mTags in the store.
    for (uint32_t id = 0; id < mTags.Count(); ++id)
    {
        while (mTags.RefCount(id) > 0)
            mTags.Release(id);
    }

    mEditorPostings.resize(std::max<size_t>(mEditors.Count(), 1));

    for (auto& entry : mJots)
    {
        Jot& jot = entry.second;
        jot.mnSummaryLen = 0;
        jot.mnBodyLen    = 0;
        jot.mnSlot       = Locked_AllocSlot(jot.mID);

        mChrono.push_back(jot.mID);
        if (!jot.msName.empty())
            mNameIndex.emplace(jot.msName, jot.mID);
    }

    std::sort(mChrono.begin(), mChrono.end());

    for (auto& entry : mJots)
        Locked_IndexJot(entry.second);
}
