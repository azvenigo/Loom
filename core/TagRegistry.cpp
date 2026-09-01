#include "TagRegistry.h"

#include <algorithm>
#include <cstring>

namespace
{
    constexpr uint32_t kMaxSuggestionDistance = 2;
    constexpr size_t   kMinLenForEditMatch    = 4;   // below this, 2 edits is most of the word
    constexpr size_t   kMinAbbrevStem         = 4;   // shortest stem worth calling an abbreviation
}

std::string TagRegistry::Normalize(std::string_view sTag)
{
    std::string sOut;
    sOut.reserve(sTag.size());

    bool bPendingSep = false;
    for (char ch : sTag)
    {
        const unsigned char c = static_cast<unsigned char>(ch);

        if (c <= ' ' || c == '_' || c == '-')
        {
            // Defer separators so a run of them collapses, and a trailing run vanishes entirely.
            bPendingSep = !sOut.empty();
            continue;
        }

        if (bPendingSep)
        {
            sOut.push_back('-');
            bPendingSep = false;
        }

        sOut.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c));
    }

    return sOut;
}

bool TagRegistry::IsReserved(std::string_view sNormalizedTag)
{
    return sNormalizedTag.find(':') != std::string_view::npos;
}

tTagID TagRegistry::Intern(std::string_view sNormalizedTag)
{
    if (sNormalizedTag.empty())
        return kInvalidTagID;

    const tTagID id = mNames.Intern(sNormalizedTag);
    if (id >= mEntries.size())
    {
        mEntries.resize(id + 1);
        mEntries[id].mbReserved = IsReserved(sNormalizedTag);
    }
    return id;
}

tTagID TagRegistry::Find(std::string_view sNormalizedTag) const
{
    const uint32_t id = mNames.Find(sNormalizedTag);
    return id == kInvalidStrID ? kInvalidTagID : id;
}

void TagRegistry::AddRef(tTagID id, int64_t nWhenUS)
{
    if (id >= mEntries.size())
        return;

    Entry& e = mEntries[id];
    if (e.mnCount == 0 && e.mFirstUS == 0)
        e.mFirstUS = nWhenUS;
    ++e.mnCount;
    if (nWhenUS > e.mLastUS)
        e.mLastUS = nWhenUS;
}

void TagRegistry::Release(tTagID id)
{
    if (id >= mEntries.size())
        return;
    Entry& e = mEntries[id];
    if (e.mnCount > 0)
        --e.mnCount;
}

uint32_t TagRegistry::RefCount(tTagID id) const
{
    return id < mEntries.size() ? mEntries[id].mnCount : 0;
}

void TagRegistry::List(std::vector<TagStat>& outStats, bool bIncludeReserved) const
{
    outStats.clear();
    outStats.reserve(mEntries.size());

    for (uint32_t id = 0; id < mEntries.size(); ++id)
    {
        const Entry& e = mEntries[id];
        if (e.mnCount == 0)
            continue;
        if (e.mbReserved && !bIncludeReserved)
            continue;

        TagStat s;
        s.msTag      = mNames.Value(id);
        s.mnCount    = e.mnCount;
        s.mFirstUS   = e.mFirstUS;
        s.mLastUS    = e.mLastUS;
        s.mbReserved = e.mbReserved;
        outStats.push_back(std::move(s));
    }

    std::sort(outStats.begin(), outStats.end(),
        [](const TagStat& a, const TagStat& b)
        {
            if (a.mnCount != b.mnCount)
                return a.mnCount > b.mnCount;
            return a.msTag < b.msTag;
        });
}

size_t TagRegistry::VocabularySize() const
{
    size_t nCount = 0;
    for (const Entry& e : mEntries)
    {
        if (e.mnCount > 0 && !e.mbReserved)
            ++nCount;
    }
    return nCount;
}

uint32_t TagRegistry::BoundedEditDistance(std::string_view a, std::string_view b, uint32_t nMax)
{
    const size_t nA = a.size();
    const size_t nB = b.size();

    // Length alone disqualifies most pairs, and this is the check that makes the all-pairs scan
    // in Clusters() affordable.
    if (nA > nB + nMax || nB > nA + nMax)
        return nMax + 1;

    // Two rolling rows rather than a full matrix.
    std::vector<uint32_t> vPrev(nB + 1);
    std::vector<uint32_t> vCurr(nB + 1);
    for (size_t j = 0; j <= nB; ++j)
        vPrev[j] = static_cast<uint32_t>(j);

    for (size_t i = 1; i <= nA; ++i)
    {
        vCurr[0] = static_cast<uint32_t>(i);
        uint32_t nRowMin = vCurr[0];

        for (size_t j = 1; j <= nB; ++j)
        {
            const uint32_t nCost = (a[i - 1] == b[j - 1]) ? 0u : 1u;
            vCurr[j] = std::min({ vPrev[j] + 1, vCurr[j - 1] + 1, vPrev[j - 1] + nCost });
            nRowMin = std::min(nRowMin, vCurr[j]);
        }

        // Every remaining row can only add to the minimum, so once the whole row is past the
        // bound the answer is definitely over it.
        if (nRowMin > nMax)
            return nMax + 1;

        vPrev.swap(vCurr);
    }

    return vPrev[nB];
}

bool TagRegistry::IsMorphologicalVariant(std::string_view a, std::string_view b)
{
    if (a == b)
        return false;

    // Order so a is the shorter.
    if (a.size() > b.size())
        std::swap(a, b);

    if (a.empty() || b.compare(0, a.size(), a) != 0)
        return false;

    const std::string_view sSuffix = b.substr(a.size());

    // "meds"/"med", "network"/"networking", "build"/"builds"/"building". A shared stem with a
    // short grammatical tail is the drift that actually happens.
    static const char* kTails[] = { "s", "es", "ing", "ed", "er", "ers" };
    for (const char* p : kTails)
    {
        if (sSuffix == p)
            return true;
    }

    // Abbreviation drift: "infra"/"infrastructure", "perf"/"performance", "config"/"configuration".
    // Two writers reaching independently for the short and long form of the same word is one of the
    // commonest ways a shared vocabulary splits, and the grammatical-tail rule above misses all of
    // it because the tail is long.
    //
    // Guarded on the stem being at least kMinAbbrevStem characters, because below that almost
    // everything is a prefix of something ("ci"/"circuit") and the warning would become noise.
    // A false positive here costs one dismissible line in a write response, so the threshold leans
    // towards catching real drift.
    if (a.size() >= kMinAbbrevStem)
        return true;

    return false;
}

void TagRegistry::Suggest(std::string_view sNormalizedTag, std::vector<TagSuggestion>& outSuggestions) const
{
    outSuggestions.clear();

    if (sNormalizedTag.empty() || IsReserved(sNormalizedTag))
        return;

    // An exact match is not a suggestion - the tag already exists and is being reused, which is
    // exactly what we want to happen.
    const tTagID existing = Find(sNormalizedTag);
    if (existing != kInvalidTagID && RefCount(existing) > 0)
        return;

    for (uint32_t id = 0; id < mEntries.size(); ++id)
    {
        const Entry& e = mEntries[id];
        if (e.mnCount == 0 || e.mbReserved)
            continue;

        const std::string& sOther = mNames.Value(id);
        if (sOther == sNormalizedTag)
            continue;

        uint32_t nDistance = 0;
        bool     bHit      = false;

        if (IsMorphologicalVariant(sNormalizedTag, sOther))
        {
            bHit = true;   // nDistance stays 0, meaning "related by form, not by typo"
        }
        else if (sNormalizedTag.size() >= kMinLenForEditMatch && sOther.size() >= kMinLenForEditMatch)
        {
            nDistance = BoundedEditDistance(sNormalizedTag, sOther, kMaxSuggestionDistance);
            bHit = (nDistance <= kMaxSuggestionDistance);
        }

        if (!bHit)
            continue;

        TagSuggestion s;
        s.msIncoming      = std::string(sNormalizedTag);
        s.msExisting      = sOther;
        s.mnExistingCount = e.mnCount;
        s.mnDistance      = nDistance;
        outSuggestions.push_back(std::move(s));
    }

    // Most-established first: the tag with 47 uses is the one worth adopting.
    std::sort(outSuggestions.begin(), outSuggestions.end(),
        [](const TagSuggestion& a, const TagSuggestion& b)
        {
            if (a.mnExistingCount != b.mnExistingCount)
                return a.mnExistingCount > b.mnExistingCount;
            return a.msExisting < b.msExisting;
        });
}

void TagRegistry::Clusters(std::vector<TagCluster>& outClusters) const
{
    outClusters.clear();

    std::vector<TagStat> vLive;
    List(vLive, false);
    if (vLive.size() < 2)
        return;

    std::vector<bool> vClaimed(vLive.size(), false);

    for (size_t i = 0; i < vLive.size(); ++i)
    {
        if (vClaimed[i])
            continue;

        TagCluster cluster;

        for (size_t j = i + 1; j < vLive.size(); ++j)
        {
            if (vClaimed[j])
                continue;

            const std::string& a = vLive[i].msTag;
            const std::string& b = vLive[j].msTag;

            bool bSimilar = IsMorphologicalVariant(a, b);
            if (!bSimilar && a.size() >= kMinLenForEditMatch && b.size() >= kMinLenForEditMatch)
                bSimilar = BoundedEditDistance(a, b, kMaxSuggestionDistance) <= kMaxSuggestionDistance;

            if (bSimilar)
            {
                if (cluster.mMembers.empty())
                    cluster.mMembers.push_back(a);   // vLive is count-sorted, so i is the leader
                cluster.mMembers.push_back(b);
                vClaimed[j] = true;
            }
        }

        if (!cluster.mMembers.empty())
        {
            vClaimed[i] = true;
            outClusters.push_back(std::move(cluster));
        }
    }
}

void TagRegistry::Clear()
{
    mNames.Clear();
    mEntries.clear();
}
