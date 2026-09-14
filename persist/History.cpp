// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/History.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

#include "vendor/json.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    // One history line. The record is nested rather than flattened so a put line contains a jot
    // document byte-identical to what the WAL and the snapshot hold - one serialization, three
    // readers, which is the same argument FlatJot itself is built on.
    std::string PutLine(uint64_t nSeq, int64_t nAtUS, uint64_t nTxnID, const std::string& sRecord)
    {
        std::string s = "{\"seq\":";
        s += std::to_string(nSeq);
        s += ",\"at\":";
        s += std::to_string(nAtUS);
        if (nTxnID != 0)
        {
            s += ",\"txn\":";
            s += std::to_string(nTxnID);
        }
        s += ",\"op\":\"put\",\"jot\":";
        s += sRecord;
        s += "}\n";
        return s;
    }

    std::string DelLine(uint64_t nSeq, int64_t nAtUS, tJotID id, const std::string& sName,
                        const std::string& sEditor, const std::string& sSummary,
                        const std::string& sOrigin, uint64_t nTxnID)
    {
        json j;
        j["seq"] = nSeq;
        j["at"]  = nAtUS;
        if (nTxnID != 0) j["txn"] = nTxnID;
        j["op"]  = "del";
        j["id"]  = id;
        if (!sName.empty())    j["name"]    = sName;
        if (!sEditor.empty())  j["editor"]  = sEditor;
        if (!sSummary.empty()) j["summary"] = sSummary;
        // A put line needs no such key: the nested record already carries the origin, and one
        // serialization read three ways is the rule this file is built on. A delete has no record.
        if (!sOrigin.empty())  j["origin"]  = sOrigin;
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
    outEntry.mnSeq   = j.value("seq", 0ull);
    outEntry.mnAtUS  = j.value("at", int64_t(0));
    outEntry.mnTxnID = j.value("txn", 0ull);

    const std::string sOp = j.value("op", std::string());
    if (sOp == "del")
    {
        outEntry.mbDelete  = true;
        outEntry.mID       = j.value("id", int64_t(kInvalidJotID));
        outEntry.msName    = j.value("name", std::string());
        outEntry.msEditor  = j.value("editor", std::string());
        outEntry.msSummary = j.value("summary", std::string());
        outEntry.msOrigin  = j.value("origin", std::string());
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
    outEntry.msOrigin = flat.msOrigin;
    outEntry.msEditor = flat.msEditor.empty() ? std::string("user") : flat.msEditor;
    outEntry.msSummary = Clip(flat.msSummary.empty() ? flat.msText : flat.msSummary, 110);
    return outEntry.mnSeq != 0;
}


//====================================================================================================
// Segments
//====================================================================================================

template <typename Fn>
void History::ForEachLine(const std::string& sPath, Fn&& fn)
{
    FILE* pFile = std::fopen(sPath.c_str(), "rb");
    if (!pFile)
        return;

    // Chunked rather than a character at a time: segments are read whole, and the log only grows.
    std::string sLine;
    char   buf[1 << 16];
    size_t nRead = 0;
    bool   bStop = false;
    while (!bStop && (nRead = std::fread(buf, 1, sizeof(buf), pFile)) > 0)
    {
        size_t nStart = 0;
        for (size_t i = 0; i < nRead; ++i)
        {
            if (buf[i] != '\n')
                continue;
            sLine.append(buf + nStart, i - nStart);
            nStart = i + 1;
            if (!sLine.empty() && !fn(sLine))
            {
                bStop = true;
                break;
            }
            sLine.clear();
        }
        if (!bStop)
            sLine.append(buf + nStart, nRead - nStart);
    }
    // Whatever is left in sLine has no newline after it - a torn final line, which is dropped here
    // exactly as the WAL drops one.
    std::fclose(pFile);
}

std::string History::SegmentPath(const std::string& sPath, uint64_t nNumber)
{
    char sz[32];
    std::snprintf(sz, sizeof(sz), ".%06llu", static_cast<unsigned long long>(nNumber));
    return sPath + sz;
}

std::vector<std::pair<uint64_t, std::string>> History::NumberedSegments(const std::string& sPath)
{
    namespace fs = std::filesystem;
    std::vector<std::pair<uint64_t, std::string>> vOut;

    const fs::path path(sPath);
    fs::path dir = path.parent_path();
    if (dir.empty())
        dir = ".";
    const std::string sPrefix = path.filename().string() + ".";

    // loom.history.<at least six digits>, and nothing else - so loom.history.1, a purge's .purge.tmp
    // and a hand-made .bak-20260903 are never mistaken for part of the sequence.
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string sName = it->path().filename().string();
        if (sName.size() < sPrefix.size() + 6 || sName.size() > sPrefix.size() + 18
            || sName.compare(0, sPrefix.size(), sPrefix) != 0)
            continue;
        const std::string sNum = sName.substr(sPrefix.size());
        if (!std::all_of(sNum.begin(), sNum.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;
        vOut.emplace_back(std::stoull(sNum), sPath + "." + sNum);
    }
    std::sort(vOut.begin(), vOut.end());
    return vOut;
}

std::vector<std::string> History::AllFiles(const std::string& sPath)
{
    std::vector<std::string> vOut;
    std::error_code ec;
    if (std::filesystem::exists(sPath + ".1", ec))
        vOut.push_back(sPath + ".1");
    for (auto& [nNumber, sSegment] : NumberedSegments(sPath))
        vOut.push_back(std::move(sSegment));
    if (std::filesystem::exists(sPath, ec))
        vOut.push_back(sPath);
    return vOut;
}

bool History::ScanFirstLine(const std::string& sPath, HistoryEntry& outEntry)
{
    bool bParsed = false;
    ForEachLine(sPath, [&](const std::string& sLine)
    {
        bParsed = ParseLine(sLine, outEntry);
        return !bParsed;
    });
    return bParsed;
}

HistorySegment History::IndexSegment(const std::string& sPath)
{
    HistorySegment seg;
    seg.msPath = sPath;

    HistoryEntry entry;
    if (ScanFirstLine(sPath, entry))
    {
        seg.mnFirstSeq  = entry.mnSeq;
        seg.mnFirstAtUS = entry.mnAtUS;
    }
    if (ScanLastLine(sPath, entry))
    {
        seg.mnLastSeq  = entry.mnSeq;
        seg.mnLastAtUS = entry.mnAtUS;
    }

    std::error_code ec;
    const auto nSize = std::filesystem::file_size(sPath, ec);
    seg.mnBytes = ec ? 0 : static_cast<uint64_t>(nSize);
    return seg;
}

std::vector<std::string> History::FilesSpanning(uint64_t nFrom, uint64_t nTo) const
{
    std::vector<std::string> vOut;
    std::unique_lock lock(mMutex);
    if (mConfig.msPath.empty())
        return vOut;

    for (const HistorySegment& seg : mSegments)
    {
        // A segment whose ends could not be read is opened rather than skipped: being slow about a
        // damaged file is better than answering "no such entry" for something that is in it.
        const bool bUnknown = seg.mnFirstSeq == 0 || seg.mnLastSeq == 0;
        if (bUnknown || (seg.mnLastSeq >= nFrom && seg.mnFirstSeq <= nTo))
            vOut.push_back(seg.msPath);
    }

    // Everything in the active file is newer than every sealed segment.
    if (mSegments.empty() || mSegments.back().mnLastSeq == 0 || nTo > mSegments.back().mnLastSeq)
        vOut.push_back(mConfig.msPath);
    return vOut;
}

std::string History::Locked_Seal()
{
    namespace fs = std::filesystem;

    FILE* pFile = static_cast<FILE*>(mpFile);
    std::fflush(pFile);
    std::fclose(pFile);
    mpFile = nullptr;

    // Never onto an existing name. Something sitting at the next number was not put there by this
    // class, and a rename would silently replace it - the exact loss this layout exists to prevent.
    std::error_code ec;
    std::string sTo = SegmentPath(mConfig.msPath, mnNextSegment);
    while (fs::exists(sTo, ec))
        sTo = SegmentPath(mConfig.msPath, ++mnNextSegment);

    std::error_code ecMove;
    fs::rename(mConfig.msPath, sTo, ecMove);

    // Reopen whatever happened. If the rename failed the old file is still the active one, appends
    // carry on into it, and the next batch tries to seal again - an oversized file costs nothing,
    // while a closed one would stop history altogether.
    mpFile = std::fopen(mConfig.msPath.c_str(), "ab");

    if (ecMove)
        return std::string();

    ++mnNextSegment;
    mnBytes = 0;
    return sTo;
}

void History::Locked_Load(const std::string& sPath)
{
    ForEachLine(sPath, [this](const std::string& sLine)
    {
        HistoryEntry entry;
        if (!ParseLine(sLine, entry))
            return true;

        ++mnEntries;
        if (entry.mnSeq >= mnNextSeq)
            mnNextSeq = entry.mnSeq + 1;
        if (!entry.mbDelete)
        {
            // ParseLine already computed the plain summary/text clip into entry.msSummary. Diff it
            // against the running mLastSeen - built up in the same seq order the live path sees -
            // before overwriting it with the change caption, exactly as OnPut does, so a restart
            // does not revert older entries to the flat caption.
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
                                          sPlain, entry.msOrigin, flat.mTags,
                                          flat.msText.size() };
            }
        }

        mRecent.push_back(std::move(entry));
        while (mRecent.size() > mConfig.mnMemory)
            mRecent.pop_front();
        return true;
    });
}


//====================================================================================================
// Lifecycle
//====================================================================================================

std::error_code History::Open(const HistoryConfig& config)
{
    namespace fs = std::filesystem;

    Close();

    std::unique_lock rot(mRotateMutex);
    std::unique_lock lock(mMutex);
    mConfig   = config;
    mnNextSeq   = 1;
    mnEntries   = 0;
    mnBytes     = 0;
    mbInTxn     = false;
    mnOpenTxnID = 0;
    mRecent.clear();
    mQueue.clear();
    mLastSeen.clear();
    mSegments.clear();
    mnNextSegment = 1;

    if (mConfig.msPath.empty())
        return LoomOK();

    const std::string& sPath = mConfig.msPath;

    // An older build kept one previous generation as .1 and overwrote it on every rotation. Whatever
    // it still holds is the oldest history there is, so it becomes the first segment rather than
    // being left where the next rotation used to destroy it. If numbered segments somehow already
    // exist it is left in place and read as the oldest file, never renamed over one of them.
    auto vNumbered = NumberedSegments(sPath);
    std::error_code ec;
    if (vNumbered.empty() && fs::exists(sPath + ".1", ec))
    {
        std::error_code ecMove;
        fs::rename(sPath + ".1", SegmentPath(sPath, 1), ecMove);
        if (!ecMove)
            vNumbered = NumberedSegments(sPath);
    }

    if (fs::exists(sPath + ".1", ec))
        mSegments.push_back(IndexSegment(sPath + ".1"));
    for (const auto& [nNumber, sSegment] : vNumbered)
    {
        mSegments.push_back(IndexSegment(sSegment));
        mnNextSegment = nNumber + 1;
    }

    // Seal BEFORE reading, so an oversized active file is not read in full only to be retired.
    std::error_code ecSize;
    const auto nSize = fs::file_size(sPath, ecSize);
    if (!ecSize && mConfig.mnMaxBytes != 0 && nSize >= mConfig.mnMaxBytes)
    {
        std::string sTo = SegmentPath(sPath, mnNextSegment);
        while (fs::exists(sTo, ec))
            sTo = SegmentPath(sPath, ++mnNextSegment);

        std::error_code ecMove;
        fs::rename(sPath, sTo, ecMove);
        if (!ecMove)
        {
            mSegments.push_back(IndexSegment(sTo));
            ++mnNextSegment;
        }
    }

    // The counter continues from the highest seq ANY file reached. Reading it from the sealed
    // segments too means a fresh active file - just sealed, or removed by hand - still cannot
    // reissue a number an older entry answers to, which /history/restore could not tell apart.
    for (const HistorySegment& seg : mSegments)
        if (seg.mnLastSeq >= mnNextSeq)
            mnNextSeq = seg.mnLastSeq + 1;

    // Seed memory from the newest sealed segment as well when the active file alone cannot fill the
    // window - otherwise every seal, and every restart after one, would leave the History view
    // looking nearly empty while the entries sit one file over.
    if (!mSegments.empty())
    {
        size_t nActiveLines = 0;
        ForEachLine(sPath, [&](const std::string&) { return ++nActiveLines < mConfig.mnMemory; });
        if (nActiveLines < mConfig.mnMemory)
            Locked_Load(mSegments.back().msPath);
    }
    Locked_Load(sPath);

    mpFile = std::fopen(sPath.c_str(), "ab");
    if (!mpFile)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    std::error_code ecNow;
    const auto nNow = fs::file_size(sPath, ecNow);
    mnBytes = ecNow ? 0 : static_cast<uint64_t>(nNow);

    mbRunning = true;
    lock.unlock();
    rot.unlock();

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

        bool bSeal = false;
        {
            std::unique_lock lock(mMutex);
            if (!mpFile)
                return;
            for (const std::string& sLine : vBatch)
            {
                std::fwrite(sLine.data(), 1, sLine.size(), static_cast<FILE*>(mpFile));
                mnBytes += sLine.size();
            }
            std::fflush(static_cast<FILE*>(mpFile));
            bSeal = mConfig.mnMaxBytes != 0 && mnBytes >= mConfig.mnMaxBytes;
        }

        // Sealed here, while running, and only ever between whole batches - so a file never ends
        // part-way through a line and a transaction is never split by anything but a file boundary,
        // which every reader already crosses. Waiting for a restart instead let one file grow without
        // limit on a service that stays up for months.
        if (bSeal)
        {
            std::unique_lock rot(mRotateMutex);
            std::string sSealed;
            {
                std::unique_lock lock(mMutex);
                if (mpFile)
                    sSealed = Locked_Seal();
            }
            if (!sSealed.empty())
            {
                // Indexed outside mMutex, which OnPut needs under the store's write lock. Nothing can
                // look for the segment in between: every scan waits on mRotateMutex, held here.
                HistorySegment seg = IndexSegment(sSealed);
                std::unique_lock lock(mMutex);
                mSegments.push_back(std::move(seg));
            }
        }
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

uint64_t History::Locked_StampTransaction(uint64_t nSeq)
{
    if (!mbInTxn)
        return 0;
    if (mnOpenTxnID == 0)
        mnOpenTxnID = nSeq;   // the group is named after its first entry - see HistoryEntry::mnTxnID
    return mnOpenTxnID;
}

void History::BeginTransaction()
{
    std::unique_lock lock(mMutex);
    mbInTxn     = true;
    mnOpenTxnID = 0;
}

uint64_t History::EndTransaction()
{
    std::unique_lock lock(mMutex);
    const uint64_t nTxnID = mnOpenTxnID;
    mbInTxn     = false;
    mnOpenTxnID = 0;
    // 0 when the bracketed operation logged nothing - the caller asked for a group and there was no
    // group, which is not an error and must not come back as a handle to something that never was.
    return nTxnID;
}

void History::OnPut(const FlatJot& jot)
{
    HistoryEntry entry;
    entry.msName   = jot.msName;
    entry.msOrigin = jot.msOrigin;
    entry.msEditor = jot.msEditor.empty() ? std::string("user") : jot.msEditor;
    const std::string sPlainCaption = Clip(jot.msSummary.empty() ? jot.msText : jot.msSummary, 110);

    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;
        entry.mnSeq   = mnNextSeq++;
        entry.mnTxnID = Locked_StampTransaction(entry.mnSeq);

        const auto it = mLastSeen.find(jot.mID);
        const std::string sDiff = DescribeChange(it != mLastSeen.end() ? &it->second : nullptr, jot);
        entry.msSummary = sDiff.empty() ? sPlainCaption : sDiff;

        // So a later delete of this same id can still be listed as "deleted <slug>: <summary>", and
        // so the NEXT put for this id has something to diff against.
        mLastSeen[jot.mID] = { entry.msName, entry.msEditor, jot.msSummary, sPlainCaption,
                               entry.msOrigin, jot.mTags, jot.msText.size() };
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mID      = jot.mID;
    entry.msRecord = JOTJSON::ToJson(jot, false);

    Append(entry, PutLine(entry.mnSeq, entry.mnAtUS, entry.mnTxnID, entry.msRecord));
}

void History::OnDelete(tJotID id)
{
    HistoryEntry entry;
    {
        std::unique_lock lock(mMutex);
        if (!mbRunning)
            return;
        entry.mnSeq   = mnNextSeq++;
        entry.mnTxnID = Locked_StampTransaction(entry.mnSeq);
        const auto it = mLastSeen.find(id);
        if (it != mLastSeen.end())
        {
            entry.msName    = it->second.msName;
            entry.msEditor  = it->second.msEditor;
            entry.msOrigin  = it->second.msOrigin;
            entry.msSummary = it->second.msCaption;   // the display line, not the raw diff field
        }
    }

    entry.mnAtUS   = LOOMTIME::NowMicros();
    entry.mbDelete = true;
    entry.mID      = id;

    Append(entry, DelLine(entry.mnSeq, entry.mnAtUS, id, entry.msName, entry.msEditor,
                          entry.msSummary, entry.msOrigin, entry.mnTxnID));
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
            //
            // So is a multi-record operation, and for the same reason. A tag merge that rewrites a
            // jot somebody was editing minutes earlier is not part of that editing session; folding
            // the two would hide the merge behind an "edited" caption and leave its transaction id
            // attached to a row that also stands for writes the merge had nothing to do with -
            // which is the one thing the id must never mean. Comparing the ids rather than testing
            // for zero also keeps two different operations apart, without special-casing either.
            const bool bMergeable = !entry.mbDelete && !run.mbDelete
                                 && entry.msEditor == run.msEditor
                                 && entry.mnTxnID  == run.mnTxnID
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

void History::ListTransaction(uint64_t nTxnID, std::vector<HistoryEntry>& outEntries) const
{
    outEntries.clear();
    if (nTxnID == 0)
        return;

    {
        std::unique_lock lock(mMutex);

        // Oldest first, which is also apply order - a group that touched one jot twice must be
        // replayed in the order it happened or the older state would win.
        bool bHasFirst = false;
        for (const HistoryEntry& e : mRecent)
        {
            if (e.mnTxnID != nTxnID)
            {
                // The group is a contiguous run of seqs, so the first entry past it ends the search.
                if (!outEntries.empty())
                    break;
                continue;
            }
            if (e.mnSeq == nTxnID)
                bHasFirst = true;
            outEntries.push_back(e);
        }

        // Holding the group's FIRST entry proves the memory window reaches back past its start, so
        // what is here is all of it. Without that entry the window begins somewhere inside the group
        // - or misses it entirely - and the file is the only complete copy.
        if (bHasFirst)
            return;
    }

    outEntries.clear();
    ScanFileForTransaction(nTxnID, outEntries);
}

void History::ScanFileForTransaction(uint64_t nTxnID, std::vector<HistoryEntry>& outEntries) const
{
    std::shared_lock rot(mRotateMutex);

    // Oldest first, from the segment holding the group's first entry. A group can straddle a segment
    // boundary, so reaching the end of a file that held members does not end the search; seeing a
    // later seq that is not in the group does - the group is a contiguous run.
    for (const std::string& sFile : FilesSpanning(nTxnID, UINT64_MAX))
    {
        bool bDone = false;
        ForEachLine(sFile, [&](const std::string& sLine)
        {
            HistoryEntry entry;
            if (!ParseLine(sLine, entry))
                return true;
            if (entry.mnTxnID == nTxnID)
                outEntries.push_back(std::move(entry));
            else if (entry.mnSeq > nTxnID)
                bDone = true;
            return !bDone;
        });
        if (bDone)
            break;
    }
}

bool History::ScanFileFor(uint64_t nSeq, HistoryEntry& outEntry) const
{
    std::shared_lock rot(mRotateMutex);

    // Linear within a file, but the index has already narrowed it to the one segment whose seq range
    // holds nSeq. This only runs for an entry older than the in-memory window - a manual act.
    bool bFound = false;
    for (const std::string& sFile : FilesSpanning(nSeq, nSeq))
    {
        ForEachLine(sFile, [&](const std::string& sLine)
        {
            HistoryEntry entry;
            if (ParseLine(sLine, entry) && entry.mnSeq == nSeq)
            {
                outEntry = std::move(entry);
                bFound   = true;
            }
            return !bFound;
        });
        if (bFound)
            return true;
    }
    return false;
}

bool History::ScanFileForPrevious(uint64_t nSeq, tJotID id, HistoryEntry& outEntry) const
{
    std::shared_lock rot(mRotateMutex);

    // NEWEST file first. Lines are in increasing seq order within and across files, so the last
    // match below nSeq in the newest file that has one is the entry immediately before it - and the
    // search stops there instead of reading every year of history behind it.
    const std::vector<std::string> vFiles = FilesSpanning(1, nSeq);
    for (auto it = vFiles.rbegin(); it != vFiles.rend(); ++it)
    {
        bool bFound = false;
        ForEachLine(*it, [&](const std::string& sLine)
        {
            HistoryEntry entry;
            if (!ParseLine(sLine, entry))
                return true;
            if (entry.mnSeq >= nSeq)
                return false;   // nothing earlier follows in this file
            if (entry.mID == id && !entry.mbDelete)
            {
                outEntry = std::move(entry);
                bFound   = true;
            }
            return true;
        });
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
    st.mnSegments = mSegments.size();
    for (const HistorySegment& seg : mSegments)
        st.mnSealedBytes += seg.mnBytes;
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
