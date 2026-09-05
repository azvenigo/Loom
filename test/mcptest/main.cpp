// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// mcptest - assertions over the MCP protocol surface.
//
// Runs against McpHandler directly, with no socket. That is possible because the handler takes a
// request string and returns a response string, which is the same property that would let a stdio
// transport reuse it unchanged.
//
// The cases here are the ones a client actually exercises during a handshake, plus the two error
// behaviours that are easy to get backwards:
//
//   - a NOTIFICATION (no id) produces no response at all, not an empty result
//   - a TOOL FAILURE is a successful JSON-RPC response carrying isError:true, so the model can see
//     it; only malformed requests and unknown methods are protocol errors
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "core/JotStore.h"
#include "core/Ops.h"
#include "mcp/McpHandler.h"
#include "persist/History.h"

#include "vendor/json.hpp"

#include <cstdio>
#include <filesystem>
#include <string>

using json = nlohmann::json;

namespace
{
    int gnChecks = 0, gnFailed = 0;

    void Check(bool b, const char* pWhat)
    {
        ++gnChecks;
        std::printf("  %s  %s\n", b ? "ok  " : "FAIL", pWhat);
        if (!b) ++gnFailed;
    }
    void Section(const char* p) { std::printf("\n[%s]\n", p); }

    std::string Rpc(McpHandler& h, const std::string& sMethod, const json& params, const json& id)
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["method"]  = sMethod;
        if (!id.is_null()) req["id"] = id;
        if (!params.is_null()) req["params"] = params;
        return h.Handle(req.dump());
    }

    // Tool results arrive as a JSON string inside content[0].text; unwrap it for assertions.
    json ToolBody(const std::string& sResponse, bool& outbIsError)
    {
        json r = json::parse(sResponse, nullptr, false);
        outbIsError = false;
        if (r.is_discarded() || !r.contains("result")) return json();
        const json& res = r["result"];
        outbIsError = res.value("isError", false);
        if (!res.contains("content") || res["content"].empty()) return json();
        const std::string sText = res["content"][0].value("text", "");
        json inner = json::parse(sText, nullptr, false);
        return inner.is_discarded() ? json{{"raw", sText}} : inner;
    }

    json Call(McpHandler& h, const char* pTool, const json& args, bool& outbIsError)
    {
        const std::string s = Rpc(h, "tools/call",
                                  json{{"name", pTool}, {"arguments", args}}, json(1));
        return ToolBody(s, outbIsError);
    }
}


int main()
{
    std::printf("mcptest - Loom phase 4\n");

    JotStore   store;
    Ops        ops(store);
    McpHandler mcp(ops, store);

    //--------------------------------------------------------------------------------------
    Section("handshake");
    {
        const std::string s = Rpc(mcp, "initialize",
            json{{"protocolVersion","2025-06-18"},{"capabilities",json::object()}}, json(1));
        json r = json::parse(s, nullptr, false);
        Check(!r.is_discarded() && r.contains("result"), "initialize returns a result");
        Check(r["result"]["protocolVersion"] == "2025-06-18", "echoes a supported protocol version");
        Check(r["result"]["serverInfo"]["name"] == "loom", "identifies itself");
        Check(r["result"].contains("instructions"), "carries usage instructions for the model");

        // An unknown version must not fail the handshake - it falls back to one we do speak.
        const std::string s2 = Rpc(mcp, "initialize",
            json{{"protocolVersion","1999-01-01"}}, json(2));
        json r2 = json::parse(s2, nullptr, false);
        Check(r2.contains("result") && r2["result"]["protocolVersion"] != "1999-01-01",
              "an unsupported version falls back instead of failing");
    }

    Section("notifications");
    {
        json req;
        req["jsonrpc"] = "2.0";
        req["method"]  = "notifications/initialized";
        Check(mcp.Handle(req.dump()).empty(),
              "a notification produces NO response body (caller answers 202)");
    }

    Section("discovery");
    {
        const std::string s = Rpc(mcp, "tools/list", json(nullptr), json(3));
        json r = json::parse(s, nullptr, false);
        const json& tools = r["result"]["tools"];
        Check(tools.is_array() && tools.size() >= 10, "tools/list returns the tool table");

        bool bAllDescribed = true, bAllSchemas = true;
        for (const json& t : tools)
        {
            if (!t.contains("description") || t["description"].get<std::string>().size() < 40)
                bAllDescribed = false;
            if (!t.contains("inputSchema") || t["inputSchema"]["type"] != "object")
                bAllSchemas = false;
        }
        // The descriptions ARE the interface - an agent picks a tool from this text alone.
        Check(bAllDescribed, "every tool carries a substantive description");
        Check(bAllSchemas, "every tool carries an object inputSchema");

        // Undo has to be IN THE CATALOG. History and restore existed over REST for a while, which
        // meant the dashboard could undo a bad write and the agent that made it could not.
        bool bHistory = false, bRestore = false, bUpdateRequiresGuard = false;
        for (const json& t : tools)
        {
            const std::string sName = t.value("name", std::string());
            if (sName == "loom_history") bHistory = true;
            if (sName == "loom_restore") bRestore = true;
            if (sName == "loom_update")
            {
                for (const json& req : t["inputSchema"]["required"])
                    if (req == "expect_updated") bUpdateRequiresGuard = true;
            }
        }
        Check(bHistory && bRestore, "loom_history and loom_restore are advertised, not REST-only");
        Check(bUpdateRequiresGuard, "loom_update's schema marks expect_updated required");

        Check(json::parse(Rpc(mcp,"resources/list",json(nullptr),json(4)))["result"]
                .contains("resources"),
              "resources/list answers empty rather than method-not-found");
    }

    Section("tool round trip");
    {
        bool bErr = false;
        json add = Call(mcp, "loom_add",
            json{{"text","homelab serves the fast pool over Samba"},
                 {"name","homelab-network"},
                 {"summary","homelab network topology"},
                 {"tags",json::array({"infra"})},
                 {"editor","claude"}}, bErr);
        Check(!bErr && add.contains("id"), "loom_add creates a jot");
        const int64_t nID = add.value("id", (int64_t)0);

        json found = Call(mcp, "loom_search", json{{"query","homelab topology"}}, bErr);
        Check(!bErr && found["matched"] == 1, "loom_search finds it");

        json got = Call(mcp, "loom_get", json{{"name","homelab-network"}}, bErr);
        Check(!bErr && got.value("id",(int64_t)0) == nID, "loom_get resolves a slug");

        json tags = Call(mcp, "loom_tags", json::object(), bErr);
        Check(!bErr && tags["count"] == 1, "loom_tags reports the vocabulary");
    }

    Section("tool failures are results, not protocol errors");
    {
        bool bErr = false;
        json miss = Call(mcp, "loom_get", json{{"id",424242}}, bErr);
        Check(bErr, "a missing jot comes back as isError:true");

        const std::string s = Rpc(mcp, "tools/call",
            json{{"name","loom_get"},{"arguments",json{{"id",424242}}}}, json(5));
        json r = json::parse(s, nullptr, false);
        Check(r.contains("result") && !r.contains("error"),
              "and NOT as a JSON-RPC error - the model has to be able to see it");

        json dup = Call(mcp, "loom_add",
            json{{"text","different text"},{"name","homelab-network"}}, bErr);
        Check(bErr, "a taken slug fails");
        Check(dup.value("raw","").find("loom_upsert") != std::string::npos,
              "and the message names the tool that would work instead");
    }

    Section("optimistic concurrency over MCP");
    {
        bool bErr = false;
        json up = Call(mcp, "loom_upsert",
            json{{"name","contested"},{"text","v1"},{"editor","user"}}, bErr);
        const int64_t nID = up.value("id",(int64_t)0);

        json a = Call(mcp, "loom_update",
            json{{"id",nID},{"text","writer A"},{"expect_updated",nID}}, bErr);
        Check(!bErr, "the first writer succeeds");

        json b = Call(mcp, "loom_update",
            json{{"id",nID},{"text","writer B"},{"expect_updated",nID}}, bErr);
        Check(bErr, "a stale second writer is refused");
        Check(b.value("raw","").find("Re-read") != std::string::npos,
              "and is told how to recover, not just that it failed");
    }

    Section("patch semantics");
    {
        bool bErr = false;
        json j = Call(mcp, "loom_add",
            json{{"text","body text"},{"summary","the summary"},
                 {"tags",json::array({"keep"})}}, bErr);
        const int64_t nID = j.value("id",(int64_t)0);

        // Only tags supplied: everything else must survive untouched. expect_updated is the id
        // here because a jot that has never been edited has updated == id.
        json patched = Call(mcp, "loom_update",
            json{{"id",nID},{"tags",json::array({"changed"})},{"expect_updated",nID}}, bErr);
        Check(!bErr, "a partial update succeeds");
        Check(patched.value("summary","") == "the summary",
              "omitting a field leaves it alone rather than clearing it");
        Check(patched.value("text","") == "body text", "text untouched too");
    }

    Section("the conflict guard is mandatory on update");
    {
        bool bErr = false;
        json j = Call(mcp, "loom_add", json{{"text","guarded"}}, bErr);
        const int64_t nID = j.value("id",(int64_t)0);

        // The schema marks expect_updated required; this is the half that holds when a client
        // ignores the schema, which is the case that actually happens.
        json refused = Call(mcp, "loom_update",
            json{{"id",nID},{"text","no guard passed"}}, bErr);
        Check(bErr, "an update with no expect_updated is REFUSED, not silently forced through");
        Check(refused.value("raw","").find("loom_get") != std::string::npos,
              "and the message says how to get one, rather than just naming the field");

        // The write must not have happened.
        json unchanged = Call(mcp, "loom_get", json{{"id",nID}}, bErr);
        Check(unchanged.value("text","") == "guarded", "and the refused write changed nothing");

        // Upsert is deliberately still permissive - it addresses by slug and may be creating.
        Call(mcp, "loom_upsert", json{{"name","unguarded-upsert"},{"text","v1"}}, bErr);
        Check(!bErr, "loom_upsert still accepts a write with no expect_updated");
    }

    Section("history and restore over MCP");
    {
        bool bErr = false;

        // Without persistence there is no log, and the tools must say so rather than crash.
        Call(mcp, "loom_history", json::object(), bErr);
        Check(bErr, "loom_history on a store with no history log is a clean tool error");

        // A second store, wired to a real history log, for the rest of this section.
        const std::string sDir = "mcptest-data";
        std::error_code fsec;
        std::filesystem::remove_all(sDir, fsec);
        std::filesystem::create_directories(sDir, fsec);

        HistoryConfig hc;
        hc.msPath = sDir + "/loom.history";
        History history;
        Check(!history.Open(hc), "the history log opens");

        JotStore   hstore;
        hstore.SetJournalSink(&history);
        Ops        hops(hstore);
        McpHandler hmcp(hops, hstore, &history);

        json made = Call(hmcp, "loom_add",
            json{{"text","the original"},{"summary","first"},{"editor","claude"}}, bErr);
        const int64_t nID = made.value("id",(int64_t)0);

        // A DIFFERENT editor, deliberately. History folds a run of edits to one jot by ONE editor
        // into a single listed row carrying the run's newest state - so had both writes been
        // 'claude' there would be one row, nothing below it, and nothing to undo to. Two agents
        // stepping on each other is the case this whole tool exists for, and it is also the case
        // that produces two rows.
        json wrecked = Call(hmcp, "loom_update",
            json{{"id",nID},{"text","clobbered"},{"editor","codex"},{"expect_updated",nID}}, bErr);
        Check(!bErr && wrecked.value("text","") == "clobbered", "a bad write lands");

        json log = Call(hmcp, "loom_history", json{{"id",nID}}, bErr);
        Check(!bErr && log.contains("entries"), "loom_history lists that jot's changes");
        Check(log["entries"].size() == 2, "both writes are listed, newest first");
        Check(log["entries"][0].value("editor","") == "codex",
              "and each row names who made that change");
        // The listing must stay cheap: it is for finding a seq, not for pulling every version.
        Check(!log["entries"][0].contains("record"),
              "a listing carries no record bodies");

        // Newest first, so the ORIGINAL is the older row - which is what undoing means.
        uint64_t nOriginalSeq = 0;
        for (const json& e : log["entries"])
            nOriginalSeq = e.value("seq",(uint64_t)0);   // last one seen is the oldest

        json back = Call(hmcp, "loom_restore", json{{"seq",nOriginalSeq}}, bErr);
        Check(!bErr, "loom_restore accepts a seq from that listing");
        Check(back.value("restored",false), "and says it restored something");

        json now = Call(hmcp, "loom_get", json{{"id",nID}}, bErr);
        Check(now.value("text","") == "the original",
              "the agent undid its own bad write with no human involved");
        Check(now.value("id",(int64_t)0) == nID, "and the jot kept its id, so links survive");

        Call(hmcp, "loom_restore", json{{"seq",(uint64_t)999999}}, bErr);
        Check(bErr, "an unknown seq is a tool error, not a crash");

        Call(hmcp, "loom_restore", json::object(), bErr);
        Check(bErr, "loom_restore with no seq is refused");

        history.Close();
        std::filesystem::remove_all(sDir, fsec);
    }

    Section("malformed input");
    {
        json r1 = json::parse(mcp.Handle("{not json"), nullptr, false);
        Check(r1["error"]["code"] == -32700, "unparseable body is a parse error");

        json r2 = json::parse(Rpc(mcp, "no_such_method", json(nullptr), json(7)), nullptr, false);
        Check(r2["error"]["code"] == -32601, "unknown method is method-not-found");

        json r3 = json::parse(Rpc(mcp, "tools/call", json{{"nope",1}}, json(8)), nullptr, false);
        Check(r3["error"]["code"] == -32602, "tools/call with no name is invalid params");

        bool bErr = false;
        Call(mcp, "loom_no_such_tool", json::object(), bErr);
        Check(bErr, "an unknown TOOL is a tool error, not a protocol error");
    }

    Section("batch");
    {
        json batch = json::array();
        batch.push_back(json{{"jsonrpc","2.0"},{"id",10},{"method","ping"}});
        batch.push_back(json{{"jsonrpc","2.0"},{"method","notifications/initialized"}});
        batch.push_back(json{{"jsonrpc","2.0"},{"id",11},{"method","tools/list"}});

        json r = json::parse(mcp.Handle(batch.dump()), nullptr, false);
        Check(r.is_array() && r.size() == 2,
              "a batch answers only the requests, not the notification inside it");
    }

    std::printf("\n%d checks, %d failed\n", gnChecks, gnFailed);
    return gnFailed == 0 ? 0 : 1;
}
