// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "mcp/McpHandler.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"
#include "persist/History.h"
#include "persist/Undo.h"

#include "vendor/json.hpp"

#include <string>
#include <vector>

using json = nlohmann::json;

namespace
{
    constexpr const char* kServerName    = "loom";
    constexpr const char* kServerVersion = "0.1.0";

    // Newest first. An initialize that asks for one of these gets it echoed back; anything else
    // gets the newest we know, which is what the spec says to do when the client's version is not
    // supported. Being permissive here costs nothing and avoids a handshake cliff every time the
    // spec revs.
    const char* kSupportedVersions[] = { "2025-06-18", "2025-03-26", "2024-11-05" };

    //--------------------------------------------------------------------------------------------
    // JSON-RPC envelopes
    //--------------------------------------------------------------------------------------------

    json RpcResult(const json& id, json result)
    {
        json r;
        r["jsonrpc"] = "2.0";
        r["id"]      = id;
        r["result"]  = std::move(result);
        return r;
    }

    json RpcError(const json& id, int nCode, const std::string& sMessage)
    {
        json r;
        r["jsonrpc"] = "2.0";
        r["id"]      = id.is_null() ? json(nullptr) : id;
        r["error"]   = { {"code", nCode}, {"message", sMessage} };
        return r;
    }

    // A tool that ran and failed. Deliberately a SUCCESSFUL rpc response - see the header note.
    json ToolFailure(const std::string& sMessage)
    {
        json r;
        r["content"]  = json::array({ json{ {"type","text"}, {"text", sMessage} } });
        r["isError"]  = true;
        return r;
    }

    json ToolText(const std::string& sText)
    {
        json r;
        r["content"] = json::array({ json{ {"type","text"}, {"text", sText} } });
        r["isError"] = false;
        return r;
    }

    //--------------------------------------------------------------------------------------------
    // Schemas.
    //
    // These descriptions are the actual interface. An agent decides whether to call a tool, and
    // with what, almost entirely from this text - so each one says what the tool is FOR and names
    // the trap in using it, rather than restating the parameter list the schema already gives.
    //--------------------------------------------------------------------------------------------

    json StrArray(const std::string& sDesc)
    {
        return json{ {"type","array"}, {"items", json{{"type","string"}}}, {"description", sDesc} };
    }

    json Str(const std::string& sDesc)
    {
        return json{ {"type","string"}, {"description", sDesc} };
    }

    json Tool(const char* pName, const std::string& sDesc, json props, json required)
    {
        json schema;
        schema["type"]       = "object";
        schema["properties"] = std::move(props);
        if (!required.empty())
            schema["required"] = std::move(required);

        json t;
        t["name"]        = pName;
        t["description"] = sDesc;
        t["inputSchema"] = std::move(schema);
        return t;
    }

    json ToolList()
    {
        json tools = json::array();

        tools.push_back(Tool("loom_search",
            "Search the shared memory. Ranked by relevance when 'query' is given, newest-first "
            "otherwise. A match in a jot's summary counts far more than the same words in its body, "
            "so short specific queries work better than long ones. Brief by default - each hit "
            "comes back as id/name/summary/tags only, with has_text set if a body exists, so N "
            "results cost a fraction of the tokens of pulling whole records. loom_get (or a "
            "one-off brief:false) the specific hits whose summary alone isn't enough - do not "
            "flip brief:false to fetch a whole result set 'just in case'.",
            json{
                {"query",    Str("Free text. Leave empty to browse by filter alone.")},
                {"tags",     StrArray("Every tag listed must be present.")},
                {"not_tags", StrArray("Exclude jots carrying any of these.")},
                {"editor",   Str("Only jots last written by this author, e.g. user, claude, codex.")},
                {"since",    Str("Lower time bound: '30d', '2026-08-01', or raw microseconds.")},
                {"until",    Str("Upper time bound, same forms as 'since'.")},
                {"order",    Str("relevance | newest | oldest. Defaults to relevance when there is "
                                 "a query, newest otherwise.")},
                {"limit",    json{{"type","integer"},{"description","Max results (default 20)."}}},
                {"offset",   json{{"type","integer"},
                             {"description","Results to skip, for paging past a truncated result "
                                            "set - pass the previous call's 'returned' count rather "
                                            "than re-requesting with a larger 'limit', which "
                                            "re-sends jots you already have."}}},
                {"brief",    json{{"type","boolean"},
                             {"description","Drop each hit's body (default true - this is the "
                                            "normal way to search). Set false only once you know "
                                            "you need full bodies for every hit, not just some."}}}
            }, json::array()));

        tools.push_back(Tool("loom_get",
            "Fetch one jot by its id, or by its slug name. Use this only when you already have an "
            "identifier; to find something, use loom_search.",
            json{
                {"id",   json{{"type","integer"},{"description","Numeric jot id."}}},
                {"name", Str("Slug, e.g. 'user-preferences'. Use this or 'id', not both.")}
            }, json::array()));

        tools.push_back(Tool("loom_add",
            "Write a NEW jot. Fails if 'name' is already taken - that is deliberate, so an import "
            "or a careless write cannot silently replace a memory somebody wrote by hand. Use "
            "loom_upsert when replacing is what you actually mean.\n\n"
            "Give a 'name' and 'summary' only for durable memories worth recalling later; a passing "
            "thought needs neither. The summary is what search matches against most strongly, so "
            "write it as the sentence you would want to see when looking for this again.\n\n"
            "The response may carry 'warnings' about tags that look like near-duplicates of "
            "existing ones. The write still succeeded - but prefer the established tag next time.\n\n"
            "It may also carry 'duplicate_candidates': existing memories whose name and summary "
            "score close to the one you just wrote. THIS IS THE 'search before writing' RULE BEING "
            "CHECKED FOR YOU, after the fact. The write is not in question - you may well have meant "
            "to record something distinct - but read those jots before writing more, and if one of "
            "them is really the same fact, fold yours into it with loom_update and loom_delete the "
            "duplicate rather than leaving two.",
            json{
                {"text",    Str("The content. Required.")},
                {"name",    Str("Optional stable slug for a durable memory, e.g. 'homelab-network'.")},
                {"summary", Str("Optional one-line description. Weighted above body when ranking.")},
                {"tags",    StrArray("Reuse existing tags where possible - check loom_tags first. "
                                     "Structural tags use a prefix, e.g. 'type:project'.")},
                {"links",   StrArray("Jot ids or slugs. A slug that does not exist yet is kept as a "
                                     "pending link and connects itself when that jot is written.")},
                {"editor",  Str("Who is writing. Defaults to user; set it to your own name.")},
                {"created", Str("When this was ACTUALLY created, if that is not now - for content "
                                "carried in from somewhere else that has its own date. "
                                "'YYYY-MM-DD', 'YYYY-MM-DD HH:MM:SS', microseconds since the "
                                "epoch, or a relative age like '30d'. Omit for anything you are "
                                "writing now. Cannot be in the future, and cannot be changed "
                                "afterwards.")}
            }, json::array({"text"})));

        tools.push_back(Tool("loom_upsert",
            "Create a jot, or replace one that already has this slug. Requires 'name'. This is the "
            "tool for maintaining a durable memory whose content changes over time.\n\n"
            "'expect_updated' is OPTIONAL here, unlike loom_update: upsert addresses a jot by slug "
            "and is meant for writers that have not read the record and may be creating it. Pass it "
            "(from a prior read) to be told about a concurrent edit instead "
            "of silently overwriting it. Without it, the last writer wins - which is usually wrong "
            "in a store several agents share.",
            json{
                {"name",           Str("Slug identifying the memory. Required.")},
                {"text",           Str("The content.")},
                {"summary",        Str("One-line description, weighted above body when ranking.")},
                {"tags",           StrArray("Tags to set. Replaces the existing set.")},
                {"links",          StrArray("Jot ids or slugs.")},
                {"editor",         Str("Who is writing.")},
                {"created",        Str("When this was ACTUALLY created, for content carried in "
                                       "from elsewhere. Applies ONLY if this call creates the jot; "
                                       "if the slug already exists its original date is kept. "
                                       "'YYYY-MM-DD', microseconds, or a relative age like '30d'.")},
                {"expect_updated", json{{"type","integer"},
                    {"description","The 'updated' (or 'id' if never edited) you last saw. "
                                   "Mismatch returns a conflict instead of overwriting."}}}
            }, json::array({"name"})));

        tools.push_back(Tool("loom_update",
            "Edit an existing jot by id. Only the fields you pass change; anything omitted is left "
            "alone, so you can add a tag without resending the text.\n\n"
            "REQUIRES 'expect_updated' - the 'updated' value from your last read of this jot. Loom "
            "is written to by several agents and a human at once, so an edit built on a copy you "
            "read some minutes ago may be about to erase somebody's work. If the jot changed since "
            "you read it you get a conflict instead: re-read, merge, retry. If you do not have an "
            "'updated' to hand, you have not read the jot, and you should loom_get it first.",
            json{
                {"id",             json{{"type","integer"},{"description","Jot id. Required."}}},
                {"text",           Str("Replacement text.")},
                {"name",           Str("Set or change the slug.")},
                {"summary",        Str("Replacement summary.")},
                {"tags",           StrArray("Replaces the whole tag set.")},
                {"links",          StrArray("Replaces the whole link set.")},
                {"editor",         Str("Who is editing.")},
                {"expect_updated", json{{"type","integer"},
                    {"description","REQUIRED. The 'updated' you last saw on this jot. Get it from "
                                   "loom_get or loom_search."}}}
            }, json::array({"id","expect_updated"})));

        tools.push_back(Tool("loom_delete",
            "Permanently remove a jot. There is no undo and no tombstone. Anything linking to it "
            "keeps the connection as a pending link, so re-creating the memory under the same slug "
            "restores the graph.",
            json{ {"id", json{{"type","integer"},{"description","Jot id. Required."}}} },
            json::array({"id"})));

        tools.push_back(Tool("loom_links",
            "Everything connected to a jot, following links in both directions - what it points at "
            "AND what points at it. Use it to pull the surrounding context after a search hit; the "
            "link graph is usually where the other half of an answer lives.",
            json{
                {"id",    json{{"type","integer"},{"description","Jot id. Required."}}},
                {"depth", json{{"type","integer"},{"description","Hops to follow (default 1)."}}}
            }, json::array({"id"})));

        tools.push_back(Tool("loom_tags",
            "The tag vocabulary with usage counts. Read this BEFORE inventing a tag - reusing an "
            "existing one is the entire mechanism keeping the vocabulary usable across several "
            "agents writing independently.",
            json{ {"include_reserved", json{{"type","boolean"},
                   {"description","Include structural prefix:value tags (default false)."}}} },
            json::array()));

        tools.push_back(Tool("loom_tag_drift",
            "Groups of tags that look like variants of each other - typos, plurals, and "
            "abbreviations like infra/infrastructure. This is what surfaces a vocabulary quietly "
            "splitting in two. Report what you find rather than merging unprompted.",
            json::object(), json::array()));

        tools.push_back(Tool("loom_merge_tags",
            "Rewrite every jot carrying any tag in 'from' to carry 'to' instead. DESTRUCTIVE - it "
            "edits many records at once. Ask before running it unless you were told to clean up a "
            "specific pair.\n\n"
            "It IS reversible, as one act: the response carries a 'txn' naming the whole rewrite, "
            "and loom_restore with that 'txn' and undo:true puts every jot it touched back the way "
            "it was. Keep the number if there is any chance the merge was wrong.",
            json{
                {"from", StrArray("Tags to retire.")},
                {"to",   Str("Tag that survives.")}
            }, json::array({"from","to"})));

        tools.push_back(Tool("loom_stats",
            "Store size, tag and term counts, and durability state. Useful for a health check or "
            "to see whether persistence is actually on.",
            json::object(), json::array()));

        tools.push_back(Tool("loom_history",
            "Every change ever made, newest first - who wrote it, when, and to which jot. Pass 'id' "
            "to get one jot's history instead of the whole store's.\n\n"
            "This is how you find out what a write actually did, and how you get the 'seq' that "
            "loom_restore takes. A run of edits to one jot by one editor within a few minutes is "
            "listed as ONE row carrying the run's newest state, with 'edits' saying how many "
            "mutations it stands for - so restoring the row below it undoes that whole editing "
            "session, which is almost always what was meant.",
            json{
                {"id",     json{{"type","integer"},
                    {"description","Only this jot's changes. Omit for the whole store."}}},
                {"limit",  json{{"type","integer"},{"description","Max rows (default 60)."}}},
                {"offset", json{{"type","integer"},{"description","Rows to skip, for paging."}}}
            }, json::array()));

        tools.push_back(Tool("loom_restore",
            "Put a logged version of a jot back, undoing everything done to it since. Takes a 'seq' "
            "from loom_history.\n\n"
            "USE THIS WHEN YOU HAVE JUST WRITTEN SOMETHING WRONG. You do not need a human to undo "
            "your own bad write. Restoring a 'del' row brings the jot back with its id intact, so "
            "links to it reconnect.\n\n"
            "The jot keeps its id and its 'updated' is stamped now, because the restore is itself a "
            "change and it happened now. Pass 'expect_updated' for the same reason loom_update "
            "requires it: a restore is a whole-record overwrite, so without it you may be discarding "
            "an edit somebody made while you were deciding.\n\n"
            "TO REVERSE A TAG MERGE, pass 'txn' instead of 'seq' - the number loom_merge_tags "
            "returned - with undo:true. That puts every jot the merge touched back as it was before "
            "it, in one call, and reports each one separately so you can see which were already "
            "edited since. Without undo:true a 'txn' RE-APPLIES that operation instead, which on a "
            "merge nobody has touched since changes nothing.",
            json{
                {"seq",            json{{"type","integer"},
                    {"description","History sequence number to restore. Required unless 'txn' is "
                                   "given."}}},
                {"txn",            json{{"type","integer"},
                    {"description","A multi-record operation id, from loom_merge_tags or the 'txn' "
                                   "on a loom_history row. Restores every jot it touched."}}},
                {"undo",           json{{"type","boolean"},
                    {"description","With 'txn': put the jots back as they were BEFORE that "
                                   "operation, rather than as it left them. This is what undoing a "
                                   "merge means. Default false."}}},
                {"expect_updated", json{{"type","integer"},
                    {"description","The 'updated' you last saw on the target jot. Omit only when "
                                   "restoring a jot that is currently deleted. Not used with "
                                   "'txn' - a group restore has no single revision to check."}}}
            }, json::array()));

        return tools;
    }

    //--------------------------------------------------------------------------------------------

    std::vector<std::string> ReadStrArray(const json& args, const char* pKey)
    {
        std::vector<std::string> v;
        if (!args.contains(pKey) || !args[pKey].is_array())
            return v;
        for (const json& e : args[pKey])
        {
            if (e.is_string())
                v.push_back(e.get<std::string>());
            else if (e.is_number_integer())
                v.push_back(std::to_string(e.get<int64_t>()));
        }
        return v;
    }

    std::string ReadStr(const json& args, const char* pKey)
    {
        return (args.contains(pKey) && args[pKey].is_string()) ? args[pKey].get<std::string>()
                                                               : std::string();
    }

    int64_t ReadInt(const json& args, const char* pKey, int64_t nDefault)
    {
        if (!args.contains(pKey))
            return nDefault;
        if (args[pKey].is_number_integer())
            return args[pKey].get<int64_t>();
        // Some clients stringify numbers; accepting that costs one branch and saves a support
        // conversation.
        if (args[pKey].is_string())
            return std::strtoll(args[pKey].get<std::string>().c_str(), nullptr, 10);
        return nDefault;
    }

    bool ReadBool(const json& args, const char* pKey, bool bDefault)
    {
        return (args.contains(pKey) && args[pKey].is_boolean()) ? args[pKey].get<bool>() : bDefault;
    }

    // Builds a JotInput, engaging ONLY the fields actually present. That is what makes
    // loom_update a genuine patch instead of a full replace that silently blanks what you omitted.
    JotInput ReadJotInput(const json& args)
    {
        JotInput in;
        if (args.contains("text")    && args["text"].is_string())    in.msText    = args["text"].get<std::string>();
        if (args.contains("name")    && args["name"].is_string())    in.msName    = args["name"].get<std::string>();
        if (args.contains("summary") && args["summary"].is_string()) in.msSummary = args["summary"].get<std::string>();
        if (args.contains("editor")  && args["editor"].is_string())  in.msEditor  = args["editor"].get<std::string>();
        if (args.contains("tags")    && args["tags"].is_array())     in.mTags     = ReadStrArray(args, "tags");
        if (args.contains("links")   && args["links"].is_array())    in.mLinks    = ReadStrArray(args, "links");

        // Same accepted spellings as the REST body - see JOTJSON::ParseCreatedSpec. A value that
        // does not parse is dropped rather than rejected here, and CallTool reports it; models pass
        // dates in whatever shape they please, so the message has to say what was wanted.
        if (args.contains("created"))
        {
            std::string sSpec;
            if (args["created"].is_string())               sSpec = args["created"].get<std::string>();
            else if (args["created"].is_number_integer())  sSpec = std::to_string(args["created"].get<int64_t>());

            int64_t nCreatedUS = 0;
            if (!sSpec.empty() && JOTJSON::ParseCreatedSpec(sSpec, nCreatedUS))
                in.mnCreatedUS = nCreatedUS;
        }
        return in;
    }

    // True when the caller supplied a 'created' the parser could not make sense of - so the tool
    // can say so instead of silently stamping the jot with today's date, which is the one outcome
    // somebody passing a creation time would never want.
    bool CreatedWasUnusable(const json& args, const JotInput& in)
    {
        return args.contains("created") && !in.mnCreatedUS;
    }
}


//====================================================================================================

McpHandler::McpHandler(Ops& ops, JotStore& store, History* pHistory)
    : mOps(ops), mStore(store), mpHistory(pHistory)
{
}

namespace
{
    // One tool invocation. Returns the MCP tool-result object; failures come back as isError:true
    // results rather than as protocol errors, so the model can see and act on them.
    json CallTool(Ops& ops, JotStore& store, History* pHistory, const std::string& sOrigin,
                  const std::string& sName, const json& args)
    {
        NameTables names;
        store.SnapshotNames(names);

        //-- reads ---------------------------------------------------------------------------
        if (sName == "loom_search")
        {
            Ops::QuerySpec spec;
            spec.msText   = ReadStr(args, "query");
            spec.mTags    = ReadStrArray(args, "tags");
            spec.mNotTags = ReadStrArray(args, "not_tags");
            spec.msEditor = ReadStr(args, "editor");
            spec.msSince  = ReadStr(args, "since");
            spec.msUntil  = ReadStr(args, "until");
            spec.msOrder  = ReadStr(args, "order");
            spec.mnLimit  = static_cast<size_t>(ReadInt(args, "limit", 20));
            spec.mnOffset = static_cast<size_t>(ReadInt(args, "offset", 0));
            const bool bBrief = ReadBool(args, "brief", true);

            Query query;
            if (std::error_code ec = ops.BuildQuery(spec, query))
                return ToolFailure(ec.message());

            SearchResultSet results;
            if (std::error_code ec = ops.Search(query, results))
                return ToolFailure(ec.message());

            if (results.mJots.empty())
                return ToolText("No matches.");

            return ToolText(JOTJSON::SearchToJson(results, names, false, bBrief));
        }

        if (sName == "loom_get")
        {
            Jot jot;
            std::error_code ec;
            const std::string sSlug = ReadStr(args, "name");
            if (!sSlug.empty())
                ec = ops.GetByName(sSlug, jot);
            else
                ec = ops.Get(ReadInt(args, "id", 0), jot);

            if (ec)
                return ToolFailure(ec.message());
            return ToolText(JOTJSON::ToJson(Flatten(jot, names), false));
        }

        if (sName == "loom_links")
        {
            std::vector<Jot> vJots;
            const std::error_code ec =
                ops.Links(ReadInt(args, "id", 0), static_cast<size_t>(ReadInt(args, "depth", 1)), vJots);
            if (ec)
                return ToolFailure(ec.message());
            if (vJots.empty())
                return ToolText("Nothing links to or from that jot.");
            return ToolText(JOTJSON::JotListToJson(vJots, names, false));
        }

        if (sName == "loom_tags")
        {
            std::vector<TagStat> vTags;
            ops.ListTags(vTags, args.contains("include_reserved")
                                && args["include_reserved"].is_boolean()
                                && args["include_reserved"].get<bool>());
            return ToolText(JOTJSON::TagsToJson(vTags));
        }

        if (sName == "loom_tag_drift")
        {
            std::vector<TagCluster> vClusters;
            ops.TagClusters(vClusters);
            if (vClusters.empty())
                return ToolText("No tag drift detected.");
            return ToolText(JOTJSON::ClustersToJson(vClusters));
        }

        if (sName == "loom_stats")
        {
            PersistStats persist;   // the handler has no journal reference; store half is enough here
            return ToolText(JOTJSON::StatsToJson(ops.Stats(), persist));
        }

        //-- writes --------------------------------------------------------------------------
        if (sName == "loom_add" || sName == "loom_upsert" || sName == "loom_update")
        {
            JotInput in = ReadJotInput(args);
            // Server-stamped, never read from the arguments - the tool schemas have no 'origin'
            // field and an agent cannot invent one. See Jot::msOrigin.
            if (!sOrigin.empty())
                in.msOrigin = sOrigin;

            AddResult result;
            std::error_code ec;

            if (CreatedWasUnusable(args, in))
                return ToolFailure("'created' is not a recognizable time. Use 'YYYY-MM-DD', "
                                   "'YYYY-MM-DD HH:MM:SS', microseconds since the epoch, or a "
                                   "relative age like '30d'.");

            if (sName == "loom_add")
            {
                ec = ops.Add(in, result);
            }
            else if (sName == "loom_upsert")
            {
                if (!in.msName || in.msName->empty())
                    return ToolFailure("loom_upsert requires 'name'; use loom_add for an unnamed jot.");
                ec = ops.Upsert(in, ReadInt(args, "expect_updated", 0), result);
            }
            else
            {
                const tJotID id = ReadInt(args, "id", 0);
                if (id == 0)
                    return ToolFailure("loom_update requires 'id'.");

                // THE CONFLICT GUARD IS NOT OPTIONAL HERE. The store takes 0 to mean "skip the
                // check", which REST, the importer and the replay path all rely on - so leaving
                // this defaulted made every agent that simply omitted the field a last-writer-wins
                // client, in the one store whose whole premise is that several agents and a human
                // write to it at once. The schema marks it required; this is the half that holds
                // when a client ignores the schema, which they do.
                //
                // Refused rather than defaulted, because there is no safe value to pick: guessing
                // would either skip the check or invent a revision the caller never read.
                if (!args.contains("expect_updated"))
                    return ToolFailure("loom_update requires 'expect_updated' - the 'updated' value "
                                       "from your last read of jot " + std::to_string(id) + ". "
                                       "Without it this edit would silently overwrite anything "
                                       "changed since. Call loom_get first and pass the 'updated' "
                                       "it returns.");

                ec = ops.Update(id, in, ReadInt(args, "expect_updated", 0), result);
            }

            if (ec)
            {
                // Spelled out, because these are the two an agent can actually recover from and a
                // bare error string would leave it guessing at what to do next.
                if (LoomErrorOf(ec) == eLoomErr::kConflict)
                    return ToolFailure("Conflict: this jot changed since the revision you passed. "
                                       "Re-read it, merge your change, and retry.");
                if (LoomErrorOf(ec) == eLoomErr::kNameInUse)
                    return ToolFailure("A jot already uses that name. Use loom_upsert to replace it, "
                                       "or pick a different slug.");
                return ToolFailure(ec.message());
            }

            // Refresh: a new tag may have been interned by this very write.
            store.SnapshotNames(names);
            return ToolText(JOTJSON::MutationToJson(result, names, false));
        }

        if (sName == "loom_delete")
        {
            const tJotID id = ReadInt(args, "id", 0);
            if (id == 0)
                return ToolFailure("loom_delete requires 'id'.");
            if (std::error_code ec = ops.Delete(id))
                return ToolFailure(ec.message());
            return ToolText("{\"deleted\":" + std::to_string(id) + "}");
        }

        if (sName == "loom_merge_tags")
        {
            const std::vector<std::string> vFrom = ReadStrArray(args, "from");
            const std::string sTo = ReadStr(args, "to");
            if (vFrom.empty() || sTo.empty())
                return ToolFailure("loom_merge_tags requires 'from' (array) and 'to' (string).");

            size_t   nChanged = 0;
            uint64_t nTxnID   = 0;
            if (std::error_code ec = ops.MergeTags(vFrom, sTo, nChanged, nTxnID))
                return ToolFailure(ec.message());

            json out;
            out["changed"] = nChanged;
            // The handle that undoes the whole rewrite. Emitted only when there is a history log to
            // undo it from, so an agent never gets a transaction id that nothing will honour.
            if (nTxnID != 0)
                out["txn"] = nTxnID;
            return ToolText(out.dump());
        }

        //-- history -------------------------------------------------------------------------
        if (sName == "loom_history" || sName == "loom_restore")
        {
            // Same answer the REST routes give as a 403, in the shape the model can read.
            if (!pHistory)
                return ToolFailure("This loom is running without persistence, so there is no "
                                   "history log and nothing to restore from.");

            if (sName == "loom_history")
            {
                std::vector<HistoryEntry> vEntries;
                size_t nTotal = 0;
                pHistory->List(static_cast<tJotID>(ReadInt(args, "id", kInvalidJotID)),
                               static_cast<size_t>(ReadInt(args, "limit", 60)),
                               static_cast<size_t>(ReadInt(args, "offset", 0)),
                               vEntries, nTotal);

                json out;
                out["entries"] = json::array();
                for (const HistoryEntry& e : vEntries)
                {
                    // msRecord - the full FlatJot the restore replays - is deliberately NOT here.
                    // A history listing is for finding the seq you want; handing back every version
                    // of every jot would make the cheapest browsing call the most expensive one.
                    json row{
                        {"seq",     e.mnSeq},
                        {"at",      e.mnAtUS},
                        {"op",      e.mbDelete ? "del" : "put"},
                        {"id",      e.mID},
                        {"name",    e.msName},
                        {"editor",  e.msEditor},
                        {"summary", e.msSummary},
                        {"edits",   e.mnCoalesced}
                    };
                    if (!e.msOrigin.empty())
                        row["origin"] = e.msOrigin;
                    // Present when this row was part of one act that touched several jots - a tag
                    // merge. Pass it to loom_restore with undo:true to reverse the whole thing.
                    if (e.mnTxnID != 0)
                        row["txn"] = e.mnTxnID;
                    out["entries"].push_back(std::move(row));
                }

                const HistoryStats st = pHistory->Stats();
                out["total"]    = nTotal;
                out["recorded"] = st.mnEntries;
                return ToolText(out.dump());
            }

            // A whole multi-record operation, which is a different request from a single version -
            // see persist/Undo.h - so it is answered before any sequence number is read.
            if (args.contains("txn"))
            {
                const uint64_t nTxnID = static_cast<uint64_t>(ReadInt(args, "txn", 0));
                const bool bUndo = args.contains("undo") && args["undo"].is_boolean()
                                && args["undo"].get<bool>();

                UndoReport report;
                std::string sUndoError;
                if (UNDO::ByTransaction(ops, *pHistory, nTxnID, bUndo, sOrigin, report, sUndoError))
                    return ToolFailure(sUndoError);

                json out;
                out["txn"]       = nTxnID;
                out["undo"]      = bUndo;
                out["restored"]  = report.mnRestored;
                out["unchanged"] = report.mnUnchanged;
                out["failed"]    = report.mnFailed;
                out["jots"]      = json::array();
                for (const UndoItem& item : report.mItems)
                {
                    json j{ {"id", item.mID}, {"name", item.msName},
                            {"restored", item.mbRestored}, {"no_change", item.mbNoChange} };
                    if (item.mnFromSeq != 0)   j["from_seq"] = item.mnFromSeq;
                    if (!item.msError.empty()) j["error"]    = item.msError;
                    out["jots"].push_back(std::move(j));
                }
                return ToolText(out.dump());
            }

            if (!args.contains("seq"))
                return ToolFailure("loom_restore requires 'seq' - a sequence number from "
                                   "loom_history - or 'txn' for a whole multi-record operation.");
            const uint64_t nSeq = static_cast<uint64_t>(ReadInt(args, "seq", 0));

            HistoryEntry entry;
            if (!pHistory->Get(nSeq, entry))
                return ToolFailure("No history entry " + std::to_string(nSeq) + ". It may have aged "
                                   "out of the log - call loom_history for what is still there.");

            // A `del` row restores what was in force immediately BEFORE it, because "undo this
            // delete" is the only thing anybody means by restoring a deletion.
            HistoryEntry target = entry;
            if (entry.mbDelete && !pHistory->Previous(nSeq, target))
                return ToolFailure("Nothing to restore: that entry is a delete, and no earlier "
                                   "version of the jot is still in the log.");

            FlatJot record;
            std::string sError;
            if (target.msRecord.empty() || !JOTJSON::ParseFlat(target.msRecord, record, sError))
                return ToolFailure("That history entry is unreadable: " + sError);

            // A restore is a write, and it is happening now, from here - so it is attributed here
            // rather than to whoever wrote the version being put back.
            if (!sOrigin.empty())
                record.msOrigin = sOrigin;

            tJotID    conflictID = kInvalidJotID;
            AddResult result;
            if (std::error_code ec = ops.Restore(record, ReadInt(args, "expect_updated", 0),
                                                 conflictID, result))
            {
                if (conflictID != kInvalidJotID)
                    return ToolFailure("The name '" + record.msName + "' now belongs to jot " +
                                       std::to_string(conflictID) + ". Restoring would leave two "
                                       "jots claiming one slug - rename or remove that one first.");
                if (LoomErrorOf(ec) == eLoomErr::kConflict)
                    return ToolFailure("Conflict: this jot changed since the revision you passed. "
                                       "Re-read it and decide again whether you still want this "
                                       "version back.");
                return ToolFailure(ec.message());
            }

            store.SnapshotNames(names);
            json out = json::parse(JOTJSON::MutationToJson(result, names, false), nullptr, false);
            if (out.is_discarded())
                out = json::object();
            out["restored"] = true;
            out["from_seq"] = target.mnSeq;
            return ToolText(out.dump());
        }

        return ToolFailure("Unknown tool: " + sName);
    }

    json HandleOne(Ops& ops, JotStore& store, History* pHistory, const std::string& sOrigin,
                   const json& msg, bool& outbIsNotification)
    {
        outbIsNotification = false;

        if (!msg.is_object() || !msg.contains("method") || !msg["method"].is_string())
            return RpcError(nullptr, -32600, "Invalid Request");

        const std::string sMethod = msg["method"].get<std::string>();
        const json id = msg.contains("id") ? msg["id"] : json(nullptr);

        // No "id" means a notification: acknowledge by doing the work and saying nothing.
        if (!msg.contains("id"))
        {
            outbIsNotification = true;
            return json();
        }

        if (sMethod == "initialize")
        {
            std::string sVersion = kSupportedVersions[0];
            if (msg.contains("params") && msg["params"].is_object()
                && msg["params"].contains("protocolVersion")
                && msg["params"]["protocolVersion"].is_string())
            {
                const std::string sWanted = msg["params"]["protocolVersion"].get<std::string>();
                for (const char* p : kSupportedVersions)
                {
                    if (sWanted == p) { sVersion = sWanted; break; }
                }
            }

            json result;
            result["protocolVersion"] = sVersion;
            result["capabilities"]    = json{ {"tools", json::object()} };
            result["serverInfo"]      = json{ {"name", kServerName}, {"version", kServerVersion} };
            result["instructions"] =
                "Loom is a shared memory that several agents and a human write to at once.\n"
                "Search before writing - the thing you are about to record may already be here.\n"
                "loom_add checks that for you and answers with duplicate_candidates when it finds\n"
                "one; read them rather than leaving two records of the same fact.\n"
                "Check loom_tags before inventing a tag.\n"
                "loom_update REQUIRES expect_updated from your last read, so a concurrent write is\n"
                "reported rather than lost. If you wrote something wrong, loom_history then\n"
                "loom_restore undoes it - you do not need a human for that.\n"
                "Tag actionable open work `todo`. When it is finished, ADD `status:done` and KEEP\n"
                "`todo` - do not remove it. The pair is the record that the work happened; removing\n"
                "`todo` erases it. Open work is todo minus status:done.\n"
                "loom_search is brief by default - summaries only. Skim brief, then loom_get (or a\n"
                "one-off brief:false) only the specific jots the task actually needs; do not pull\n"
                "whole records to browse.";
            return RpcResult(id, result);
        }

        if (sMethod == "ping")
            return RpcResult(id, json::object());

        if (sMethod == "tools/list")
            return RpcResult(id, json{ {"tools", ToolList()} });

        if (sMethod == "tools/call")
        {
            if (!msg.contains("params") || !msg["params"].is_object())
                return RpcError(id, -32602, "Missing params");

            const json& params = msg["params"];
            if (!params.contains("name") || !params["name"].is_string())
                return RpcError(id, -32602, "Missing tool name");

            const json args = (params.contains("arguments") && params["arguments"].is_object())
                            ? params["arguments"] : json::object();

            return RpcResult(id, CallTool(ops, store, pHistory, sOrigin,
                                         params["name"].get<std::string>(), args));
        }

        // Loom exposes tools only. Answering these with empty lists rather than "method not found"
        // keeps clients that probe every capability from logging errors on a healthy server.
        if (sMethod == "resources/list")
            return RpcResult(id, json{ {"resources", json::array()} });
        if (sMethod == "prompts/list")
            return RpcResult(id, json{ {"prompts", json::array()} });

        return RpcError(id, -32601, "Method not found: " + sMethod);
    }
}

std::string McpHandler::Handle(const std::string& sRequestJson, const std::string& sOrigin)
{
    json msg = json::parse(sRequestJson, nullptr, false);
    if (msg.is_discarded())
        return RpcError(nullptr, -32700, "Parse error").dump();

    // A batch. Notifications inside it contribute nothing to the response, and a batch of only
    // notifications gets no response at all.
    if (msg.is_array())
    {
        json out = json::array();
        for (const json& one : msg)
        {
            bool bNotification = false;
            json r = HandleOne(mOps, mStore, mpHistory, sOrigin, one, bNotification);
            if (!bNotification)
                out.push_back(std::move(r));
        }
        return out.empty() ? std::string() : out.dump();
    }

    bool bNotification = false;
    json r = HandleOne(mOps, mStore, mpHistory, sOrigin, msg, bNotification);
    return bNotification ? std::string() : r.dump();
}
