#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <chrono>
#include <cstdint>
#include <string>

//////////////////////////////////////////////////////////////////////////////////////////////////
// LoomTime - microseconds since the Unix epoch, which is also the jot id space.
//
// system_clock rather than steady_clock, deliberately: an id has to mean a wall-clock instant to a
// human reading it, and has to survive a restart. The cost is that the clock can step backwards
// (NTP correction, a VM resuming), which is exactly what JotStore::NextID is written to absorb.
//////////////////////////////////////////////////////////////////////////////////////////////////

namespace LOOMTIME
{
    inline int64_t NowMicros()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    constexpr int64_t kMicrosPerSecond = 1000000LL;
    constexpr int64_t kMicrosPerMinute = 60LL * kMicrosPerSecond;
    constexpr int64_t kMicrosPerHour   = 60LL * kMicrosPerMinute;
    constexpr int64_t kMicrosPerDay    = 24LL * kMicrosPerHour;

    // "2026-08-31 13:54:02.123456" in UTC. Used for logs and for the verbose wire form; the id
    // itself is always the authoritative value.
    std::string FormatUS(int64_t nUS);

    // Accepts, in order of attempt:
    //   - a raw microsecond count            "1756661962123456"
    //   - a relative age                     "30d", "12h", "45m", "90s", "2w"
    //   - an ISO-ish date or datetime        "2026-08-01", "2026-08-01 13:54:02"
    // Relative forms resolve against nNowUS and mean "that long ago".
    // Returns false when nothing parses, leaving outUS untouched.
    bool ParseTimeSpec(const std::string& sSpec, int64_t nNowUS, int64_t& outUS);
}
