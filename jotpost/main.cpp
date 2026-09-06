// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// jotpost - a deliberately tiny write-only HTTP endpoint: POST a body, it gets appended as one
// line to a file. Built to give ZHotkey (or anything else) a network-reachable way to land a jot
// somewhere Loom's WatchList (persist/WatchList.h) can pick it up, without either side needing to
// share a filesystem directly. See the zhotkey-web-post-endpoint-plan jot in Loom for the design
// discussion this came out of.
//
// DELIBERATELY NOT a JSON parser. The body is expected to already be one JSON object - ZHotkey
// builds `{"ts":"...","entry":"..."}` itself, matching persist/Importer.cpp's JotsLog format - so
// this only does cheap byte-level validation, not grammar. A malformed-but-shape-passing line is
// caught and dropped by Loom's importer at ingest time (same as any other torn/bad JSONL line), so
// this layer's one job is to be a safe front door: never corrupt the file, never run anything,
// never read anything back.
//
// Validation, all byte-level, no parsing:
//   - size capped (default 16KB) - also the DoS guard; crow rejects an oversized body itself, this
//     just makes the limit explicit and ours to change
//   - no raw C0 control bytes (0x00-0x1F) or 0x7F - a compliant JSON string escapes these, so a raw
//     one either means a bogus payload or an attempt to inject extra lines into the file (a raw \n
//     here would forge a second JSONL entry out of one POST)
//   - valid UTF-8 - JSON text is Unicode; a garbage byte sequence would desync anything downstream
//     that assumes valid UTF-8 (Loom's tokenizer, the dashboard, a browser)
//   - trimmed body starts with '{' and ends with '}' - catches wrong-content-entirely for free
//
// Appended with O_APPEND and one write() call for the whole line, which POSIX guarantees is atomic
// against other appenders - concurrent POSTs cannot interleave mid-line even with no lock.
//
// Optional shared-secret auth via --token: when set, requires `Authorization: Bearer <token>`. No
// token configured (the default) leaves the endpoint open to anything that can reach it - accepted
// only because this is a LAN-internal tool; see the jot for the tradeoff as discussed.
//
// Warning level is relaxed for this file (CMakeLists.txt) the same way it is for HttpServer.cpp -
// crow is vendored and does not survive /Wall /WX.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "vendor/crow/crow.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    constexpr size_t kMaxBodyBytes = 16 * 1024;

    // ASCII whitespace only - this is a shape check, not a JSON parser.
    std::string_view Trim(std::string_view s)
    {
        size_t nA = 0, nB = s.size();
        while (nA < nB && std::isspace(static_cast<unsigned char>(s[nA]))) ++nA;
        while (nB > nA && std::isspace(static_cast<unsigned char>(s[nB - 1]))) --nB;
        return s.substr(nA, nB - nA);
    }

    // One pass over the bytes: rejects C0 controls/DEL and checks UTF-8 continuation bytes at the
    // same time, so a bad payload is caught without a second walk over it.
    bool HasIllegalBytes(std::string_view s)
    {
        for (size_t i = 0; i < s.size(); )
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c <= 0x1F || c == 0x7F)
                return true;

            size_t nContinuation = 0;
            if      ((c & 0x80) == 0x00) { ++i; continue; }   // ASCII
            else if ((c & 0xE0) == 0xC0) nContinuation = 1;
            else if ((c & 0xF0) == 0xE0) nContinuation = 2;
            else if ((c & 0xF8) == 0xF0) nContinuation = 3;
            else return true;                                 // stray continuation byte or invalid lead byte

            if (i + nContinuation >= s.size())
                return true;                                  // truncated multi-byte sequence
            for (size_t j = 1; j <= nContinuation; ++j)
                if ((static_cast<unsigned char>(s[i + j]) & 0xC0) != 0x80)
                    return true;
            i += 1 + nContinuation;
        }
        return false;
    }

    bool LooksLikeJsonObject(std::string_view sBody)
    {
        const std::string_view sTrimmed = Trim(sBody);
        return !sTrimmed.empty() && sTrimmed.front() == '{' && sTrimmed.back() == '}';
    }

    // O_APPEND + one write() of the whole line. Opened and closed per request rather than held
    // open - this runs at human-typing rate, not a hot path, and a held-open fd would need its own
    // locking story for nothing gained here.
    bool AppendLine(const std::string& sPath, const std::string& sBody)
    {
        const int fd = ::open(sPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            return false;

        std::string sLine = sBody;
        sLine.push_back('\n');
        const ssize_t nWritten = ::write(fd, sLine.data(), sLine.size());
        ::close(fd);
        return nWritten == static_cast<ssize_t>(sLine.size());
    }
}

int main(int argc, char* argv[])
{
    std::string sBind  = "0.0.0.0";
    uint16_t    nPort  = 7701;
    std::string sPath  = "/mnt/fast/web/jot_log/jots.json";
    std::string sToken;

    for (int i = 1; i < argc; ++i)
    {
        const std::string sArg = argv[i];
        auto Value = [&](const std::string& sPrefix) -> const char*
        {
            return sArg.rfind(sPrefix, 0) == 0 ? sArg.c_str() + sPrefix.size() : nullptr;
        };
        if      (const char* p = Value("--bind="))  sBind  = p;
        else if (const char* p = Value("--port="))  nPort  = static_cast<uint16_t>(std::atoi(p));
        else if (const char* p = Value("--path="))  sPath  = p;
        else if (const char* p = Value("--token=")) sToken = p;
        else if (sArg == "--help")
        {
            std::printf("jotpost --bind=IP --port=N --path=FILE --token=SECRET\n"
                        "  --bind=IP      address to listen on   (default: 0.0.0.0)\n"
                        "  --port=N       port to listen on      (default: 7701)\n"
                        "  --path=FILE    file to append jots to (default: /mnt/fast/web/jot_log/jots.json)\n"
                        "  --token=SECRET require Authorization: Bearer SECRET (default: none, open)\n");
            return 0;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(sPath).parent_path(), ec);
    if (ec)
    {
        std::fprintf(stderr, "jotpost: could not create parent directory of %s: %s\n",
                     sPath.c_str(), ec.message().c_str());
        return 1;
    }

    crow::logger::setLogLevel(crow::LogLevel::Warning);
    crow::SimpleApp app;

    CROW_ROUTE(app, "/jot").methods(crow::HTTPMethod::Post)
    ([sPath, sToken](const crow::request& req)
    {
        if (!sToken.empty() && req.get_header_value("Authorization") != ("Bearer " + sToken))
            return crow::response(401);

        if (req.body.size() > kMaxBodyBytes)
            return crow::response(413);

        if (HasIllegalBytes(req.body) || !LooksLikeJsonObject(req.body))
            return crow::response(400);

        return crow::response(AppendLine(sPath, req.body) ? 204 : 500);
    });

    std::printf("jotpost listening on http://%s:%u -> %s%s\n", sBind.c_str(), nPort, sPath.c_str(),
                sToken.empty() ? " (no auth token set)" : "");
    app.bindaddr(sBind).port(nPort).run();
    return 0;
}
