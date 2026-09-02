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
}


//====================================================================================================

struct HttpServer::Impl
{
    Ops&              mOps;
    JotStore&         mStore;
    HttpConfig        mConfig;
    Journal*          mpJournal = nullptr;
    McpHandler        mMcp;
    SnapshotConfig    mSnapConfig;
    crow::SimpleApp   mApp;
    std::atomic<bool> mbStopping{ false };

    Impl(Ops& ops, JotStore& store, const HttpConfig& config,
         Journal* pJournal, const SnapshotConfig& snapConfig)
        : mOps(ops), mStore(store), mConfig(config), mpJournal(pJournal), mMcp(ops, store),
          mSnapConfig(snapConfig)
    {
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
            return Ok(JOTJSON::StatsToJson(mOps.Stats(), persist));
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
                       Journal* pJournal, const SnapshotConfig& snapConfig)
    : mpImpl(std::make_unique<Impl>(ops, store, config, pJournal, snapConfig))
{
    mpImpl->Routes();
}

HttpServer::~HttpServer() = default;

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
