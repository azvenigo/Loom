#include "persist/Snapshot.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

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

namespace SNAPSHOT
{
    std::error_code Write(const SnapshotConfig& config, const JotStore& store, Journal* pJournal,
                          size_t& outRecords)
    {
        outRecords = 0;

        if (config.msPath.empty())
            return MakeLoomError(eLoomErr::kInvalidArgument);

        // Everything queued before this point must reach the log first. Otherwise a mutation that
        // happened before the snapshot could still be sitting in the queue when the log is
        // truncated below - and it is in neither file.
        if (pJournal)
        {
            if (std::error_code ec = pJournal->Flush())
                return ec;
        }

        // One consistent point in time. FlattenAll takes the read lock once for the whole store, so
        // the file cannot be a smear of records from before and after a concurrent write.
        std::vector<FlatJot> vAll;
        store.FlattenAll(vAll);

        const std::string sTmp = config.msPath + ".tmp";

        FILE* pFile = std::fopen(sTmp.c_str(), "wb");
        if (!pFile)
            return MakeLoomError(eLoomErr::kReadOnly);

        for (const FlatJot& jot : vAll)
        {
            const std::string sLine = JOTJSON::ToJson(jot, false) + "\n";
            if (std::fwrite(sLine.data(), 1, sLine.size(), pFile) != sLine.size())
            {
                std::fclose(pFile);
                std::remove(sTmp.c_str());
                return MakeLoomError(eLoomErr::kReadOnly);
            }
            ++outRecords;
        }

        // Step 2: the bytes must be durable BEFORE the rename publishes them, or a crash can leave
        // the name pointing at a file whose contents never made it to disk.
        std::fflush(pFile);
        if (LOOM_COMMIT(LOOM_FILENO(pFile)) != 0)
        {
            std::fclose(pFile);
            std::remove(sTmp.c_str());
            return MakeLoomError(eLoomErr::kReadOnly);
        }
        std::fclose(pFile);

        // Step 3: the commit point. Windows rename() fails if the destination exists, unlike POSIX,
        // so the old snapshot is moved aside first and only removed once the new one is in place.
        const std::string sOld = config.msPath + ".old";
        std::remove(sOld.c_str());
        std::rename(config.msPath.c_str(), sOld.c_str());   // fine if absent - first snapshot

        if (std::rename(sTmp.c_str(), config.msPath.c_str()) != 0)
        {
            // Put the previous snapshot back rather than leaving the store with none.
            std::rename(sOld.c_str(), config.msPath.c_str());
            std::remove(sTmp.c_str());
            return MakeLoomError(eLoomErr::kReadOnly);
        }
        std::remove(sOld.c_str());

        // Step 4: only now is the log redundant.
        if (pJournal)
        {
            if (std::error_code ec = pJournal->Truncate())
                return ec;
            pJournal->NoteSnapshot(LOOMTIME::NowMicros());
        }

        return LoomOK();
    }

    std::error_code Load(const SnapshotConfig& config, JotStore& store,
                         size_t& outLoaded, size_t& outReplayed, size_t& outDropped)
    {
        outLoaded   = 0;
        outReplayed = 0;
        outDropped  = 0;

        // --- snapshot ---
        if (!config.msPath.empty())
        {
            FILE* pFile = std::fopen(config.msPath.c_str(), "rb");
            if (pFile)
            {
                std::vector<FlatJot> vBatch;
                std::string sLine;
                int ch = 0;

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
                    if (JOTJSON::ParseFlat(sLine, flat, sError))
                        vBatch.push_back(std::move(flat));
                    else
                        ++outDropped;

                    sLine.clear();
                }

                // A snapshot is written tmp-then-renamed, so it should never be torn. If the last
                // line has no newline anyway, something is wrong with the file - count it rather
                // than guess at it.
                if (!sLine.empty())
                {
                    FlatJot flat;
                    std::string sError;
                    if (JOTJSON::ParseFlat(sLine, flat, sError))
                        vBatch.push_back(std::move(flat));
                    else
                        ++outDropped;
                }

                std::fclose(pFile);

                store.LoadFlatBatch(vBatch, outLoaded);
            }
        }

        // --- log written since the snapshot ---
        if (!config.msWalPath.empty())
        {
            size_t nDroppedWal = 0;
            if (std::error_code ec = Journal::Replay(config.msWalPath, store, outReplayed, nDroppedWal))
                return ec;
            outDropped += nDroppedWal;
        }

        return LoomOK();
    }
}
