// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/Importer.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

#include "vendor/json.hpp"

#include <cstdio>
#include <ctime>
#include <set>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace IMPORT
{
    bool ParseLocalTimestamp(const std::string& sTS, int64_t& outUS)
    {
        int nY = 0, nMo = 0, nD = 0, nH = 0, nMi = 0, nS = 0;
        const int nFields = std::sscanf(sTS.c_str(), "%d-%d-%d %d:%d:%d",
                                        &nY, &nMo, &nD, &nH, &nMi, &nS);
        if (nFields < 3)
            return false;
        if (nMo < 1 || nMo > 12 || nD < 1 || nD > 31)
            return false;

        std::tm tm{};
        tm.tm_year  = nY - 1900;
        tm.tm_mon   = nMo - 1;
        tm.tm_mday  = nD;
        tm.tm_hour  = nH;
        tm.tm_min   = nMi;
        tm.tm_sec   = nS;

        // -1 means "work out DST yourself". These stamps have no offset recorded, so the machine's
        // rules for that date are the only information available - and they are the right ones,
        // since the entries were written on this machine in the first place.
        tm.tm_isdst = -1;

        const std::time_t t = std::mktime(&tm);
        if (t == static_cast<std::time_t>(-1))
            return false;

        outUS = static_cast<int64_t>(t) * LOOMTIME::kMicrosPerSecond;
        return true;
    }

    std::error_code JotsLog(const std::string& sPath, JotStore& store,
                            const std::string& sEditor, ImportStats& outStats)
    {
        outStats = ImportStats();

        FILE* pFile = std::fopen(sPath.c_str(), "rb");
        if (!pFile)
            return MakeLoomError(eLoomErr::kNotFound);

        std::vector<FlatJot> vBatch;
        std::string sLine;

        // Ids claimed by THIS batch. Checking only the store is not enough: records here have not
        // been inserted yet, so two entries written in the same second would both see the id as
        // free, and the second would overwrite the first on load.
        std::set<tJotID> claimed;

        const auto Consume = [&](const std::string& sText)
        {
            if (sText.empty())
                return;

            ++outStats.mnLines;

            json j = json::parse(sText, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("ts") || !j.contains("entry"))
            {
                ++outStats.mnMalformed;
                return;
            }
            if (!j["ts"].is_string() || !j["entry"].is_string())
            {
                ++outStats.mnMalformed;
                return;
            }

            int64_t nUS = 0;
            if (!ParseLocalTimestamp(j["ts"].get<std::string>(), nUS))
            {
                ++outStats.mnMalformed;
                return;
            }

            const std::string sEntry = j["entry"].get<std::string>();
            if (sEntry.empty())
            {
                ++outStats.mnMalformed;
                return;
            }

            // Walk forward off any id already taken. The log has second resolution, so two entries
            // written in the same second land on the same microsecond; and on a re-import every id
            // is already present, which is what makes the whole operation idempotent.
            Jot existing;
            bool bBumped = false;
            for (;;)
            {
                if (store.Get(nUS, existing))
                {
                    if (existing.msText == sEntry)
                    {
                        // Same instant AND same text: this exact line was imported before.
                        ++outStats.mnSkipped;
                        return;
                    }
                }
                else if (claimed.count(nUS) == 0)
                {
                    break;
                }
                ++nUS;
                bBumped = true;
            }
            claimed.insert(nUS);
            if (bBumped)
                ++outStats.mnBumped;

            FlatJot flat;
            flat.mID      = nUS;
            flat.msText   = sEntry;
            flat.msEditor = sEditor;   // empty resolves to "user"

            if (outStats.mnOldestUS == 0 || nUS < outStats.mnOldestUS) outStats.mnOldestUS = nUS;
            if (nUS > outStats.mnNewestUS)                             outStats.mnNewestUS = nUS;

            vBatch.push_back(std::move(flat));
            ++outStats.mnImported;
        };

        int ch = 0;
        while ((ch = std::fgetc(pFile)) != EOF)
        {
            if (ch == '\n')
            {
                // Tolerate CRLF: the file is written by a .NET app on Windows.
                if (!sLine.empty() && sLine.back() == '\r')
                    sLine.pop_back();
                Consume(sLine);
                sLine.clear();
                continue;
            }
            sLine.push_back(static_cast<char>(ch));
        }
        if (!sLine.empty() && sLine.back() == '\r')
            sLine.pop_back();
        Consume(sLine);   // last line may have no trailing newline

        std::fclose(pFile);

        // One batch, one lock. Note this goes through the journal like any other write, so an
        // import is durable the moment the committer catches up - no special casing.
        // bJournal = true: these records are new and exist nowhere on disk yet, unlike a replay.
        size_t nLoaded = 0;
        return store.LoadFlatBatch(vBatch, nLoaded, true);
    }
}
