#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <atomic>
#include <shared_mutex>
#include <thread>

//////////////////////////////////////////////////////////////////////////////////////////////////
// FairSharedMutex - a std::shared_mutex that a stream of readers cannot starve a writer out of.
//
// std::shared_mutex says nothing about which waiter wins, and glibc's implementation is
// reader-preferring: a new reader may take the lock while a writer is already blocked on it. With
// enough readers arriving back-to-back the shared count never reaches zero and the writer waits
// forever. That is not a hypothetical - four threads querying in a tight loop deadlocked both
// writers in coretest's [concurrent readers and writers] section, indefinitely, at 400% CPU.
//
// The fix is one atomic: a writer announces itself before blocking, and readers park rather than
// join a queue somebody is already waiting to drain. Existing readers finish, the writer gets in.
//
// The counter is a scheduling hint and nothing more - mutual exclusion and every happens-before
// edge still come from mLock alone, so relaxed ordering is enough. A reader that reads a stale
// zero and slips past the gate is harmless: it is one reader, not an unbounded stream, and the
// writer acquires as soon as it drains.
//
// NOT recursive, in the one way that matters here: taking the shared lock while already holding it
// on the same thread can now deadlock outright, because the inner acquire waits on a writer that
// waits on the outer one. Plain std::shared_mutex makes that undefined too, but reader-preference
// hides it in practice - here it will hang. JotStore is safe by construction (every public method
// takes the lock exactly once and all internals are Locked_-prefixed); keep it that way.
//////////////////////////////////////////////////////////////////////////////////////////////////

class FairSharedMutex
{
public:
    void lock()
    {
        mnWaitingWriters.fetch_add(1, std::memory_order_relaxed);
        mLock.lock();
        // Cleared on acquisition, not on release. While this writer actually holds the lock there
        // is nothing to protect it from, and leaving the gate shut would spin every reader through
        // yield() for the whole write instead of letting them block on the mutex like normal.
        mnWaitingWriters.fetch_sub(1, std::memory_order_relaxed);
    }

    void unlock() { mLock.unlock(); }

    bool try_lock() { return mLock.try_lock(); }

    void lock_shared()
    {
        while (mnWaitingWriters.load(std::memory_order_relaxed) != 0)
            std::this_thread::yield();

        mLock.lock_shared();
    }

    void unlock_shared() { mLock.unlock_shared(); }

    bool try_lock_shared()
    {
        if (mnWaitingWriters.load(std::memory_order_relaxed) != 0)
            return false;

        return mLock.try_lock_shared();
    }

private:
    std::shared_mutex     mLock;
    std::atomic<uint32_t> mnWaitingWriters{ 0 };
};
