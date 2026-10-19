#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "persist/Importer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Places - names for the spots a phone jot was sent from.
//
// THE LIST IS A JOT, `known-places`, one place per line:
//
//     Home: 47.61012,-122.34211 150 m
//
// Name, latitude,longitude, and an optional radius (150 m if left out). Lines starting with # and
// anything else that is not in that form are ignored, so the jot can carry its own notes. It lives
// in the store rather than in a config file so it can be edited from the dashboard like anything
// else, and never in the repo, because it says where somebody lives.
//
// A jot whose location line (Importer.h) falls inside a place is tagged place:<name> - the nearest
// place when circles overlap. Watcher tags a jot at triage, and when the list changes it walks every
// jot with a location once and brings its place tag into line (Watcher::SyncPlaceTags), so a place
// added later still finds the jots sent from there before.
//
// THE PHONE NAMES A SPOT by sending a jot that is only "[set-place:Work]", with its location. Triage
// writes that into the list and keeps the jot as the record of it (Watcher::DeterministicTriage).
//////////////////////////////////////////////////////////////////////////////////////////////////

namespace PLACES
{
    inline constexpr const char* ksJotName        = "known-places";
    inline constexpr const char* ksTagPrefix      = "place:";
    inline constexpr double      kdDefaultRadiusM = 150.0;
    inline constexpr double      kdMaxRadiusM     = 50000.0;

    struct Place
    {
        std::string msName;
        double      mdLat     = 0.0;
        double      mdLon     = 0.0;
        double      mdRadiusM = kdDefaultRadiusM;
    };

    // What the tag carries: "Home" -> "home", "Mom's place" -> "mom-s-place". Letters and digits,
    // runs of anything else become one dash. Empty when the name has nothing usable.
    inline std::string Slug(const std::string& sName)
    {
        std::string s;
        bool bDash = false;
        for (const char c : sName)
        {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u < 128 && std::isalnum(u))
            {
                if (bDash && !s.empty())
                    s.push_back('-');
                bDash = false;
                s.push_back(static_cast<char>(std::tolower(u)));
            }
            else
                bDash = true;
        }
        if (s.size() > 32)
            s.resize(32);
        while (!s.empty() && s.back() == '-')
            s.pop_back();
        return s;
    }

    inline bool InRange(double dLat, double dLon)
    {
        return std::isfinite(dLat) && std::isfinite(dLon) && dLat >= -90.0 && dLat <= 90.0 &&
               dLon >= -180.0 && dLon <= 180.0;
    }

    inline bool ParseLine(const std::string& sLine, Place& outPlace)
    {
        if (sLine.empty() || sLine[0] == '#')
            return false;
        const size_t nColon = sLine.find(':');
        if (nColon == std::string::npos || nColon == 0 || nColon > 40)
            return false;

        Place p;
        size_t a = 0, b = nColon;
        while (a < b && std::isspace(static_cast<unsigned char>(sLine[a]))) ++a;
        while (b > a && std::isspace(static_cast<unsigned char>(sLine[b - 1]))) --b;
        p.msName = sLine.substr(a, b - a);
        if (Slug(p.msName).empty())
            return false;

        double dRadius = 0.0;
        const int nFields = std::sscanf(sLine.c_str() + nColon + 1, " %lf , %lf %lf",
                                        &p.mdLat, &p.mdLon, &dRadius);
        if (nFields < 2 || !InRange(p.mdLat, p.mdLon))
            return false;
        if (nFields >= 3)
        {
            if (!std::isfinite(dRadius) || dRadius <= 0.0 || dRadius > kdMaxRadiusM)
                return false;
            p.mdRadiusM = dRadius;
        }
        outPlace = p;
        return true;
    }

    inline std::vector<Place> Parse(const std::string& sText)
    {
        std::vector<Place> v;
        std::istringstream iss(sText);
        std::string sLine;
        Place p;
        while (std::getline(iss, sLine))
            if (ParseLine(sLine, p))
                v.push_back(p);
        return v;
    }

    inline std::string Format(const Place& p)
    {
        char sz[160];
        std::snprintf(sz, sizeof(sz), "%s: %.5f,%.5f %.0f m", p.msName.c_str(), p.mdLat, p.mdLon, p.mdRadiusM);
        return sz;
    }

    // The coordinates on a jot's location line, if it has one in exactly the importer's form.
    inline bool LocationOf(const std::string& sText, double& outLat, double& outLon)
    {
        const std::string sLead = IMPORT::ksLocationLead;
        const size_t nAt = sText.rfind(sLead);
        if (nAt == std::string::npos || sText.find('\n', nAt + 2) != std::string::npos)
            return false;
        return std::sscanf(sText.c_str() + nAt + sLead.size(), "%lf,%lf", &outLat, &outLon) == 2 &&
               InRange(outLat, outLon);
    }

    inline double MetresBetween(double dLat1, double dLon1, double dLat2, double dLon2)
    {
        constexpr double kRad = 3.14159265358979323846 / 180.0;
        const double dPhi = (dLat2 - dLat1) * kRad, dLam = (dLon2 - dLon1) * kRad;
        const double h = std::sin(dPhi / 2) * std::sin(dPhi / 2) +
                         std::cos(dLat1 * kRad) * std::cos(dLat2 * kRad) * std::sin(dLam / 2) * std::sin(dLam / 2);
        return 2.0 * 6371000.0 * std::asin(std::sqrt(std::min(1.0, h)));
    }

    // The nearest place whose circle holds the point, or null.
    inline const Place* At(const std::vector<Place>& vPlaces, double dLat, double dLon)
    {
        const Place* pBest = nullptr;
        double dBest = 0.0;
        for (const Place& p : vPlaces)
        {
            const double d = MetresBetween(p.mdLat, p.mdLon, dLat, dLon);
            if (d <= p.mdRadiusM && (!pBest || d < dBest))
            {
                pBest = &p;
                dBest = d;
            }
        }
        return pBest;
    }

    // The list's text with this place in it: an existing line for the same name is replaced and
    // keeps its radius, otherwise the place is added at the end.
    inline std::string WithPlace(const std::string& sText, Place p)
    {
        static const char* kHeader =
            "# One place per line: name: latitude,longitude and an optional radius in metres (150 if left out).\n"
            "# A jot sent with a location inside a place is tagged place:<name>. Edit freely.";
        const std::string sSlug = Slug(p.msName);
        std::istringstream iss(sText.empty() ? std::string(kHeader) : sText);
        std::string sLine, sOut;
        bool bReplaced = false;
        while (std::getline(iss, sLine))
        {
            Place old;
            if (!bReplaced && ParseLine(sLine, old) && Slug(old.msName) == sSlug)
            {
                p.mdRadiusM = old.mdRadiusM;
                sLine       = Format(p);
                bReplaced   = true;
            }
            sOut += (sOut.empty() ? "" : "\n") + sLine;
        }
        if (!bReplaced)
            sOut += (sOut.empty() ? "" : "\n") + Format(p);
        return sOut;
    }
}
