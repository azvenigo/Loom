#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/IpAcl.h"
#include "core/Ops.h"
#include "persist/History.h"
#include "persist/Journal.h"
#include "persist/Snapshot.h"

#include <cstdint>
#include <memory>
#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// HttpServer - the REST front end.
//
// NOTE WHAT IS NOT IN THIS HEADER: crow. Not a type, not an include, not a forward declaration.
// The implementation is the only translation unit in Loom that knows a web framework exists, and
// that buys three specific things:
//
//   1. The /Wall /WX fallout from vendored third-party headers is quarantined to one file, via
//      set_source_files_properties in CMakeLists.txt, instead of forcing the whole project to
//      relax its warning level.
//   2. The store is benchmarked and tested without a socket - which is why loombench can measure
//      the ceiling and coretest can assert on behaviour, neither of them linking any of this.
//   3. If Crow disappoints under load, replacing it touches one file. The plan's benchmark
//      compares in-process against over-HTTP precisely so that decision has evidence behind it.
//
// Everything the routes do goes through Ops, never JotStore directly. That is what will make the
// MCP layer cheap: it becomes a schema table over the same calls rather than a second
// implementation of every route that then drifts from this one.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct HttpConfig
{
    std::string msBind    = "127.0.0.1";   // loopback by default; --bind opens it deliberately
    uint16_t    mnPort    = 7700;
    size_t      mnThreads = 0;             // 0 == hardware concurrency

    // Optional shared secret. When set, every request must carry "Authorization: Bearer <token>".
    // Empty means no auth, which is only reasonable on loopback.
    std::string msToken;
};

class HttpServer
{
public:
    // pJournal may be null (RAM-only). snapConfig is only read when pJournal is non-null.
    //
    // acl is taken by reference and outlives the server: it is enforced in a crow middleware rather
    // than at the top of each handler, which is the difference between a control that covers every
    // route and one that covers every route somebody remembered. A route added later is gated
    // whether or not its author knew the list existed.
    //
    // pHistory may be null, in which case /history and the purge routes report themselves
    // unavailable rather than pretending to work.
    HttpServer(Ops& ops, JotStore& store, const HttpConfig& config,
               Journal* pJournal, const SnapshotConfig& snapConfig, IpAcl& acl,
               History* pHistory);
    ~HttpServer();

    HttpServer(const HttpServer&)            = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Blocks until Stop() is called from another thread or a signal handler.
    std::error_code Run();

    // Safe to call from a signal handler context or another thread.
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> mpImpl;
};
