// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
#include "core/IpAcl.h"

#include "vendor/json.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>

using json = nlohmann::json;

namespace
{
    // The first 12 bytes of an IPv4-mapped IPv6 address. Every v4 rule and every v4 caller is
    // normalized through this prefix so the two families compare as one.
    const uint8_t kV4Prefix[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

    bool ParseV4(const std::string& s, uint8_t out[4])
    {
        size_t nPos = 0;
        for (int i = 0; i < 4; ++i)
        {
            if (nPos >= s.size() || s[nPos] < '0' || s[nPos] > '9')
                return false;

            unsigned int nVal   = 0;
            size_t       nStart = nPos;
            while (nPos < s.size() && s[nPos] >= '0' && s[nPos] <= '9')
            {
                nVal = nVal * 10 + static_cast<unsigned int>(s[nPos] - '0');
                if (nVal > 255)
                    return false;
                ++nPos;
            }
            // Reject 01 and 0001: a leading zero reads as octal to some parsers and as decimal to
            // others, and an allow list that means different things to different readers is worse
            // than one that refuses the input.
            if (nPos - nStart > 1 && s[nStart] == '0')
                return false;

            out[i] = static_cast<uint8_t>(nVal);

            if (i < 3)
            {
                if (nPos >= s.size() || s[nPos] != '.')
                    return false;
                ++nPos;
            }
        }
        return nPos == s.size();
    }

    // Full IPv6 including "::" compression and a trailing dotted-quad ("::ffff:192.168.1.1").
    bool ParseV6(const std::string& s, uint8_t out[16])
    {
        std::memset(out, 0, 16);

        // Split on "::" if present. Two occurrences is malformed - the compression is unambiguous
        // only because it may appear once.
        const size_t nDouble = s.find("::");
        if (nDouble != std::string::npos && s.find("::", nDouble + 1) != std::string::npos)
            return false;

        const std::string sHead = (nDouble == std::string::npos) ? s : s.substr(0, nDouble);
        const std::string sTail = (nDouble == std::string::npos) ? std::string() : s.substr(nDouble + 2);

        // Each half becomes a list of 16-bit groups; a trailing dotted quad contributes two.
        const auto Groups = [](const std::string& sPart, std::vector<uint16_t>& out16) -> bool
        {
            if (sPart.empty())
                return true;

            size_t nPos = 0;
            while (nPos <= sPart.size())
            {
                size_t nEnd = sPart.find(':', nPos);
                if (nEnd == std::string::npos)
                    nEnd = sPart.size();

                const std::string sGroup = sPart.substr(nPos, nEnd - nPos);
                if (sGroup.empty())
                    return false;

                if (sGroup.find('.') != std::string::npos)
                {
                    // Only legal as the final element.
                    if (nEnd != sPart.size())
                        return false;
                    uint8_t v4[4];
                    if (!ParseV4(sGroup, v4))
                        return false;
                    out16.push_back(static_cast<uint16_t>((v4[0] << 8) | v4[1]));
                    out16.push_back(static_cast<uint16_t>((v4[2] << 8) | v4[3]));
                }
                else
                {
                    if (sGroup.size() > 4)
                        return false;
                    uint32_t nVal = 0;
                    for (char c : sGroup)
                    {
                        int nDigit;
                        if      (c >= '0' && c <= '9') nDigit = c - '0';
                        else if (c >= 'a' && c <= 'f') nDigit = c - 'a' + 10;
                        else if (c >= 'A' && c <= 'F') nDigit = c - 'A' + 10;
                        else return false;
                        nVal = (nVal << 4) | static_cast<uint32_t>(nDigit);
                    }
                    out16.push_back(static_cast<uint16_t>(nVal));
                }

                if (nEnd == sPart.size())
                    break;
                nPos = nEnd + 1;
            }
            return true;
        };

        std::vector<uint16_t> vHead, vTail;
        if (!Groups(sHead, vHead) || !Groups(sTail, vTail))
            return false;

        if (nDouble == std::string::npos)
        {
            if (vHead.size() != 8)
                return false;
        }
        else if (vHead.size() + vTail.size() > 7)
        {
            // "::" must stand for at least one zero group, so a full 8 either side means the
            // compression was pointless and the text is malformed.
            return false;
        }

        size_t nOut = 0;
        for (uint16_t g : vHead)
        {
            out[nOut++] = static_cast<uint8_t>(g >> 8);
            out[nOut++] = static_cast<uint8_t>(g & 0xFF);
        }
        nOut = 16 - vTail.size() * 2;
        for (uint16_t g : vTail)
        {
            out[nOut++] = static_cast<uint8_t>(g >> 8);
            out[nOut++] = static_cast<uint8_t>(g & 0xFF);
        }
        return true;
    }

    std::string Trim(const std::string& s)
    {
        size_t nA = 0, nB = s.size();
        while (nA < nB && (s[nA] == ' ' || s[nA] == '\t')) ++nA;
        while (nB > nA && (s[nB - 1] == ' ' || s[nB - 1] == '\t')) --nB;
        return s.substr(nA, nB - nA);
    }
}


//====================================================================================================
// Parsing
//====================================================================================================

bool IpAcl::ParseAddress(const std::string& sAddrIn, uint8_t outBytes[16])
{
    std::string sAddr = Trim(sAddrIn);
    if (sAddr.empty())
        return false;

    // A bracketed literal is how a v6 address travels in a URL; accept it so pasting one works.
    if (sAddr.size() >= 2 && sAddr.front() == '[' && sAddr.back() == ']')
        sAddr = sAddr.substr(1, sAddr.size() - 2);

    // A zone id ("fe80::1%eth0") is scoped to one host's interface table and means nothing in a
    // rule written on another machine. Drop it rather than fail - the address part is still usable.
    const size_t nPercent = sAddr.find('%');
    if (nPercent != std::string::npos)
        sAddr.resize(nPercent);

    if (sAddr.find(':') != std::string::npos)
        return ParseV6(sAddr, outBytes);

    uint8_t v4[4];
    if (!ParseV4(sAddr, v4))
        return false;

    std::memcpy(outBytes, kV4Prefix, 12);
    std::memcpy(outBytes + 12, v4, 4);
    return true;
}

bool IpAcl::ParseRule(const std::string& sRuleIn, AclRule& outRule)
{
    const std::string sRule = Trim(sRuleIn);
    if (sRule.empty())
        return false;

    const size_t nSlash = sRule.rfind('/');
    const std::string sAddr = (nSlash == std::string::npos) ? sRule : sRule.substr(0, nSlash);

    if (!ParseAddress(sAddr, outRule.mBytes))
        return false;

    // Whether the text was a v4 address decides what a prefix length means: /24 written by a person
    // means 24 bits of a 32-bit address, which is 120 bits of the mapped 128-bit form.
    const bool bWasV4 = sAddr.find(':') == std::string::npos;

    if (nSlash == std::string::npos)
    {
        outRule.mnPrefixBits = 128;
        return true;
    }

    const std::string sBits = Trim(sRule.substr(nSlash + 1));
    if (sBits.empty() || sBits.size() > 3)
        return false;
    uint32_t nBits = 0;
    for (char c : sBits)
    {
        if (c < '0' || c > '9')
            return false;
        nBits = nBits * 10 + static_cast<uint32_t>(c - '0');
    }

    if (bWasV4)
    {
        if (nBits > 32)
            return false;
        outRule.mnPrefixBits = 96 + nBits;
    }
    else
    {
        if (nBits > 128)
            return false;
        outRule.mnPrefixBits = nBits;
    }

    // Zero the host bits. Two spellings of the same network ("192.168.1.5/24" and "192.168.1.0/24")
    // must match identically, and normalizing here means the comparison below never has to care.
    for (uint32_t i = 0; i < 128; ++i)
    {
        if (i >= outRule.mnPrefixBits)
            outRule.mBytes[i / 8] &= static_cast<uint8_t>(~(0x80u >> (i % 8)));
    }
    return true;
}

bool IpAcl::Matches(const AclRule& rule, const uint8_t addr[16])
{
    const uint32_t nFullBytes = rule.mnPrefixBits / 8;
    const uint32_t nOddBits   = rule.mnPrefixBits % 8;

    if (nFullBytes && std::memcmp(rule.mBytes, addr, nFullBytes) != 0)
        return false;

    if (nOddBits)
    {
        const uint8_t nMask = static_cast<uint8_t>(0xFF << (8 - nOddBits));
        if ((rule.mBytes[nFullBytes] & nMask) != (addr[nFullBytes] & nMask))
            return false;
    }
    return true;
}

bool IpAcl::IsLoopback(const std::string& sAddr)
{
    uint8_t bytes[16];
    if (!ParseAddress(sAddr, bytes))
        return false;

    // ::1
    static const uint8_t kV6Loop[16] = { 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,1 };
    if (std::memcmp(bytes, kV6Loop, 16) == 0)
        return true;

    // 127.0.0.0/8, in either the bare or the mapped spelling.
    return std::memcmp(bytes, kV4Prefix, 12) == 0 && bytes[12] == 127;
}


//====================================================================================================
// Lifecycle
//====================================================================================================

std::error_code IpAcl::Load(const std::string& sPath, std::string& outWarning)
{
    outWarning.clear();

    std::unique_lock lock(mMutex);
    msPath = sPath;
    mbEnabled = false;
    mEntries.clear();
    mRules.clear();

    FILE* pFile = std::fopen(sPath.c_str(), "rb");
    if (!pFile)
        return LoomOK();   // first run

    std::string sBody;
    char buf[4096];
    size_t nRead = 0;
    while ((nRead = std::fread(buf, 1, sizeof(buf), pFile)) > 0)
        sBody.append(buf, nRead);
    std::fclose(pFile);

    json j = json::parse(sBody, nullptr, false);
    if (j.is_discarded() || !j.is_object())
    {
        // Deliberately not fatal, and deliberately not "deny everything". A corrupt allow list is
        // an operator problem to fix at the keyboard, not a reason for the service to refuse to
        // start or to start unreachable.
        outWarning = "access list at " + sPath + " is unreadable - starting with it disabled";
        return LoomOK();
    }

    const bool bEnabled = j.value("enabled", false);

    std::vector<AclEntry> vEntries;
    std::vector<AclRule>  vRules;
    if (j.contains("entries") && j["entries"].is_array())
    {
        for (const auto& e : j["entries"])
        {
            AclEntry entry;
            if (e.is_string())
                entry.msRule = e.get<std::string>();
            else if (e.is_object())
            {
                entry.msRule = e.value("rule", std::string());
                entry.msNote = e.value("note", std::string());
            }

            AclRule rule;
            if (entry.msRule.empty() || !ParseRule(entry.msRule, rule))
            {
                if (outWarning.empty())
                    outWarning = "dropped unparseable access rule '" + entry.msRule + "'";
                continue;
            }
            vEntries.push_back(entry);
            vRules.push_back(rule);
        }
    }

    // An enabled list that parsed down to nothing would deny every non-loopback caller on the
    // strength of a bad file. Say so and leave it off.
    if (bEnabled && vEntries.empty())
    {
        outWarning = "access list at " + sPath + " is enabled but has no usable rules - "
                     "starting with it disabled";
        return LoomOK();
    }

    mbEnabled = bEnabled;
    mEntries.swap(vEntries);
    mRules.swap(vRules);
    return LoomOK();
}

std::error_code IpAcl::SaveLocked() const
{
    if (msPath.empty())
        return LoomOK();   // RAM-only instance; nothing to persist to

    json j;
    j["enabled"] = mbEnabled;
    j["entries"] = json::array();
    for (const AclEntry& e : mEntries)
    {
        json entry;
        entry["rule"] = e.msRule;
        if (!e.msNote.empty())
            entry["note"] = e.msNote;
        j["entries"].push_back(entry);
    }

    // tmp -> rename, the same commit discipline the snapshot writer uses. A torn allow list would
    // come back either wide open or fully closed depending on where the write stopped.
    const std::string sTmp = msPath + ".tmp";
    FILE* pFile = std::fopen(sTmp.c_str(), "wb");
    if (!pFile)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    const std::string sBody = j.dump(2);
    const bool bWrote = std::fwrite(sBody.data(), 1, sBody.size(), pFile) == sBody.size();
    std::fclose(pFile);

    if (!bWrote)
    {
        std::error_code ecRm;
        std::filesystem::remove(sTmp, ecRm);
        return MakeLoomError(eLoomErr::kInvalidArgument);
    }

    std::error_code ecMove;
    std::filesystem::rename(sTmp, msPath, ecMove);
    if (ecMove)
        return MakeLoomError(eLoomErr::kInvalidArgument);

    return LoomOK();
}


//====================================================================================================
// Hot path
//====================================================================================================

bool IpAcl::Allows(const std::string& sRemoteAddr) const
{
    std::shared_lock lock(mMutex);

    if (!mbEnabled)
        return true;

    // Checked before parsing so that a caller from the machine itself gets in even if crow hands
    // up an address spelling this parser does not recognize.
    if (IsLoopback(sRemoteAddr))
        return true;

    uint8_t addr[16];
    if (!ParseAddress(sRemoteAddr, addr))
        return false;

    for (const AclRule& rule : mRules)
    {
        if (Matches(rule, addr))
            return true;
    }
    return false;
}

bool IpAcl::Enabled() const
{
    std::shared_lock lock(mMutex);
    return mbEnabled;
}

void IpAcl::Get(bool& outEnabled, std::vector<AclEntry>& outEntries) const
{
    std::shared_lock lock(mMutex);
    outEnabled = mbEnabled;
    outEntries = mEntries;
}

std::error_code IpAcl::Set(bool bEnabled, const std::vector<AclEntry>& entries,
                           std::string& outBadRule)
{
    outBadRule.clear();

    // Parse everything BEFORE taking the write lock or touching state: a rejected change must
    // leave the running list exactly as it was.
    std::vector<AclEntry> vEntries;
    std::vector<AclRule>  vRules;
    for (const AclEntry& e : entries)
    {
        const std::string sRule = Trim(e.msRule);
        if (sRule.empty())
            continue;

        AclRule rule;
        if (!ParseRule(sRule, rule))
        {
            outBadRule = sRule;
            return MakeLoomError(eLoomErr::kInvalidArgument);
        }

        AclEntry kept;
        kept.msRule = sRule;
        kept.msNote = e.msNote;
        vEntries.push_back(kept);
        vRules.push_back(rule);
    }

    std::unique_lock lock(mMutex);
    mbEnabled = bEnabled;
    mEntries.swap(vEntries);
    mRules.swap(vRules);
    return SaveLocked();
}
