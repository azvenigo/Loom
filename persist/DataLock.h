#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdio>
#include <string>

#ifdef _WIN32
  #include <io.h>
  #include <windows.h>
#else
  #include <fcntl.h>
  #include <unistd.h>
  #ifndef F_OFD_SETLK
    #define F_OFD_SETLK 37
  #endif
#endif

//////////////////////////////////////////////////////////////////////////////////////////////////
// DataLock - one loom per data directory, enforced by the OS.
//
// Nothing else in Loom stops two processes from opening the same --data dir. Journal's durability
// comes from an in-process mutex, so a second instance would interleave writes into loom.wal with
// no error and no warning. This is the guard.
//
// HELD BY THE KERNEL, NOT BY US. An exclusive byte-range lock on loom.lock is released when the
// process dies, however it dies - clean exit, SIGKILL, panic, power loss. That is the whole reason
// to prefer it over a pid file, which survives a crash and then has to be reaped by something that
// can tell a stale pid from a live one, and cannot, because pids are reused.
//
// fcntl, NOT flock. On Linux those are independent lock namespaces that do not see each other, and
// Samba maps the byte-range locks an SMB client takes onto POSIX fcntl locks. The data dir here is
// deliberately shared - /mnt/fast/Alex/dev/loom_repo locally, the same bytes over the [fast] share
// from Windows - so flock would be correct between two local processes and silently no protection
// at all across the pair of machines that motivated this. F_OFD_SETLK rather than plain F_SETLK
// because classic POSIX record locks are dropped when the process closes ANY descriptor to the
// file; an open-file-description lock is tied to this handle and behaves.
//
// The locked byte is at 1<<62, far past EOF. Locking beyond end of file is legal on both platforms
// and a range nothing will ever read keeps this from interacting with real I/O.
//////////////////////////////////////////////////////////////////////////////////////////////////

class DataLock
{
public:
    DataLock() = default;
    ~DataLock() { Release(); }

    DataLock(const DataLock&)            = delete;
    DataLock& operator=(const DataLock&) = delete;

    // True if this process now holds the directory. False means somebody else does - the caller
    // should refuse to start rather than proceed and corrupt the log.
    bool Acquire(const std::string& sPath)
    {
        Release();

        // "ab" so the file is created if absent and never truncated if present. Nothing is ever
        // written to it; only the lock on it matters.
        mpFile = std::fopen(sPath.c_str(), "ab");
        if (!mpFile)
            return false;

        if (!TakeLock())
        {
            std::fclose(mpFile);
            mpFile = nullptr;
            return false;
        }
        return true;
    }

    void Release()
    {
        if (!mpFile)
            return;

        // No explicit unlock. Closing the handle drops the lock on both platforms, and on the way
        // out of a crash there is nobody to run this anyway - which is the point.
        std::fclose(mpFile);
        mpFile = nullptr;
    }

private:
    static constexpr long long kLockByte = 1LL << 62;

    bool TakeLock()
    {
#ifdef _WIN32
        const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(mpFile)));
        if (h == INVALID_HANDLE_VALUE)
            return false;

        OVERLAPPED ov = {};
        ov.Offset     = static_cast<DWORD>(kLockByte & 0xFFFFFFFF);
        ov.OffsetHigh = static_cast<DWORD>(kLockByte >> 32);
        return LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0;
#else
        struct flock fl = {};
        fl.l_type   = F_WRLCK;
        fl.l_whence = SEEK_SET;
        fl.l_start  = static_cast<off_t>(kLockByte);
        fl.l_len    = 1;
        return fcntl(fileno(mpFile), F_OFD_SETLK, &fl) != -1;
#endif
    }

    std::FILE* mpFile = nullptr;
};
