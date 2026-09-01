// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "mcp/McpHandler.h"

#include "codec/JotJson.h"
#include "core/LoomTime.h"

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
            "so short specific queries work better than long ones. Returns whole records, so a "
            "search is usually all you need - do not follow up with loom_get on every hit.",
            json{
                {"query",    Str("Free text. Leave empty to browse by filter alone.")},
                {"tags",     StrArray("Every tag listed must be present.")},
                {"not_tags", StrArray("Exclude jots carrying any of these.")},
                {"editor",   Str("Only jots last written by this author, e.g. user, claude, codex.")},
                {"since",    Str("Lower time bound: '30d', '2026-08-01', or raw microseconds.")},
                {"until",    Str("Upper time bound, same forms as 'since'.")},
                {"order",    Str("relevance | newest | oldest. Defaults to relevance when there is "
                                 "a query, newest otherwise.")},
                {"limit",    json{{"type","integer"},{"description","Max results (default 20)."}}}
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
            "existing ones. The write still succeeded - but prefer the established tag next time.",
            json{
                {"text",    Str("The content. Required.")},
                {"name",    Str("Optional stable slug for a durable memory, e.g. 'homelab-network'.")},
                {"summary", Str("Optional one-line description. Weighted above body when ranking.")},
                {"tags",    StrArray("Reuse existing tags where possible - check loom_tags first. "
                                     "Structural tags use a prefix, e.g. 'type:project'.")},
                {"links",   StrArray("Jot ids or slugs. A slug that does not exist yet is kept as a "
                                     "pending link and connects itself when that jot is written.")},
                {"editor",  Str("Who is writing. Defaults to user; set it to your own name.")}
            }, json::array({"text"})));

        tools.push_back(Tool("loom_upsert",
            "Create a jot, or replace one that already has this slug. Requires 'name'. This is the "
            "tool for maintaining a durable memory whose content changes over time.\n\n"
            "Pass 'expect_updated' (from a prior read) to be told about a concurrent edit instead "
            "of silently overwriting it. Without it, the last writer wins - which is usually wrong "
            "in a store several agents share.",
            json{
                {"name",           Str("Slug identifying the memory. Required.")},
                {"text",           Str("The content.")},
                {"summary",        Str("One-line description, weighted above body when ranking.")},
                {"tags",           StrArray("Tags to set. Replaces the existing set.")},
                {"links",          StrArray("Jot ids or slugs.")},
                {"editor",         Str("Who is writing.")},
                {"expect_updated", json{{"type","integer"},
                    {"description","The 'updated' (or 'id' if never edited) you last saw. "
                                   "Mismatch returns a conflict instead of overwriting."}}}
            }, json::array({"name"})));

        tools.push_back(Tool("loom_update",
            "Edit an existing jot by id. Only the fields you pass change; anything omitted is left "
            "alone, so you can add a tag without resending the text.\n\n"
            "Pass 'expect_updated' from your last read. If someone else edited the jot since, you "
            "get a conflict rather than quietly destroying their write - re-read, merge, retry.",
            json{
                {"id",             json{{"type","integer"},{"description","Jot id. Required."}}},
                {"text",           Str("Replacement text.")},
                {"name",           Str("Set or change the slug.")},
                {"summary",        Str("Replacement summary.")},
                {"tags",           StrArray("Replaces the whole tag set.")},
                {"links",          StrArray("Replaces the whole link set.")},
                {"editor",         Str("Who is editing.")},
                {"expect_updated", json{{"type","integer"},
                    {"description","The 'updated' (or 'id') you last saw."}}}
            }, json::array({"id"})));

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
            "Rewrite every jot carrying any tag in 'from' to carry 'to' instead. DESTRUCTIVE and "
            "not reversible - it edits many records at once. Ask before running it unless you were "
            "told to clean up a specific pair.",
            json{
                {"from", StrArray("Tags to retire.")},
                {"to",   Str("Tag that survives.")}
            }, json::array({"from","to"})));

        tools.push_back(Tool("loom_stats",
            "Store size, tag and term counts, and durability state. Useful for a health check or "
            "to see whether persistence is actually on.",
            json::object(), json::array()));

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
        return in;
    }
}


//====================================================================================================

McpHandler::McpHandler(Ops& ops, JotStore& store)
    : mOps(ops), mStore(store)
{
}

namespace
{
    // One tool invocation. Returns the MCP tool-result object; failures come back as isError:true
    // results rather than as protocol errors, so the model can see and act on them.
    json CallTool(Ops& ops, JotStore& store, const std::string& sName, const json& args)
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

            Query query;
            if (std::error_code ec = ops.BuildQuery(spec, query))
                return ToolFailure(ec.message());

            SearchResultSet results;
            if (std::error_code ec = ops.Search(query, results))
                return ToolFailure(ec.message());

            if (results.mJots.empty())
                return ToolText("No matches.");

            return ToolText(JOTJSON::SearchToJson(results, names, false));
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
            const JotInput in = ReadJotInput(args);
            AddResult result;
            std::error_code ec;

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

            size_t nChanged = 0;
            if (std::error_code ec = ops.MergeTags(vFrom, sTo, nChanged))
                return ToolFailure(ec.message());
            return ToolText("{\"changed\":" + std::to_string(nChanged) + "}");
        }

        return ToolFailure("Unknown tool: " + sName);
    }

    json HandleOne(Ops& ops, JotStore& store, const json& msg, bool& outbIsNotification)
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
                "Check loom_tags before inventing a tag.\n"
                "When editing, pass expect_updated so a concurrent write is reported rather than lost.";
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

            return RpcResult(id, CallTool(ops, store, params["name"].get<std::string>(), args));
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

std::string McpHandler::Handle(const std::string& sRequestJson)
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
            json r = HandleOne(mOps, mStore, one, bNotification);
            if (!bNotification)
                out.push_back(std::move(r));
        }
        return out.empty() ? std::string() : out.dump();
    }

    bool bNotification = false;
    json r = HandleOne(mOps, mStore, msg, bNotification);
    return bNotification ? std::string() : r.dump();
}
