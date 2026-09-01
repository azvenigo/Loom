// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "codec/JotJson.h"

#include "core/LoomTime.h"

#include "vendor/json.hpp"

using json = nlohmann::json;

namespace
{
    // The omit-empty rule, in one place. Everything else in this file defers to it.
    json FlatToObject(const FlatJot& jot, bool bVerbose, bool bBrief = false)
    {
        json j;
        j["id"] = jot.mID;

        if (bBrief)
        {
            // Skim mode: the point is to fit many jots in one response, so the body - usually the
            // largest field by far - is dropped. `has_text` says whether there was more to fetch,
            // since a name+summary-only jot and a jot with a long body otherwise look identical.
            if (!jot.msText.empty())
                j["has_text"] = true;
        }
        else
        {
            j["text"] = jot.msText;
        }

        if (bVerbose || !jot.msName.empty())
            j["name"] = jot.msName;

        if (bVerbose || !jot.msSummary.empty())
            j["summary"] = jot.msSummary;

        if (bVerbose || !jot.mTags.empty())
            j["tags"] = jot.mTags;

        if (bVerbose || !jot.mLinks.empty())
            j["links"] = jot.mLinks;

        // Unresolved [[slug]] targets. Surfaced rather than hidden - a dangling link marks a memory
        // worth writing, and an agent that cannot see them cannot act on them.
        if (bVerbose || !jot.mPendingLinks.empty())
            j["pending"] = jot.mPendingLinks;

        if (bVerbose || jot.mnUpdatedUS != 0)
            j["updated"] = jot.mnUpdatedUS;

        if (bVerbose || !jot.IsDefaultEditor())
            j["editor"] = jot.msEditor.empty() ? std::string("user") : jot.msEditor;

        // Human-readable timestamps are strictly a verbose convenience. The id is authoritative.
        if (bVerbose)
        {
            j["created_at"] = LOOMTIME::FormatUS(jot.mID);
            if (jot.mnUpdatedUS != 0)
                j["updated_at"] = LOOMTIME::FormatUS(jot.mnUpdatedUS);
        }

        return j;
    }

    bool ReadStringArray(const json& value, std::vector<std::string>& out, std::string& outError,
                         const char* pField, bool bAllowNumbers)
    {
        if (!value.is_array())
        {
            outError = std::string(pField) + " must be an array";
            return false;
        }

        for (const json& entry : value)
        {
            if (entry.is_string())
            {
                out.push_back(entry.get<std::string>());
            }
            else if (bAllowNumbers && entry.is_number_integer())
            {
                out.push_back(std::to_string(entry.get<int64_t>()));
            }
            else
            {
                outError = std::string(pField) + " entries must be strings"
                         + (bAllowNumbers ? " or integers" : "");
                return false;
            }
        }
        return true;
    }
}


namespace JOTJSON
{
    std::string ToJson(const FlatJot& jot, bool bVerbose)
    {
        return FlatToObject(jot, bVerbose).dump();
    }

    bool ParseFlat(const std::string& sJson, FlatJot& outJot, std::string& outError)
    {
        outJot = FlatJot();
        outError.clear();

        json j = json::parse(sJson, nullptr, false);
        if (j.is_discarded() || !j.is_object())
        {
            outError = "not a JSON object";
            return false;
        }

        // The id is the only thing a record cannot survive without - it is the identity.
        if (!j.contains("id") || !j["id"].is_number_integer())
        {
            outError = "missing or non-integer id";
            return false;
        }
        outJot.mID = j["id"].get<int64_t>();

        // Everything else is best-effort. A record written by an older build simply lacks the newer
        // keys, and they must come back as defaults rather than failing the whole replay - losing a
        // snapshot to a schema addition would be a far worse failure than a missing field.
        if (j.contains("text")    && j["text"].is_string())    outJot.msText    = j["text"].get<std::string>();
        if (j.contains("name")    && j["name"].is_string())    outJot.msName    = j["name"].get<std::string>();
        if (j.contains("summary") && j["summary"].is_string()) outJot.msSummary = j["summary"].get<std::string>();
        if (j.contains("editor")  && j["editor"].is_string())  outJot.msEditor  = j["editor"].get<std::string>();
        if (j.contains("updated") && j["updated"].is_number_integer())
            outJot.mnUpdatedUS = j["updated"].get<int64_t>();

        std::string sIgnored;
        if (j.contains("tags"))
            ReadStringArray(j["tags"], outJot.mTags, sIgnored, "tags", false);
        if (j.contains("pending"))
            ReadStringArray(j["pending"], outJot.mPendingLinks, sIgnored, "pending", false);

        if (j.contains("links") && j["links"].is_array())
        {
            for (const json& entry : j["links"])
            {
                if (entry.is_number_integer())
                    outJot.mLinks.push_back(entry.get<int64_t>());
            }
        }

        return true;
    }

    bool ParseInput(const std::string& sBody, JotInput& outInput, std::string& outError)
    {
        outInput = JotInput();
        outError.clear();

        json j = json::parse(sBody, nullptr, false);
        if (j.is_discarded())
        {
            outError = "body is not valid JSON";
            return false;
        }
        if (!j.is_object())
        {
            outError = "body must be a JSON object";
            return false;
        }

        // Only keys that are actually PRESENT become engaged optionals. That is what makes PATCH
        // leave untouched fields alone instead of clearing them.
        if (j.contains("text"))
        {
            if (!j["text"].is_string()) { outError = "text must be a string"; return false; }
            outInput.msText = j["text"].get<std::string>();
        }
        if (j.contains("name"))
        {
            if (!j["name"].is_string()) { outError = "name must be a string"; return false; }
            outInput.msName = j["name"].get<std::string>();
        }
        if (j.contains("summary"))
        {
            if (!j["summary"].is_string()) { outError = "summary must be a string"; return false; }
            outInput.msSummary = j["summary"].get<std::string>();
        }
        if (j.contains("editor"))
        {
            if (!j["editor"].is_string()) { outError = "editor must be a string"; return false; }
            outInput.msEditor = j["editor"].get<std::string>();
        }
        if (j.contains("tags"))
        {
            std::vector<std::string> vTags;
            if (!ReadStringArray(j["tags"], vTags, outError, "tags", false))
                return false;
            outInput.mTags = std::move(vTags);
        }
        if (j.contains("links"))
        {
            std::vector<std::string> vLinks;
            if (!ReadStringArray(j["links"], vLinks, outError, "links", true))
                return false;
            outInput.mLinks = std::move(vLinks);
        }

        return true;
    }

    std::string SearchToJson(const SearchResultSet& results, const NameTables& names, bool bVerbose,
                              bool bBrief)
    {
        json out;
        out["matched"]   = results.mnMatched;
        out["returned"]  = results.mJots.size();
        out["truncated"] = results.mbTruncated;

        json arr = json::array();
        for (size_t i = 0; i < results.mJots.size(); ++i)
        {
            json entry = FlatToObject(Flatten(results.mJots[i], names), bVerbose, bBrief);
            // Only meaningful for a ranked query; a filter-only query scores everything zero and
            // printing that column would imply a relevance that was never computed.
            if (i < results.mScores.size() && results.mScores[i] != 0.0f)
                entry["score"] = results.mScores[i];
            arr.push_back(std::move(entry));
        }
        out["jots"] = std::move(arr);

        return out.dump();
    }

    std::string JotListToJson(const std::vector<Jot>& vJots, const NameTables& names, bool bVerbose)
    {
        json arr = json::array();
        for (const Jot& jot : vJots)
            arr.push_back(FlatToObject(Flatten(jot, names), bVerbose));

        json out;
        out["jots"]     = std::move(arr);
        out["returned"] = vJots.size();
        return out.dump();
    }

    std::string TagsToJson(const std::vector<TagStat>& vTags)
    {
        json arr = json::array();
        for (const TagStat& s : vTags)
        {
            json e;
            e["tag"]   = s.msTag;
            e["count"] = s.mnCount;
            e["first"] = s.mFirstUS;
            e["last"]  = s.mLastUS;
            if (s.mbReserved)
                e["reserved"] = true;
            arr.push_back(std::move(e));
        }

        json out;
        out["tags"]  = std::move(arr);
        out["count"] = vTags.size();
        return out.dump();
    }

    std::string ClustersToJson(const std::vector<TagCluster>& vClusters)
    {
        json arr = json::array();
        for (const TagCluster& c : vClusters)
            arr.push_back(c.mMembers);

        json out;
        out["clusters"] = std::move(arr);
        out["count"]    = vClusters.size();
        return out.dump();
    }

    std::string StatsToJson(const StoreStats& stats, const PersistStats& persist)
    {
        json out;
        out["jots"]          = stats.mnJots;
        out["named"]         = stats.mnNamed;
        out["tags"]          = stats.mnTags;
        out["terms"]         = stats.mnTerms;
        out["pending_links"] = stats.mnPendingLinks;
        out["editors"]       = stats.mnEditors;
        out["mutations"]     = stats.mnMutations;
        if (stats.mnOldestUS != 0)
        {
            out["oldest"]    = stats.mnOldestUS;
            out["newest"]    = stats.mnNewestUS;
            out["oldest_at"] = LOOMTIME::FormatUS(stats.mnOldestUS);
            out["newest_at"] = LOOMTIME::FormatUS(stats.mnNewestUS);
        }

        json p;
        p["enabled"]        = persist.mbEnabled;
        p["queued"]         = persist.mnQueued;
        p["appended"]       = persist.mnAppended;
        p["synced"]         = persist.mnSynced;
        p["wal_bytes"]      = persist.mnWalBytes;
        p["snapshots"]      = persist.mnSnapshots;
        if (persist.mnLastSnapshotUS != 0)
            p["last_snapshot_at"] = LOOMTIME::FormatUS(persist.mnLastSnapshotUS);
        out["persistence"] = std::move(p);

        return out.dump();
    }

    std::string MutationToJson(const AddResult& result, const NameTables& names, bool bVerbose)
    {
        json out = FlatToObject(Flatten(result.mJot, names), bVerbose);
        out["created"] = result.mbCreated;

        if (!result.mWarnings.Empty())
            out["warnings"] = result.mWarnings.mMessages;

        return out.dump();
    }

    std::string ErrorToJson(const std::string& sMessage)
    {
        json out;
        out["error"] = sMessage;
        return out.dump();
    }
}
