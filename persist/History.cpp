// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/History.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    // One history line. The record is nested rather than flattened so a put line contains a jot
    // document byte-identical to what the WAL and the snapshot hold - one serialization, three
    // readers, which is the same argument FlatJot itself is built on.
    std::string PutLine(uint64_t nSeq, int64_t nAtUS, const std::string& sRecord)
    {
        std::string s = "{\"seq\":";
        s += std::to_string(nSeq);
        s += ",\"at\":";
        s += std::to_string(nAtUS);
        s += ",\"op\":\"put\",\"jot\":";
        s += sRecord;
        s += "}\n";
        return s;
    }

    std::string DelLine(uint64_t nSeq, int64_t nAtUS, tJotID id, const std::string& sName,
                        const std::string& sEditor, const std::string& sSummary)
    {
        json j;
        j["seq"] = nSeq;
        j["at"]  = nAtUS;
        j["op"]  = "del";
        j["id"]  = id;
        if (!sName.empty())    j["name"]    = sName;
        if (!sEditor.empty())  j["editor"]  = sEditor;
        if (!sSummary.empty()) j["summary"] = sSummary;
        return j.dump() + "\n";
    }
}

std::string History::DescribeChange(const LastSeen* pPrev, const FlatJot& jot)
{
    if (!pPrev)
        return std::string();

    std::vector<std::string> vParts;
    if (pPrev->msName != jot.msName)
        vParts.push_back(jot.msName.empty() ? "name cleared" : "renamed '" + jot.msName + "'");
    if (pPrev->msSummary != jot.msSummary)
        vParts.push_back("summary edited");
    if (pPrev->mnTextLen != jot.msText.size())
        vParts.push_back("text edited");

    for (const std::string& sTag : jot.mTags)
        if (std::find(pPrev->mTags.begin(), pPrev->mTags.end(), sTag) == pPrev->mTags.end())
            vParts.push_back("+" + sTag);
    for (const std::string& sTag : pPrev->mTags)
        if (std::find(jot.mTags.begin(), jot.mTags.end(), sTag) == jot.mTags.end())
            vParts.push_back("-" + sTag);

    if (vParts.empty())
        return std::string();

    std::string sOut;
    for (size_t i = 0; i < vParts.size(); ++i)
    {
        if (i) sOut += ", ";
        sOut += vParts[i];
    }
    return sOut;
}

void History::Recaption(HistoryEntry& burst, const HistoryEntry& baseline)
{
    // A row standing for one mutation already carries exactly this diff from OnPut, so only a
    // coalesced run needs rebuilding - and it is rebuilt against the state the run STARTED from, not
    // accumulated from the individual captions, which would repeat "text edited" once per save.
    if (burst.mnCoalesced <= 1 || burst.mbDelete || baseline.mbDelete)
        return;

    FlatJot before, after;
    std::string sErrIgnored;
    if (!JOTJSON::ParseFlat(baseline.msRecord, before, sErrIgnored)
        || !JOTJSON::ParseFlat(burst.msRecord, after, sErrIgnored))
        return;

    LastSeen prev;
    prev.msName    = before.msName;
    prev.msSummary = before.msSummary;
    prev.mTags     = before.mTags;
    prev.mnTextLen = before.msText.size();

    const std::string sDiff = DescribeChange(&prev, after);
    if (!sDiff.empty())
        burst.msSummary = sDiff;
}


//====================================================================================================

History::History() = default;

History::~History()
{
    Close();
}

std::string History::Clip(const std::string& s, size_t nMax)
{
    // First line only, and short. This is a list caption, not the record - the record is right
    // there in msRecord for anything that needs the real content.
    size_t nEnd = s.find('\n');
    if (nEnd == std::string::npos)
        nEnd = s.size();
    if (nEnd > nMax)
        nEnd = nMax;

    std::string sOut = s.substr(0, nEnd);
    while (!sOut.empty() && (sOut.back() == ' ' || sOut.back() == '\r'))
        sOut.pop_back();
    if (nEnd < s.size())
        sOut += "...";
    return sOut;
}

bool History::ScanLastLine(const std::string& sPath, HistoryEntry& outEntry)
{
    FILE* pFile = std::fopen(sPath.c_str(), "rb");
    if (!pFile)
        return false;

    if (std::fseek(pFile, 0, SEEK_END) != 0)
    {
        std::fclose(pFile);
        return false;
    }

    // Walk back a line at a time until one PARSES, rather than trusting the last one. A torn final
    // line is the expected residue of a crash - the read loop in Open() drops it for that reason -
    // and a crash is precisely when a log gets left oversized, so stopping at an unparseable tail
    // would leave the counter at 1 in the one case this function exists to protect.
    long nPos    = std::ftell(pFile);
    bool bParsed = false;
    while (nPos > 0 && !bParsed)
    {
        std::string sLine;
        while (nPos > 0)
        {
            --nPos;
            std::fseek(pFile, nPos, SEEK_SET);
            const int ch = std::fgetc(pFile);
            if (ch == '\n')
            {
                if (!sLine.empty())
                    break;
                continue;   // the trailing newline, or a blank line
            }
            sLine.push_back(static_cast<char>(ch));
        }
        if (sLine.empty())
            break;

        std::reverse(sLine.begin(), sLine.end());
        bParsed = ParseLine(sLine, outEntry);
    }

    std::fclose(pFile);
    return bParsed;
}

bool History::ParseLine(const std::string& sLine, HistoryEntry& outEntry)
{
    json j = json::parse(sLine, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("seq"))
        return false;

    outEntry = HistoryEntry();
    outEntry.mnSeq  = j.value("seq", 0ull);
    outEntry.mnAtUS = j.value("at", int64_t(0));

    const std::string sOp = j.value("op", std::string());
    if (sOp == "del")
    {
        outEntry.mbDelete  = true;
        outEntry.mID       = j.value("id", int64_t(kInvalidJotID));
        outEntry.msName    = j.value("name", std::string());
        outEntry.msEditor  = j.value("editor", std::string());
        outEntry.msSummary = j.value("summary", std::string());
        return outEntry.mnSeq != 0;
    }

    if (!j.contains("jot"))
        return false;

    const json& jot = j["jot"];
    outEntry.msRecord = jot.dump();

    FlatJot flat;
    std::string sError;
    if (!JOTJSON::ParseFlat(outEntry.msRecord, flat, sError))
        return false;

    outEntry.mID      = flat.mID;
    outEntry.msName   = flat.msName;
    outEntry.msEditor = flat.msEditor.empty() ? std::string("user") : flat.msEditor;
    outEntry.msSummary = Clip(flat.msSummary.empty() ? flat.msText : flat.msSummary, 110);
    return outEntry.mnSeq != 0;
}


//====================================================================================================
// Lifecycle
//====================================================================================================

std::error_code History::Open(const HistoryConfig& config)
{
    Close();

    std::unique_lock lock(mMutex);
    mConfig   = config;
    mnNextSeq = 1;
    mnEntries = 0;
    mnBytes   = 0;
    mRecent.clear();
    mQueue.clear();
    mLastSeen.clear();

    if (mConfig.msPath.empty())
        return LoomOK();

    // Rotate BEFORE reading. An oversized log is about to be replaced by an empty one, so reading
    // all of it first would be work thrown away - and the generation being retired is still on disk
    // as .1 if anybody wants it.
    std::error_code ecSize;
    const auto nSize = std::filesystem::file_size(mConfig.msPath, ecSize);
    if (!ecSize && nSize > mConfig.mnMaxBytes)
    {
        // The generation about to be retired holds the highest sequence number issued so far. Read
        // it before the rename, or the counter below would restart at 1 and reissue numbers the
        // retired generation already used - two different entries answering to the same seq, with
        // /history/restore having no way to tell them apart.
        HistoryEntry tail;
        if (ScanLastLine(mConfig.msPath, tail) && tail.mnSeq >= mnNextSeq)
            mnNextSeq = tail.mnSeq + 1;

        std::error_code ecMove;
        std::filesystem::rename(mConfig.msPath, mConfig.msPath + ".1", ecMove);
    }

    if (FILE* pRead = std::fopen(mConfig.msPath.c_str(), "rb"))
    {
        std::string sLine;
        int ch = 0;
        while ((ch = std::fgetc(pRead)) != EOF)
        {
            if (ch != '\n')
            {
                sLine.push_back(static_cast<char>(ch));
                continue;
            }

            HistoryEntry entry;
            if (!sLine.empty() && ParseLine(sLine, entry))
            {
                ++mnEntries;
                if (entry.mnSeq >= mnNextSeq)
                    mnNextSeq = entry.mnSeq + 1;
                if (!entry.mbDelete)
                {
                    // ParseLine already computed the plain summary/text clip into entry.msSummary.
                    // Diff it against the running mLastSeen - built up in the same seq order the
                    // live path sees - before overwriting it with the change caption, exactly as
                    // OnPut does, so a restart does not revert older entries to the flat caption.
                    FlatJot flat;
                    std::string sErrIgnored;
                    if (JOTJSON::ParseFlat(entry.msRecord, flat, sErrIgnored))
                    {
                        const auto it = mLastSeen.find(entry.mID);
                        const std::string sDiff =
                            DescribeChange(it != mLastSeen.end() ? &it->second : nullptr, flat);
                        const std::string sPlain = entry.msSummary;
                        if (!sDiff.empty())
                            entry.msSummary = sDiff;
                        mLastSeen[entry.mID] = { entry.msName, entry.msEditor, flat.msSummary,
                                                  sPlain, flat.mTags, flat.msText.size() };
                    }
                }

                mRecent.push_back(std::move(entry));
                while (mRecent.size() > mConfig.mnMemory)
                    mRecent.pop_front();
            }
            sLine.clear();
        }
        // A torn final line is the expected residue of a crash, exactly as in the WAL - it is
        // dropped rather than treated as corruption.
        std::fclose(pRead);
    }

    mpFile = std::fopen(mConfig.msPath.c_str(), "ab");
    if (!mpFile)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    std::error_code ecNow;
    const auto nNow = std::filesystem::file_size(mConfig.msPath, ecNow);
    mnBytes = ecNow ? 0 : static_cast<uint64_t>(nNow);

    mbRunning = true;
    lock.unlock();

    mCommitter = std::thread([this] { CommitterLoop(); });
    return LoomOK();
}

void History::Close()
{
    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
        {
            if (mpFile)
            {
                std::fclose(static_cast<FILE*>(mpFile));
                mpFile = nullptr;
            }
            return;
        }
        mbRunning = false;
    }
    mQueueCV.notify_all();

    if (mCommitter.joinable())
        mCommitter.join();

    std::unique_lock lock(mMutex);
    if (mpFile)
    {
        // Drain whatever the thread did not get to, so a clean shutdown loses nothing.
        for (const std::string& sLine : mQueue)
        {
            std::fwrite(sLine.data(), 1, sLine.size(), static_cast<FILE*>(mpFile));
            mnBytes += sLine.size();
        }
        mQueue.clear();
        std::fflush(static_cast<FILE*>(mpFile));
        std::fclose(static_cast<FILE*>(mpFile));
        mpFile = nullptr;
    }
}

void History::CommitterLoop()
{
    for (;;)
    {
        std::deque<std::string> vBatch;
        {
            std::unique_lock lock(mMutex);
            mQueueCV.wait_for(lock, std::chrono::milliseconds(mConfig.mnFlushIntervalMS),
                              [this] { return !mQueue.empty() || !mbRunning; });
            if (!mbRunning && mQueue.empty())
                return;
            vBatch.swap(mQueue);
        }

        if (vBatch.empty())
            continue;

        std::unique_lock lock(mMutex);
        if (!mpFile)
            return;
        for (const std::string& sLine : vBatch)
        {
            std::fwrite(sLine.data(), 1, sLine.size(), static_cast<FILE*>(mpFile));
            mnBytes += sLine.size();
        }
        std::fflush(static_cast<FILE*>(mpFile));
    }
}


//====================================================================================================
// Sink - under the store's write lock. Serialize, queue, return.
//====================================================================================================

void History::Append(const HistoryEntry& entry, const std::string& sLine)
{
    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;

        ++mnEntries;
        mQueue.push_back(sLine);
        mRecent.push_back(entry);
        while (mRecent.size() > mConfig.mnMemory)
            mRecent.pop_front();
    }
    mQueueCV.notify_one();
}

void History::OnPut(const FlatJot& jot)
{
    HistoryEntry entry;
    entry.msName   = jot.msName;
    entry.msEditor = jot.msEditor.empty() ? std::string("user") : jot.msEditor;
    const std::string sPlainCaption = Clip(jot.msSummary.empty() ? jot.msText : jot.msSummary, 110);

    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;
        entry.mnSeq = mnNextSeq++;

        const auto it = mLastSeen.find(jot.mID);
        const std::string sDiff = DescribeChange(it != mLastSeen.end() ? &it->second : nullptr, jot);
        entry.msSummary = sDiff.empty() ? sPlainCaption : sDiff;

        // So a later delete of this same id can still be listed as "deleted <slug>: <summary>", and
        // so the NEXT put for this id has something to diff against.
        mLastSeen[jot.mID] = { entry.msName, entry.msEditor, jot.msSummary, sPlainCaption,
                               jot.mTags, jot.msText.size() };
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mID      = jot.mID;
    entry.msRecord = JOTJSON::ToJson(jot, false);

    Append(entry, PutLine(entry.mnSeq, entry.mnAtUS, entry.msRecord));
}

void History::OnDelete(tJotID id)
{
    HistoryEntry entry;
    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;
        entry.mnSeq = mnNextSeq++;
        const auto it = mLastSeen.find(id);
        if (it != mLastSeen.end())
        {
            entry.msName    = it->second.msName;
            entry.msEditor  = it->second.msEditor;
            entry.msSummary = it->second.msCaption;   // the display line, not the raw diff field
        }
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mbDelete = true;
    entry.mID      = id;

    Append(entry, DelLine(entry.mnSeq, entry.mnAtUS, id, entry.msName, entry.msEditor, entry.msSummary));
}


//====================================================================================================
// Reads
//====================================================================================================

void History::List(tJotID idFilter, size_t nLimit, size_t nOffset,
                   std::vector<HistoryEntry>& outEntries, size_t& outTotal) const
{
    // outTotal counts ROWS - coalesced runs, not raw mutations - within the in-memory window, since
    // that is what the offset/limit here page over and what a caller is showing "N of M" for. It is
    // not a whole-log count: for an idFilter whose jot has changes old enough to have aged out of
    // that window it undercounts, and HistoryStats::mnEntries remains the only honest total.
    outEntries.clear();
    outTotal = 0;

    std::unique_lock lock(mMutex);
    if (nLimit == 0)
        nLimit = 100;

    const int64_t nWindowUS = mConfig.mnCoalesceWindowMS * 1000;

    // COALESCE FIRST, PAGE SECOND. Offsets have to walk the same rows the caller can see, so folding
    // after slicing would hand back short pages and an offset that skips different amounts each time.
    //
    // The walk is newest-first, so a run is discovered from its end: the newest member becomes the
    // row - it holds the state the run arrived at, which is what restoring the row should reproduce
    // - and each older member within the window is absorbed into it. The entry that finally breaks a
    // run is, by construction, the state that run started from, so it is also the baseline the row's
    // caption is rebuilt against before it is closed out.
    std::vector<HistoryEntry>           vRows;       // newest first
    std::unordered_map<tJotID, size_t>  mOpen;       // jot id -> index of its still-growing run
    std::unordered_map<tJotID, int64_t> mOldestAt;   // that run's oldest timestamp so far

    for (auto it = mRecent.rbegin(); it != mRecent.rend(); ++it)
    {
        const HistoryEntry& entry = *it;
        if (idFilter != kInvalidJotID && entry.mID != idFilter)
            continue;

        const auto open = mOpen.find(entry.mID);
        if (open != mOpen.end())
        {
            HistoryEntry& run = vRows[open->second];

            // A delete is its own event at both ends: it never joins a run of edits, and a run does
            // not reach back across one into the jot's previous life. Editors are kept apart too, so
            // one agent's work is never folded into another's.
            const bool bMergeable = !entry.mbDelete && !run.mbDelete
                                 && entry.msEditor == run.msEditor
                                 && (mOldestAt[entry.mID] - entry.mnAtUS) <= nWindowUS;
            if (bMergeable)
            {
                ++run.mnCoalesced;
                mOldestAt[entry.mID] = entry.mnAtUS;
                continue;
            }

            Recaption(run, entry);
            mOpen.erase(open);
        }

        vRows.push_back(entry);
        mOpen[entry.mID]     = vRows.size() - 1;
        mOldestAt[entry.mID] = entry.mnAtUS;
    }

    // Runs still open ran out of older entries rather than being broken by one - they reach back to
    // the jot's creation, or to the edge of the memory window. Either way there is nothing to diff
    // against, and a caption reading "text edited" for a row that includes the jot being created
    // describes the last save rather than the row. Fall back to the jot's own summary.
    for (const auto& [id, nRow] : mOpen)
    {
        HistoryEntry& run = vRows[nRow];
        if (run.mnCoalesced <= 1 || run.mbDelete)
            continue;

        FlatJot flat;
        std::string sErrIgnored;
        if (JOTJSON::ParseFlat(run.msRecord, flat, sErrIgnored))
            run.msSummary = Clip(flat.msSummary.empty() ? flat.msText : flat.msSummary, 110);
    }

    outTotal = vRows.size();
    for (size_t i = nOffset; i < vRows.size() && outEntries.size() < nLimit; ++i)
        outEntries.push_back(std::move(vRows[i]));
}

bool History::Get(uint64_t nSeq, HistoryEntry& outEntry) const
{
    {
        std::unique_lock lock(mMutex);
        for (auto it = mRecent.rbegin(); it != mRecent.rend(); ++it)
        {
            if (it->mnSeq == nSeq)
            {
                outEntry = *it;
                return true;
            }
        }
    }
    return ScanFileFor(nSeq, outEntry);
}

bool History::Previous(uint64_t nSeq, HistoryEntry& outEntry) const
{
    HistoryEntry anchor;
    if (!Get(nSeq, anchor))
        return false;

    {
        std::unique_lock lock(mMutex);
        for (auto it = mRecent.rbegin(); it != mRecent.rend(); ++it)
        {
            if (it->mnSeq >= nSeq || it->mID != anchor.mID || it->mbDelete)
                continue;
            outEntry = *it;
            return true;   // the deque is in sequence order, so the first match walking back is it
        }
    }

    // Not in the in-memory window. Get() falls back to the file for this same reason; Previous()
    // has to as well, or "undo this delete" for anything older than the window wrongly reports
    // "nothing to restore" instead of finding the earlier version that is really sitting there.
    return ScanFileForPrevious(nSeq, anchor.mID, outEntry);
}

bool History::ScanFileFor(uint64_t nSeq, HistoryEntry& outEntry) const
{
    std::string sPath;
    {
        std::unique_lock lock(mMutex);
        sPath = mConfig.msPath;
    }
    if (sPath.empty())
        return false;

    // Both generations, oldest first. Linear, and deliberately not indexed: this only runs for an
    // entry older than the in-memory window, which is a rare manual act, not a hot path.
    for (const std::string& sTry : { sPath + ".1", sPath })
    {
        FILE* pFile = std::fopen(sTry.c_str(), "rb");
        if (!pFile)
            continue;

        std::string sLine;
        int ch = 0;
        bool bFound = false;
        while ((ch = std::fgetc(pFile)) != EOF)
        {
            if (ch != '\n')
            {
                sLine.push_back(static_cast<char>(ch));
                continue;
            }
            HistoryEntry entry;
            if (!sLine.empty() && ParseLine(sLine, entry) && entry.mnSeq == nSeq)
            {
                outEntry = std::move(entry);
                bFound = true;
                break;
            }
            sLine.clear();
        }
        std::fclose(pFile);
        if (bFound)
            return true;
    }
    return false;
}

bool History::ScanFileForPrevious(uint64_t nSeq, tJotID id, HistoryEntry& outEntry) const
{
    std::string sPath;
    {
        std::unique_lock lock(mMutex);
        sPath = mConfig.msPath;
    }
    if (sPath.empty())
        return false;

    // Both generations, oldest first, same as ScanFileFor - lines within and across generations are
    // in increasing seq order, so the last match seen before hitting nSeq is the one immediately
    // before it, and hitting nSeq itself means every later line can only be later still.
    bool bFound = false;
    for (const std::string& sTry : { sPath + ".1", sPath })
    {
        FILE* pFile = std::fopen(sTry.c_str(), "rb");
        if (!pFile)
            continue;

        std::string sLine;
        int ch = 0;
        bool bStop = false;
        while (!bStop && (ch = std::fgetc(pFile)) != EOF)
        {
            if (ch != '\n')
            {
                sLine.push_back(static_cast<char>(ch));
                continue;
            }
            HistoryEntry entry;
            if (!sLine.empty() && ParseLine(sLine, entry))
            {
                if (entry.mnSeq >= nSeq)
                {
                    bStop = true;   // no earlier lines follow in either file from here on
                }
                else if (entry.mID == id && !entry.mbDelete)
                {
                    outEntry = std::move(entry);
                    bFound = true;
                }
            }
            sLine.clear();
        }
        std::fclose(pFile);
        if (bStop)
            break;
    }
    return bFound;
}

HistoryStats History::Stats() const
{
    std::unique_lock lock(mMutex);
    HistoryStats st;
    st.mbEnabled  = mbRunning;
    st.mnEntries  = mnEntries;
    st.mnInMemory = mRecent.size();
    st.mnBytes    = mnBytes;
    return st;
}


//====================================================================================================
// Offline purge
//====================================================================================================

std::error_code History::PurgeFile(const std::string& sPath, const std::vector<tJotID>& vIDs,
                                   size_t& outRemoved, size_t& outKept)
{
    outRemoved = 0;
    outKept    = 0;

    FILE* pRead = std::fopen(sPath.c_str(), "rb");
    if (!pRead)
        return LoomOK();   // nothing to scrub is success, not failure

    const std::string sTmp = sPath + ".purge.tmp";
    FILE* pWrite = std::fopen(sTmp.c_str(), "wb");
    if (!pWrite)
    {
        std::fclose(pRead);
        return MakeLoomError(eLoomErr::kInvalidArgument);
    }

    const auto Wanted = [&vIDs](tJotID id)
    {
        for (tJotID want : vIDs)
        {
            if (want == id)
                return true;
        }
        return false;
    };

    std::string sLine;
    int  ch = 0;
    bool bOK = true;
    const auto Flush = [&]()
    {
        HistoryEntry entry;
        if (sLine.empty())
            return;

        // An unparseable line is DROPPED, not kept. This is the one place in Loom where a torn
        // record is not simply tolerated: the whole point of the operation is that certain bytes
        // must not survive it, and a line nobody can parse is a line nobody can clear.
        if (ParseLine(sLine, entry) && !Wanted(entry.mID))
        {
            sLine.push_back('\n');
            if (std::fwrite(sLine.data(), 1, sLine.size(), pWrite) != sLine.size())
                bOK = false;
            ++outKept;
        }
        else
        {
            ++outRemoved;
        }
        sLine.clear();
    };

    while ((ch = std::fgetc(pRead)) != EOF)
    {
        if (ch == '\n')
            Flush();
        else
            sLine.push_back(static_cast<char>(ch));
    }
    Flush();   // a file not ending in a newline still has a last record

    std::fflush(pWrite);
    std::fclose(pWrite);
    std::fclose(pRead);

    if (!bOK)
    {
        std::error_code ecRm;
        std::filesystem::remove(sTmp, ecRm);
        return MakeLoomError(eLoomErr::kInvalidArgument);
    }

    std::error_code ecMove;
    std::filesystem::rename(sTmp, sPath, ecMove);
    if (ecMove)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    return LoomOK();
}
