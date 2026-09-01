#include "LoomTime.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace LOOMTIME
{
    namespace
    {
        // Portable UTC breakdown. gmtime_r on POSIX, gmtime_s on Windows - the plain gmtime is not
        // thread-safe and this runs from request threads.
        bool BreakDownUTC(int64_t nSeconds, std::tm& outTM)
        {
            const std::time_t t = static_cast<std::time_t>(nSeconds);
#ifdef _WIN32
            return gmtime_s(&outTM, &t) == 0;
#else
            return gmtime_r(&t, &outTM) != nullptr;
#endif
        }

        // Days from 1970-01-01 to the given civil date. Howard Hinnant's algorithm - correct for
        // the whole proleptic Gregorian range and, importantly, free of any timezone database.
        int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d)
        {
            y -= (m <= 2);
            const int64_t era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
            const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
            return era * 146097LL + static_cast<int64_t>(doe) - 719468LL;
        }
    }

    std::string FormatUS(int64_t nUS)
    {
        const int64_t nSeconds = nUS / kMicrosPerSecond;
        const int64_t nFrac    = nUS % kMicrosPerSecond;

        std::tm tmUTC{};
        if (!BreakDownUTC(nSeconds, tmUTC))
            return std::to_string(nUS);

        char szBuf[40];
        std::snprintf(szBuf, sizeof(szBuf), "%04d-%02d-%02d %02d:%02d:%02d.%06d",
            tmUTC.tm_year + 1900, tmUTC.tm_mon + 1, tmUTC.tm_mday,
            tmUTC.tm_hour, tmUTC.tm_min, tmUTC.tm_sec,
            static_cast<int>(nFrac));
        return std::string(szBuf);
    }

    bool ParseTimeSpec(const std::string& sSpec, int64_t nNowUS, int64_t& outUS)
    {
        if (sSpec.empty())
            return false;

        // --- raw microseconds ------------------------------------------------------------------
        // A bare run of digits long enough to be a real timestamp. Short digit runs fall through
        // to the relative form, so "30" is not silently read as 30 microseconds past the epoch.
        bool bAllDigits = true;
        for (char c : sSpec)
        {
            if (c < '0' || c > '9')
            {
                bAllDigits = false;
                break;
            }
        }
        if (bAllDigits && sSpec.size() >= 10)
        {
            outUS = std::strtoll(sSpec.c_str(), nullptr, 10);
            return true;
        }

        // --- relative age ----------------------------------------------------------------------
        {
            char* pEnd = nullptr;
            const long long nValue = std::strtoll(sSpec.c_str(), &pEnd, 10);
            if (pEnd != sSpec.c_str() && pEnd != nullptr && *pEnd != '\0' && nValue >= 0)
            {
                const std::string sUnit(pEnd);
                int64_t nScale = 0;
                if      (sUnit == "s")  nScale = kMicrosPerSecond;
                else if (sUnit == "m")  nScale = kMicrosPerMinute;
                else if (sUnit == "h")  nScale = kMicrosPerHour;
                else if (sUnit == "d")  nScale = kMicrosPerDay;
                else if (sUnit == "w")  nScale = 7LL * kMicrosPerDay;

                if (nScale != 0)
                {
                    outUS = nNowUS - nValue * nScale;
                    return true;
                }
            }
        }

        // --- ISO date / datetime, interpreted as UTC -------------------------------------------
        {
            int nYear = 0, nMonth = 0, nDay = 0, nHour = 0, nMin = 0, nSec = 0;
            const int nFields = std::sscanf(sSpec.c_str(), "%d-%d-%d%*[ T]%d:%d:%d",
                                            &nYear, &nMonth, &nDay, &nHour, &nMin, &nSec);
            if (nFields >= 3 && nMonth >= 1 && nMonth <= 12 && nDay >= 1 && nDay <= 31)
            {
                const int64_t nDays = DaysFromCivil(nYear, static_cast<unsigned>(nMonth),
                                                    static_cast<unsigned>(nDay));
                const int64_t nSeconds = nDays * 86400LL + nHour * 3600LL + nMin * 60LL + nSec;
                outUS = nSeconds * kMicrosPerSecond;
                return true;
            }
        }

        return false;
    }
}
