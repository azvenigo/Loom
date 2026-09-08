#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdint>
#include <mutex>
#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// JotpostStatus - answers "is jotpost reachable right now" for GET /stats and the dashboard's
// Health page.
//
// A RAW TCP CONNECT TO host:port, NOTHING MORE. jotpost (jotpost/main.cpp) has no /health route of
// its own - its only route is POST /jot - so this does not attempt an HTTP request at all. A
// successful connect() is exactly the fact being asked for: something is listening on that port.
// Getting an HTTP response back would prove nothing more, for the cost of writing an HTTP client
// Loom does not otherwise need - see http/HttpServer.h's own note on why crow is quarantined to one
// file; the same reasoning argues against pulling in a client library for one boolean.
//
// CACHED, NOT POLLED. Unlike Watcher (persist/Watcher.h - "the only background thread in Loom"),
// this owns no thread of its own: GET /stats is already hit every few seconds by the dashboard, so
// the check rides that request and remembers the answer for nCacheSec rather than adding a second
// background loop for one probe. A cache miss can block the calling HTTP thread for up to
// nTimeoutMS - acceptable for an internal tool polled this rarely, and bounded either way.
//
// POSIX ONLY FOR NOW, same shape as Watcher's child-process exec path (see Watcher.h): a Windows
// build compiles this, but Probe() always reports unreachable there, the same "always fails
// cleanly" stub Watcher.cpp uses for RunChildWithTimeout on Windows, rather than a second
// implementation for a feature nothing currently deploys on Windows.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct JotpostConfig
{
    std::string sHost;                 // empty = feature off; GET /stats omits the "jotpost" block
    uint16_t    nPort      = 7701;
    int         nTimeoutMS = 800;      // bound on how long a cache-miss can block the caller
    int         nCacheSec  = 15;       // how long a probe result is reused before probing again
};

class JotpostStatus
{
public:
    explicit JotpostStatus(JotpostConfig config) : mConfig(std::move(config)) {}

    JotpostStatus(const JotpostStatus&)            = delete;
    JotpostStatus& operator=(const JotpostStatus&) = delete;

    bool Configured() const { return !mConfig.sHost.empty(); }

    // Re-probes if the cached result is older than nCacheSec, otherwise returns it as-is.
    // Thread-safe: concurrent callers arriving during a cache miss block on the same probe rather
    // than each opening their own socket.
    bool Reachable();

    // Microseconds of the last actual probe (not the last call to Reachable()) - 0 if nothing has
    // probed yet.
    int64_t CheckedAtUS() const;

private:
    bool Probe() const;   // the actual connect(); no locking of its own - Reachable() holds it

    JotpostConfig mConfig;
    std::mutex    mMutex;
    bool          mbReachable = false;
    int64_t       mnCheckedUS = 0;
};
