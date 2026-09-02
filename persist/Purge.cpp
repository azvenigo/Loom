// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/Purge.h"

#include "core/JotStore.h"
#include "core/LoomTime.h"
#include "persist/History.h"
#include "persist/Snapshot.h"

#include "vendor/json.hpp"

#include <cstdio>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    bool ReadWholeFile(const std::string& sPath, std::string& outBody)
    {
        FILE* pFile = std::fopen(sPath.c_str(), "rb");
        if (!pFile)
            return false;

        char   buf[8192];
        size_t nRead = 0;
        while ((nRead = std::fread(buf, 1, sizeof(buf), pFile)) > 0)
            outBody.append(buf, nRead);
        std::fclose(pFile);
        return true;
    }

    std::error_code WriteWholeFile(const std::string& sPath, const std::string& sBody)
    {
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
}


//====================================================================================================
// The request file
//====================================================================================================

std::string PURGE::RequestPath(const std::string& sDir)
{
    return sDir + "/loom.purge-request.json";
}

std::error_code PURGE::WriteRequest(const std::string& sPath, const PurgeRequest& request)
{
    json j;
    j["created"]      = request.mnCreatedUS;
    j["created_utc"]  = LOOMTIME::FormatUS(request.mnCreatedUS);
    j["reason"]       = request.msReason;
    j["requested_by"] = request.msRequestedBy;

    j["ids"] = json::array();
    for (tJotID id : request.mIDs)
        j["ids"].push_back(id);

    // Captured now, while the jots still exist. After the purge there is nothing left to look them
    // up against, and "confirm these are the right ones" has to remain answerable at that point.
    j["jots"] = json::array();
    for (const PurgeLabel& label : request.mLabels)
    {
        json entry;
        entry["id"] = label.mID;
        if (!label.msName.empty())    entry["name"]    = label.msName;
        if (!label.msSummary.empty()) entry["summary"] = label.msSummary;
        j["jots"].push_back(entry);
    }

    return WriteWholeFile(sPath, j.dump(2) + "\n");
}

bool PURGE::ReadRequest(const std::string& sPath, PurgeRequest& outRequest, std::string& outError)
{
    outRequest = PurgeRequest();
    outError.clear();

    std::string sBody;
    if (!ReadWholeFile(sPath, sBody))
    {
        outError = "no purge request at " + sPath;
        return false;
    }

    json j = json::parse(sBody, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        outError = "purge request at " + sPath + " is not valid JSON";
        return false;
    }

    outRequest.mnCreatedUS   = j.value("created", int64_t(0));
    outRequest.msReason      = j.value("reason", std::string());
    outRequest.msRequestedBy = j.value("requested_by", std::string());

    if (j.contains("ids") && j["ids"].is_array())
    {
        for (const auto& id : j["ids"])
        {
            if (id.is_number_integer())
                outRequest.mIDs.push_back(id.get<int64_t>());
        }
    }

    if (j.contains("jots") && j["jots"].is_array())
    {
        for (const auto& entry : j["jots"])
        {
            PurgeLabel label;
            label.mID       = entry.value("id", int64_t(kInvalidJotID));
            label.msName    = entry.value("name", std::string());
            label.msSummary = entry.value("summary", std::string());
            outRequest.mLabels.push_back(label);
        }
    }

    if (outRequest.mIDs.empty())
    {
        outError = "purge request names no ids";
        return false;
    }
    return true;
}

std::error_code PURGE::ClearRequest(const std::string& sPath)
{
    std::error_code ecRm;
    std::filesystem::remove(sPath, ecRm);
    return LoomOK();
}


//====================================================================================================
// The offline half
//====================================================================================================

std::error_code PURGE::Run(const std::string& sDir, const PurgeRequest& request,
                           PurgeReport& outReport, std::string& outError)
{
    outReport = PurgeReport();
    outError.clear();

    SnapshotConfig snapConfig;
    snapConfig.msPath    = sDir + "/loom.snapshot";
    snapConfig.msWalPath = sDir + "/loom.wal";

    // Rebuild the live state exactly as a startup would: snapshot, then whatever the WAL added on
    // top. Anything less and the purge would miss a jot that only exists in the log.
    JotStore store;
    size_t nLoaded = 0, nReplayed = 0, nDropped = 0;
    if (std::error_code ec = SNAPSHOT::Load(snapConfig, store, nLoaded, nReplayed, nDropped))
    {
        outError = "could not load the store: " + ec.message();
        return ec;
    }

    for (tJotID id : request.mIDs)
    {
        if (store.Remove(id))
            ++outReport.mnNotFound;   // already gone from the live state; its copies still are not
        else
            ++outReport.mnRemovedFromStore;
    }

    // Snapshot FIRST and let rename commit it. Only once the scrubbed snapshot is the file on disk
    // is the WAL - which still holds the original text - safe to discard.
    if (std::error_code ec = SNAPSHOT::Write(snapConfig, store, nullptr, outReport.mnSnapshotRecords))
    {
        outError = "could not write the scrubbed snapshot: " + ec.message();
        return ec;
    }

    std::error_code ecSize;
    const auto nWal = std::filesystem::file_size(snapConfig.msWalPath, ecSize);
    outReport.mnWalBytesDiscarded = ecSize ? 0 : static_cast<size_t>(nWal);

    // Truncate rather than delete: an empty WAL beside a current snapshot is exactly the state a
    // clean shutdown leaves, so the next start takes its normal path.
    if (FILE* pWal = std::fopen(snapConfig.msWalPath.c_str(), "wb"))
        std::fclose(pWal);

    // Both history generations. This is the copy that a plain delete deliberately preserves, so it
    // is the one that matters most here.
    for (const std::string& sHist : { sDir + "/loom.history", sDir + "/loom.history.1" })
    {
        size_t nRemoved = 0, nKept = 0;
        if (std::error_code ec = History::PurgeFile(sHist, request.mIDs, nRemoved, nKept))
        {
            outError = "could not rewrite " + sHist + ": " + ec.message();
            return ec;
        }
        outReport.mnHistoryDropped += nRemoved;
        outReport.mnHistoryKept    += nKept;
    }

    return LoomOK();
}


//====================================================================================================

std::string PURGE::AgentInstructions(const std::string& sDir, const PurgeRequest& request)
{
    std::string s;
    s += "PURGE REQUEST - do not skip the confirmation step.\n\n";
    s += "Someone has asked to permanently erase " + std::to_string(request.mIDs.size()) +
         " jot(s) from this Loom\n"
         "instance, on the grounds that they contain something that should never have been\n"
         "written down. This is NOT a delete: it rewrites the snapshot, discards the write-ahead\n"
         "log and scrubs the history log, so it cannot be undone by anything - including the undo\n"
         "log, which is one of the things being scrubbed.\n\n";

    if (!request.msReason.empty())
        s += "Stated reason: " + request.msReason + "\n\n";

    s += "Jots named in the request:\n";
    for (const PurgeLabel& label : request.mLabels)
    {
        s += "  " + std::to_string(label.mID);
        if (!label.msName.empty())
            s += "  " + label.msName;
        if (!label.msSummary.empty())
            s += "  - " + label.msSummary;
        s += "\n";
    }

    s += "\nSteps:\n\n";
    s += "1. READ THE REQUEST BACK TO THE PERSON and get an explicit yes. Name the jots above.\n";
    s += "   The list is captured from before the purge on purpose: afterwards there is nothing\n";
    s += "   left to check it against. If they are unsure, stop - the request file is harmless\n";
    s += "   until step 3, and deleting it cancels everything.\n\n";
    s += "2. Stop the service.  systemctl stop loom   (or however it is run here)\n";
    s += "   The purge takes the same data lock the server holds, so it will refuse to run while\n";
    s += "   Loom is up rather than corrupt anything. Do not work around that.\n\n";
    s += "3. Dry run first. On its own the flag changes nothing and prints what would go:\n\n";
    s += "       loom --purge=" + sDir + "\n\n";
    s += "   Then, once that output matches what was agreed in step 1:\n\n";
    s += "       loom --purge=" + sDir + " --yes\n\n";
    s += "   That rewrites the snapshot, empties the WAL, scrubs both history generations, and\n";
    s += "   prints what it removed.\n\n";
    s += "4. Start the service again.  systemctl start loom\n\n";
    s += "5. Confirm. Search Loom for the content that prompted this. It should return nothing,\n";
    s += "   and the jots should be absent from the History view as well as from search.\n\n";
    s += "Anything outside this data directory is out of scope and still needs handling by hand:\n";
    s += "backups and snapshots of the volume, anything that already read the jot over the API,\n";
    s += "and - if the secret was real - the credential itself, which should be rotated rather\n";
    s += "than merely forgotten.\n";
    return s;
}
