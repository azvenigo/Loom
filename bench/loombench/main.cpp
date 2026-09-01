// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
//////////////////////////////////////////////////////////////////////////////////////////////////
// loombench - what the store can actually do, with no socket in the way.
//
// This is the in-process half of the benchmark plan: it links JotStore directly and measures the
// ceiling. The over-HTTP half arrives with the Crow layer, and the interesting number is the
// DIFFERENCE between the two - that is what says whether the transport is worth what it costs.
// Reporting only the HTTP figure would tell us the framework's throughput and nothing about ours.
//
// The corpus is generated from a seeded Zipf term distribution rather than random words, because
// uniform-random text produces a posting-list shape no real corpus has: every term equally rare,
// every intersection tiny, and a search path that looks far faster than it will ever be in
// practice. Zipf gives the handful of brutally common terms that actually stress scoring.
//
// Same seed produces the same corpus and the same operation mix on every run, so two builds are
// comparable.
//
// Arg parsing here is deliberately minimal. It swaps to CLP::CLI_Parser when the ZLibraries
// submodule lands in phase 3 - no point pulling in the dependency for four integers.
//////////////////////////////////////////////////////////////////////////////////////////////////

#include "core/JotStore.h"
#include "core/LoomTime.h"
#include "core/Ops.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace
{
    //--------------------------------------------------------------------------------------------
    // Deterministic RNG. xorshift64* - fast enough that the generator is not what we are timing.
    //--------------------------------------------------------------------------------------------
    struct Rng
    {
        uint64_t mnState;

        explicit Rng(uint64_t nSeed) : mnState(nSeed ? nSeed : 0x9E3779B97F4A7C15ull) {}

        uint64_t Next()
        {
            mnState ^= mnState >> 12;
            mnState ^= mnState << 25;
            mnState ^= mnState >> 27;
            return mnState * 0x2545F4914F6CDD1Dull;
        }

        uint32_t Below(uint32_t nBound) { return nBound ? static_cast<uint32_t>(Next() % nBound) : 0; }
        double   Unit() { return static_cast<double>(Next() >> 11) * (1.0 / 9007199254740992.0); }
    };

    //--------------------------------------------------------------------------------------------
    // Zipf sampler over a fixed vocabulary. The CDF is precomputed once; sampling is a binary
    // search, so the distribution costs about the same as a uniform draw.
    //--------------------------------------------------------------------------------------------
    class Zipf
    {
    public:
        Zipf(size_t nVocab, double fExponent)
        {
            mCDF.resize(nVocab);
            double fSum = 0.0;
            for (size_t i = 0; i < nVocab; ++i)
            {
                fSum += 1.0 / std::pow(static_cast<double>(i + 1), fExponent);
                mCDF[i] = fSum;
            }
            for (double& v : mCDF)
                v /= fSum;
        }

        size_t Sample(Rng& rng) const
        {
            const double f = rng.Unit();
            const auto it = std::lower_bound(mCDF.begin(), mCDF.end(), f);
            return static_cast<size_t>(std::min<ptrdiff_t>(it - mCDF.begin(),
                                                           static_cast<ptrdiff_t>(mCDF.size()) - 1));
        }

    private:
        std::vector<double> mCDF;
    };

    struct Corpus
    {
        std::vector<std::string> mTerms;
        std::vector<std::string> mTags;
        std::vector<std::string> mEditors{ "user", "claude", "codex" };
    };

    Corpus BuildVocabulary(size_t nTerms, size_t nTags)
    {
        Corpus c;
        c.mTerms.reserve(nTerms);
        for (size_t i = 0; i < nTerms; ++i)
            c.mTerms.push_back("t" + std::to_string(i) + "word");

        c.mTags.reserve(nTags);
        for (size_t i = 0; i < nTags; ++i)
            c.mTags.push_back("tag-" + std::to_string(i));

        return c;
    }

    std::string MakeText(Rng& rng, const Corpus& corpus, const Zipf& zipf, size_t nWords)
    {
        std::string s;
        s.reserve(nWords * 8);
        for (size_t i = 0; i < nWords; ++i)
        {
            if (i)
                s.push_back(' ');
            s += corpus.mTerms[zipf.Sample(rng)];
        }
        return s;
    }

    JotInput MakeJot(Rng& rng, const Corpus& corpus, const Zipf& zipfTerms, const Zipf& zipfTags,
                     size_t nIndex)
    {
        JotInput in;
        in.msText = MakeText(rng, corpus, zipfTerms, 10 + rng.Below(50));

        // A quarter of the corpus carries a summary and a slug, mirroring the memory-store shape
        // where those two fields exist and the plain-jot shape where they do not.
        if (rng.Below(4) == 0)
        {
            in.msSummary = MakeText(rng, corpus, zipfTerms, 6 + rng.Below(10));
            in.msName    = "memory-" + std::to_string(nIndex);
        }

        const size_t nTags = rng.Below(5);   // 0-4, most jots with few or none
        if (nTags)
        {
            std::vector<std::string> vTags;
            for (size_t i = 0; i < nTags; ++i)
                vTags.push_back(corpus.mTags[zipfTags.Sample(rng)]);
            in.mTags = std::move(vTags);
        }

        in.msEditor = corpus.mEditors[rng.Below(static_cast<uint32_t>(corpus.mEditors.size()))];
        return in;
    }

    //--------------------------------------------------------------------------------------------
    // Latency collection. Raw nanosecond samples per thread, merged at the end - no locking and no
    // atomics on the measured path, since the instrument must not be what we measure.
    //--------------------------------------------------------------------------------------------
    struct Samples
    {
        std::vector<uint32_t> mQueryNS;
        std::vector<uint32_t> mWriteNS;
    };

    double Percentile(std::vector<uint32_t>& v, double f)
    {
        if (v.empty())
            return 0.0;
        const size_t n = std::min(v.size() - 1,
                                  static_cast<size_t>(f * static_cast<double>(v.size())));
        std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(n), v.end());
        return static_cast<double>(v[n]) / 1000.0;   // microseconds
    }

    void ReportLatency(const char* pLabel, std::vector<uint32_t>& v)
    {
        if (v.empty())
        {
            std::printf("  %-8s  (none)\n", pLabel);
            return;
        }
        const double p50 = Percentile(v, 0.50);
        const double p95 = Percentile(v, 0.95);
        const double p99 = Percentile(v, 0.99);
        const auto   max = *std::max_element(v.begin(), v.end());
        std::printf("  %-8s  n=%-9zu  p50=%8.2fus  p95=%8.2fus  p99=%8.2fus  max=%8.2fus\n",
                    pLabel, v.size(), p50, p95, p99, static_cast<double>(max) / 1000.0);
    }

    size_t ArgValue(int argc, char** argv, const char* pName, size_t nDefault)
    {
        const size_t nLen = std::strlen(pName);
        for (int i = 1; i < argc; ++i)
        {
            if (std::strncmp(argv[i], pName, nLen) == 0 && argv[i][nLen] == '=')
                return static_cast<size_t>(std::strtoull(argv[i] + nLen + 1, nullptr, 10));
        }
        return nDefault;
    }
}


int main(int argc, char** argv)
{
    const size_t nCorpus   = ArgValue(argc, argv, "--corpus",  100000);
    const size_t nThreads  = ArgValue(argc, argv, "--threads",
                                      std::max(1u, std::thread::hardware_concurrency()));
    const size_t nOps      = ArgValue(argc, argv, "--ops",     200000);
    const size_t nVocab    = ArgValue(argc, argv, "--vocab",   50000);
    const size_t nTagVocab = ArgValue(argc, argv, "--tags",    150);
    const size_t nSeed     = ArgValue(argc, argv, "--seed",    20260831);

    // Percent of operations that are queries; the rest split 4:1 add:update, matching the plan's
    // 90/8/2 default.
    const size_t nQueryPct = ArgValue(argc, argv, "--query-pct", 90);

    std::printf("loombench - in-process store ceiling\n");
    std::printf("  corpus=%zu  vocab=%zu  tags=%zu  threads=%zu  ops=%zu  query=%zu%%  seed=%zu\n\n",
                nCorpus, nVocab, nTagVocab, nThreads, nOps, nQueryPct, nSeed);

    const Corpus corpus = BuildVocabulary(nVocab, nTagVocab);
    const Zipf   zipfTerms(nVocab, 1.07);
    const Zipf   zipfTags(nTagVocab, 1.20);

    JotStore store;
    Ops      ops(store);

    //------------------------------------------------------------------------------------------
    // Load phase - also the "how fast can we ingest" number.
    //------------------------------------------------------------------------------------------
    {
        Rng rng(nSeed);
        const auto tStart = std::chrono::steady_clock::now();

        for (size_t i = 0; i < nCorpus; ++i)
        {
            AddResult result;
            const JotInput in = MakeJot(rng, corpus, zipfTerms, zipfTags, i);
            if (std::error_code ec = ops.Add(in, result))
            {
                std::printf("load failed at %zu: %s\n", i, ec.message().c_str());
                return 1;
            }
        }

        const double fSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - tStart).count();

        const StoreStats stats = store.GetStats();
        std::printf("load:   %zu jots in %.2fs  =  %.0f adds/sec (single-threaded)\n",
                    nCorpus, fSeconds, static_cast<double>(nCorpus) / fSeconds);
        std::printf("        %zu distinct terms, %zu live tags, %zu named\n\n",
                    stats.mnTerms, stats.mnTags, stats.mnNamed);
    }

    //------------------------------------------------------------------------------------------
    // Mixed workload.
    //------------------------------------------------------------------------------------------
    std::vector<Samples>     vSamples(nThreads);
    std::vector<std::thread> vThreads;
    std::atomic<uint64_t>    nErrors{ 0 };
    std::atomic<bool>        bGo{ false };

    const size_t nOpsPerThread = std::max<size_t>(1, nOps / nThreads);

    for (size_t t = 0; t < nThreads; ++t)
    {
        vThreads.emplace_back([&, t]()
        {
            Rng rng(nSeed + 0x1000 * (t + 1));
            Samples& samples = vSamples[t];
            samples.mQueryNS.reserve(nOpsPerThread);
            samples.mWriteNS.reserve(nOpsPerThread / 4 + 1);

            // Spin until every thread is up, so the measured window is genuinely concurrent rather
            // than the first thread running alone while the last is still starting.
            while (!bGo.load(std::memory_order_acquire))
                std::this_thread::yield();

            for (size_t i = 0; i < nOpsPerThread; ++i)
            {
                const bool bQuery = (rng.Below(100) < nQueryPct);

                const auto tOp = std::chrono::steady_clock::now();

                if (bQuery)
                {
                    Ops::QuerySpec spec;
                    spec.msText = corpus.mTerms[zipfTerms.Sample(rng)];
                    if (rng.Below(3) == 0)
                        spec.msText += " " + corpus.mTerms[zipfTerms.Sample(rng)];
                    if (rng.Below(4) == 0)
                        spec.mTags.push_back(corpus.mTags[zipfTags.Sample(rng)]);
                    if (rng.Below(8) == 0)
                        spec.msSince = "30d";
                    spec.mnLimit = 20;

                    Query query;
                    SearchResultSet results;
                    if (ops.BuildQuery(spec, query) || ops.Search(query, results))
                        nErrors.fetch_add(1, std::memory_order_relaxed);
                }
                else if (rng.Below(5) != 0)
                {
                    AddResult result;
                    const JotInput in = MakeJot(rng, corpus, zipfTerms, zipfTags,
                                                nCorpus + t * nOpsPerThread + i);
                    if (ops.Add(in, result))
                        nErrors.fetch_add(1, std::memory_order_relaxed);
                }
                else
                {
                    // Update a jot from the loaded corpus. Ids are timestamps, so pick one that
                    // exists by querying rather than by guessing an id.
                    Query query;
                    Ops::QuerySpec spec;
                    spec.msText  = corpus.mTerms[zipfTerms.Sample(rng)];
                    spec.mnLimit = 1;

                    SearchResultSet results;
                    if (!ops.BuildQuery(spec, query) && !ops.Search(query, results)
                        && !results.mJots.empty())
                    {
                        JotInput patch;
                        patch.msText = MakeText(rng, corpus, zipfTerms, 10 + rng.Below(30));

                        AddResult result;
                        // Last-write-wins here: several bench threads deliberately target the same
                        // hot jots, and a 409 is the correct answer, not an error to count.
                        const std::error_code ec = ops.Update(results.mJots[0].mID, patch, 0, result);
                        if (ec && LoomErrorOf(ec) != eLoomErr::kNotFound)
                            nErrors.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                const auto nNS = std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - tOp).count();

                if (bQuery)
                    samples.mQueryNS.push_back(static_cast<uint32_t>(std::min<int64_t>(nNS, 0xFFFFFFFF)));
                else
                    samples.mWriteNS.push_back(static_cast<uint32_t>(std::min<int64_t>(nNS, 0xFFFFFFFF)));
            }
        });
    }

    const auto tStart = std::chrono::steady_clock::now();
    bGo.store(true, std::memory_order_release);

    for (std::thread& th : vThreads)
        th.join();

    const double fSeconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - tStart).count();

    //------------------------------------------------------------------------------------------
    // Report.
    //------------------------------------------------------------------------------------------
    std::vector<uint32_t> vAllQuery;
    std::vector<uint32_t> vAllWrite;
    for (Samples& s : vSamples)
    {
        vAllQuery.insert(vAllQuery.end(), s.mQueryNS.begin(), s.mQueryNS.end());
        vAllWrite.insert(vAllWrite.end(), s.mWriteNS.begin(), s.mWriteNS.end());
    }

    const size_t nTotal = vAllQuery.size() + vAllWrite.size();

    std::printf("mixed:  %zu ops on %zu threads in %.2fs\n", nTotal, nThreads, fSeconds);
    std::printf("        %.0f ops/sec total   (%.0f queries/sec, %.0f writes/sec)\n\n",
                static_cast<double>(nTotal) / fSeconds,
                static_cast<double>(vAllQuery.size()) / fSeconds,
                static_cast<double>(vAllWrite.size()) / fSeconds);

    ReportLatency("query", vAllQuery);
    ReportLatency("write", vAllWrite);

    const StoreStats stats = store.GetStats();
    std::printf("\nfinal:  %zu jots, %zu terms, %zu tags, %llu mutations\n",
                stats.mnJots, stats.mnTerms, stats.mnTags,
                static_cast<unsigned long long>(stats.mnMutations));

    const uint64_t nErr = nErrors.load();
    if (nErr)
    {
        std::printf("\n*** %llu operations returned an unexpected error ***\n",
                    static_cast<unsigned long long>(nErr));
        return 1;
    }

    return 0;
}
