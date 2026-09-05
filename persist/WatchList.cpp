// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/WatchList.h"

#include "vendor/json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    std::string Trim(const std::string& s)
    {
        size_t nA = 0, nB = s.size();
        while (nA < nB && (s[nA] == ' ' || s[nA] == '\t')) ++nA;
        while (nB > nA && (s[nB - 1] == ' ' || s[nB - 1] == '\t')) --nB;
        return s.substr(nA, nB - nA);
    }

    // false leaves out* untouched, which callers treat as "does not exist" - a file that vanished
    // between a directory read and this call is not a reason to fail the whole request.
    bool StatFile(const std::string& sPath, uint64_t& outSizeBytes, int64_t& outMtimeUS)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(sPath, ec) || ec)
            return false;

        const uintmax_t nSize = std::filesystem::file_size(sPath, ec);
        if (ec)
            return false;

        const auto fTime = std::filesystem::last_write_time(sPath, ec);
        if (ec)
            return false;

        // last_write_time is file_clock, not system_clock - clock_cast is the C++20 way to line
        // the two up so the value can be compared and stored the same way every other Loom
        // timestamp is (microseconds since the Unix epoch).
        const auto sysTime = std::chrono::clock_cast<std::chrono::system_clock>(fTime);
        outSizeBytes = static_cast<uint64_t>(nSize);
        outMtimeUS   = std::chrono::duration_cast<std::chrono::microseconds>(
                          sysTime.time_since_epoch()).count();
        return true;
    }

    // Whole-file read, same shape as every other small config file in Loom (see IpAcl::Load).
    bool ReadWholeFile(const std::string& sPath, std::string& outBody)
    {
        FILE* pFile = std::fopen(sPath.c_str(), "rb");
        if (!pFile)
            return false;

        outBody.clear();
        char buf[4096];
        size_t nRead = 0;
        while ((nRead = std::fread(buf, 1, sizeof(buf), pFile)) > 0)
            outBody.append(buf, nRead);
        std::fclose(pFile);
        return true;
    }

    // tmp -> rename, the same commit discipline as IpAcl::SaveLocked and the snapshot writer.
    std::error_code WriteWholeFileAtomic(const std::string& sPath, const std::string& sBody)
    {
        if (sPath.empty())
            return LoomOK();   // RAM-only instance; nothing to persist to

        const std::string sTmp = sPath + ".tmp";
        FILE* pFile = std::fopen(sTmp.c_str(), "wb");
        if (!pFile)
            return MakeLoomError(eLoomErr::kInvalidArgument);

        const bool bWrote = std::fwrite(sBody.data(), 1, sBody.size(), pFile) == sBody.size();
        std::fclose(pFile);

        if (!bWrote)
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
}


//====================================================================================================
// Lifecycle
//====================================================================================================

std::error_code WatchList::Load(const std::string& sConfigPath, const std::string& sStatePath,
                                std::string& outWarning)
{
    outWarning.clear();

    std::unique_lock lock(mMutex);
    msConfigPath = sConfigPath;
    msStatePath  = sStatePath;
    mPaths.clear();
    mState.clear();

    std::string sBody;
    if (ReadWholeFile(sConfigPath, sBody))
    {
        json j = json::parse(sBody, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            outWarning = "watch list at " + sConfigPath + " is unreadable - starting empty";
        }
        else if (j.contains("paths") && j["paths"].is_array())
        {
            for (const auto& p : j["paths"])
            {
                if (p.is_string())
                    mPaths.push_back(p.get<std::string>());
            }
        }
    }
    // A missing config file is a first run, not an error - nothing to warn about.

    if (ReadWholeFile(sStatePath, sBody))
    {
        json j = json::parse(sBody, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            // The state is only bookkeeping (see the header) - losing it just means every path
            // reads as pending again until re-ingested, which is safe. Warn about the config file
            // problem in preference to this one if both are bad.
            if (outWarning.empty())
                outWarning = "watch state at " + sStatePath + " is unreadable - "
                             "every watched path will show as pending until re-ingested";
        }
        else
        {
            for (auto it = j.begin(); it != j.end(); ++it)
            {
                if (!it.value().is_object())
                    continue;
                WatchRecord rec;
                rec.mnSizeBytes    = it.value().value("size", (uint64_t)0);
                rec.mnMtimeUS      = it.value().value("mtimeUS", (int64_t)0);
                rec.mnLastIngestUS = it.value().value("lastIngestUS", (int64_t)0);
                mState[it.key()] = rec;
            }
        }
    }

    return LoomOK();
}

std::error_code WatchList::SaveConfigLocked() const
{
    json j;
    j["paths"] = mPaths;
    return WriteWholeFileAtomic(msConfigPath, j.dump(2));
}

std::error_code WatchList::SaveStateLocked() const
{
    json j = json::object();
    for (const auto& [sPath, rec] : mState)
    {
        json entry;
        entry["size"]         = rec.mnSizeBytes;
        entry["mtimeUS"]       = rec.mnMtimeUS;
        entry["lastIngestUS"] = rec.mnLastIngestUS;
        j[sPath] = std::move(entry);
    }
    return WriteWholeFileAtomic(msStatePath, j.dump(2));
}


//====================================================================================================
// Editing
//====================================================================================================

void WatchList::Get(std::vector<std::string>& outPaths) const
{
    std::shared_lock lock(mMutex);
    outPaths = mPaths;
}

std::error_code WatchList::Set(const std::vector<std::string>& paths, std::string& outError)
{
    outError.clear();

    std::vector<std::string> vKept;
    for (const std::string& sRaw : paths)
    {
        const std::string sPath = Trim(sRaw);
        if (sPath.empty())
            continue;
        // First occurrence wins - a later duplicate is silently dropped rather than rejected,
        // since it changes nothing about what ends up watched.
        bool bDup = false;
        for (const std::string& sExisting : vKept)
        {
            if (sExisting == sPath) { bDup = true; break; }
        }
        if (!bDup)
            vKept.push_back(sPath);
    }

    std::unique_lock lock(mMutex);
    mPaths.swap(vKept);
    std::error_code ec = SaveConfigLocked();
    if (ec)
        outError = "could not write " + msConfigPath + ": " + ec.message();
    return ec;
}

std::vector<WatchStatus> WatchList::Resolve() const
{
    std::shared_lock lock(mMutex);

    std::vector<WatchStatus> vOut;
    vOut.reserve(mPaths.size());
    for (const std::string& sPath : mPaths)
    {
        WatchStatus status;
        status.msPath = sPath;

        const auto it = mState.find(sPath);
        if (it != mState.end())
            status.mnLastIngestUS = it->second.mnLastIngestUS;

        uint64_t nSizeBytes = 0;
        int64_t  nMtimeUS   = 0;
        if (StatFile(sPath, nSizeBytes, nMtimeUS))
        {
            status.mbExists    = true;
            status.mnSizeBytes = nSizeBytes;
            status.mnMtimeUS   = nMtimeUS;

            if (it == mState.end())
            {
                status.mbPending = true;   // never ingested
            }
            else
            {
                status.mbPending = nSizeBytes != it->second.mnSizeBytes &&
                                   nMtimeUS   != it->second.mnMtimeUS;
            }
        }

        vOut.push_back(std::move(status));
    }
    return vOut;
}

std::error_code WatchList::MarkIngested(const std::string& sPath, uint64_t nSizeBytes,
                                        int64_t nMtimeUS)
{
    std::unique_lock lock(mMutex);

    WatchRecord& rec = mState[sPath];
    rec.mnSizeBytes    = nSizeBytes;
    rec.mnMtimeUS      = nMtimeUS;
    rec.mnLastIngestUS = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();

    return SaveStateLocked();
}
