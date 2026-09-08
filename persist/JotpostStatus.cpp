// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "persist/JotpostStatus.h"

#include <chrono>

#ifndef _WIN32
    #include <cerrno>
    #include <fcntl.h>
    #include <netdb.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace
{
    int64_t NowUS()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
    }
}

bool JotpostStatus::Reachable()
{
    std::lock_guard<std::mutex> lock(mMutex);
    const int64_t nNowUS = NowUS();
    if (mnCheckedUS != 0 &&
        (nNowUS - mnCheckedUS) < static_cast<int64_t>(mConfig.nCacheSec) * 1000000)
        return mbReachable;

    mbReachable = Probe();
    mnCheckedUS = nNowUS;
    return mbReachable;
}

int64_t JotpostStatus::CheckedAtUS() const
{
    // Not locked - a display value that could be a few instructions stale is a fine tradeoff
    // against locking for every read, same call Watcher.h makes for its own atomics.
    return mnCheckedUS;
}

#ifndef _WIN32

bool JotpostStatus::Probe() const
{
    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* pResult = nullptr;
    const std::string sPort = std::to_string(mConfig.nPort);
    if (::getaddrinfo(mConfig.sHost.c_str(), sPort.c_str(), &hints, &pResult) != 0 || !pResult)
        return false;

    bool bOk = false;
    for (const struct addrinfo* p = pResult; p && !bOk; p = p->ai_next)
    {
        const int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;

        const int nFlags = ::fcntl(fd, F_GETFL, 0);
        if (nFlags >= 0)
            ::fcntl(fd, F_SETFL, nFlags | O_NONBLOCK);

        const int nConnect = ::connect(fd, p->ai_addr, p->ai_addrlen);
        if (nConnect == 0)
        {
            bOk = true;
        }
        else if (errno == EINPROGRESS)
        {
            struct pollfd pfd{};
            pfd.fd     = fd;
            pfd.events = POLLOUT;
            if (::poll(&pfd, 1, mConfig.nTimeoutMS) > 0 && (pfd.revents & POLLOUT))
            {
                int       nErr    = 0;
                socklen_t nErrLen = sizeof(nErr);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &nErr, &nErrLen) == 0 && nErr == 0)
                    bOk = true;
            }
        }
        ::close(fd);
    }

    ::freeaddrinfo(pResult);
    return bOk;
}

#else

bool JotpostStatus::Probe() const
{
    return false;
}

#endif
