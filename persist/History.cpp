// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/History.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

#include "vendor/json.hpp"

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

    std::string DelLine(uint64_t nSeq, int64_t nAtUS, tJotID id,
                        const std::string& sName, const std::string& sEditor)
    {
        json j;
        j["seq"] = nSeq;
        j["at"]  = nAtUS;
        j["op"]  = "del";
        j["id"]  = id;
        if (!sName.empty())   j["name"]   = sName;
        if (!sEditor.empty()) j["editor"] = sEditor;
        return j.dump() + "\n";
    }
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
        outEntry.mbDelete = true;
        outEntry.mID      = j.value("id", int64_t(kInvalidJotID));
        outEntry.msName   = j.value("name", std::string());
        outEntry.msEditor = j.value("editor", std::string());
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
                    mLastSeen[entry.mID] = { entry.msName, entry.msEditor };

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
    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;
        entry.mnSeq = mnNextSeq++;
        mLastSeen[jot.mID] = { jot.msName, jot.msEditor.empty() ? std::string("user") : jot.msEditor };
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mID      = jot.mID;
    entry.msName   = jot.msName;
    entry.msEditor = jot.msEditor.empty() ? std::string("user") : jot.msEditor;
    entry.msSummary = Clip(jot.msSummary.empty() ? jot.msText : jot.msSummary, 110);
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
            entry.msName   = it->second.first;
            entry.msEditor = it->second.second;
        }
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mbDelete = true;
    entry.mID      = id;

    Append(entry, DelLine(entry.mnSeq, entry.mnAtUS, id, entry.msName, entry.msEditor));
}


//====================================================================================================
// Reads
//====================================================================================================

void History::List(tJotID idFilter, size_t nLimit, size_t nOffset,
                   std::vector<HistoryEntry>& outEntries, size_t& outTotal) const
{
    outEntries.clear();
    outTotal = 0;

    std::unique_lock lock(mMutex);
    if (nLimit == 0)
        nLimit = 100;

    // Newest first, so the walk is back-to-front over the deque.
    size_t nSeen = 0;
    for (auto it = mRecent.rbegin(); it != mRecent.rend(); ++it)
    {
        if (idFilter != kInvalidJotID && it->mID != idFilter)
            continue;

        ++outTotal;
        if (nSeen++ < nOffset)
            continue;
        if (outEntries.size() < nLimit)
            outEntries.push_back(*it);
    }
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

    std::unique_lock lock(mMutex);
    for (auto it = mRecent.rbegin(); it != mRecent.rend(); ++it)
    {
        if (it->mnSeq >= nSeq || it->mID != anchor.mID || it->mbDelete)
            continue;
        outEntry = *it;
        return true;   // the deque is in sequence order, so the first match walking back is it
    }
    return false;
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
