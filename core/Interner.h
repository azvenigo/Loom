#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// Interner - a bidirectional string <-> uint32 map with stable ids.
//
// Used for tags, editors and search terms. Ids are handed out densely from zero and are never
// reused or reordered, so an id can be stored inside a Jot and stay valid for the life of the
// process. That is what lets a Jot hold four-byte tag ids instead of strings: comparison and
// set-intersection become integer work, and the record stays small.
//
// NOT thread-safe on its own, and deliberately so - every instance lives inside JotStore and is
// covered by that one shared_mutex. Intern() only ever runs under the write lock.
//
// GOTCHA: Value() returns a reference into an internal vector that grows when new strings are
// interned. It is valid only while the caller holds the store lock and nothing has interned since.
// Readers hold a shared_lock and so cannot intern, which makes this safe by construction - but do
// not stash the reference past the lock.
//////////////////////////////////////////////////////////////////////////////////////////////////

constexpr uint32_t kInvalidStrID = 0xFFFFFFFFu;

class Interner
{
public:
    // Returns the existing id, or creates one. Only call under the store's write lock.
    uint32_t Intern(std::string_view sValue)
    {
        const auto it = mMap.find(sValue);
        if (it != mMap.end())
            return it->second;

        const uint32_t nID = static_cast<uint32_t>(mValues.size());
        mValues.emplace_back(sValue);
        // The map keeps its own copy of the key. Aliasing it to mValues.back() with a string_view
        // would halve the memory but is not safe: vector growth moves the string objects, and
        // anything short enough for SSO carries its bytes inline, so those views would dangle.
        // Two copies of each distinct tag/term is a few MB at the scales here - not worth the trap.
        mMap.emplace(std::string(sValue), nID);
        return nID;
    }

    // kInvalidStrID when absent. Safe under a shared_lock.
    uint32_t Find(std::string_view sValue) const
    {
        const auto it = mMap.find(sValue);
        return it == mMap.end() ? kInvalidStrID : it->second;
    }

    bool IsValid(uint32_t nID) const { return nID < mValues.size(); }

    // See the GOTCHA above about reference lifetime.
    const std::string& Value(uint32_t nID) const
    {
        static const std::string ksEmpty;
        return nID < mValues.size() ? mValues[nID] : ksEmpty;
    }

    size_t Count() const { return mValues.size(); }

    void Clear()
    {
        mMap.clear();
        mValues.clear();
    }

private:
    // Transparent hashing so Find(string_view) does not allocate a temporary std::string on every
    // query term - which, on the hot path, it otherwise would.
    struct Hash
    {
        using is_transparent = void;
        size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
        size_t operator()(const std::string& s) const noexcept { return std::hash<std::string_view>{}(s); }
    };
    struct Equal
    {
        using is_transparent = void;
        bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    };

    std::unordered_map<std::string, uint32_t, Hash, Equal> mMap;
    std::vector<std::string>                               mValues;
};
