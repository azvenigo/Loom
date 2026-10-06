// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// jot - a deliberately tiny HTTP endpoint: POST a body, it gets appended as one line to a file.
// Built to give ZHotkey (or anything else) a network-reachable way to land a jot somewhere Loom's
// WatchList (persist/WatchList.h) can pick it up, without either side needing to share a
// filesystem directly. See the zhotkey-web-post-endpoint-plan jot in Loom for the design
// discussion this came out of.
//
// It also holds ONE shared clip (PUT/GET /jot/clip): an opaque, client-side-encrypted blob that a
// device writes and other devices read back. The server never sees a key or any plaintext - it
// stores the sealed envelope verbatim, so a compromised host leaks nothing readable. Single slot,
// last write wins; it is not a history and nothing here ever interprets the contents. Jots stay
// write-only: nothing ever reads a jot back out over HTTP.
//
// DELIBERATELY NOT a JSON parser. The body is expected to already be one JSON object - ZHotkey
// builds `{"ts":"...","entry":"..."}` itself, matching persist/Importer.cpp's JotsLog format - so
// this only does cheap byte-level validation, not grammar. A malformed-but-shape-passing line is
// caught and dropped by Loom's importer at ingest time (same as any other torn/bad JSONL line), so
// this layer's one job is to be a safe front door: never corrupt the file, never run anything,
// never read a jot back.
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
// EVERY REJECTION IS THE SAME BARE 404. A bad token, a malformed body, an oversized body, a wrong
// method and a path that does not exist are indistinguishable on the wire, and no Server: header
// goes out, so a prober cannot tell this endpoint from a web server that simply has nothing there.
// That is worth stating plainly: this hides WHAT is listening, not THAT something is listening -
// a port scanner reads the TCP handshake and will still report the port open no matter what we
// answer. Do not treat this as a substitute for not exposing the port. The intended public
// deployment is behind an existing TLS vhost (see packaging/systemd/jot.service), which adds no
// new open port at all; this file's job is to make sure that /jot does not stand out from the rest
// of that site's 404s once a crawler finds it.
//
// The corollary is that a legitimate client gets no diagnostic either, so a rejection that already
// proved it holds the token is logged to stderr (journald) with the actual reason. Unauthenticated
// noise is dropped silently, which is what keeps scanner traffic out of the journal.
//
// Auth is a shared secret: --token=SECRET, or --token-file=PATH to keep the secret out of both the
// unit file (which is public in git) and `ps` output. No token configured leaves POST /jot open
// to anything that can reach it - fine for a LAN-only bind, NOT acceptable once anything off-LAN
// can route to it. The clip routes are stricter: with no token configured they do not exist at
// all (bare 404), because a clip is private data even sealed.
//
// The clip file is replaced atomically (temp file + fsync + rename), so a GET racing a PUT sees
// the old clip or the new one, never a torn mix. The body gets the same byte-level checks as a
// jot (no control bytes, valid UTF-8, one braced object), which means the client must send its
// sealed envelope as single-line JSON - base64 for the ciphertext, not raw bytes.
//
// Warning level is relaxed for this file (CMakeLists.txt) the same way it is for HttpServer.cpp -
// crow is vendored and does not survive /Wall /WX.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "vendor/crow/crow.h"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

namespace
{
    constexpr size_t kMaxBodyBytes = 16 * 1024;

    // A sealed clip is base64 inside JSON, so this is roughly 6MB of image. Keep in step with
    // LimitRequestBody for /jot/clip in packaging/apache/jot-proxy.conf.example.
    constexpr size_t kMaxClipBytes = 8 * 1024 * 1024;

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

    // The one answer to everything this endpoint does not serve. Crow's stock 404 - a 15-byte
    // "404 Not Found" body, no hint of what was actually wrong. Identical for every rejection, so
    // the only thing a caller learns is that it did not work. Note it is still distinguishable
    // from a full site's 404 page by length alone, which is what ProxyErrorOverride handles in
    // packaging/apache/jot-proxy.conf.example.
    crow::response Deny()
    {
        return crow::response(404);
    }

    // Same 404 on the wire, but say why in the journal. Only ever called once the caller has
    // already proved it holds the token, so scanner traffic cannot use this to fill the disk.
    crow::response DenyLogged(const char* szReason)
    {
        std::fprintf(stderr, "jot: rejected an authenticated request: %s\n", szReason);
        return Deny();
    }

    // The token is a secret that now travels the public internet, so don't let the compare bail at
    // the first differing byte. Length still leaks, which is fine - a token's length is not the
    // part worth hiding.
    bool SecretEquals(std::string_view sA, std::string_view sB)
    {
        if (sA.size() != sB.size())
            return false;

        unsigned char nDiff = 0;
        for (size_t i = 0; i < sA.size(); ++i)
            nDiff |= static_cast<unsigned char>(sA[i] ^ sB[i]);
        return nDiff == 0;
    }

    // Whole file, minus trailing newline(s) - so `echo secret > token` does the obvious thing.
    bool ReadTokenFile(const std::string& sFile, std::string& sTokenOut)
    {
        std::ifstream in(sFile, std::ios::binary);
        if (!in)
            return false;

        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();

        sTokenOut = s;
        return !sTokenOut.empty();
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

    // Temp file in the same directory (so rename stays on one filesystem and is atomic), fsynced
    // before the rename so a crash leaves the old clip or the new one, never an empty file. 0600:
    // sealed or not, nothing else on the box has a reason to read it.
    bool ReplaceFile(const std::string& sPath, const std::string& sBody)
    {
        const std::string sTmp = sPath + ".tmp";
        const int fd = ::open(sTmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0)
            return false;

        bool bOk = true;
        for (size_t nDone = 0; bOk && nDone < sBody.size(); )
        {
            const ssize_t n = ::write(fd, sBody.data() + nDone, sBody.size() - nDone);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                bOk = false;
            else
                nDone += static_cast<size_t>(n);
        }
        bOk = bOk && ::fsync(fd) == 0;
        ::close(fd);

        if (!bOk || ::rename(sTmp.c_str(), sPath.c_str()) != 0)
        {
            ::unlink(sTmp.c_str());
            return false;
        }
        return true;
    }

    // PUT /jot/clip. Caller has already proved it holds the token.
    crow::response StoreClip(const std::string& sClipPath, std::mutex& mtxClip, const std::string& sBody)
    {
        if (sBody.size() > kMaxClipBytes)
            return DenyLogged("clip over the size cap");
        if (HasIllegalBytes(sBody))
            return DenyLogged("clip has control bytes or invalid UTF-8");
        if (!LooksLikeJsonObject(sBody))
            return DenyLogged("clip is not a braced JSON object");

        // Serialise writers: two PUTs would otherwise share one temp path and interleave.
        std::lock_guard<std::mutex> lock(mtxClip);
        return ReplaceFile(sClipPath, sBody) ? crow::response(204) : crow::response(500);
    }

    // GET /jot/clip. 204 (not 404) when nothing has been stored yet: the caller already proved it
    // holds the token, and 404 is reserved for "rejected", so a client can tell the two apart.
    crow::response ServeClip(const std::string& sClipPath)
    {
        std::ifstream in(sClipPath, std::ios::binary);
        if (!in)
            return crow::response(204);

        std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        crow::response res(200, std::move(s));
        res.set_header("Content-Type", "application/json");
        res.set_header("Cache-Control", "no-store");
        return res;
    }
}

int main(int argc, char* argv[])
{
    std::string sBind  = "0.0.0.0";
    uint16_t    nPort  = 7701;
    std::string sPath  = "/mnt/fast/web/jot_log/jots.json";
    // Deliberately NOT under the jot_log directory: Loom's WatchList ingests that, and a clip is
    // not a jot. Empty (--clip-path=) turns the clip routes off.
    std::string sClipPath = "/mnt/fast/web/jot_clip/clip.json";
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
        else if (const char* p = Value("--clip-path=")) sClipPath = p;
        else if (const char* p = Value("--token=")) sToken = p;
        else if (const char* p = Value("--token-file="))
        {
            // Hard failure rather than silently running open - an unreadable token file is exactly
            // the case where falling back to "no auth" would be worst.
            if (!ReadTokenFile(p, sToken))
            {
                std::fprintf(stderr, "jot: could not read a token from %s\n", p);
                return 1;
            }
        }
        else if (sArg == "--help")
        {
            std::printf("jot --bind=IP --port=N --path=FILE --token=SECRET\n"
                        "  --bind=IP          address to listen on   (default: 0.0.0.0)\n"
                        "  --port=N           port to listen on      (default: 7701)\n"
                        "  --path=FILE        file to append jots to (default: /mnt/fast/web/jot_log/jots.json)\n"
                        "  --clip-path=FILE   file holding the one shared clip, PUT/GET /jot/clip\n"
                        "                     (default: /mnt/fast/web/jot_clip/clip.json; empty = off;\n"
                        "                     also off whenever no token is set)\n"
                        "  --token=SECRET     require Authorization: Bearer SECRET (default: none, open)\n"
                        "  --token-file=PATH  same, read from PATH - keeps the secret out of ps and out of git\n"
                        "\n"
                        "Every rejection answers with a bare 404. Set a token before anything off-LAN\n"
                        "can reach this; run it behind an existing TLS vhost rather than opening a port.\n");
            return 0;
        }
    }

    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(sPath).parent_path(), ec);
    if (ec)
    {
        std::fprintf(stderr, "jot: could not create parent directory of %s: %s\n",
                     sPath.c_str(), ec.message().c_str());
        return 1;
    }

    if (!sClipPath.empty())
    {
        std::filesystem::create_directories(std::filesystem::path(sClipPath).parent_path(), ec);
        if (ec)
        {
            std::fprintf(stderr, "jot: could not create parent directory of %s: %s\n",
                         sClipPath.c_str(), ec.message().c_str());
            return 1;
        }
    }

    crow::logger::setLogLevel(crow::LogLevel::Warning);
    crow::SimpleApp app;
    std::mutex mtxClip;

    // No Server: header - crow would otherwise announce "Crow/x.y" on every response, including the
    // 404s that are supposed to look like nothing was ever there.
    app.server_name("");

    // Deliberately a catchall instead of CROW_ROUTE(app, "/jot"): a registered route makes crow
    // answer a wrong-method request with `405 Allow: POST`, which tells a prober precisely what
    // lives here and how to call it. Matching the path and method by hand means there is exactly
    // one code path off this handler that is not a 404.
    CROW_CATCHALL_ROUTE(app)
    ([sPath, sClipPath, sToken, &mtxClip](const crow::request& req)
    {
        // The clip routes exist only with a token: a clip is private data, so it never rides the
        // open-LAN-bind allowance that POST /jot has.
        const bool bClipRoute = req.url == "/jot/clip" && !sClipPath.empty() && !sToken.empty() &&
                                (req.method == crow::HTTPMethod::Put || req.method == crow::HTTPMethod::Get);
        const bool bJotRoute  = req.url == "/jot" && req.method == crow::HTTPMethod::Post;
        if (!bClipRoute && !bJotRoute)
            return Deny();

        if (!sToken.empty() && !SecretEquals(req.get_header_value("Authorization"), "Bearer " + sToken))
            return Deny();

        if (bClipRoute)
            return req.method == crow::HTTPMethod::Get ? ServeClip(sClipPath)
                                                       : StoreClip(sClipPath, mtxClip, req.body);

        // Past this point the caller holds the token, so a rejection is a real client bug worth
        // seeing in the journal rather than scanner noise.
        if (req.body.size() > kMaxBodyBytes)
            return DenyLogged("body over the size cap");

        if (HasIllegalBytes(req.body))
            return DenyLogged("control bytes or invalid UTF-8 in body");

        if (!LooksLikeJsonObject(req.body))
            return DenyLogged("body is not a braced JSON object");

        // The one non-404 failure: the request was good and we could not honour it. A caller that
        // got this far has the token, and silently dropping a jot on a full disk would be worse
        // than telling it the truth.
        return AppendLine(sPath, req.body) ? crow::response(204) : crow::response(500);
    });

    std::printf("jot listening on http://%s:%u -> %s%s\n", sBind.c_str(), nPort, sPath.c_str(),
                sToken.empty() ? " (NO AUTH TOKEN SET - LAN binds only)" : "");
    std::printf("jot clip: %s\n", sClipPath.empty() ? "off (--clip-path is empty)"
                                  : sToken.empty()  ? "off (needs a token)"
                                                    : sClipPath.c_str());
    std::fflush(stdout);   // journald gets a pipe, so this line would otherwise sit in the buffer
    app.bindaddr(sBind).port(nPort).run();
    // Crow returns from run() on a failed bind as well as on a clean stop; only is_bound() tells
    // them apart. Exit non-zero on the former so systemd's Restart=on-failure gets to retry.
    return app.is_bound() ? 0 : 1;
}
