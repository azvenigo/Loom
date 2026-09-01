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

#include "core/JotStore.h"
#include "core/LoomTime.h"
#include "core/Ops.h"
#include "core/TagRegistry.h"
#include "core/Tokenizer.h"

#include <algorithm>
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
    TestConcurrentReadWrite();

    std::printf("\n%d checks, %d failed\n", gnChecks, gnFailed);
    return gnFailed == 0 ? 0 : 1;
}
