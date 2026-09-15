#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/LoomTime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// ActivityMeter - what is actually talking to this process, right now.
//
// Loom's other counters answer "what is in the store" (StoreStats) and "is durability keeping up"
// (PersistStats). Neither answers the question this exists for: HOW IS THE SERVICE BEING USED, and
// by whom - a store that gained four jots today looks identical whether one agent wrote them in a
// burst or four agents have been polling it every second since Tuesday.
//
// TWO NUMBERS OF DIFFERENT KINDS, and keeping them apart is the whole design:
//
//   TOTALS are since process start. Not lifetime, not persisted, and deliberately so - this is a
//   picture of the running process, and a restart is exactly the event that should reset it. The
//   same rule /stats already applies to jots_added and the resolver counters.
//
//   THE WINDOW is a ring of one-second buckets, kWindowSeconds of them (30 minutes), for the walking graph. A
//   ring rather than a growing list because the memory has to be bounded by TIME and not by
//   traffic: a busy hour must not cost more than a quiet one.
//
// GAPS READ AS ZERO, NOT AS STALE. Each slot carries the epoch second it stands for, and a reader
// asking for a second the slot no longer holds gets an empty bucket. Without that, five idle
// minutes would replay the last busy minute's traffic forever, which is the specific way a naive
// ring lies - and it lies most convincingly exactly when the service is idle.
//
// A MUTEX, not atomics. One uncontended lock at the end of a request is nothing beside the JSON
// parse and the store lock that same request already paid for, and the alternative - a bucket of
// six independent atomics - cannot be read as ONE consistent sample, which is the only thing a
// graph wants from it.
//
// WHAT "BYTES" MEANS HERE: request and response BODY bytes. Not headers, not TLS, not TCP framing.
// It is the payload the two sides actually exchanged, which is the figure that scales with how the
// service is used; counting headers would mostly measure crow.
//////////////////////////////////////////////////////////////////////////////////////////////////

// Which front door a request came through: "mcp" is agents, "rest" is anything scripted, and
// "dashboard" is a human with the page open.
//
// THE DASHBOARD IS CLASSIFIED BUT NOT COUNTED. The meter exists to show how much traffic USING Loom
// generates, and the operator watching the meter is not use: the page polls /stats every fifteen
// seconds, the Activity view polls /activity as often as every second, and a single page load is a
// quarter of a megabyte of HTML. Counted, that drowned the thing being measured - a quiet store
// with the tab open read as steady traffic, and every reload was the tallest spike on the bytes-out
// plot. So kDashboard sits deliberately OUTSIDE the counted range: the meter's arrays are
// kCountedSurfaces wide, and Record() drops anything beyond them.
enum class eSurface : size_t
{
    kMcp       = 0,
    kRest      = 1,
    kDashboard = 2   // must stay the first value past kCountedSurfaces - see above
};

inline constexpr size_t kCountedSurfaces = 2;

inline const char* SurfaceName(eSurface surface)
{
    switch (surface)
    {
    case eSurface::kMcp:       return "mcp";
    case eSurface::kRest:      return "rest";
    case eSurface::kDashboard: return "dashboard";
    }
    return "rest";
}

// Which surface a request came through. Lives here rather than in the route table so the rule is
// one rule in one place, testable without a socket, and applied identically to requests that never
// reached a route at all (an ACL refusal, a 404).
//
// TWO INPUTS, because the path alone cannot tell the page apart from an agent. The dashboard is a
// browser talking to the very same REST routes a script would call - /stats, /jots, /tags - so
// classifying by path would file the page's own polling under "rest" and leave the agent share,
// the number this whole feature exists to show, sitting next to a category that is mostly the
// operator looking at the graph. So the page SAYS who it is, in X-Loom-Client.
//
// THAT HEADER IS NOT A CREDENTIAL and nothing here treats it as one: anything can claim to be the
// dashboard, exactly as anything can put "claude" in a jot's editor field. The history log has
// always shown the editor name beside the address crow actually saw, for that reason.
//
// NOTE WHAT THE LABEL NOW DOES: since dashboard traffic is not counted, a REST caller that sends
// the header is left off the graph. That is acceptable for a graph and would be unacceptable for
// any control - which is why nothing but this meter reads it, and why /mcp cannot opt out below.
//
// Takes the raw URL, so a query string has to be tolerated: "/jots?tag=todo" is REST.
inline eSurface SurfaceFor(const std::string& sUrl, const std::string& sClient = {})
{
    const size_t nEnd  = sUrl.find_first_of("?#");
    const std::string sPath = (nEnd == std::string::npos) ? sUrl : sUrl.substr(0, nEnd);

    // MCP wins over the header: a request to /mcp is an agent call whatever it calls itself, and
    // that is the one classification worth making unspoofable-by-accident.
    if (sPath == "/mcp" || sPath.rfind("/mcp/", 0) == 0)
        return eSurface::kMcp;

    if (sClient == "dashboard")
        return eSurface::kDashboard;

    // The page itself and the art it loads, for the first request of a session - before any script
    // is running to send the header.
    if (sPath == "/" || sPath == "/icon.png" || sPath == "/icon-full.png")
        return eSurface::kDashboard;

    return eSurface::kRest;
}

//------------------------------------------------------------------------------------------------
// One second of traffic, counted PER SURFACE rather than as a total plus an "of which agents"
// subset. The three-way split is the answer to the question the meter exists for, and a graph that
// stacks agents / dashboard / other needs all three bands - a total minus the agent share would
// have to paint the operator's own polling the same colour as a script's, which is exactly the
// distinction worth drawing.
//
// mnSecond == 0 means "this slot has never been written", which is distinct from a second that
// really did see zero requests - the reader turns both into the same zero row, but only after
// checking the stamp matches the second it asked for.
//------------------------------------------------------------------------------------------------
struct ActivitySample
{
    int64_t                             mnSecond = 0;   // epoch seconds
    std::array<uint32_t, kCountedSurfaces> mnRequests{};   // indexed by eSurface
    uint32_t                            mnErrors   = 0; // of which answered >= 400
    uint64_t                            mnBytesIn  = 0;
    uint64_t                            mnBytesOut = 0;

    uint32_t Requests() const
    {
        uint32_t nTotal = 0;
        for (uint32_t n : mnRequests)
            nTotal += n;
        return nTotal;
    }
};

struct ActivityTotals
{
    int64_t  mnStartedUS     = 0;
    int64_t  mnLastRequestUS = 0;   // 0 == nothing has called yet
    uint64_t mnRequests      = 0;
    uint64_t mnErrors        = 0;
    uint64_t mnBytesIn       = 0;
    uint64_t mnBytesOut      = 0;
    uint32_t mnPeakPerSecond = 0;

    std::array<uint64_t, kCountedSurfaces> mnRequestsBy{};
    std::array<uint64_t, kCountedSurfaces> mnBytesInBy{};
    std::array<uint64_t, kCountedSurfaces> mnBytesOutBy{};
};

class ActivityMeter
{
public:
    // Thirty minutes of one-second buckets - the widest span the dashboard offers. About 70 KB for
    // the whole ring, fixed at construction. Wider views do not get coarser STORAGE; they get
    // coarser READS (see Window's nStep), so the 2-minute view keeps its per-second detail even
    // though the same ring also serves half an hour.
    static constexpr size_t kWindowSeconds = 1800;
    static constexpr size_t kMaxStepSeconds = 60;

    explicit ActivityMeter(int64_t nStartedUS = LOOMTIME::NowMicros())
        : mnStartedUS(nStartedUS)
    {
    }

    // Called once per request, after the response body is final. nStatus is the HTTP code.
    // Dashboard traffic is dropped here, before the lock - see eSurface. Dropped in the meter rather
    // than by the caller so the arrays cannot be indexed past their end by anyone who forgets.
    void Record(eSurface surface, size_t nBytesIn, size_t nBytesOut, int nStatus,
                int64_t nNowUS = LOOMTIME::NowMicros())
    {
        const int64_t nSecond = nNowUS / LOOMTIME::kMicrosPerSecond;
        const size_t  nSlot   = SlotFor(nSecond);
        const size_t  nSurf   = static_cast<size_t>(surface);
        if (nSurf >= kCountedSurfaces)
            return;

        std::lock_guard<std::mutex> lock(mMutex);

        ActivitySample& bucket = mRing[nSlot];
        if (bucket.mnSecond != nSecond)
        {
            bucket = ActivitySample{};
            bucket.mnSecond = nSecond;
        }

        ++bucket.mnRequests[nSurf];
        if (nStatus >= 400) ++bucket.mnErrors;
        bucket.mnBytesIn  += nBytesIn;
        bucket.mnBytesOut += nBytesOut;

        ++mTotals.mnRequests;
        if (nStatus >= 400) ++mTotals.mnErrors;
        mTotals.mnBytesIn  += nBytesIn;
        mTotals.mnBytesOut += nBytesOut;
        ++mTotals.mnRequestsBy[nSurf];
        mTotals.mnBytesInBy[nSurf]  += nBytesIn;
        mTotals.mnBytesOutBy[nSurf] += nBytesOut;
        mTotals.mnLastRequestUS = nNowUS;

        // Exact rather than sampled: the peak is the busiest bucket the ring ever held, and it is
        // known the moment that bucket is incremented past the previous best. Computed here
        // because the ring itself forgets it half an hour later.
        const uint32_t nThisSecond = bucket.Requests();
        if (nThisSecond > mTotals.mnPeakPerSecond)
            mTotals.mnPeakPerSecond = nThisSecond;
    }

    // The last nSeconds of traffic, OLDEST FIRST, summed into buckets of nStep seconds each. The
    // newest bucket contains the second nNowUS falls in and is still accruing, so the caller must
    // expect it to be partial. Quiet buckets come back as explicit zero rows rather than being
    // skipped: a graph needs one sample per tick, and "no row" and "a row of zeroes" are the same
    // fact, only one of which the drawing code can use.
    //
    // BUCKETS ARE ALIGNED TO THE EPOCH, not to the moment of the call. A 10-second bucket always
    // covers :00-:09, :10-:19 and so on, whenever it is asked for. Anchored to "now" instead, every
    // poll would regroup the same seconds into different buckets, and a graph refreshed every ten
    // seconds would reshape its whole history each time rather than only moving left.
    //
    // Each row's mnSecond is the FIRST second of its bucket. nSeconds is clamped to what the ring
    // holds, rounded to whole buckets, and never reaches back past the oldest second still in the
    // ring - a bucket straddling that edge would under-report, and look like a quiet spell.
    void Window(size_t nSeconds, std::vector<ActivitySample>& vOut,
                int64_t nNowUS = LOOMTIME::NowMicros(), size_t nStep = 1) const
    {
        if (nStep == 0)                nStep = 1;
        if (nStep > kMaxStepSeconds)   nStep = kMaxStepSeconds;
        if (nSeconds == 0)             nSeconds = kWindowSeconds;
        if (nSeconds > kWindowSeconds) nSeconds = kWindowSeconds;

        const int64_t nStepS   = static_cast<int64_t>(nStep);
        const int64_t nNow     = nNowUS / LOOMTIME::kMicrosPerSecond;
        const int64_t nLast    = nNow - (((nNow % nStepS) + nStepS) % nStepS);   // start of the live bucket
        const int64_t nOldest  = nNow - static_cast<int64_t>(kWindowSeconds) + 1;  // oldest second the ring holds

        int64_t nBuckets = (static_cast<int64_t>(nSeconds) + nStepS - 1) / nStepS;
        const int64_t nFits = (nLast - nOldest) / nStepS + 1;
        if (nBuckets > nFits)
            nBuckets = nFits;

        vOut.clear();
        vOut.reserve(static_cast<size_t>(nBuckets));

        std::lock_guard<std::mutex> lock(mMutex);
        for (int64_t nB = nBuckets - 1; nB >= 0; --nB)
        {
            ActivitySample sum;
            sum.mnSecond = nLast - nB * nStepS;

            for (int64_t nSecond = sum.mnSecond; nSecond < sum.mnSecond + nStepS; ++nSecond)
            {
                const ActivitySample& bucket = mRing[SlotFor(nSecond)];
                if (bucket.mnSecond != nSecond)
                    continue;
                for (size_t n = 0; n < kCountedSurfaces; ++n)
                    sum.mnRequests[n] += bucket.mnRequests[n];
                sum.mnErrors   += bucket.mnErrors;
                sum.mnBytesIn  += bucket.mnBytesIn;
                sum.mnBytesOut += bucket.mnBytesOut;
            }
            vOut.push_back(sum);
        }
    }

    ActivityTotals Totals() const
    {
        std::lock_guard<std::mutex> lock(mMutex);
        ActivityTotals out = mTotals;
        out.mnStartedUS    = mnStartedUS;
        return out;
    }

private:
    // Seconds before the epoch never reach here in practice, but a negative modulo would index
    // outside the ring, and "in practice" is not what a bounds check is for.
    static size_t SlotFor(int64_t nSecond)
    {
        const int64_t nCap = static_cast<int64_t>(kWindowSeconds);
        return static_cast<size_t>(((nSecond % nCap) + nCap) % nCap);
    }

    mutable std::mutex                              mMutex;
    std::array<ActivitySample, kWindowSeconds>      mRing{};
    ActivityTotals                                  mTotals;
    int64_t                                         mnStartedUS = 0;
};
