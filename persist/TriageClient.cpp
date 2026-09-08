// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/TriageClient.h"

#include "vendor/json.hpp"

#include <cstdio>
#include <ctime>
#include <string>

#ifndef _WIN32
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

using json = nlohmann::json;

namespace
{
    int64_t NowUS()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // Loom's due: tag form, matching Watcher.cpp's FormatDueLocal and Dashboard.h's dueOf/setDue.
    std::string FormatDueLocal(int64_t nUS)
    {
        const std::time_t t = static_cast<std::time_t>(nUS / 1000000);
        std::tm tmLocal{};
#ifdef _WIN32
        localtime_s(&tmLocal, &t);
#else
        localtime_r(&t, &tmLocal);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dt%H:%M", &tmLocal);
        return buf;
    }

    // ISO 8601 with a numeric UTC offset - what the service's now_local field expects. Built from
    // the jot's own id so relative phrases resolve from when the jot was written.
    std::string FormatIsoLocal(int64_t nUS)
    {
        const std::time_t t = static_cast<std::time_t>(nUS / 1000000);
        std::tm tmLocal{};
#ifdef _WIN32
        localtime_s(&tmLocal, &t);
#else
        localtime_r(&t, &tmLocal);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tmLocal);
        // strftime gives "-0700"; ISO 8601 wants "-07:00". Insert the colon rather than hand the
        // service a form it may parse strictly and reject.
        std::string s(buf);
        if (s.size() >= 5)
        {
            const char c = s[s.size() - 5];
            if (c == '+' || c == '-')
                s.insert(s.size() - 2, ":");
        }
        return s;
    }

    // Parses "due:2026-09-08t15:00" (or a bare "2026-09-08t15:00") back to local-time microseconds.
    // Returns false on anything that is not exactly that shape - a service that starts emitting a
    // different format should fail closed, not have its output half-understood.
    bool ParseDueTag(const std::string& sTag, int64_t& outUS, std::tm& outTm)
    {
        std::string s = sTag;
        if (s.rfind("due:", 0) == 0)
            s = s.substr(4);

        int nY = 0, nMo = 0, nD = 0, nH = 0, nMi = 0;
        int nConsumed = 0;
        if (std::sscanf(s.c_str(), "%4d-%2d-%2dt%2d:%2d%n", &nY, &nMo, &nD, &nH, &nMi, &nConsumed) != 5)
            return false;
        if (nConsumed != static_cast<int>(s.size()))
            return false;

        std::tm tmLocal{};
        tmLocal.tm_year  = nY - 1900;
        tmLocal.tm_mon   = nMo - 1;
        tmLocal.tm_mday  = nD;
        tmLocal.tm_hour  = nH;
        tmLocal.tm_min   = nMi;
        tmLocal.tm_isdst = -1;   // let the C library decide DST for this local date

        const std::time_t t = std::mktime(&tmLocal);
        if (t == static_cast<std::time_t>(-1))
            return false;

        outUS = static_cast<int64_t>(t) * 1000000;
        outTm = tmLocal;
        return true;
    }

    // "2026-09-08T15:00:00-07:00" -> the local wall-clock fields the service meant. Only the
    // leading date and time are read: the offset is the service's, already applied, and comparing
    // wall-clock to wall-clock is exactly the check we want.
    bool ParseIsoLocalFields(const std::string& s, int& outY, int& outMo, int& outD,
                             int& outH, int& outMi)
    {
        return std::sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d",
                           &outY, &outMo, &outD, &outH, &outMi) == 5;
    }

    // Reads a nullable string field. nlohmann throws on a null-to-string conversion, and every
    // interesting field in this contract is documented as nullable.
    std::string StrOr(const json& j, const char* pKey)
    {
        const auto it = j.find(pKey);
        if (it == j.end() || !it->is_string())
            return std::string();
        return it->get<std::string>();
    }
}

bool TriageClient::Available() const
{
    if (!Configured())
        return false;
    if (mnUsedThisPoll >= mConfig.nMaxPerPoll)
        return false;
    return !IsBreakerOpen();
}

bool TriageClient::IsBreakerOpen() const
{
    if (mnConsecutiveFailures < mConfig.nFailsToOpen)
        return false;
    return (NowUS() - mnBreakerOpenedUS) < static_cast<int64_t>(mConfig.nFailBackoffSec) * 1000000;
}

void TriageClient::VerifyDue(TriageResult& result, const std::string& sDueLocal, int64_t nRefUS)
{
    if (!result.bHaveDue)
        return;

    if (result.sDueTag.empty())
    {
        result.bHaveDue = false;
        return;
    }

    int64_t nDueUS = 0;
    std::tm tmDue{};
    if (!ParseDueTag(result.sDueTag, nDueUS, tmDue))
    {
        result.bHaveDue = false;
        result.sReason  = "service returned an unparseable due value: " + result.sDueTag;
        return;
    }

    // Normalize to the canonical tag text, so "DUE:2026-09-08T15:00" or a missing prefix still
    // lands as the one form the rest of Loom reads.
    result.sDueTag = "due:" + FormatDueLocal(nDueUS);

    // THE TWO MACHINE FIELDS MUST AGREE. due_local and due_tag are two renderings of one instant;
    // if they disagree, something between the service's date arithmetic and its tag formatting is
    // wrong - a timezone applied twice, a normalized impossible date. This is the closest we can
    // independently get to catching the class of bug that already bit once (a Thursday's date
    // returned for "Friday") without parsing the prose the contract tells us not to parse.
    int nY = 0, nMo = 0, nD = 0, nH = 0, nMi = 0;
    if (!sDueLocal.empty() && ParseIsoLocalFields(sDueLocal, nY, nMo, nD, nH, nMi))
    {
        if (nY != tmDue.tm_year + 1900 || nMo != tmDue.tm_mon + 1 || nD != tmDue.tm_mday ||
            nH != tmDue.tm_hour        || nMi != tmDue.tm_min)
        {
            result.bHaveDue = false;
            result.sReason  = "service's due_tag (" + result.sDueTag + ") disagrees with its own "
                              "due_local (" + sDueLocal + ")";
            return;
        }
    }

    // Sanity bound. Not a correctness check - a legitimately distant reminder exists - but a date
    // in 2126, or well before the jot was written, means a slot was misread, and accepting it
    // silently buries a card at an end of the board nobody scrolls to.
    const int64_t kYearUS = 365LL * 86400 * 1000000;
    if (nDueUS < nRefUS - kYearUS || nDueUS > nRefUS + 5 * kYearUS)
    {
        result.bHaveDue = false;
        result.sReason  = "service returned " + result.sDueTag + ", implausibly far from the jot's "
                          "own timestamp";
    }
}

bool TriageClient::Triage(int64_t nJotID,
                          const std::string& sName,
                          const std::string& sSummary,
                          const std::string& sText,
                          const std::vector<std::string>& vTags,
                          const TriageWants& wants,
                          const std::vector<std::string>& vVocabulary,
                          const std::vector<TriageCandidate>& vCandidates,
                          TriageResult& out)
{
    out = TriageResult{};

    if (!Available())
        return false;

    // `want` carries only what Loom could not settle itself, and the service is required not to
    // run a prompt for anything absent from it. This is the cost discipline of the whole design:
    // a jot needing one answer buys one prompt, not six.
    json want = json::array();
    if (wants.bSummary)  want.push_back("summary");
    if (wants.bClassify) want.push_back("classify");
    if (wants.bTopics)   want.push_back("tags");
    if (wants.bPriority) want.push_back("priority");
    if (wants.bDue)      want.push_back("due");
    if (wants.bDedupe)   want.push_back("dedupe");
    if (want.empty())
        return false;   // nothing to ask - do not spend a round trip saying so

    json body;
    body["service_version"] = 2;
    body["jot"] = { { "id",      nJotID },
                    { "name",    sName },
                    { "summary", sSummary },
                    { "text",    sText },
                    { "tags",    vTags } };
    body["want"] = std::move(want);
    body["vocabulary"] = { { "topical", vVocabulary } };
    // The jot's own id IS its ingestion timestamp, so this is when the jot was WRITTEN - not when
    // zserver happens to answer. See the header.
    body["now_local"] = FormatIsoLocal(nJotID);

    if (!vCandidates.empty())
    {
        json arr = json::array();
        for (const TriageCandidate& c : vCandidates)
            arr.push_back({ { "id",      c.nID },
                            { "name",    c.sName },
                            { "summary", c.sSummary },
                            { "tags",    c.vTags } });
        body["duplicate_candidates"] = std::move(arr);
    }

    ++mnUsedThisPoll;
    ++mnCalls;

    std::string sResponse;
    if (!PostTriage(body.dump(), sResponse))
    {
        // Transport/auth failure - NOT the service declining. Count it toward the breaker so a
        // sleeping zserver costs one connect timeout per backoff window rather than one per jot
        // per poll.
        if (++mnConsecutiveFailures >= mConfig.nFailsToOpen)
            mnBreakerOpenedUS = NowUS();
        return false;
    }

    json parsed;
    try
    {
        parsed = json::parse(sResponse);
    }
    catch (const json::exception&)
    {
        if (++mnConsecutiveFailures >= mConfig.nFailsToOpen)
            mnBreakerOpenedUS = NowUS();
        return false;
    }

    // A well-formed answer, whatever it says, means the service is up - so a run of refusals must
    // not creep the breaker open. Only transport and parse failures count against it.
    mnConsecutiveFailures = 0;

    out.sStatus = StrOr(parsed, "status");
    out.sReason = StrOr(parsed, "reason");
    if (out.sStatus == "deferred")
        out.nRetryAfterSec = parsed.value("retry_after_sec", 0);

    const auto itProps = parsed.find("proposals");
    if (itProps == parsed.end() || !itProps->is_object())
        return true;   // answered, proposed nothing - a valid outcome, see the header
    const json& props = *itProps;

    // Every sub-proposal is read defensively and independently: a service that implements five of
    // six capabilities must not cost us the five.
    const auto Sub = [&props](const char* pKey) -> const json*
    {
        const auto it = props.find(pKey);
        if (it == props.end() || !it->is_object())
            return nullptr;
        return &(*it);
    };

    if (const json* p = Sub("summary"))
    {
        out.sSummary     = StrOr(*p, "value");
        out.bHaveSummary = !out.sSummary.empty();
    }
    if (const json* p = Sub("kind"))
    {
        out.sKind     = StrOr(*p, "value");
        out.bHaveKind = !out.sKind.empty();
    }
    if (const json* p = Sub("topics"))
    {
        const auto itV = p->find("value");
        if (itV != p->end() && itV->is_array())
            for (const json& t : *itV)
                if (t.is_string())
                    out.vTopics.push_back(t.get<std::string>());

        const auto itN = p->find("new");
        if (itN != p->end() && itN->is_array())
            for (const json& t : *itN)
                if (t.is_string())
                    out.vNewTopics.push_back(t.get<std::string>());

        out.bHaveTopics = !out.vTopics.empty();
    }
    if (const json* p = Sub("priority"))
    {
        out.sPriority     = StrOr(*p, "value");
        out.bHavePriority = !out.sPriority.empty();
    }
    if (const json* p = Sub("due"))
    {
        out.sDueTag     = StrOr(*p, "value");
        out.bRecurring  = p->value("recurring", false);
        out.bHaveDue    = !out.sDueTag.empty();
        VerifyDue(out, StrOr(*p, "due_local"), nJotID);
    }
    if (const json* p = Sub("duplicate"))
    {
        out.sRelation = StrOr(*p, "relation");
        const auto itV = p->find("value");
        if (itV != p->end() && itV->is_number_integer())
            out.nDuplicateOf = itV->get<int64_t>();
        out.bHaveDuplicate = out.nDuplicateOf != 0 &&
                             (out.sRelation == "duplicate" || out.sRelation == "extends");
    }

    const auto itNeeds = parsed.find("needs_input");
    if (itNeeds != parsed.end() && itNeeds->is_object())
    {
        out.sNeedsSummary  = StrOr(*itNeeds, "summary");
        out.sNeedsQuestion = StrOr(*itNeeds, "question");
        out.sNeedsPrompt   = StrOr(*itNeeds, "resolution_prompt");
        out.bNeedsInput    = !out.sNeedsSummary.empty() || !out.sNeedsQuestion.empty();
    }

    if (out.bHaveSummary || out.bHaveKind || out.bHaveTopics || out.bHavePriority ||
        out.bHaveDue || out.bHaveDuplicate || out.bNeedsInput)
        ++mnApplied;

    return true;
}

#ifndef _WIN32

bool TriageClient::PostTriage(const std::string& sBody, std::string& outBody) const
{
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* pResult = nullptr;
    const std::string sPort = std::to_string(mConfig.nPort);
    if (::getaddrinfo(mConfig.sHost.c_str(), sPort.c_str(), &hints, &pResult) != 0 || !pResult)
        return false;

    int fd = -1;
    for (const struct addrinfo* p = pResult; p && fd < 0; p = p->ai_next)
    {
        const int nTry = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (nTry < 0)
            continue;

        const int nFlags = ::fcntl(nTry, F_GETFL, 0);
        if (nFlags >= 0)
            ::fcntl(nTry, F_SETFL, nFlags | O_NONBLOCK);

        if (::connect(nTry, p->ai_addr, p->ai_addrlen) == 0)
        {
            fd = nTry;
            break;
        }
        if (errno == EINPROGRESS)
        {
            struct pollfd pfd{};
            pfd.fd     = nTry;
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, mConfig.nTimeoutMS) > 0 && (pfd.revents & POLLOUT))
            {
                int       nErr    = 0;
                socklen_t nErrLen = sizeof(nErr);
                if (::getsockopt(nTry, SOL_SOCKET, SO_ERROR, &nErr, &nErrLen) == 0 && nErr == 0)
                {
                    fd = nTry;
                    break;
                }
            }
        }
        ::close(nTry);
    }
    ::freeaddrinfo(pResult);

    if (fd < 0)
        return false;

    // HTTP/1.0 with an explicit Connection: close, so "the server closed the socket" is an
    // unambiguous end-of-body and this needs no chunked-transfer decoder. The service is a Python
    // http.server (it announces itself as LoomResolver/1 Python/3.12.10) and answers 1.0 anyway.
    const std::string sReq =
        "POST /triage HTTP/1.0\r\n"
        "Host: " + mConfig.sHost + ":" + sPort + "\r\n"
        "Authorization: Bearer " + mConfig.sToken + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + std::to_string(sBody.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + sBody;

    const int64_t nDeadlineUS = NowUS() + static_cast<int64_t>(mConfig.nTimeoutMS) * 1000;
    bool bOk = true;

    size_t nSent = 0;
    while (nSent < sReq.size() && bOk)
    {
        const int nLeftMS = static_cast<int>((nDeadlineUS - NowUS()) / 1000);
        if (nLeftMS <= 0) { bOk = false; break; }

        struct pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLOUT;
        if (::poll(&pfd, 1, nLeftMS) <= 0 || !(pfd.revents & POLLOUT)) { bOk = false; break; }

        const ssize_t n = ::send(fd, sReq.data() + nSent, sReq.size() - nSent, MSG_NOSIGNAL);
        if (n > 0)                                            nSent += static_cast<size_t>(n);
        else if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        else                                                  bOk = false;
    }

    std::string sRaw;
    while (bOk)
    {
        const int nLeftMS = static_cast<int>((nDeadlineUS - NowUS()) / 1000);
        if (nLeftMS <= 0) { bOk = false; break; }

        struct pollfd pfd{};
        pfd.fd     = fd;
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, nLeftMS) <= 0 || !(pfd.revents & POLLIN)) { bOk = false; break; }

        char buf[4096];
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0)                                 break;   // clean close = end of body
        if (n > 0)                                  sRaw.append(buf, static_cast<size_t>(n));
        else if (errno == EAGAIN || errno == EINTR) continue;
        else                                        bOk = false;

        if (sRaw.size() > (1u << 20)) { bOk = false; break; }   // runaway response = broken service
    }
    ::close(fd);

    if (!bOk)
        return false;

    // Status line first: a 401 carries a perfectly well-formed JSON body ({"error":"unauthorized"})
    // and must not be mistaken for an answer.
    const size_t nSp = sRaw.find(' ');
    if (nSp == std::string::npos)
        return false;
    const int nStatus = std::atoi(sRaw.c_str() + nSp + 1);

    const size_t nSplit = sRaw.find("\r\n\r\n");
    if (nSplit == std::string::npos)
        return false;
    outBody = sRaw.substr(nSplit + 4);

    return nStatus == 200;
}

#else

bool TriageClient::PostTriage(const std::string&, std::string&) const
{
    return false;
}

#endif
