#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "core/LoomError.h"

#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// IpAcl - the runtime-editable address allow list.
//
// WHY THIS IS NOT --token. The bearer token answers "does the caller know the secret"; this answers
// "is the caller somewhere I expect a caller to be". They compose rather than replace: the token
// still applies to every route it applied to before, and an address that clears this list still has
// to present it. A LAN service wants both, because a token that leaks is a token that works from
// anywhere and an address list that stands alone trusts every process on an allowed machine.
//
// WHY IT LIVES IN core/ AND NOT http/. Nothing here knows what a request is - it takes an address
// string and answers yes or no - which is what lets coretest exercise the CIDR matcher without
// binding a socket. Same reasoning that keeps crow out of HttpServer.h.
//
// LOOPBACK IS ALWAYS ALLOWED AND CANNOT BE REMOVED. This is the whole anti-lockout design, and it
// is deliberately not configurable. The list is edited over the network by the very thing the list
// controls, so a typo is not a mistake with a support ticket - it is a service you can no longer
// reach to fix. Somebody at the machine can always get in and repair it. The route layer adds a
// second guard on top (a change that would exclude the caller's own address is refused unless
// forced), but that one is advice; this one is the floor.
//
// MATCHING IS ON BYTES, NOT ON TEXT. Every address is normalized to the 16-byte IPv6 form, with
// IPv4 mapped into ::ffff:a.b.c.d, and a rule is those bytes plus a prefix length. That is what
// makes "192.168.1.0/24" and a caller crow reports as "::ffff:192.168.1.112" agree - which they
// must, because whether a dual-stack listener hands up the mapped form or the bare one is not a
// property this code gets to depend on.
//
// The file is written whole on every change (it is a few hundred bytes) via tmp -> rename, so a
// crash mid-write leaves the previous list rather than a truncated one that would open the service
// to everybody or to nobody.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct AclEntry
{
    std::string msRule;   // exactly as typed: "192.168.1.5", "192.168.1.0/24", "2001:db8::/32"
    std::string msNote;   // free text, for remembering which machine this was
};

// One parsed rule: 16 bytes of network address plus how many leading bits of it are significant.
struct AclRule
{
    uint8_t  mBytes[16] = {};
    uint32_t mnPrefixBits = 128;
};

class IpAcl
{
public:
    //----------------------------------------------------------------------------------------
    // Parsing. Static and side-effect free so a route can validate what somebody typed before
    // deciding whether to accept the whole list.
    //----------------------------------------------------------------------------------------

    // "192.168.1.5", "192.168.1.0/24", "2001:db8::1", "2001:db8::/32", "::1".
    // A bare address is an exact match (/32 for v4, /128 for v6).
    static bool ParseRule(const std::string& sRule, AclRule& outRule);

    // An address with no prefix. Accepts the IPv4-mapped IPv6 form and normalizes it.
    static bool ParseAddress(const std::string& sAddr, uint8_t outBytes[16]);

    static bool Matches(const AclRule& rule, const uint8_t addr[16]);

    // ::1 and 127.0.0.0/8, in either spelling.
    static bool IsLoopback(const std::string& sAddr);

    //----------------------------------------------------------------------------------------
    // Lifecycle
    //----------------------------------------------------------------------------------------

    // Remembers the path and reads it if it exists. A MISSING FILE IS NOT AN ERROR - it is a first
    // run, and a first run must come up allowing everything or upgrading would lock the operator
    // out of their own service. A CORRUPT file is also not fatal for the same reason: it is
    // reported and the list comes up disabled rather than refusing every request.
    std::error_code Load(const std::string& sPath, std::string& outWarning);

    //----------------------------------------------------------------------------------------
    // The hot path. Called on every request, from every worker thread.
    //----------------------------------------------------------------------------------------

    // False only when the list is enabled AND the address is neither loopback nor covered by a
    // rule. A disabled list allows everything, which is the shipped default.
    bool Allows(const std::string& sRemoteAddr) const;

    bool Enabled() const;

    //----------------------------------------------------------------------------------------
    // Editing
    //----------------------------------------------------------------------------------------

    void Get(bool& outEnabled, std::vector<AclEntry>& outEntries) const;

    // Validates every rule, swaps the list in, and writes the file. Rejects the whole change if any
    // rule is unparseable - a partially applied allow list is a security control nobody can reason
    // about. outBadRule names the offender.
    std::error_code Set(bool bEnabled, const std::vector<AclEntry>& entries, std::string& outBadRule);

private:
    std::error_code SaveLocked() const;

    mutable std::shared_mutex mMutex;
    std::string               msPath;
    bool                      mbEnabled = false;
    std::vector<AclEntry>     mEntries;
    std::vector<AclRule>      mRules;      // parallel to mEntries, parsed once at Set/Load
};
