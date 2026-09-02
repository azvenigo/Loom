#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/JotStore.h"

#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// SinkFanout - one IJournalSink that forwards to several.
//
// JotStore takes exactly one sink, and that is the right shape for it: the store's job is to hand
// each mutation to durability once, under the lock, in apply order. Once there are two consumers of
// that stream - the WAL and the history log - somebody has to fan it out, and it should not be the
// store, which would then be choosing a policy about how many logs exist.
//
// ORDER IS THE CONTRACT. Sinks are called in the order they were added, still under the store's
// write lock, so every sink sees the same sequence. The WAL goes first: it is the durability path,
// and if a later sink were ever to become slow or throw, the record it is protecting is already
// queued.
//
// Sinks must outlive this object, which is trivially true - both live in main's frame alongside it.
//////////////////////////////////////////////////////////////////////////////////////////////////

class SinkFanout : public IJournalSink
{
public:
    // Null is ignored, so a caller can pass an optional sink without branching.
    void Add(IJournalSink* pSink)
    {
        if (pSink)
            mSinks.push_back(pSink);
    }

    bool Empty() const { return mSinks.empty(); }

    void OnPut(const FlatJot& jot) override
    {
        for (IJournalSink* pSink : mSinks)
            pSink->OnPut(jot);
    }

    void OnDelete(tJotID id) override
    {
        for (IJournalSink* pSink : mSinks)
            pSink->OnDelete(id);
    }

private:
    std::vector<IJournalSink*> mSinks;
};
