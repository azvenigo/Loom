#pragma once

#include <string>
#include <system_error>

//////////////////////////////////////////////////////////////////////////////////////////////////
// LoomError - the service's error codes, plumbed as std::error_code.
//
// House style from ZLibraries/Common/zhelpers/ZError.h: nothing in core throws, failures come back
// as a std::error_code. This is the same pattern, kept standalone so core has no dependency on
// ZLibraries yet - when the submodule lands this category can be folded in beside the others.
//
// The category maps each code onto a portable std::errc condition where an honest equivalent
// exists, so callers can ask the general question:
//
//      if (ec == std::errc::no_such_file_or_directory)   // matches kNotFound
//
// Codes with no sensible std::errc equivalent (kConflict, kNameInUse) deliberately map to nothing.
// Comparing those against std::errc is meant to fail - the HTTP layer switches on the enum itself
// to pick a status, which is the only place the distinction matters.
//////////////////////////////////////////////////////////////////////////////////////////////////

enum class eLoomErr
{
    kOK = 0,

    kNotFound,          // no jot with that id or name
    kNameInUse,         // another jot already holds that slug          -> HTTP 409
    kConflict,          // expect_updated did not match current state   -> HTTP 409
    kInvalidArgument,   // malformed id, bad range, unparseable query
    kEmptyJot,          // nothing to store - no text, no summary, no name
    kTooLarge,          // text or tag count beyond the configured cap
    kReadOnly,          // store opened read-only (memory-mode dry run)
};


namespace LoomErrorDetail
{
    class Category : public std::error_category
    {
    public:
        const char* name() const noexcept override { return "loom"; }

        std::string message(int nValue) const override
        {
            switch (static_cast<eLoomErr>(nValue))
            {
            case eLoomErr::kOK:              return "ok";
            case eLoomErr::kNotFound:        return "no such jot";
            case eLoomErr::kNameInUse:       return "name already in use";
            case eLoomErr::kConflict:        return "jot changed since the expected revision";
            case eLoomErr::kInvalidArgument: return "invalid argument";
            case eLoomErr::kEmptyJot:        return "jot has no content";
            case eLoomErr::kTooLarge:        return "content exceeds configured limit";
            case eLoomErr::kReadOnly:        return "store is read-only";
            }
            return "unknown loom error";
        }

        // Only the codes with an honest portable equivalent are mapped. kConflict and kNameInUse
        // are intentionally absent - see the header note.
        std::error_condition default_error_condition(int nValue) const noexcept override
        {
            switch (static_cast<eLoomErr>(nValue))
            {
            case eLoomErr::kOK:
                return std::error_condition();
            case eLoomErr::kNotFound:
                return std::make_error_condition(std::errc::no_such_file_or_directory);
            case eLoomErr::kInvalidArgument:
            case eLoomErr::kEmptyJot:
                return std::make_error_condition(std::errc::invalid_argument);
            case eLoomErr::kTooLarge:
                return std::make_error_condition(std::errc::file_too_large);
            case eLoomErr::kReadOnly:
                return std::make_error_condition(std::errc::read_only_file_system);
            default:
                return std::error_condition(nValue, *this);
            }
        }
    };

    inline const Category& Instance()
    {
        static Category theCategory;
        return theCategory;
    }
}

inline const std::error_category& LoomCategory() { return LoomErrorDetail::Instance(); }

inline std::error_code MakeLoomError(eLoomErr e)
{
    return std::error_code(static_cast<int>(e), LoomCategory());
}

// Convenience for the common "succeeded" return.
inline std::error_code LoomOK() { return std::error_code(); }

// True when ec came from this category, so the HTTP/MCP layers can switch on the enum safely.
inline bool IsLoomError(const std::error_code& ec)
{
    return ec.category() == LoomCategory();
}

inline eLoomErr LoomErrorOf(const std::error_code& ec)
{
    return IsLoomError(ec) ? static_cast<eLoomErr>(ec.value()) : eLoomErr::kOK;
}

// Lets std::error_code be constructed straight from the enum.
namespace std
{
    template <> struct is_error_code_enum<eLoomErr> : true_type {};
}

inline std::error_code make_error_code(eLoomErr e) { return MakeLoomError(e); }
