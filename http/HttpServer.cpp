// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// The only translation unit in Loom that includes crow. See HttpServer.h for why that matters.
//
// Warning level is relaxed for this file alone (CMakeLists.txt, set_source_files_properties) so the
// vendored headers do not force the whole project off /Wall /WX.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "http/HttpServer.h"

#include "core/LoomTime.h"
#include "codec/JotJson.h"
#include "mcp/McpHandler.h"
#include "persist/Purge.h"
#include "web/Dashboard.h"
#include "web/IconAssets.h"

#include "vendor/crow/crow.h"

#include <atomic>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
    //--------------------------------------------------------------------------------------------
    // error_code -> HTTP status. The single place the mapping lives; every route defers to it, so
    // a new error code cannot end up meaning different things on different routes.
    //--------------------------------------------------------------------------------------------
    int StatusFor(const std::error_code& ec)
    {
        switch (LoomErrorOf(ec))
        {
        case eLoomErr::kOK:              return 200;
        case eLoomErr::kNotFound:        return 404;
        case eLoomErr::kNameInUse:       return 409;
        case eLoomErr::kConflict:        return 409;
        case eLoomErr::kInvalidArgument: return 400;
        case eLoomErr::kEmptyJot:        return 400;
        case eLoomErr::kTooLarge:        return 413;
        case eLoomErr::kReadOnly:        return 403;
        }
        return 500;
    }

    crow::response Fail(const std::error_code& ec)
    {
        crow::response res(StatusFor(ec), JOTJSON::ErrorToJson(ec.message()));
        res.set_header("Content-Type", "application/json");
        return res;
    }

    crow::response Fail(int nStatus, const std::string& sMessage)
    {
        crow::response res(nStatus, JOTJSON::ErrorToJson(sMessage));
        res.set_header("Content-Type", "application/json");
        return res;
    }

    crow::response Ok(std::string sBody)
    {
        crow::response res(200, std::move(sBody));
        res.set_header("Content-Type", "application/json");
        return res;
    }

    //--------------------------------------------------------------------------------------------
    // Query-string helpers. Crow gives repeated params back as a list, which is exactly the shape
    // ?tag=a&tag=b needs - repeating a parameter beats nesting a syntax inside one.
    //--------------------------------------------------------------------------------------------
    std::string ParamOr(const crow::request& req, const char* pName, const std::string& sDefault = "")
    {
        const char* p = req.url_params.get(pName);
        return p ? std::string(p) : sDefault;
    }

    std::vector<std::string> ParamList(const crow::request& req, const char* pName)
    {
        std::vector<std::string> v;
        for (const std::string& s : req.url_params.get_list(pName, false))
        {
            if (!s.empty())
                v.push_back(s);
        }
        return v;
    }

    // Same, but gathers several spellings of one filter. REST grew the singular `tag=` and MCP the
    // plural `tags`; agents reach for whichever they saw last, so accept both rather than making
    // the two interfaces disagree over an "s".
    std::vector<std::string> ParamListAny(const crow::request& req,
                                          std::initializer_list<const char*> names)
    {
        std::vector<std::string> v;
        for (const char* pName : names)
        {
            // Both spellings of a repeated parameter: ?tag=a&tag=b and ?tag[]=a&tag[]=b. crow reads
            // them through separate paths, and accepting a name in validation while reading only
            // one path would drop the filter and return everything - the bug this all exists to fix.
            for (bool bBrackets : { false, true })
            {
                for (const std::string& s : req.url_params.get_list(pName, bBrackets))
                {
                    if (!s.empty())
                        v.push_back(s);
                }
            }
        }
        return v;
    }

    // Returns the first query parameter the caller does not recognize, or empty if all are known.
    //
    // WHY THIS EXISTS: crow silently drops a parameter nobody asks for, so `?tags=todo` used to
    // return the ENTIRE store - the filter vanished and the request degraded into an unfiltered
    // browse that looks exactly like success. A filter that matches nothing must not answer with
    // everything. Failing loudly on an unknown name is the only way the caller finds out.
    std::string UnknownParam(const crow::request& req, std::initializer_list<const char*> allowed)
    {
        for (std::string sKey : req.url_params.keys())
        {
            // ?tag[]=a is the bracketed spelling of a repeated parameter; validate the base name.
            if (sKey.size() > 2 && sKey.compare(sKey.size() - 2, 2, "[]") == 0)
                sKey.resize(sKey.size() - 2);

            bool bKnown = false;
            for (const char* pName : allowed)
            {
                if (sKey == pName) { bKnown = true; break; }
            }
            if (!bKnown)
                return sKey;
        }
        return std::string();
    }

    size_t ParamSize(const crow::request& req, const char* pName, size_t nDefault)
    {
        const char* p = req.url_params.get(pName);
        if (!p)
            return nDefault;
        char* pEnd = nullptr;
        const unsigned long long n = std::strtoull(p, &pEnd, 10);
        return (pEnd == p) ? nDefault : static_cast<size_t>(n);
    }

    bool ParamFlag(const crow::request& req, const char* pName)
    {
        const char* p = req.url_params.get(pName);
        if (!p)
            return false;
        const std::string s(p);
        return s == "1" || s == "true" || s == "yes" || s.empty();
    }

    int64_t ParamI64(const crow::request& req, const char* pName, int64_t nDefault)
    {
        const char* p = req.url_params.get(pName);
        if (!p)
            return nDefault;
        char* pEnd = nullptr;
        const long long n = std::strtoll(p, &pEnd, 10);
        return (pEnd == p) ? nDefault : static_cast<int64_t>(n);
    }

    //--------------------------------------------------------------------------------------------
    // The origin a REMOTE machine should use to reach this server: "http://<addr>:<port>".
    //
    // The dashboard cannot work this out for itself, and that is the whole reason this exists.
    // location.origin is only ever whatever the browser was pointed at, and on the machine running
    // loom that is usually localhost - so the agent brief, which is built in the page, would hand
    // every other machine an address that resolves to its own loopback. Only the server knows
    // where it actually is, so it says so once, at startup, and /stats carries it to the page.
    //
    // A concrete --bind IS the answer already; there is nothing to discover. A wildcard bind means
    // "every interface", which is not an address anyone can be given, so ask the routing table
    // which source address this machine would use to reach the outside world. UDP connect() sends
    // no packets - it only selects a route - so this touches the network stack, not the network.
    //--------------------------------------------------------------------------------------------
    std::string ResolveAdvertisedOrigin(const HttpConfig& config)
    {
        std::string sHost = config.msBind;
        if (sHost.empty() || sHost == "0.0.0.0" || sHost == "::" || sHost == "*")
        {
            // Last resort. A machine with no route out still answers on loopback, and a brief
            // saying localhost is at least true for the agent running on this box.
            sHost = "127.0.0.1";

            asio::io_context     io;
            asio::ip::udp::socket sock(io);
            asio::error_code     ec;
            // TEST-NET-1 (RFC 5737): reserved for documentation, so it is guaranteed never to be
            // a real destination - which is safe precisely because nothing is ever sent to it.
            sock.connect(asio::ip::udp::endpoint(asio::ip::make_address("192.0.2.1"), 9), ec);
            if (!ec)
            {
                const asio::ip::udp::endpoint local = sock.local_endpoint(ec);
                if (!ec && !local.address().is_unspecified())
                    sHost = local.address().to_string();
            }
        }
        // An IPv6 literal has to be bracketed or the port reads as another group of the address.
        if (sHost.find(':') != std::string::npos)
            sHost = "[" + sHost + "]";
        return "http://" + sHost + ":" + std::to_string(config.mnPort);
    }
}


//====================================================================================================
// AclGuard - the address allow list, enforced ahead of routing.
//
// A MIDDLEWARE AND NOT A LINE IN EVERY HANDLER. The token check is the latter, and it shows why:
// it is repeated verbatim in eighteen places and every new route has to remember it. Fine when the
// consequence of forgetting is an open read; not fine when the consequence is that the address list
// the operator turned on silently does not cover the route added after it. before_handle runs for
// everything crow can route, including the dashboard, the icons and /health.
//
// /health IS GATED TOO, deliberately. It is exempt from the token because a health check that needs
// a credential is one more thing that can wrongly report the service down - but that argument is
// about credentials, not about reachability. An address that is not allowed to talk to Loom should
// not be able to confirm Loom is here.
//====================================================================================================

struct AclGuard
{
    struct context {};

    IpAcl* mpAcl = nullptr;

    void before_handle(crow::request& req, crow::response& res, context&)
    {
        if (!mpAcl || mpAcl->Allows(req.remote_ip_address))
            return;

        // Says which address was refused, and nothing else. The caller already knows their own
        // address, so this leaks nothing - and without it the only symptom of a mistyped rule is a
        // service that has silently stopped answering.
        res.code = 403;
        res.set_header("Content-Type", "application/json");
        res.body = JOTJSON::ErrorToJson("address " + req.remote_ip_address +
                                        " is not on this server's access list");
        res.end();
    }

    void after_handle(crow::request&, crow::response&, context&) {}
};


//====================================================================================================

struct HttpServer::Impl
{
    Ops&                 mOps;
    JotStore&            mStore;
    HttpConfig           mConfig;
    Journal*             mpJournal = nullptr;
    IpAcl&               mAcl;
    History*             mpHistory = nullptr;
    McpHandler           mMcp;
    SnapshotConfig       mSnapConfig;
    crow::App<AclGuard>  mApp;
    std::atomic<bool>    mbStopping{ false };
    // Resolved once, here, and not per request: the answer cannot change while we are bound, and
    // a wildcard bind makes it a routing-table lookup that has no business being on a hot path.
    std::string          msOrigin;

    Impl(Ops& ops, JotStore& store, const HttpConfig& config,
         Journal* pJournal, const SnapshotConfig& snapConfig, IpAcl& acl, History* pHistory)
        : mOps(ops), mStore(store), mConfig(config), mpJournal(pJournal), mAcl(acl),
          mpHistory(pHistory), mMcp(ops, store), mSnapConfig(snapConfig),
          msOrigin(ResolveAdvertisedOrigin(config))
    {
        // The middleware instance is owned by the app, so it is wired here rather than constructed
        // with a reference.
        mApp.get_middleware<AclGuard>().mpAcl = &mAcl;
    }

    // The purge request file sits beside the snapshot. Derived rather than passed so there is one
    // definition of "the data directory" and it is the one persistence already uses.
    std::string DataDir() const
    {
        const size_t nSlash = mSnapConfig.msPath.find_last_of("/\\");
        return nSlash == std::string::npos ? std::string(".") : mSnapConfig.msPath.substr(0, nSlash);
    }

    NameTables Names() const
    {
        NameTables names;
        mStore.SnapshotNames(names);
        return names;
    }

    // Empty token means no auth. Constant-time comparison is not warranted here - this is a LAN
    // shared secret, not a password hash, and the timing channel leaks nothing an attacker on the
    // same network could not get more easily.
    bool Authorized(const crow::request& req) const
    {
        if (mConfig.msToken.empty())
            return true;

        const std::string sHeader = req.get_header_value("Authorization");
        const std::string sExpect = "Bearer " + mConfig.msToken;
        return sHeader == sExpect;
    }

    void BuildQuerySpec(const crow::request& req, Ops::QuerySpec& spec) const
    {
        spec.msText   = ParamOr(req, "q");
        spec.mTags    = ParamListAny(req, { "tag", "tags" });
        spec.mNotTags = ParamListAny(req, { "notag", "notags", "not_tag", "not_tags" });
        spec.msEditor = ParamOr(req, "editor");
        spec.msName   = ParamOr(req, "name");
        spec.msSince  = ParamOr(req, "since");
        spec.msUntil  = ParamOr(req, "until");
        spec.msLink   = ParamOr(req, "link");
        spec.msOrder  = ParamOr(req, "order");
        spec.mnLimit  = ParamSize(req, "limit", 0);
        spec.mnOffset = ParamSize(req, "offset", 0);
        spec.mbPrefix = ParamFlag(req, "prefix");
    }

    void Routes()
    {
        //------------------------------------------------------------------------------------
        // Reads
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/jots").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            // Every parameter this route understands. A misspelling here used to silently widen
            // the result set to the whole store instead of narrowing it.
            const std::string sBad = UnknownParam(req, {
                "q", "tag", "tags", "notag", "notags", "not_tag", "not_tags",
                "editor", "name", "since", "until", "link", "order",
                "limit", "offset", "prefix", "verbose", "brief" });
            if (!sBad.empty())
                return Fail(400, "unknown query parameter '" + sBad + "'");

            Ops::QuerySpec spec;
            BuildQuerySpec(req, spec);

            Query query;
            if (std::error_code ec = mOps.BuildQuery(spec, query))
                return Fail(ec);

            SearchResultSet results;
            if (std::error_code ec = mOps.Search(query, results))
                return Fail(ec);

            return Ok(JOTJSON::SearchToJson(results, Names(), ParamFlag(req, "verbose"),
                                             ParamFlag(req, "brief")));
        });

        CROW_ROUTE(mApp, "/jots/<int>").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req, int64_t nID)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            Jot jot;
            if (std::error_code ec = mOps.Get(nID, jot))
                return Fail(ec);

            return Ok(JOTJSON::ToJson(Flatten(jot, Names()), ParamFlag(req, "verbose")));
        });

        CROW_ROUTE(mApp, "/jots/by-name/<string>").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req, const std::string& sName)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            Jot jot;
            if (std::error_code ec = mOps.GetByName(sName, jot))
                return Fail(ec);

            return Ok(JOTJSON::ToJson(Flatten(jot, Names()), ParamFlag(req, "verbose")));
        });

        CROW_ROUTE(mApp, "/jots/<int>/links").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req, int64_t nID)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            std::vector<Jot> vJots;
            if (std::error_code ec = mOps.Links(nID, ParamSize(req, "depth", 1), vJots))
                return Fail(ec);

            return Ok(JOTJSON::JotListToJson(vJots, Names(), ParamFlag(req, "verbose")));
        });

        //------------------------------------------------------------------------------------
        // Writes
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/jots").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            JotInput input;
            std::string sError;
            if (!JOTJSON::ParseInput(req.body, input, sError))
                return Fail(400, sError);

            AddResult result;
            // ?upsert=1 keys on the slug and updates in place. Kept opt-in so a plain POST can
            // never silently overwrite a memory somebody wrote by hand.
            const std::error_code ec = ParamFlag(req, "upsert")
                ? mOps.Upsert(input, ParamI64(req, "expect_updated", 0), result)
                : mOps.Add(input, result);

            if (ec)
                return Fail(ec);

            crow::response res(result.mbCreated ? 201 : 200,
                JOTJSON::MutationToJson(result, Names(), ParamFlag(req, "verbose")));
            res.set_header("Content-Type", "application/json");
            return res;
        });

        CROW_ROUTE(mApp, "/jots/<int>").methods(crow::HTTPMethod::Patch)
        ([this](const crow::request& req, int64_t nID)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            JotInput patch;
            std::string sError;
            if (!JOTJSON::ParseInput(req.body, patch, sError))
                return Fail(400, sError);

            AddResult result;
            // expect_updated is the multi-agent guard: pass the value you last saw and a
            // concurrent edit comes back 409 instead of silently losing one of the two writes.
            const std::error_code ec =
                mOps.Update(nID, patch, ParamI64(req, "expect_updated", 0), result);
            if (ec)
                return Fail(ec);

            return Ok(JOTJSON::MutationToJson(result, Names(), ParamFlag(req, "verbose")));
        });

        CROW_ROUTE(mApp, "/jots/<int>").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req, int64_t nID)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            if (std::error_code ec = mOps.Delete(nID))
                return Fail(ec);

            return Ok("{\"deleted\":true}");
        });

        //------------------------------------------------------------------------------------
        // Tags
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/tags").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            std::vector<TagStat> vTags;
            mOps.ListTags(vTags, ParamFlag(req, "reserved"));
            return Ok(JOTJSON::TagsToJson(vTags));
        });

        CROW_ROUTE(mApp, "/tags/similar").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            std::vector<TagCluster> vClusters;
            mOps.TagClusters(vClusters);
            return Ok(JOTJSON::ClustersToJson(vClusters));
        });

        CROW_ROUTE(mApp, "/tags/merge").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            auto body = crow::json::load(req.body);
            if (!body || !body.has("from") || !body.has("to"))
                return Fail(400, "expected {\"from\":[...],\"to\":\"...\"}");

            std::vector<std::string> vFrom;
            for (const auto& entry : body["from"])
                vFrom.push_back(entry.s());

            size_t nChanged = 0;
            if (std::error_code ec = mOps.MergeTags(vFrom, body["to"].s(), nChanged))
                return Fail(ec);

            return Ok("{\"changed\":" + std::to_string(nChanged) + "}");
        });

        //------------------------------------------------------------------------------------
        // Service
        //------------------------------------------------------------------------------------

        //------------------------------------------------------------------------------------
        // MCP - Streamable HTTP transport.
        //
        // A POST carries one JSON-RPC message (or a batch) and gets a plain JSON response. The
        // spec also allows answering with an SSE stream; that only earns its complexity for a
        // server with messages of its own to push, and Loom has none.
        //
        // A notification produces no response body at all, which is 202 Accepted rather than an
        // empty 200 - the difference matters to a strict client.
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/mcp").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            const std::string sResponse = mMcp.Handle(req.body);
            if (sResponse.empty())
                return crow::response(202);

            crow::response res(200, sResponse);
            res.set_header("Content-Type", "application/json");
            return res;
        });

        // GET on the endpoint is how a client opens a server-to-client SSE stream. Loom never
        // initiates messages, so it declines - which the spec explicitly permits.
        CROW_ROUTE(mApp, "/mcp").methods(crow::HTTPMethod::Get)
        ([]()
        {
            return Fail(405, "this MCP server does not open server-initiated streams");
        });

        // The dashboard. Served from the same origin as the API, which is what lets the page call
        // it with no CORS configuration and no separate static server.
        CROW_ROUTE(mApp, "/").methods(crow::HTTPMethod::Get)
        ([]()
        {
            crow::response res(200, std::string(LoomDashboardHtml()));
            res.set_header("Content-Type", "text/html; charset=utf-8");
            return res;
        });

        // Icon art for the dashboard itself - see web/IconAssets.h for why these are embedded
        // base64 rather than files on disk. Small is the favicon and header mark; full is used
        // only by the About panel, so it is fetched on demand rather than on every page load.
        CROW_ROUTE(mApp, "/icon.png").methods(crow::HTTPMethod::Get)
        ([]()
        {
            const auto& png = LoomIconSmallPng();
            crow::response res(200, std::string(reinterpret_cast<const char*>(png.data()), png.size()));
            res.set_header("Content-Type", "image/png");
            return res;
        });

        CROW_ROUTE(mApp, "/icon-full.png").methods(crow::HTTPMethod::Get)
        ([]()
        {
            const auto& png = LoomIconFullPng();
            crow::response res(200, std::string(reinterpret_cast<const char*>(png.data()), png.size()));
            res.set_header("Content-Type", "image/png");
            return res;
        });

        CROW_ROUTE(mApp, "/health").methods(crow::HTTPMethod::Get)
        ([]()
        {
            // Deliberately unauthenticated: a health check that needs a credential is one more
            // thing that can wrongly report the service as down.
            return Ok("{\"ok\":true}");
        });

        CROW_ROUTE(mApp, "/stats").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            PersistStats persist;
            if (mpJournal)
                mpJournal->FillStats(persist);
            return Ok(JOTJSON::StatsToJson(mOps.Stats(), persist, msOrigin,
                                           !mConfig.msToken.empty()));
        });

        //------------------------------------------------------------------------------------
        // Access list
        //
        // The list is edited over the network by the thing the list controls, which is what makes
        // this different from every other route here: a mistake is not a bad response, it is a
        // service that has stopped answering. Two guards, at different strengths:
        //
        //   IpAcl always allows loopback           - a floor, not overridable, so somebody at the
        //                                            machine can always repair a bad list.
        //   PUT refuses to lock the caller out     - advice, overridable with ?force=1, because
        //                                            "allow the LAN but not this laptop" is a
        //                                            legitimate thing to want to configure from
        //                                            the laptop you are about to stop using.
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/acl").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            bool bEnabled = false;
            std::vector<AclEntry> vEntries;
            mAcl.Get(bEnabled, vEntries);

            crow::json::wvalue out;
            out["enabled"] = bEnabled;
            // Echoing the caller's address back is what lets the dashboard offer "add this machine"
            // without the operator having to know what address they arrive from - which, behind any
            // kind of NAT or a dual-stack listener, they routinely do not.
            out["caller"]  = req.remote_ip_address;
            out["caller_is_loopback"] = IpAcl::IsLoopback(req.remote_ip_address);

            std::vector<crow::json::wvalue> vOut;
            for (const AclEntry& e : vEntries)
            {
                crow::json::wvalue entry;
                entry["rule"] = e.msRule;
                entry["note"] = e.msNote;
                vOut.push_back(std::move(entry));
            }
            out["entries"] = std::move(vOut);
            return Ok(out.dump());
        });

        CROW_ROUTE(mApp, "/acl").methods(crow::HTTPMethod::Put)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");

            const std::string sBad = UnknownParam(req, { "force" });
            if (!sBad.empty())
                return Fail(400, "unknown query parameter '" + sBad + "'");

            auto body = crow::json::load(req.body);
            if (!body || !body.has("enabled"))
                return Fail(400, "expected {\"enabled\":bool,\"entries\":[{\"rule\",\"note\"}]}");

            const bool bEnabled = body["enabled"].b();

            std::vector<AclEntry> vEntries;
            if (body.has("entries"))
            {
                for (const auto& e : body["entries"])
                {
                    AclEntry entry;
                    if (e.t() == crow::json::type::String)
                    {
                        entry.msRule = e.s();
                    }
                    else
                    {
                        if (!e.has("rule"))
                            return Fail(400, "each entry needs a 'rule'");
                        entry.msRule = e["rule"].s();
                        if (e.has("note"))
                            entry.msNote = e["note"].s();
                    }
                    vEntries.push_back(entry);
                }
            }

            // Enabling with nothing listed would leave only loopback able to reach the service.
            // That is a coherent thing to ask for but almost never what somebody means, so it has
            // to be asked for explicitly.
            if (bEnabled && vEntries.empty() && !ParamFlag(req, "force"))
                return Fail(400, "enabling an empty list would leave only this machine able to "
                                 "reach Loom - pass ?force=1 if that is what you want");

            // The lockout check runs against the PROPOSED list, before anything is applied. Parse
            // failures are reported here too so the caller gets the offending rule rather than a
            // generic rejection out of Set().
            if (bEnabled && !ParamFlag(req, "force") && !IpAcl::IsLoopback(req.remote_ip_address))
            {
                uint8_t caller[16];
                if (!IpAcl::ParseAddress(req.remote_ip_address, caller))
                    return Fail(400, "cannot verify this change would not lock you out: your own "
                                     "address '" + req.remote_ip_address + "' is unparseable - "
                                     "pass ?force=1 to apply it anyway");

                bool bCovered = false;
                for (const AclEntry& e : vEntries)
                {
                    AclRule rule;
                    if (!IpAcl::ParseRule(e.msRule, rule))
                        return Fail(400, "'" + e.msRule + "' is not an address or CIDR range");
                    if (IpAcl::Matches(rule, caller))
                    {
                        bCovered = true;
                        break;
                    }
                }

                if (!bCovered)
                    return Fail(403, "this list does not include your own address " +
                                     req.remote_ip_address + ", so applying it would lock you out "
                                     "of this page - add it, or pass ?force=1 if you mean to");
            }

            std::string sBadRule;
            if (std::error_code ec = mAcl.Set(bEnabled, vEntries, sBadRule))
            {
                if (!sBadRule.empty())
                    return Fail(400, "'" + sBadRule + "' is not an address or CIDR range");
                return Fail(500, "could not write the access list: " + ec.message());
            }

            bool bNowEnabled = false;
            std::vector<AclEntry> vNow;
            mAcl.Get(bNowEnabled, vNow);
            return Ok("{\"enabled\":" + std::string(bNowEnabled ? "true" : "false") +
                      ",\"count\":" + std::to_string(vNow.size()) + "}");
        });

        //------------------------------------------------------------------------------------
        // History, restore and purge
        //------------------------------------------------------------------------------------

        CROW_ROUTE(mApp, "/history").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpHistory)       return Fail(403, "running without persistence - no history log");

            const std::string sBad = UnknownParam(req, { "limit", "offset", "id" });
            if (!sBad.empty())
                return Fail(400, "unknown query parameter '" + sBad + "'");

            const tJotID idFilter = ParamI64(req, "id", kInvalidJotID);

            std::vector<HistoryEntry> vEntries;
            size_t nTotal = 0;
            mpHistory->List(idFilter, ParamSize(req, "limit", 60), ParamSize(req, "offset", 0),
                            vEntries, nTotal);

            std::vector<crow::json::wvalue> vOut;
            for (const HistoryEntry& e : vEntries)
            {
                crow::json::wvalue entry;
                entry["seq"]     = e.mnSeq;
                entry["at"]      = e.mnAtUS;
                entry["op"]      = e.mbDelete ? "del" : "put";
                entry["id"]      = e.mID;
                entry["name"]    = e.msName;
                entry["editor"]  = e.msEditor;
                entry["summary"] = e.msSummary;
                vOut.push_back(std::move(entry));
            }

            const HistoryStats st = mpHistory->Stats();
            crow::json::wvalue out;
            out["entries"]  = std::move(vOut);
            out["total"]    = nTotal;
            out["recorded"] = st.mnEntries;
            out["bytes"]    = st.mnBytes;
            return Ok(out.dump());
        });

        // Re-applies one logged version. A `put` entry restores itself; a `del` entry restores what
        // was in force immediately before it, because "undo this delete" is the only thing anybody
        // means by restoring a deletion.
        CROW_ROUTE(mApp, "/history/restore").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpHistory)       return Fail(403, "running without persistence - no history log");

            auto body = crow::json::load(req.body);
            if (!body || !body.has("seq"))
                return Fail(400, "expected {\"seq\":N}");

            const uint64_t nSeq = static_cast<uint64_t>(body["seq"].i());

            HistoryEntry entry;
            if (!mpHistory->Get(nSeq, entry))
                return Fail(404, "no history entry " + std::to_string(nSeq) +
                                 " - it may have aged out of the log");

            HistoryEntry target = entry;
            if (entry.mbDelete && !mpHistory->Previous(nSeq, target))
                return Fail(404, "nothing to restore: this jot was deleted with no earlier version "
                                 "still in the log");

            FlatJot record;
            std::string sError;
            if (target.msRecord.empty() || !JOTJSON::ParseFlat(target.msRecord, record, sError))
                return Fail(500, "history entry is unreadable: " + sError);

            tJotID   conflictID = kInvalidJotID;
            AddResult result;
            if (std::error_code ec = mOps.Restore(record, conflictID, result))
            {
                if (conflictID != kInvalidJotID)
                    return Fail(409, "the name '" + record.msName + "' now belongs to jot " +
                                     std::to_string(conflictID) + " - rename or remove that one "
                                     "first, or this restore would leave two jots claiming it");
                return Fail(ec);
            }

            crow::json::wvalue out;
            out["restored"]      = true;
            out["from_seq"]      = target.mnSeq;
            out["undid_delete"]  = entry.mbDelete;
            out["jot"]           = crow::json::load(
                JOTJSON::ToJson(Flatten(result.mJot, Names()), false));
            return Ok(out.dump());
        });

        // Writes the request file and hands back the instructions. NOTHING IS ERASED HERE - see
        // persist/Purge.h for why the destructive half cannot run inside a live server.
        CROW_ROUTE(mApp, "/purge/request").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpJournal)       return Fail(403, "running without persistence - nothing to purge");

            auto body = crow::json::load(req.body);
            if (!body || !body.has("ids"))
                return Fail(400, "expected {\"ids\":[...],\"reason\":\"...\"}");

            PurgeRequest request;
            request.mnCreatedUS   = LOOMTIME::NowMicros();
            request.msRequestedBy = req.remote_ip_address;
            if (body.has("reason"))
                request.msReason = body["reason"].s();

            for (const auto& id : body["ids"])
                request.mIDs.push_back(static_cast<tJotID>(id.i()));

            if (request.mIDs.empty())
                return Fail(400, "name at least one jot id to purge");

            // Labels are captured NOW, while the jots still exist, because the confirmation step
            // happens after the service is stopped and there is nothing left to look them up in.
            for (tJotID id : request.mIDs)
            {
                PurgeLabel label;
                label.mID = id;
                Jot jot;
                if (!mOps.Get(id, jot))
                {
                    const FlatJot flat = Flatten(jot, Names());
                    label.msName = flat.msName;
                    const std::string& sSrc = flat.msSummary.empty() ? flat.msText : flat.msSummary;
                    label.msSummary = sSrc.substr(0, 100);
                    if (sSrc.size() > 100)
                        label.msSummary += "...";
                }
                request.mLabels.push_back(label);
            }

            const std::string sPath = PURGE::RequestPath(DataDir());
            if (std::error_code ec = PURGE::WriteRequest(sPath, request))
                return Fail(500, "could not write the purge request: " + ec.message());

            crow::json::wvalue out;
            out["written"]      = sPath;
            out["ids"]          = request.mIDs.size();
            out["instructions"] = PURGE::AgentInstructions(DataDir(), request);
            return Ok(out.dump());
        });

        CROW_ROUTE(mApp, "/purge/request").methods(crow::HTTPMethod::Get)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpJournal)       return Fail(403, "running without persistence");

            const std::string sPath = PURGE::RequestPath(DataDir());
            PurgeRequest request;
            std::string sError;
            if (!PURGE::ReadRequest(sPath, request, sError))
                return Fail(404, sError);

            std::vector<crow::json::wvalue> vJots;
            for (const PurgeLabel& label : request.mLabels)
            {
                crow::json::wvalue entry;
                entry["id"]      = label.mID;
                entry["name"]    = label.msName;
                entry["summary"] = label.msSummary;
                vJots.push_back(std::move(entry));
            }

            crow::json::wvalue out;
            out["created"]      = request.mnCreatedUS;
            out["reason"]       = request.msReason;
            out["requested_by"] = request.msRequestedBy;
            out["jots"]         = std::move(vJots);
            out["instructions"] = PURGE::AgentInstructions(DataDir(), request);
            return Ok(out.dump());
        });

        // Cancelling is just removing the file, and it is the reason the request is inert until
        // somebody runs the offline half: right up to that point this undoes the whole thing.
        CROW_ROUTE(mApp, "/purge/request").methods(crow::HTTPMethod::Delete)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpJournal)       return Fail(403, "running without persistence");

            PURGE::ClearRequest(PURGE::RequestPath(DataDir()));
            return Ok("{\"cancelled\":true}");
        });

        CROW_ROUTE(mApp, "/admin/snapshot").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpJournal)       return Fail(403, "running without persistence");

            size_t nWritten = 0;
            if (std::error_code ec = SNAPSHOT::Write(mSnapConfig, mStore, mpJournal, nWritten))
                return Fail(ec);

            return Ok("{\"records\":" + std::to_string(nWritten) + "}");
        });

        // Forces everything queued to be written and fsynced. This is what a caller uses when it
        // needs to KNOW a write survived, without paying --sync=always on every request.
        CROW_ROUTE(mApp, "/admin/flush").methods(crow::HTTPMethod::Post)
        ([this](const crow::request& req)
        {
            if (!Authorized(req)) return Fail(401, "missing or invalid bearer token");
            if (!mpJournal)       return Fail(403, "running without persistence");

            if (std::error_code ec = mpJournal->Flush())
                return Fail(ec);

            return Ok("{\"flushed\":true}");
        });
    }
};


//====================================================================================================

HttpServer::HttpServer(Ops& ops, JotStore& store, const HttpConfig& config,
                       Journal* pJournal, const SnapshotConfig& snapConfig, IpAcl& acl,
                       History* pHistory)
    : mpImpl(std::make_unique<Impl>(ops, store, config, pJournal, snapConfig, acl, pHistory))
{
    mpImpl->Routes();
}

HttpServer::~HttpServer() = default;

const std::string& HttpServer::AdvertisedOrigin() const
{
    return mpImpl->msOrigin;
}

std::error_code HttpServer::Run()
{
    // Crow's own logging is chatty and formats nothing like the rest of the service; route it
    // through the same output path so there is one log, not two.
    crow::logger::setLogLevel(crow::LogLevel::Warning);

    const size_t nThreads = mpImpl->mConfig.mnThreads != 0
        ? mpImpl->mConfig.mnThreads
        : std::thread::hardware_concurrency();

    mpImpl->mApp
        .bindaddr(mpImpl->mConfig.msBind)
        .port(mpImpl->mConfig.mnPort)
        .concurrency(static_cast<std::uint16_t>(nThreads))
        .run();

    return LoomOK();
}

void HttpServer::Stop()
{
    if (mpImpl && !mpImpl->mbStopping.exchange(true))
        mpImpl->mApp.stop();
}
