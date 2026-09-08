// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "codec/JotJson.h"

#include "core/LoomTime.h"

#include "vendor/json.hpp"

using json = nlohmann::json;

namespace
{
    // First line, clipped - a caption for a jot that has neither a name nor a summary of its own.
    // Local rather than shared because every file with a one-line version of this keeps its own
    // (see core/Ops.cpp's ClipLine, persist/History.h's msCaption) rather than exporting a
    // substring helper across a module boundary for it.
    std::string ClipLine(const std::string& s, size_t nMax)
    {
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
            // `snippet` is the one exception to "body is dropped": a jot fresh off an import has
            // neither a name nor a summary, and without SOME text a brief-fed list (the dashboard's
            // activity feed, its TODO cards, its Needs Attention rows) has nothing to show but a
            // timestamp - which is exactly how those rows ended up rendering "(untitled)".
            if (!jot.msText.empty())
            {
                j["has_text"] = true;
                if (jot.msName.empty() && jot.msSummary.empty())
                    j["snippet"] = ClipLine(jot.msText, 100);
            }
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

        // Server-stamped, so unlike `editor` there is no default that could be materialized -
        // a record written before origins existed, or by the importer, simply has none.
        if (!jot.msOrigin.empty())
            j["origin"] = jot.msOrigin;

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
        if (j.contains("origin")  && j["origin"].is_string())  outJot.msOrigin  = j["origin"].get<std::string>();
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

    bool ParseCreatedSpec(const std::string& sSpec, int64_t& outUS)
    {
        return LOOMTIME::ParseTimeSpec(sSpec, LOOMTIME::NowMicros(), outUS);
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

        // `origin` IS NOT READ HERE, and its absence is the feature. It is the one field on a jot
        // that a caller may not set: the whole value of an origin is that the server observed it
        // rather than being told it, so it is stamped by the front door after this parse. An
        // "origin" key in a request body is silently ignored, exactly like any other unknown key.
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
        if (j.contains("created"))
        {
            // A number is handed to the same parser as its decimal text, so there is one definition
            // of what a creation time may look like rather than one per JSON type.
            std::string sSpec;
            if (j["created"].is_string())
                sSpec = j["created"].get<std::string>();
            else if (j["created"].is_number_integer())
                sSpec = std::to_string(j["created"].get<int64_t>());
            else
            {
                outError = "created must be a string or an integer";
                return false;
            }

            int64_t nCreatedUS = 0;
            if (!ParseCreatedSpec(sSpec, nCreatedUS))
            {
                outError = "created is not a recognizable time: use microseconds, "
                           "'YYYY-MM-DD', 'YYYY-MM-DD HH:MM:SS', or a relative age like '30d'";
                return false;
            }
            outInput.mnCreatedUS = nCreatedUS;
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

    std::string StatsToJson(const StoreStats& stats, const PersistStats& persist,
                            const std::string& sOrigin, bool bAuthRequired,
                            const TriageStats* pTriage, const AttentionStats* pAttention,
                            const JotpostStats* pJotpost,
                            const ResolverStats* pResolver)
    {
        json out;
        out["jots"]          = stats.mnJots;
        out["named"]         = stats.mnNamed;
        out["tags"]          = stats.mnTags;
        out["terms"]         = stats.mnTerms;
        out["pending_links"] = stats.mnPendingLinks;
        out["editors"]       = stats.mnEditors;
        out["mutations"]     = stats.mnMutations;
        out["jots_added"]    = stats.mnAdded;
        out["tags_added"]    = stats.mnTagsCreated;
        out["todos_added"]   = stats.mnTodosAdded;
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

        // Where this server is, told by the only party that knows. Omitted rather than guessed
        // when the caller did not supply one, so the dashboard falls back to its own origin
        // instead of printing something confidently wrong into an agent brief.
        if (!sOrigin.empty())
        {
            json server;
            server["origin"] = sOrigin;
            server["auth"]   = bAuthRequired;
            out["server"]    = std::move(server);
        }

        if (pTriage)
        {
            json t;
            t["breaker_open"]           = pTriage->mbBreakerOpen;
            t["consecutive_failures"]   = pTriage->mnConsecutiveFailures;
            t["budget_exceeded"]        = pTriage->mbBudgetExceeded;
            t["runs_24h"]               = pTriage->mnRuns24h;
            t["cost_usd_24h"]           = pTriage->mfCostUSD24h;
            t["input_tokens_24h"]       = pTriage->mnInputTokens24h;
            t["output_tokens_24h"]      = pTriage->mnOutputTokens24h;
            t["calls_this_run"]         = pTriage->mnRunsThisProcess;
            t["runs_lifetime"]          = pTriage->mnRunsLifetime;
            t["cost_usd_lifetime"]      = pTriage->mfCostUSDLifetime;
            if (pTriage->mbHasLastRun)
            {
                const TriageRun& r = pTriage->mLastRun;
                json last;
                last["at"]           = r.mnAtUS;
                last["duration_ms"]  = r.mnDurationMS;
                last["cost_usd"]     = r.mfCostUSD;
                last["jots_given"]   = r.mnJotsGiven;
                last["success"]      = r.mbSuccess;
                last["trigger"]      = r.msTrigger;
                if (!r.msFailure.empty())
                    last["failure"] = r.msFailure;
                t["last_run"] = std::move(last);
            }
            out["triage"] = std::move(t);
        }

        if (pAttention)
        {
            json a;
            a["jots"]  = pAttention->mnUnprocessedJots;
            a["files"] = pAttention->mnPendingFiles;
            a["total"] = pAttention->mnUnprocessedJots + pAttention->mnPendingFiles;
            out["needs_attention"] = std::move(a);
        }

        if (pJotpost)
        {
            json j;
            j["reachable"]  = pJotpost->mbReachable;
            j["checked_at"] = pJotpost->mnCheckedUS;
            out["jotpost"]  = std::move(j);
        }

        // The offline triage service (persist/TriageClient.h). A SEPARATE block from "triage"
        // rather than more fields inside it, because the two measure different mechanisms: that
        // one is a persisted ledger of billable agent runs, this one is a since-start counter for
        // a free LAN call. Folding them together would invite a dashboard to add up numbers that
        // do not belong in the same total.
        if (pResolver && pResolver->bConfigured)
        {
            json r;
            r["endpoint"]             = pResolver->sEndpoint;
            r["calls"]                = pResolver->nCalls;
            r["applied"]              = pResolver->nApplied;
            r["declined"]             = pResolver->nDeclined;
            r["failed"]               = pResolver->nFailed;
            r["last_ms"]              = pResolver->nLastMS;
            // Mean over calls that actually completed a round trip; a failed call's time is real
            // but is mostly connect timeout, which would make the model look slow when it is the
            // network that is broken.
            const uint64_t nOk = pResolver->nApplied + pResolver->nDeclined;
            r["avg_ms"]               = nOk ? (pResolver->nTotalMS / nOk) : 0;
            r["breaker_open"]         = pResolver->bBreakerOpen;
            r["consecutive_failures"] = pResolver->nConsecutiveFailures;
            out["resolver"]           = std::move(r);
        }

        return out.dump();
    }

    std::string MutationToJson(const AddResult& result, const NameTables& names, bool bVerbose)
    {
        json out = FlatToObject(Flatten(result.mJot, names), bVerbose);
        out["created"] = result.mbCreated;

        // Emitted only when true, per the omit-empty rule - a write that changed something says
        // nothing, which is the common case and the uninteresting one.
        if (result.mbNoChange)
            out["no_change"] = true;

        if (!result.mWarnings.Empty())
            out["warnings"] = result.mWarnings.mMessages;

        // Existing records the new one may duplicate. The same advice is also in `warnings` as a
        // sentence, for clients that read only that; this is the machine-readable half, with ids to
        // fetch and a similarity to judge by. Omitted when empty, per the rule at the top of this
        // file - a create that duplicates nothing says nothing.
        if (!result.mDuplicates.empty())
        {
            json arr = json::array();
            for (const DuplicateCandidate& c : result.mDuplicates)
            {
                json e;
                e["id"]         = c.mID;
                if (!c.msName.empty())
                    e["name"]   = c.msName;
                e["summary"]    = c.msSummary;
                e["similarity"] = c.mfSimilarity;
                arr.push_back(std::move(e));
            }
            out["duplicate_candidates"] = std::move(arr);
        }

        return out.dump();
    }

    std::string ErrorToJson(const std::string& sMessage)
    {
        json out;
        out["error"] = sMessage;
        return out.dump();
    }
}
