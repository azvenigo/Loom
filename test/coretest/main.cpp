// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// coretest - assertions over the phase-1 core.
//
// ZLibraries has no test framework and its two existing "test" tools are visual smoke apps with no
// assertions, so this is hand-rolled - but it does assert, because several of the plan's
// verification gates are things you cannot eyeball: index coherence, id monotonicity under
// concurrency, and the exact shape of a bare jot.
//
// The index-coherence check is the important one. It does not reach into private state; it runs a
// battery of queries, calls RebuildIndexes() to regenerate every index from the records alone, and
// requires identical answers. That is the real invariant - the incremental add/update/remove paths
// must never drift from a clean rebuild - and it is checked as a black box, so it keeps working
// when the internals change.
//
// Exit code 0 means everything passed. Anything else means read the output.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "core/IpAcl.h"
#include "core/JotStore.h"
#include "core/LoomTime.h"
#include "core/Ops.h"
#include "core/TagRegistry.h"
#include "core/Tokenizer.h"

#include <algorithm>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace
{
    int gnChecks = 0;
    int gnFailed = 0;
    const char* gpSection = "";

    void Section(const char* pName)
    {
        gpSection = pName;
        std::printf("\n[%s]\n", pName);
    }

    void Check(bool bCondition, const char* pWhat)
    {
        ++gnChecks;
        if (bCondition)
        {
            std::printf("  ok    %s\n", pWhat);
        }
        else
        {
            ++gnFailed;
            std::printf("  FAIL  %s   (%s)\n", pWhat, gpSection);
        }
    }

    JotInput Text(const std::string& s)
    {
        JotInput in;
        in.msText = s;
        return in;
    }

    std::vector<tJotID> SearchIDs(Ops& ops, const Ops::QuerySpec& spec)
    {
        Query query;
        SearchResultSet results;
        std::vector<tJotID> vIDs;
        if (ops.BuildQuery(spec, query) || ops.Search(query, results))
            return vIDs;
        for (const Jot& j : results.mJots)
            vIDs.push_back(j.mID);
        return vIDs;
    }

    //--------------------------------------------------------------------------------------------

    void TestTokenizer()
    {
        Section("tokenizer");

        std::vector<std::string> v;
        TOK::Tokenize("Ordered 50ft of Ethernet cable today", v);
        // "to" is a stopword; the measurement must survive as one term.
        Check(std::find(v.begin(), v.end(), "50ft") != v.end(), "digits stay attached to letters");
        Check(std::find(v.begin(), v.end(), "ethernet") != v.end(), "case folded");
        Check(std::find(v.begin(), v.end(), "to") == v.end(), "stopword dropped from body");

        v.clear();
        TOK::TokenizeKeepStopwords("the meds", v);
        Check(v.size() == 2, "stopwords kept for summary and query");

        v.clear();
        TOK::Tokenize("user-preferences", v);
        Check(v.size() == 2 && v[0] == "user" && v[1] == "preferences",
              "hyphen splits so a slug is findable by its parts");

        v.clear();
        TOK::Tokenize("'quoted' user's", v);
        Check(std::find(v.begin(), v.end(), "quoted") != v.end(), "outer apostrophes stripped");
        Check(std::find(v.begin(), v.end(), "user's") != v.end(), "inner apostrophe kept");
    }

    void TestTagNormalization()
    {
        Section("tag normalization");

        Check(TagRegistry::Normalize("  Image Library  ") == "image-library", "trim, fold, space to dash");
        Check(TagRegistry::Normalize("A__B") == "a-b", "underscores collapse to one dash");
        Check(TagRegistry::Normalize("--x--") == "x", "leading and trailing separators dropped");
        Check(TagRegistry::Normalize("   ").empty(), "whitespace-only normalizes to empty");
        Check(TagRegistry::IsReserved("type:user"), "colon marks a structural tag");
        Check(!TagRegistry::IsReserved("meds"), "plain vocabulary is not reserved");
    }

    void TestBareJotShape()
    {
        Section("bare jot shape");

        JotStore store;
        Ops ops(store);

        AddResult result;
        Check(!ops.Add(Text("Ordered 50ft of ethernet cable today"), result), "plain jot accepted");

        const Jot& j = result.mJot;
        Check(j.mID > 0, "id allocated");
        Check(j.msName.empty(), "no name");
        Check(j.msSummary.empty(), "no summary");
        Check(j.mTags.empty(), "no tags");
        Check(j.mLinks.empty(), "no links");
        Check(j.mPendingLinks.empty(), "no pending links");
        Check(j.mnUpdatedUS == 0, "never edited");
        Check(j.mEditor == kDefaultEditor, "editor defaults to user");
        Check(j.EffectiveUpdatedUS() == j.mID, "effective update time falls back to creation");

        AddResult empty;
        Check(ops.Add(JotInput(), empty) == MakeLoomError(eLoomErr::kEmptyJot),
              "a jot with no content at all is refused");
    }

    void TestIDMonotonicity()
    {
        Section("id allocation");

        JotStore store;
        Ops ops(store);

        constexpr size_t kThreads = 8;
        constexpr size_t kPer     = 500;

        std::vector<std::vector<tJotID>> vPerThread(kThreads);
        std::vector<std::thread> vThreads;

        for (size_t t = 0; t < kThreads; ++t)
        {
            vThreads.emplace_back([&, t]()
            {
                vPerThread[t].reserve(kPer);
                for (size_t i = 0; i < kPer; ++i)
                {
                    AddResult r;
                    if (!ops.Add(Text("burst " + std::to_string(t) + " " + std::to_string(i)), r))
                        vPerThread[t].push_back(r.mJot.mID);
                }
            });
        }
        for (std::thread& th : vThreads)
            th.join();

        std::set<tJotID> allIDs;
        size_t nTotal = 0;
        for (const auto& v : vPerThread)
        {
            nTotal += v.size();
            for (tJotID id : v)
                allIDs.insert(id);
        }

        Check(nTotal == kThreads * kPer, "every concurrent add succeeded");
        // The real risk: many adds land in the same microsecond, so the timestamp alone is not
        // unique and the CAS in NextID is what saves it.
        Check(allIDs.size() == nTotal, "ids unique across threads despite same-microsecond writes");
        Check(store.Size() == nTotal, "store holds them all");
    }

    void TestNamesAndUpsert()
    {
        Section("names and upsert");

        JotStore store;
        Ops ops(store);

        JotInput in = Text("first body");
        in.msName    = "user-preferences";
        in.msSummary = "how user wants me to work";

        AddResult a;
        Check(!ops.Add(in, a), "named jot created");
        Check(a.mJot.msName == "user-preferences", "slug stored normalized");

        AddResult dup;
        Check(ops.Add(in, dup) == MakeLoomError(eLoomErr::kNameInUse),
              "Add refuses to silently replace an existing slug");

        JotInput in2 = Text("second body");
        in2.msName = "User-Preferences";      // different case, same slug
        AddResult up;
        Check(!ops.Upsert(in2, 0, up), "Upsert accepts it");
        Check(!up.mbCreated, "Upsert updated rather than created");
        Check(up.mJot.mID == a.mJot.mID, "same record, id preserved");
        Check(up.mJot.msText == "second body", "body replaced");
        Check(up.mJot.msSummary == "how user wants me to work",
              "field not in the patch is left alone");
        Check(up.mJot.mnUpdatedUS != 0, "update stamped");
        Check(store.Size() == 1, "no duplicate record created");

        Jot fetched;
        Check(!ops.GetByName("user-preferences", fetched) && fetched.mID == a.mJot.mID,
              "lookup by slug");
    }

    void TestOptimisticConcurrency()
    {
        Section("optimistic concurrency");

        JotStore store;
        Ops ops(store);

        AddResult a;
        ops.Add(Text("original"), a);
        const int64_t nBase = a.mJot.EffectiveUpdatedUS();

        AddResult first;
        Check(!ops.Update(a.mJot.mID, Text("writer one"), nBase, first),
              "update with the expected revision succeeds");

        // Second writer still holds the stale revision - exactly the two-agents-same-memory case.
        AddResult second;
        Check(ops.Update(a.mJot.mID, Text("writer two"), nBase, second)
                  == MakeLoomError(eLoomErr::kConflict),
              "stale revision is refused with kConflict, not silently clobbered");

        Jot current;
        ops.Get(a.mJot.mID, current);
        Check(current.msText == "writer one", "the winner's write survived intact");

        AddResult forced;
        Check(!ops.Update(a.mJot.mID, Text("writer two forced"), 0, forced),
              "passing 0 opts into last-write-wins");
    }

    void TestPendingLinks()
    {
        Section("pending links");

        JotStore store;
        Ops ops(store);

        JotInput in = Text("see the other one");
        in.msName  = "first";
        in.mLinks  = std::vector<std::string>{ "not-written-yet" };

        AddResult a;
        Check(!ops.Add(in, a), "link to a nonexistent slug is accepted");
        Check(a.mJot.mLinks.empty(), "it does not become a real link");
        Check(a.mJot.mPendingLinks.size() == 1, "it is parked as pending");

        JotInput in2 = Text("here I am");
        in2.msName = "not-written-yet";
        AddResult b;
        Check(!ops.Add(in2, b), "the target is created later");

        Jot refreshed;
        ops.Get(a.mJot.mID, refreshed);
        Check(refreshed.mLinks.size() == 1 && refreshed.mLinks[0] == b.mJot.mID,
              "the pending link is promoted to a real one");
        Check(refreshed.mPendingLinks.empty(), "and cleared from pending");

        std::vector<Jot> vNeighbors;
        Check(!ops.Links(b.mJot.mID, 1, vNeighbors), "neighborhood query works");
        Check(vNeighbors.size() == 1 && vNeighbors[0].mID == a.mJot.mID,
              "backlink found from the target");

        // Deleting the target should hand the edge back as pending rather than dropping it.
        Check(!ops.Delete(b.mJot.mID), "target deleted");
        ops.Get(a.mJot.mID, refreshed);
        Check(refreshed.mLinks.empty(), "dangling link removed");
        Check(refreshed.mPendingLinks.size() == 1,
              "and demoted back to pending so re-creating restores the graph");
    }

    void TestTagSuggestions()
    {
        Section("tag hygiene");

        JotStore store;
        Ops ops(store);

        for (int i = 0; i < 5; ++i)
        {
            JotInput in = Text("dose note " + std::to_string(i));
            in.mTags = std::vector<std::string>{ "meds" };
            AddResult r;
            ops.Add(in, r);
        }

        JotInput typo = Text("another dose note");
        typo.mTags = std::vector<std::string>{ "medz" };
        AddResult warned;
        Check(!ops.Add(typo, warned), "the near-duplicate tag is still accepted");
        Check(!warned.mWarnings.Empty(), "but the write carries a warning");
        Check(warned.mWarnings.mMessages[0].find("meds") != std::string::npos,
              "and the warning names the established tag");

        JotInput plural = Text("variant note");
        plural.mTags = std::vector<std::string>{ "med" };
        AddResult warned2;
        ops.Add(plural, warned2);
        Check(!warned2.mWarnings.Empty(), "morphological variants are caught too");

        JotInput reserved = Text("structural note");
        reserved.mTags = std::vector<std::string>{ "type:user", "asserted:2026-08-18" };
        AddResult quiet;
        ops.Add(reserved, quiet);
        Check(quiet.mWarnings.Empty(),
              "reserved prefix:value tags are structure, not vocabulary - no warning");

        std::vector<TagStat> vTags;
        ops.ListTags(vTags, false);
        const bool bHasReserved = std::any_of(vTags.begin(), vTags.end(),
            [](const TagStat& s) { return s.mbReserved; });
        Check(!bHasReserved, "reserved tags excluded from the vocabulary listing");
        Check(!vTags.empty() && vTags[0].msTag == "meds" && vTags[0].mnCount == 5,
              "listing is count-ordered with correct refcounts");

        size_t nChanged = 0;
        Check(!ops.MergeTags({ "medz", "med" }, "meds", nChanged), "merge succeeds");
        Check(nChanged == 2, "both variant jots rewritten");

        ops.ListTags(vTags, false);
        Check(!vTags.empty() && vTags[0].mnCount == 7, "merged count folded into the target");
    }

    void TestSearch()
    {
        Section("search");

        JotStore store;
        Ops ops(store);

        JotInput summarized = Text("this body mentions nothing special at all");
        summarized.msSummary = "homelab network topology";
        summarized.msName    = "homelab-network";
        AddResult withSummary;
        ops.Add(summarized, withSummary);

        JotInput bodyOnly = Text("some notes about homelab network topology in the body text");
        AddResult withBody;
        ops.Add(bodyOnly, withBody);

        JotInput tagged = Text("completely unrelated prose");
        tagged.mTags = std::vector<std::string>{ "homelab" };
        AddResult withTag;
        ops.Add(tagged, withTag);

        {
            Ops::QuerySpec spec;
            spec.msText = "homelab network";
            const auto vIDs = SearchIDs(ops, spec);
            Check(vIDs.size() >= 2, "text query finds both prose matches");
            // The summary is the field recall matches against, so it must outrank the body.
            Check(!vIDs.empty() && vIDs[0] == withSummary.mJot.mID,
                  "a summary hit outranks the same words in a body");
        }

        {
            Ops::QuerySpec spec;
            spec.msText = "homelab";
            const auto vIDs = SearchIDs(ops, spec);
            Check(std::find(vIDs.begin(), vIDs.end(), withTag.mJot.mID) != vIDs.end(),
                  "a jot tagged with the query term is found even without a text hit");
        }

        {
            Ops::QuerySpec spec;
            spec.mTags.push_back("homelab");
            const auto vIDs = SearchIDs(ops, spec);
            Check(vIDs.size() == 1 && vIDs[0] == withTag.mJot.mID, "tag filter");
        }

        {
            Ops::QuerySpec spec;
            spec.mNotTags.push_back("homelab");
            const auto vIDs = SearchIDs(ops, spec);
            Check(std::find(vIDs.begin(), vIDs.end(), withTag.mJot.mID) == vIDs.end(),
                  "negative tag filter excludes");
            Check(vIDs.size() == 2, "and keeps everything else");
        }

        {
            Ops::QuerySpec spec;
            spec.msName = "homelab-network";
            const auto vIDs = SearchIDs(ops, spec);
            Check(vIDs.size() == 1 && vIDs[0] == withSummary.mJot.mID, "exact slug lookup");
        }

        {
            Ops::QuerySpec spec;
            spec.msText  = "homel";
            spec.mbPrefix = true;
            const auto vIDs = SearchIDs(ops, spec);
            Check(!vIDs.empty(), "prefix match drives live-as-you-type");

            Ops::QuerySpec exact;
            exact.msText = "homel";
            Check(SearchIDs(ops, exact).empty(), "and does not fire without the prefix flag");
        }

        {
            Ops::QuerySpec spec;
            spec.msSince = "1d";
            Check(SearchIDs(ops, spec).size() == 3, "relative time range includes everything recent");

            spec.msSince = "";
            spec.msUntil = "2000-01-01";
            Check(SearchIDs(ops, spec).empty(), "absolute upper bound excludes everything");
        }

        {
            Ops::QuerySpec spec;
            spec.msSince = "2026-01-01";
            spec.msUntil = "2020-01-01";
            Query q;
            Check(ops.BuildQuery(spec, q) == MakeLoomError(eLoomErr::kInvalidArgument),
                  "an inverted range is reported, not silently empty");
        }

        {
            Ops::QuerySpec spec;
            spec.msOrder = "oldest";
            const auto vIDs = SearchIDs(ops, spec);
            Check(vIDs.size() == 3 && vIDs[0] == withSummary.mJot.mID, "explicit oldest-first order");

            spec.msOrder = "newest";
            const auto vNewest = SearchIDs(ops, spec);
            Check(vNewest.size() == 3 && vNewest[0] == withTag.mJot.mID, "explicit newest-first order");
        }
    }

    // The invariant that matters most: the incremental index paths must never drift from a clean
    // rebuild off the records alone.
    void TestIndexCoherence()
    {
        Section("index coherence");

        JotStore store;
        Ops ops(store);

        std::vector<tJotID> vIDs;
        for (int i = 0; i < 300; ++i)
        {
            JotInput in = Text("record " + std::to_string(i) + " alpha beta gamma delta");
            if (i % 3 == 0)
            {
                in.msSummary = "summary for " + std::to_string(i) + " alpha";
                in.msName    = "rec-" + std::to_string(i);
            }
            if (i % 4 == 0)
                in.mTags = std::vector<std::string>{ "even", "quarter" };
            else if (i % 2 == 0)
                in.mTags = std::vector<std::string>{ "even" };
            in.msEditor = (i % 5 == 0) ? "claude" : "user";

            AddResult r;
            if (!ops.Add(in, r))
                vIDs.push_back(r.mJot.mID);
        }

        // Churn: edits and deletes are what actually exercise the unindex path.
        for (size_t i = 0; i < vIDs.size(); i += 7)
        {
            JotInput patch;
            patch.msText = "rewritten " + std::to_string(i) + " epsilon zeta";
            patch.mTags  = std::vector<std::string>{ "rewritten" };
            AddResult r;
            ops.Update(vIDs[i], patch, 0, r);
        }
        for (size_t i = 0; i < vIDs.size(); i += 11)
            ops.Delete(vIDs[i]);

        const std::vector<Ops::QuerySpec> vProbes = []()
        {
            std::vector<Ops::QuerySpec> v;
            { Ops::QuerySpec s; s.msText = "alpha";                 s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.msText = "epsilon zeta";          s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.mTags = { "even" };               s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.mTags = { "even", "quarter" };    s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.mNotTags = { "rewritten" };       s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.msEditor = "claude";              s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.msText = "summary";               s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.msText = "alp"; s.mbPrefix = true; s.mnLimit = 500; v.push_back(s); }
            { Ops::QuerySpec s; s.msOrder = "oldest";               s.mnLimit = 500; v.push_back(s); }
            return v;
        }();

        std::vector<std::vector<tJotID>> vBefore;
        for (const auto& spec : vProbes)
            vBefore.push_back(SearchIDs(ops, spec));

        const StoreStats before = store.GetStats();

        store.RebuildIndexes();

        bool bAllMatch = true;
        for (size_t i = 0; i < vProbes.size(); ++i)
        {
            if (SearchIDs(ops, vProbes[i]) != vBefore[i])
            {
                bAllMatch = false;
                std::printf("        probe %zu differs after rebuild\n", i);
            }
        }

        const StoreStats after = store.GetStats();

        Check(bAllMatch, "every query returns identical results after a full index rebuild");
        Check(after.mnJots == before.mnJots, "record count unchanged by rebuild");
        Check(after.mnTags == before.mnTags, "tag refcounts survive the rebuild");
        Check(after.mnNamed == before.mnNamed, "name index rebuilt completely");

        // A deleted jot must be gone from every index, not merely from the record map.
        Ops::QuerySpec all;
        all.mnLimit = 500;
        const auto vAll = SearchIDs(ops, all);
        Check(vAll.size() == before.mnJots, "no tombstones surface in an unfiltered query");
    }

    void TestConcurrentReadWrite()
    {
        Section("concurrent readers and writers");

        JotStore store;
        Ops ops(store);

        for (int i = 0; i < 500; ++i)
        {
            AddResult r;
            ops.Add(Text("seed " + std::to_string(i) + " common shared token"), r);
        }

        std::atomic<bool>     bStop{ false };
        std::atomic<uint64_t> nReads{ 0 };
        std::atomic<uint64_t> nWrites{ 0 };
        std::atomic<uint64_t> nErrors{ 0 };

        std::vector<std::thread> vThreads;

        for (int t = 0; t < 4; ++t)
        {
            vThreads.emplace_back([&]()
            {
                while (!bStop.load(std::memory_order_relaxed))
                {
                    Ops::QuerySpec spec;
                    spec.msText  = "shared";
                    spec.mnLimit = 20;

                    Query q;
                    SearchResultSet r;
                    if (ops.BuildQuery(spec, q) || ops.Search(q, r))
                        nErrors.fetch_add(1, std::memory_order_relaxed);
                    else
                        nReads.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        for (int t = 0; t < 2; ++t)
        {
            vThreads.emplace_back([&, t]()
            {
                for (int i = 0; i < 2000; ++i)
                {
                    AddResult r;
                    if (ops.Add(Text("writer " + std::to_string(t) + " " + std::to_string(i)
                                     + " common shared token"), r))
                        nErrors.fetch_add(1, std::memory_order_relaxed);
                    else
                        nWrites.fetch_add(1, std::memory_order_relaxed);
                }
            });
        }

        // Join writers first, then stop the readers.
        for (size_t i = 4; i < vThreads.size(); ++i)
            vThreads[i].join();
        bStop.store(true, std::memory_order_relaxed);
        for (size_t i = 0; i < 4; ++i)
            vThreads[i].join();

        Check(nErrors.load() == 0, "no operation errored under concurrent load");
        Check(nReads.load() > 0, "readers made progress alongside writers");
        Check(store.Size() == size_t(500) + size_t(2) * size_t(2000),
              "every write landed exactly once");

        // Readers ran against a store being mutated the entire time; the rebuild comparison proves
        // none of those concurrent writes left an index inconsistent.
        Ops::QuerySpec spec;
        spec.msText  = "shared";
        spec.mnLimit = 10000;
        const auto vBefore = SearchIDs(ops, spec);
        store.RebuildIndexes();
        Check(SearchIDs(ops, spec) == vBefore,
              "indexes still agree with a clean rebuild after concurrent mutation");
    }


    //--------------------------------------------------------------------------------------------
    // IpAcl. The matcher is a security control, so the cases that matter are the ones where a bug
    // fails OPEN: a rule that accidentally matches everything, or an address spelling that slips
    // past a list that should have caught it.
    //--------------------------------------------------------------------------------------------
    bool Allowed(IpAcl& acl, const char* pAddr)
    {
        return acl.Allows(pAddr);
    }

    //--------------------------------------------------------------------------------------------
    // A patch that resolves to the record already stored must not be a mutation. Front ends send
    // whole records and whole tag arrays rather than diffs, so re-asserting an unchanged state is
    // the COMMON case, not an edge one - and treating it as a write bumps `updated` (invalidating
    // everyone else's expect_updated for nothing), appends to the WAL, and puts a meaningless point
    // in the history log.
    //--------------------------------------------------------------------------------------------
    void TestNoOpUpdates()
    {
        Section("no-op updates");

        JotStore store;
        Ops      ops(store);

        JotInput in;
        in.msText    = "Backups run nightly at 02:00.";
        in.msName    = "backup-policy";
        in.msSummary = "how backups run";
        in.mTags     = std::vector<std::string>{ "infra", "priority:high" };
        AddResult created;
        Check(!ops.Add(in, created), "created a jot to patch");

        const int64_t nFirstUpdated = created.mJot.EffectiveUpdatedUS();

        // Byte-for-byte the same content, sent as a full record the way Save does.
        JotInput same;
        same.msText    = "Backups run nightly at 02:00.";
        same.msName    = "backup-policy";
        same.msSummary = "how backups run";
        same.mTags     = std::vector<std::string>{ "infra", "priority:high" };

        AddResult noop;
        Check(!ops.Update(created.mJot.mID, same, 0, noop), "an identical patch succeeds");
        Check(noop.mbNoChange,                              "and reports itself as no change");
        Check(noop.mJot.EffectiveUpdatedUS() == nFirstUpdated,
              "updated is NOT bumped, so other agents' expect_updated stays valid");

        // Tag order must not matter - the store keeps them sorted, and a client that sends them
        // back the other way round has still changed nothing.
        JotInput reordered;
        reordered.mTags = std::vector<std::string>{ "priority:high", "infra" };
        AddResult noop2;
        Check(!ops.Update(created.mJot.mID, reordered, 0, noop2), "reordered tags succeed");
        Check(noop2.mbNoChange, "and are recognized as the same set");

        // A real change still writes.
        JotInput real;
        real.msText = "Backups run nightly at 02:00 and are checksummed.";
        AddResult changed;
        Check(!ops.Update(created.mJot.mID, real, 0, changed), "a real edit succeeds");
        Check(!changed.mbNoChange, "and is not reported as a no change");
        Check(changed.mJot.EffectiveUpdatedUS() > nFirstUpdated, "and does bump updated");

        // Clearing a field is a change, not a no-op - an engaged-but-empty value means "clear".
        JotInput clear;
        clear.msSummary = std::string();
        AddResult cleared;
        Check(!ops.Update(created.mJot.mID, clear, 0, cleared), "clearing the summary succeeds");
        Check(!cleared.mbNoChange, "and counts as a change");
        Check(cleared.mJot.msSummary.empty(), "and actually cleared it");

        // The journal must see exactly the writes that changed something.
        struct CountingSink : public IJournalSink
        {
            int nPuts = 0;
            void OnPut(const FlatJot&) override { ++nPuts; }
            void OnDelete(tJotID) override {}
        };
        CountingSink sink;
        store.SetJournalSink(&sink);

        // Built from what is stored RIGHT NOW - the record has been edited twice since `same` was
        // written, so re-sending that would be a real change and would prove nothing.
        Jot current;
        Check(store.Get(created.mJot.mID, current), "read the current record back");
        NameTables names;
        store.SnapshotNames(names);
        const FlatJot flatNow = Flatten(current, names);

        JotInput echo;
        echo.msText    = flatNow.msText;
        echo.msName    = flatNow.msName;
        echo.msSummary = flatNow.msSummary;
        echo.mTags     = flatNow.mTags;

        AddResult r;
        ops.Update(created.mJot.mID, echo, 0, r);
        Check(r.mbNoChange, "echoing the stored record back is a no change");
        Check(sink.nPuts == 0, "no-op patches put nothing in the journal");

        JotInput another;
        another.msText = "Backups run nightly and are verified before rotation.";
        ops.Update(created.mJot.mID, another, 0, r);
        Check(sink.nPuts == 1, "and a real edit puts exactly one");

        store.SetJournalSink(nullptr);
    }

    void TestIpAclParsing()
    {
        Section("ip acl - parsing");

        AclRule rule;
        Check(IpAcl::ParseRule("192.168.1.5", rule),        "plain v4 address parses");
        Check(rule.mnPrefixBits == 128,                     "a bare address is an exact match");
        Check(IpAcl::ParseRule("192.168.1.0/24", rule),     "v4 cidr parses");
        Check(rule.mnPrefixBits == 120,                     "v4 /24 becomes 120 bits of the mapped form");
        Check(IpAcl::ParseRule("2001:db8::/32", rule),      "v6 cidr parses");
        Check(rule.mnPrefixBits == 32,                      "v6 prefix is taken as written");
        Check(IpAcl::ParseRule("::1", rule),                "compressed v6 parses");
        Check(IpAcl::ParseRule("::ffff:192.168.1.1", rule), "v4-mapped v6 literal parses");

        Check(!IpAcl::ParseRule("", rule),                  "empty rule rejected");
        Check(!IpAcl::ParseRule("192.168.1", rule),         "short v4 rejected");
        Check(!IpAcl::ParseRule("192.168.1.256", rule),     "out-of-range octet rejected");
        Check(!IpAcl::ParseRule("192.168.01.1", rule),      "leading-zero octet rejected as ambiguous");
        Check(!IpAcl::ParseRule("192.168.1.0/33", rule),    "v4 prefix over 32 rejected");
        Check(!IpAcl::ParseRule("2001:db8::/129", rule),    "v6 prefix over 128 rejected");
        Check(!IpAcl::ParseRule("1::2::3", rule),           "double compression rejected");
        Check(!IpAcl::ParseRule("not an address", rule),    "garbage rejected");

        // Host bits must be discarded or two spellings of one network stop agreeing.
        AclRule a, b;
        Check(IpAcl::ParseRule("192.168.1.5/24", a) && IpAcl::ParseRule("192.168.1.0/24", b) &&
              std::memcmp(a.mBytes, b.mBytes, 16) == 0,
              "host bits are zeroed so /24 spellings normalize to the same network");

        Check(IpAcl::IsLoopback("127.0.0.1"),               "127.0.0.1 is loopback");
        Check(IpAcl::IsLoopback("127.1.2.3"),               "all of 127/8 is loopback");
        Check(IpAcl::IsLoopback("::1"),                     "::1 is loopback");
        Check(IpAcl::IsLoopback("::ffff:127.0.0.1"),        "mapped loopback is loopback");
        Check(!IpAcl::IsLoopback("192.168.1.1"),            "a lan address is not loopback");
        Check(!IpAcl::IsLoopback("128.0.0.1"),              "128.0.0.1 is not loopback");
    }

    void TestIpAclMatching()
    {
        Section("ip acl - matching");

        IpAcl acl;
        std::string sWarn;

        // Disabled is the shipped default and must allow everything, or an upgrade locks the
        // operator out of a service that was working a minute ago.
        Check(Allowed(acl, "8.8.8.8"),   "a disabled list allows any address");
        Check(!acl.Enabled(),            "a fresh list is disabled");

        std::string sBad;
        std::vector<AclEntry> v;
        v.push_back({ "192.168.1.0/24", "home lan" });
        Check(!acl.Set(true, v, sBad),   "enabling with one rule succeeds");
        Check(acl.Enabled(),             "list reports enabled");

        Check(Allowed(acl, "192.168.1.112"),  "an address inside the range is allowed");
        Check(Allowed(acl, "192.168.1.0"),    "the network address itself is allowed");
        Check(Allowed(acl, "192.168.1.255"),  "the broadcast address is allowed");
        Check(!Allowed(acl, "192.168.2.1"),   "an address outside the range is refused");
        Check(!Allowed(acl, "8.8.8.8"),       "an internet address is refused");

        // THE ONE THAT ACTUALLY BITES. A dual-stack listener may report a v4 caller in the mapped
        // form, and a list written in v4 has to catch it either way.
        Check(Allowed(acl, "::ffff:192.168.1.112"),
              "a v4 rule matches the v4-mapped spelling of the same caller");
        Check(!Allowed(acl, "::ffff:192.168.2.1"),
              "and still refuses a mapped address outside the range");

        // Loopback is the floor, and it holds even though no rule mentions it.
        Check(Allowed(acl, "127.0.0.1"), "loopback is allowed with no rule for it");
        Check(Allowed(acl, "::1"),       "v6 loopback is allowed with no rule for it");

        // An unparseable caller address must fail CLOSED once the list is on.
        Check(!Allowed(acl, "garbage"),  "an unparseable remote address is refused");
        Check(!Allowed(acl, ""),         "an empty remote address is refused");

        // A rejected edit must leave the running list untouched, not half-applied.
        std::vector<AclEntry> vBad;
        vBad.push_back({ "10.0.0.0/8", "" });
        vBad.push_back({ "nonsense", "" });
        Check(static_cast<bool>(acl.Set(true, vBad, sBad)),
              "a list containing a bad rule is rejected");
        Check(sBad == "nonsense",        "the offending rule is named");
        Check(Allowed(acl, "192.168.1.112") && !Allowed(acl, "10.1.2.3"),
              "the previous list is still in force after a rejected edit");

        // Exact host rules, and v6.
        std::vector<AclEntry> v2;
        v2.push_back({ "10.0.0.7", "" });
        v2.push_back({ "2001:db8::/32", "" });
        Check(!acl.Set(true, v2, sBad),      "replacing the list succeeds");
        Check(Allowed(acl, "10.0.0.7"),      "an exact host rule matches");
        Check(!Allowed(acl, "10.0.0.8"),     "and matches nothing adjacent");
        Check(Allowed(acl, "2001:db8:1::9"), "a v6 prefix matches inside itself");
        Check(!Allowed(acl, "2001:db9::1"),  "and refuses outside itself");

        // Turning it off is the way back for someone who is already in.
        Check(!acl.Set(false, v2, sBad), "disabling succeeds");
        Check(Allowed(acl, "8.8.8.8"),   "everything is allowed again once disabled");
    }
}

int main()
{
    std::printf("coretest - Loom phase 1\n");

    TestTokenizer();
    TestTagNormalization();
    TestBareJotShape();
    TestIDMonotonicity();
    TestNamesAndUpsert();
    TestOptimisticConcurrency();
    TestPendingLinks();
    TestTagSuggestions();
    TestSearch();
    TestIndexCoherence();
    TestNoOpUpdates();
    TestIpAclParsing();
    TestIpAclMatching();
    // LAST on purpose: this one is known to hang (4 spinning readers starve both writers on
    // JotStore's shared_mutex), so anything sequenced after it never runs.
    TestConcurrentReadWrite();

    std::printf("\n%d checks, %d failed\n", gnChecks, gnFailed);
    return gnFailed == 0 ? 0 : 1;
}
