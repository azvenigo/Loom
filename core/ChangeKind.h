#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// ChangeKind - what the last thing to happen to a jot actually WAS.
//
// A jot already records when it last changed (mnUpdatedUS), who changed it (mEditor) and where from
// (msOrigin). What none of those answer is the question a person asks months later: what happened?
// "claude, from 192.168.1.30, at 14:02" describes a write without saying whether the todo was
// finished, snoozed for a week, or had a typo fixed in its summary.
//
// WHY THE ANSWER IS STORED RATHER THAN DERIVED. Every kind below except kAdded is a statement about
// the DIFFERENCE between two versions, so nothing reading a jot on its own can recover it - "this
// carries due:2026-09-20" cannot tell you whether that date was just set or just pushed back a
// week. The history log does hold before-images, but it is bounded and rotated by size: it answers
// "what changed this week", not "what last happened to this jot in March". Classifying at the one
// moment both versions exist - inside the store's write lock - and keeping the one-byte answer on
// the record is what makes the question survive log rotation.
//
// THE VOCABULARY IS CLOSED, and deliberately small. Each value is something a person would say out
// loud about a jot, and anything that is merely "the record is different now" is kUpdated. Adding a
// value means every reader has to learn it, so the bar is that the distinction changes what someone
// would DO about the jot.
//
// kNone IS NORMAL, not an error. Every jot written before this field existed carries it, and so
// does every record replayed from an older snapshot - the field is optional exactly so that adding
// it needed no migration. Render it as "no answer", never as "nothing happened".
//
// CLASSIFICATION IS POLICY, AND IT LIVES HERE rather than in JotStore, which is the primitive layer
// and does not decide what a tag means. This header knows two conventions - `status:done` and
// `due:` - and it is the only place in core/ that does. The store calls it at the single point a
// mutation commits; that placement is mechanical, the meaning is here.
//////////////////////////////////////////////////////////////////////////////////////////////////

enum class eChangeKind : uint8_t
{
    kNone = 0,      // unknown - written before the field existed, or by a path that does not stamp
    kAdded,         // the record was created
    kUpdated,       // content changed, and nothing more specific fits
    kDone,          // newly gained `status:done`
    kReopened,      // lost `status:done` - finished work that turned out not to be
    kScheduled,     // gained a `due:` it did not have
    kSnoozed,       // its `due:` moved later
    kRescheduled,   // its `due:` moved earlier, or changed in a way that is not plainly later
    kUnscheduled,   // lost its `due:` entirely
    kRetagged,      // tags rewritten by a tag merge, not by anyone editing this jot
    kRestored,      // put back to an earlier version out of the history log
    kCount
};

namespace CHANGE
{
    // The wire spelling. kNone is the empty string, which is what makes the omit-empty rule in the
    // codec apply to this field without a second test.
    inline const char* Name(eChangeKind kind)
    {
        switch (kind)
        {
        case eChangeKind::kAdded:       return "added";
        case eChangeKind::kUpdated:     return "updated";
        case eChangeKind::kDone:        return "done";
        case eChangeKind::kReopened:    return "reopened";
        case eChangeKind::kScheduled:   return "scheduled";
        case eChangeKind::kSnoozed:     return "snoozed";
        case eChangeKind::kRescheduled: return "rescheduled";
        case eChangeKind::kUnscheduled: return "unscheduled";
        case eChangeKind::kRetagged:    return "retagged";
        case eChangeKind::kRestored:    return "restored";
        case eChangeKind::kNone:
        case eChangeKind::kCount:
        default:                        return "";
        }
    }

    // The inverse, for a record read back off disk. An unrecognized spelling becomes kNone rather
    // than failing the parse: a snapshot written by a NEWER build that has learned another kind
    // must still load, and losing one label is a far smaller failure than refusing the record.
    inline eChangeKind Parse(std::string_view sName)
    {
        for (uint8_t i = 1; i < static_cast<uint8_t>(eChangeKind::kCount); ++i)
        {
            const eChangeKind kind = static_cast<eChangeKind>(i);
            if (sName == Name(kind))
                return kind;
        }
        return eChangeKind::kNone;
    }

    namespace DETAIL
    {
        inline bool Has(const std::vector<std::string>& vTags, std::string_view sTag)
        {
            for (const std::string& s : vTags)
                if (s == sTag)
                    return true;
            return false;
        }

        // The first tag carrying the prefix, or empty. A jot with two due: tags is malformed, and
        // taking the first is a stable answer rather than a correct one - there isn't one.
        inline std::string WithPrefix(const std::vector<std::string>& vTags, std::string_view sPrefix)
        {
            for (const std::string& s : vTags)
                if (s.size() > sPrefix.size() && s.compare(0, sPrefix.size(), sPrefix) == 0)
                    return s;
            return std::string();
        }

        // Is this a due: tag in the canonical `due:YYYY-MM-DD...` shape the schedulers emit? Only
        // those two may be ordered by string comparison; see Classify.
        inline bool IsDated(const std::string& sDue)
        {
            if (sDue.size() < 14)   // "due:" + "YYYY-MM-DD"
                return false;
            const char* p = sDue.c_str() + 4;
            for (int i = 0; i < 10; ++i)
            {
                const bool bDash = (i == 4 || i == 7);
                if (bDash ? p[i] != '-' : (p[i] < '0' || p[i] > '9'))
                    return false;
            }
            return true;
        }
    }

    //--------------------------------------------------------------------------------------------
    // What changed between two versions of one jot, judged from its tags alone.
    //
    // TAGS ONLY, on purpose. Every distinction in the vocabulary above is a tag transition; text,
    // summary and name edits are all kUpdated, so reading them here would buy nothing and would
    // mean copying a body twice per write to ask.
    //
    // PRECEDENCE, when a single write does several of these at once - finishing a todo and clearing
    // its due date is one Save on the dashboard: completion outranks scheduling, and scheduling
    // outranks a plain edit. The label answers "what happened", and what happened is that it got
    // done; that the due date also went away is a detail of how the front end spells completion.
    //
    // Returns kUpdated when nothing more specific fits, and NEVER kNone: the caller is committing a
    // write it has already established is a real change, so "no answer" would be a lie. kNone is
    // reserved for records nobody classified at all.
    //--------------------------------------------------------------------------------------------
    inline eChangeKind Classify(const std::vector<std::string>& vBefore,
                                const std::vector<std::string>& vAfter)
    {
        const bool bDoneBefore = DETAIL::Has(vBefore, "status:done");
        const bool bDoneAfter  = DETAIL::Has(vAfter,  "status:done");
        if (bDoneAfter && !bDoneBefore)
            return eChangeKind::kDone;
        if (bDoneBefore && !bDoneAfter)
            return eChangeKind::kReopened;

        const std::string sDueBefore = DETAIL::WithPrefix(vBefore, "due:");
        const std::string sDueAfter  = DETAIL::WithPrefix(vAfter,  "due:");
        if (sDueBefore != sDueAfter)
        {
            if (sDueBefore.empty())
                return eChangeKind::kScheduled;
            if (sDueAfter.empty())
                return eChangeKind::kUnscheduled;

            // ORDERED BY STRING COMPARISON, which is exact for the canonical form and only for it:
            // `due:2026-09-13t17:00` is zero-padded and most-significant-field-first, so lexical
            // order IS chronological order. A hand-typed `due:friday` is not, and comparing it
            // would confidently report the wrong direction - so anything off-format falls back to
            // kRescheduled, which says the date moved without claiming which way.
            if (!DETAIL::IsDated(sDueBefore) || !DETAIL::IsDated(sDueAfter))
                return eChangeKind::kRescheduled;

            return sDueAfter > sDueBefore ? eChangeKind::kSnoozed : eChangeKind::kRescheduled;
        }

        return eChangeKind::kUpdated;
    }
}
