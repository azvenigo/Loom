// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/RunLedger.h"

#include "core/LoomError.h"

#include "vendor/json.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    int64_t NowUS()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Same shape as every other small config file in Loom - see WatchList.cpp's identical helper.
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

    std::error_code WriteWholeFileAtomic(const std::string& sPath, const std::string& sBody)
    {
        if (sPath.empty())
            return LoomOK();

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
        return ecMove ? MakeLoomError(eLoomErr::kInvalidArgument) : LoomOK();
    }

    json RunToJson(const TriageRun& r)
    {
        json j;
        j["at"]                    = r.mnAtUS;
        j["duration_ms"]           = r.mnDurationMS;
        j["cost_usd"]              = r.mfCostUSD;
        j["input_tokens"]          = r.mnInputTokens;
        j["output_tokens"]         = r.mnOutputTokens;
        j["cache_read_tokens"]     = r.mnCacheReadTokens;
        j["cache_creation_tokens"] = r.mnCacheCreationTokens;
        j["jots_given"]            = r.mnJotsGiven;
        j["success"]               = r.mbSuccess;
        if (!r.msSessionID.empty()) j["session_id"] = r.msSessionID;
        if (!r.msTrigger.empty())   j["trigger"]    = r.msTrigger;
        if (!r.msFailure.empty())   j["failure"]    = r.msFailure;
        return j;
    }

    TriageRun RunFromJson(const json& j)
    {
        TriageRun r;
        r.mnAtUS                = j.value("at", (int64_t)0);
        r.mnDurationMS          = j.value("duration_ms", (int64_t)0);
        r.mfCostUSD             = j.value("cost_usd", 0.0);
        r.mnInputTokens         = j.value("input_tokens", (uint64_t)0);
        r.mnOutputTokens        = j.value("output_tokens", (uint64_t)0);
        r.mnCacheReadTokens     = j.value("cache_read_tokens", (uint64_t)0);
        r.mnCacheCreationTokens = j.value("cache_creation_tokens", (uint64_t)0);
        r.mnJotsGiven           = j.value("jots_given", (size_t)0);
        r.mbSuccess             = j.value("success", false);
        r.msSessionID           = j.value("session_id", std::string());
        r.msTrigger             = j.value("trigger", std::string());
        r.msFailure             = j.value("failure", std::string());
        return r;
    }
}

std::error_code RunLedger::Load(const std::string& sPath, std::string& outWarning)
{
    outWarning.clear();

    std::lock_guard lock(mMutex);
    msPath = sPath;
    mRuns.clear();
    mnLifetimeRuns    = 0;
    mfLifetimeCostUSD = 0.0;

    std::string sBody;
    if (!ReadWholeFile(sPath, sBody))
        return LoomOK();   // first run - nothing to warn about

    json j = json::parse(sBody, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        outWarning = "triage run ledger at " + sPath + " is unreadable - starting empty "
                     "(usage history is lost, but the daily spend guard fails safe: an unreadable "
                     "ledger reads as zero spend, never as unlimited)";
        return LoomOK();
    }

    mnLifetimeRuns    = j.value("lifetime_runs", (size_t)0);
    mfLifetimeCostUSD = j.value("lifetime_cost_usd", 0.0);
    if (j.contains("runs") && j["runs"].is_array())
    {
        for (const auto& r : j["runs"])
            mRuns.push_back(RunFromJson(r));
    }
    return LoomOK();
}

std::error_code RunLedger::SaveLocked() const
{
    json j;
    j["lifetime_runs"]     = mnLifetimeRuns;
    j["lifetime_cost_usd"] = mfLifetimeCostUSD;
    json arr = json::array();
    for (const TriageRun& r : mRuns)
        arr.push_back(RunToJson(r));
    j["runs"] = std::move(arr);
    return WriteWholeFileAtomic(msPath, j.dump(2));
}

std::error_code RunLedger::Record(const TriageRun& run)
{
    std::lock_guard lock(mMutex);
    mRuns.push_back(run);
    while (mRuns.size() > kMaxRing)
        mRuns.pop_front();

    ++mnLifetimeRuns;
    mfLifetimeCostUSD += run.mfCostUSD;

    return SaveLocked();
}

double RunLedger::Sum24hUSD() const
{
    std::lock_guard lock(mMutex);
    const int64_t nCutoffUS = NowUS() - 24LL * 3600 * 1000000;
    double fSum = 0.0;
    for (const TriageRun& r : mRuns)
        if (r.mnAtUS >= nCutoffUS)
            fSum += r.mfCostUSD;
    return fSum;
}

size_t RunLedger::Count24h() const
{
    std::lock_guard lock(mMutex);
    const int64_t nCutoffUS = NowUS() - 24LL * 3600 * 1000000;
    size_t nCount = 0;
    for (const TriageRun& r : mRuns)
        if (r.mnAtUS >= nCutoffUS)
            ++nCount;
    return nCount;
}

TriageStats RunLedger::Stats(bool bBreakerOpen, int nConsecutiveFailures, bool bBudgetExceeded,
                            size_t nRunsThisProcess) const
{
    std::lock_guard lock(mMutex);

    TriageStats out;
    out.mbBreakerOpen         = bBreakerOpen;
    out.mnConsecutiveFailures = nConsecutiveFailures;
    out.mbBudgetExceeded      = bBudgetExceeded;
    out.mnRunsThisProcess     = nRunsThisProcess;
    out.mnRunsLifetime        = mnLifetimeRuns;
    out.mfCostUSDLifetime     = mfLifetimeCostUSD;

    if (!mRuns.empty())
    {
        out.mbHasLastRun = true;
        out.mLastRun     = mRuns.back();
    }

    const int64_t nCutoffUS = NowUS() - 24LL * 3600 * 1000000;
    for (const TriageRun& r : mRuns)
    {
        if (r.mnAtUS < nCutoffUS)
            continue;
        ++out.mnRuns24h;
        out.mfCostUSD24h      += r.mfCostUSD;
        out.mnInputTokens24h  += r.mnInputTokens;
        out.mnOutputTokens24h += r.mnOutputTokens;
    }
    return out;
}

std::vector<TriageRun> RunLedger::Recent(size_t nLimit) const
{
    std::lock_guard lock(mMutex);
    std::vector<TriageRun> out;
    const size_t nCount = mRuns.size() < nLimit ? mRuns.size() : nLimit;
    out.reserve(nCount);
    // Newest first.
    for (auto it = mRuns.rbegin(); it != mRuns.rend() && out.size() < nCount; ++it)
        out.push_back(*it);
    return out;
}
