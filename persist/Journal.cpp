#include "persist/Journal.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
  #include <io.h>
  #define LOOM_FILENO  _fileno
  #define LOOM_COMMIT  _commit
#else
  #include <unistd.h>
  #define LOOM_FILENO  fileno
  #define LOOM_COMMIT  fsync
#endif

namespace
{
    FILE* AsFile(void* p) { return static_cast<FILE*>(p); }

    std::error_code SystemFail(eLoomErr e) { return MakeLoomError(e); }
}


Journal::Journal() = default;

Journal::~Journal()
{
    Close();
}

std::error_code Journal::Open(const JournalConfig& config)
{
    Close();

    mConfig = config;
    if (mConfig.msPath.empty())
        return SystemFail(eLoomErr::kInvalidArgument);

    // Append mode, binary so the runtime does not rewrite newlines - a WAL is a byte log, and a
    // \n silently becoming \r\n would make every offset and every replay parser subtly wrong.
    FILE* pFile = std::fopen(mConfig.msPath.c_str(), "ab");
    if (!pFile)
        return SystemFail(eLoomErr::kInvalidArgument);

    mpFile = pFile;

    std::fseek(pFile, 0, SEEK_END);
    const long nSize = std::ftell(pFile);
    mnBytes.store(nSize > 0 ? static_cast<uint64_t>(nSize) : 0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        mbRunning  = true;
        mbDraining = false;
    }
    mCommitter = std::thread([this]() { CommitterLoop(); });

    return LoomOK();
}

void Journal::Close()
{
    {
        std::lock_guard<std::mutex> lock(mQueueMutex);
        if (!mbRunning && !mCommitter.joinable())
            return;
        mbRunning = false;
    }
    mQueueCV.notify_all();

    if (mCommitter.joinable())
        mCommitter.join();

    if (mpFile)
    {
        std::fflush(AsFile(mpFile));
        LOOM_COMMIT(LOOM_FILENO(AsFile(mpFile)));
        std::fclose(AsFile(mpFile));
        mpFile = nullptr;
    }
}

//====================================================================================================
// Sink - runs under the store's write lock. Serialize, enqueue, return.
//====================================================================================================

void Journal::OnPut(const FlatJot& jot)
{
    if (!mpFile)
        return;

    std::string sLine = "{\"op\":\"put\",\"jot\":" + JOTJSON::ToJson(jot, false) + "}\n";

    std::unique_lock<std::mutex> lock(mQueueMutex);

    // Back-pressure rather than an unbounded queue. Blocking the writer is unpleasant; running the
    // process out of memory because the disk fell behind is worse, and much harder to diagnose.
    mDrainedCV.wait(lock, [this]() { return mQueue.size() < mConfig.mnMaxQueue || !mbRunning; });

    mQueue.push_back(std::move(sLine));
    lock.unlock();
    mQueueCV.notify_one();
}

void Journal::OnDelete(tJotID id)
{
    if (!mpFile)
        return;

    std::string sLine = "{\"op\":\"del\",\"id\":" + std::to_string(id) + "}\n";

    std::unique_lock<std::mutex> lock(mQueueMutex);
    mDrainedCV.wait(lock, [this]() { return mQueue.size() < mConfig.mnMaxQueue || !mbRunning; });
    mQueue.push_back(std::move(sLine));
    lock.unlock();
    mQueueCV.notify_one();
}

//====================================================================================================
// Committer
//====================================================================================================

std::error_code Journal::AppendLocked(const std::string& sLine)
{
    if (!mpFile)
        return SystemFail(eLoomErr::kReadOnly);

    if (std::fwrite(sLine.data(), 1, sLine.size(), AsFile(mpFile)) != sLine.size())
        return SystemFail(eLoomErr::kReadOnly);

    mnAppended.fetch_add(1, std::memory_order_relaxed);
    mnBytes.fetch_add(sLine.size(), std::memory_order_relaxed);
    return LoomOK();
}

std::error_code Journal::SyncNow()
{
    if (!mpFile)
        return LoomOK();

    std::fflush(AsFile(mpFile));
    if (LOOM_COMMIT(LOOM_FILENO(AsFile(mpFile))) != 0)
        return SystemFail(eLoomErr::kReadOnly);

    mnSynced.fetch_add(1, std::memory_order_relaxed);
    return LoomOK();
}

void Journal::CommitterLoop()
{
    auto tLastSync = std::chrono::steady_clock::now();

    for (;;)
    {
        std::vector<std::string> vBatch;

        {
            std::unique_lock<std::mutex> lock(mQueueMutex);

            // A timed wait rather than an indefinite one, so an interval fsync still happens on a
            // store that has gone quiet - otherwise the last few writes before an idle period
            // would sit unsynced indefinitely.
            mQueueCV.wait_for(lock, std::chrono::milliseconds(mConfig.mnSyncIntervalMS),
                              [this]() { return !mQueue.empty() || !mbRunning || mbDraining; });

            // Drain the whole queue in one go: batching is what turns N small writes into one
            // syscall and one fsync.
            while (!mQueue.empty())
            {
                vBatch.push_back(std::move(mQueue.front()));
                mQueue.pop_front();
            }

            const bool bStopping = !mbRunning;
            lock.unlock();
            mDrainedCV.notify_all();

            if (vBatch.empty() && bStopping)
                break;
        }

        for (const std::string& sLine : vBatch)
            AppendLocked(sLine);

        const auto tNow = std::chrono::steady_clock::now();
        const bool bIntervalElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tLastSync).count()
                >= mConfig.mnSyncIntervalMS;

        if (mConfig.mSync == eSyncPolicy::kAlways
            || (mConfig.mSync == eSyncPolicy::kInterval && bIntervalElapsed && !vBatch.empty()))
        {
            SyncNow();
            tLastSync = tNow;
        }
        else if (!vBatch.empty())
        {
            // Even with fsync deferred, get the bytes out of our buffer and into the OS - a crash
            // of this process (as opposed to the machine) then loses nothing.
            std::fflush(AsFile(mpFile));
        }

        {
            std::lock_guard<std::mutex> lock(mQueueMutex);
            if (mbDraining && mQueue.empty())
                mbDraining = false;
        }
        mDrainedCV.notify_all();
    }

    SyncNow();
}

std::error_code Journal::Flush()
{
    if (!mpFile)
        return LoomOK();

    {
        std::unique_lock<std::mutex> lock(mQueueMutex);
        if (!mbRunning)
            return LoomOK();
        mbDraining = true;
        mQueueCV.notify_all();
        mDrainedCV.wait(lock, [this]() { return mQueue.empty() || !mbRunning; });
    }

    return SyncNow();
}

std::error_code Journal::Truncate()
{
    if (std::error_code ec = Flush())
        return ec;

    if (!mpFile)
        return LoomOK();

    std::fclose(AsFile(mpFile));
    mpFile = nullptr;

    // "wb" truncates. The log is only ever discarded after a snapshot has been durably renamed
    // into place, so the records it held are already safe somewhere else.
    FILE* pFile = std::fopen(mConfig.msPath.c_str(), "wb");
    if (!pFile)
        return SystemFail(eLoomErr::kReadOnly);

    mpFile = pFile;
    mnBytes.store(0, std::memory_order_relaxed);
    return LoomOK();
}

void Journal::FillStats(PersistStats& outStats) const
{
    outStats.mbEnabled  = (mpFile != nullptr);
    outStats.mnAppended = mnAppended.load(std::memory_order_relaxed);
    outStats.mnSynced   = mnSynced.load(std::memory_order_relaxed);
    outStats.mnWalBytes = mnBytes.load(std::memory_order_relaxed);
    outStats.mnSnapshots = mnSnapshots.load(std::memory_order_relaxed);
    outStats.mnLastSnapshotUS = mnLastSnapshotUS.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mQueueMutex);
    outStats.mnQueued = mQueue.size();
}

//====================================================================================================
// Replay
//====================================================================================================

std::error_code Journal::Replay(const std::string& sPath, JotStore& store,
                                size_t& outApplied, size_t& outDropped)
{
    outApplied = 0;
    outDropped = 0;

    FILE* pFile = std::fopen(sPath.c_str(), "rb");
    if (!pFile)
        return LoomOK();   // no log is not an error - it is a first run

    std::string sLine;
    std::vector<FlatJot> vPending;
    int  ch = 0;

    // Applies whatever puts have accumulated. Batched because LoadBatch takes the write lock once
    // for the whole vector, and a replay of a large log one record at a time would be all lock.
    const auto FlushPuts = [&]()
    {
        if (vPending.empty())
            return;
        size_t nLoaded = 0;
        store.LoadFlatBatch(vPending, nLoaded);
        outApplied += nLoaded;
        vPending.clear();
    };

    while ((ch = std::fgetc(pFile)) != EOF)
    {
        if (ch != '\n')
        {
            sLine.push_back(static_cast<char>(ch));
            continue;
        }

        if (sLine.empty())
            continue;

        FlatJot flat;
        std::string sError;

        // Cheap discrimination before a full parse; the two shapes are unambiguous.
        const bool bIsDelete = sLine.find("\"op\":\"del\"") != std::string::npos;

        if (bIsDelete)
        {
            const size_t nPos = sLine.find("\"id\":");
            if (nPos == std::string::npos)
            {
                ++outDropped;
            }
            else
            {
                // Deletes must be applied in order relative to the puts around them, so the
                // pending batch is flushed first. A delete that arrives before its put would
                // otherwise be silently undone.
                FlushPuts();
                const tJotID id = std::strtoll(sLine.c_str() + nPos + 5, nullptr, 10);
                store.Remove(id);
                ++outApplied;
            }
        }
        else
        {
            const size_t nPos = sLine.find("\"jot\":");
            if (nPos == std::string::npos)
            {
                ++outDropped;
            }
            else
            {
                // Trim the trailing '}' of the envelope.
                std::string sRecord = sLine.substr(nPos + 6);
                if (!sRecord.empty() && sRecord.back() == '}')
                    sRecord.pop_back();

                if (JOTJSON::ParseFlat(sRecord, flat, sError))
                    vPending.push_back(std::move(flat));
                else
                {
                    ++outDropped;
                }
            }
        }

        sLine.clear();
    }

    // Anything left in sLine is a final record with no terminating newline - i.e. a write that was
    // interrupted. That is the EXPECTED residue of a crash, so it is dropped and counted rather
    // than treated as corruption that should stop the service from starting.
    if (!sLine.empty())
        ++outDropped;

    FlushPuts();

    std::fclose(pFile);
    return LoomOK();
}
