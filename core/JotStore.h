#pragma once
// Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.

#include "FairSharedMutex.h"
#include "Interner.h"
#include "FlatJot.h"
#include "Jot.h"
#include "LoomError.h"
#include "Query.h"
#include "TagRegistry.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

//////////////////////////////////////////////////////////////////////////////////////////////////
// JotStore - every jot in RAM, plus the indexes that make queries microseconds instead of a scan.
//
// CONCURRENCY: one FairSharedMutex over the whole store. Readers take a shared_lock, writers a
// unique_lock. That is a deliberate choice, not a placeholder:
//
//   - A query is single-digit microseconds and writers are rare, so a single reader-writer lock is
//     nowhere near the bottleneck at the scale this serves (a handful of agents, one hotkey app).
//   - Every index stays trivially coherent. There is no window where the term index knows about a
//     jot the tag index does not.
//   - Sharding is NOT the obvious next step it looks like. A term's posting list spans every
//     shard, so the text index cannot be partitioned by jot id without turning one lookup into N.
//
//   It is FairSharedMutex rather than std::shared_mutex because the plain one lets a busy enough
//   reader stream starve writes outright on glibc - see that header. Rare writers make the cost of
//   fairness nil and the cost of missing it unbounded.
//
//   If the benchmark ever shows real contention, the documented escape hatch is copy-on-write:
//   writers build a new immutable index and publish it through an atomic shared_ptr so readers
//   never block at all. Measure before building that.
//
// ALL INDEXES ARE DERIVED. Nothing here is persisted. The snapshot holds records; everything else
// is rebuilt on load. That keeps the on-disk format free to evolve without a migration, and makes
// a corrupt index a restart rather than an outage.
//
// The store is the primitive layer: it does exactly what it is told and enforces invariants. It
// does NOT decide policy - no upserts, no "create if missing", no warning text. That lives in Ops.
//////////////////////////////////////////////////////////////////////////////////////////////////

struct StoreConfig
{
    size_t mnMaxTextBytes    = 1u << 20;   // 1 MiB. A jot is a thought, not a file.
    size_t mnMaxSummaryBytes = 4096;
    size_t mnMaxNameBytes    = 200;
    size_t mnMaxTags         = 64;
    size_t mnMaxLinks        = 512;
};

struct StoreStats
{
    size_t  mnJots          = 0;
    size_t  mnNamed         = 0;
    size_t  mnTags          = 0;   // live, non-reserved
    size_t  mnTerms         = 0;   // distinct indexed terms
    size_t  mnPendingLinks  = 0;
    size_t  mnEditors       = 0;
    int64_t mnOldestUS      = 0;
    int64_t mnNewestUS      = 0;
    uint64_t mnMutations    = 0;   // since process start
};

// Durability counters, reported alongside StoreStats by /stats. Declared here rather than in
// persist/ so the codec can render it without the serializer depending on the journal.
struct PersistStats
{
    bool     mbEnabled        = false;
    size_t   mnQueued         = 0;   // lines waiting for the committer right now
    uint64_t mnAppended       = 0;   // lines written since start
    uint64_t mnSynced         = 0;   // fsyncs issued
    uint64_t mnWalBytes       = 0;
    uint64_t mnSnapshots      = 0;
    int64_t  mnLastSnapshotUS = 0;
};

//////////////////////////////////////////////////////////////////////////////////////////////////
// IJournalSink - where durable writes go.
//
// Called by JotStore WHILE THE WRITE LOCK IS HELD, and that is the entire point: it guarantees
// journal order equals apply order. If the store released the lock first and let the caller
// journal afterwards, two concurrent updates to the same jot could reach the log in the opposite
// order to the one they were applied in, and a replay would silently resurrect the losing write.
//
// The cost is that a sink runs inside the critical section, so an implementation must do the
// smallest possible thing - serialize and enqueue - and never touch the disk or the store here.
// It is handed a FlatJot precisely so it never needs to look a name up and deadlock on the lock
// that is already held.
//////////////////////////////////////////////////////////////////////////////////////////////////
class IJournalSink
{
public:
    virtual ~IJournalSink() = default;

    // A create or an update. Idempotent by design: the record is complete, so a replay applies the
    // last one written and needs no notion of which was an insert.
    virtual void OnPut(const FlatJot& jot) = 0;
    virtual void OnDelete(tJotID id) = 0;
};

// A jot holds interned tag and editor ids, so anything rendering one needs the tables to turn them
// back into strings. Taken as a single snapshot under one lock rather than resolved id-by-id: a
// fifty-result page would otherwise take a few hundred lock acquisitions to print, and the
// serializer would be coupled to the store for the whole of its work.
struct NameTables
{
    std::vector<std::string> mTags;      // indexed by tTagID
    std::vector<std::string> mEditors;   // indexed by tEditorID

    const std::string& Tag(tTagID id) const
    {
        static const std::string ksEmpty;
        return id < mTags.size() ? mTags[id] : ksEmpty;
    }

    const std::string& Editor(tEditorID id) const
    {
        static const std::string ksUser("user");
        return id < mEditors.size() ? mEditors[id] : ksUser;
    }
};

// Resolves a record for output. The store has its own locked version for the journal; this one is
// for callers that already hold a NameTables snapshot and are outside the lock entirely.
inline FlatJot Flatten(const Jot& jot, const NameTables& names)
{
    FlatJot f;
    f.mID           = jot.mID;
    f.mnUpdatedUS   = jot.mnUpdatedUS;
    f.msName        = jot.msName;
    f.msSummary     = jot.msSummary;
    f.msText        = jot.msText;
    f.mLinks        = jot.mLinks;
    f.mPendingLinks = jot.mPendingLinks;

    // The default editor stays empty rather than becoming the literal "user", so the omit-empty
    // rule in the codec has one thing to test instead of two.
    f.msEditor = (jot.mEditor == kDefaultEditor) ? std::string() : names.Editor(jot.mEditor);

    f.mTags.reserve(jot.mTags.size());
    for (tTagID id : jot.mTags)
        f.mTags.push_back(names.Tag(id));

    return f;
}

// What a mutation produced. Returned under the same lock that performed it, so the caller gets the
// authoritative record for the WAL and the response without a second acquisition (and without the
// race a second acquisition would open).
struct MutationResult
{
    Jot                        mJot;
    std::vector<TagSuggestion> mSuggestions;
    bool                       mbCreated = false;

    // True when the patch resolved to the record that was already there. Not an error - the caller
    // asked for a state and got it - but nothing was written: no `updated` bump, no WAL line, no
    // history entry. See JotStore::Update for why that matters.
    bool                       mbNoChange = false;
};


class JotStore
{
public:
    explicit JotStore(const StoreConfig& config = StoreConfig());

    JotStore(const JotStore&)            = delete;
    JotStore& operator=(const JotStore&) = delete;

    //----------------------------------------------------------------------------------------
    // Mutations. All take the write lock.
    //----------------------------------------------------------------------------------------

    // Creates a jot and allocates its id. Fails with kNameInUse if the slug is taken - it will not
    // silently update the existing one; that policy decision belongs to Ops.
    std::error_code Add(const JotInput& input, MutationResult& outResult);

    // Partial update. Fields left as nullopt are untouched; an engaged-but-empty value clears.
    // nExpectUpdatedUS is the optimistic-concurrency guard: pass the EffectiveUpdatedUS the caller
    // last saw and the update fails with kConflict if anything changed since. Pass 0 to skip the
    // check and take last-write-wins.
    std::error_code Update(tJotID id, const JotInput& patch, int64_t nExpectUpdatedUS,
                           MutationResult& outResult);

    std::error_code Remove(tJotID id);

    // Rewrites every jot carrying any tag in vFrom to carry sTo instead. Returns the number of
    // jots touched. Backs POST /tags/merge.
    std::error_code MergeTags(const std::vector<std::string>& vFrom, const std::string& sTo,
                              size_t& outJotsChanged);

    //----------------------------------------------------------------------------------------
    // Bulk load. Takes the write lock once for the whole batch.
    //
    // Trusts the records completely: ids, timestamps and editors are taken as given, nothing is
    // allocated, no suggestions are produced. This is the snapshot/WAL replay and importer path,
    // where re-deriving an id would corrupt the very thing being restored.
    //----------------------------------------------------------------------------------------
    std::error_code LoadBatch(std::vector<Jot>& vJots, size_t& outLoaded);

    // The replay and snapshot-load path. Interns tag and editor strings under the write lock,
    // which is why it cannot be a free conversion the caller does beforehand.
    // bJournal MUST be false for WAL replay and snapshot load - journalling records you just read
    // back would double the log on every restart. It MUST be true for an import, whose records are
    // new and exist nowhere on disk yet.
    std::error_code LoadFlatBatch(std::vector<FlatJot>& vFlat, size_t& outLoaded,
                                  bool bJournal = false);

    //----------------------------------------------------------------------------------------
    // Reads. All take the shared lock.
    //----------------------------------------------------------------------------------------

    bool Get(tJotID id, Jot& outJot) const;
    bool GetByName(const std::string& sName, Jot& outJot) const;
    void GetMany(const std::vector<tJotID>& vIDs, std::vector<Jot>& outJots) const;

    void Search(const Query& query, SearchResults& outResults) const;

    // Everything reachable from id within nDepth hops, following links in both directions.
    // Excludes id itself. Breadth-first, so nearer jots come first.
    void Neighborhood(tJotID id, size_t nDepth, std::vector<tJotID>& outIDs) const;

    void ListTags(std::vector<TagStat>& outStats, bool bIncludeReserved = false) const;
    void TagClusters(std::vector<TagCluster>& outClusters) const;

    // Near-duplicate warnings for a set of tags about to be written. Read-only, so Ops can preview
    // without mutating.
    void SuggestForTags(const std::vector<std::string>& vTags,
                        std::vector<TagSuggestion>& outSuggestions) const;

    // One-shot copy of the interned tag and editor tables, for rendering records.
    void SnapshotNames(NameTables& outTables) const;

    // Resolves one record's interned ids into strings. Takes the shared lock.
    bool Flatten(tJotID id, FlatJot& outFlat) const;

    // Every live record, flattened, oldest first. This is the snapshot writer's view - taken under
    // a single lock so the file is a coherent point in time rather than a smear across writes.
    void FlattenAll(std::vector<FlatJot>& outFlat) const;

    // Install the durable-write sink. Not thread-safe against concurrent mutations by design: call
    // it once during startup, before the server begins accepting requests.
    void SetJournalSink(IJournalSink* pSink) { mpJournal = pSink; }

    StoreStats GetStats() const;
    size_t     Size() const;

    // Full index rebuild from the records. Not needed in normal operation - LoadBatch already
    // indexes - but it is what the concurrency test compares against to prove the incremental
    // paths never drift.
    void RebuildIndexes();

private:
    //----------------------------------------------------------------------------------------
    // Everything below assumes the appropriate lock is already held. The naming convention is
    // strict: a method starting with Locked_ does NOT take the lock itself.
    //----------------------------------------------------------------------------------------

    // A posting is deliberately SELF-SUFFICIENT: it carries everything BM25 needs, so scoring is a
    // linear streaming scan with no random access. The first version stored only the id and looked
    // the jot up for its length - one hash probe into a large map per posting, i.e. a cache miss
    // per posting, which dominated query cost entirely. Trading 8 bytes for that is not close.
    struct Posting
    {
        tJotID   mID      = kInvalidJotID;
        uint32_t mnSlot   = 0;   // dense ordinal, indexes the flat score array
        uint32_t mnTF     = 0;   // term frequency in this field
        uint32_t mnDocLen = 0;   // this field's term count, for length normalization
    };
    using tPostings = std::vector<Posting>;

    tJotID Locked_NextID();

    // Index maintenance for one jot. These are exact inverses; an edit is Unindex then Index.
    // Index takes a non-const reference because it writes back the derived term-count fields.
    void Locked_IndexJot(Jot& jot);
    void Locked_UnindexJot(const Jot& jot);

    void Locked_IndexField(Jot& jot, const std::string& sText, eField field, uint32_t& outLen);
    void Locked_UnindexField(tJotID id, const std::string& sText, eField field);

    void Locked_AddPosting(tTermID term, eField field, const Posting& posting);
    void Locked_RemovePosting(tTermID term, eField field, tJotID id);

    void Locked_InsertTermLex(tTermID term);
    void Locked_ExpandPrefix(std::string_view sPrefix, std::vector<tTermID>& outTerms) const;

    // Resolves the caller's link strings into ids, parking unresolvable slugs as pending links.
    void Locked_ResolveLinks(const std::vector<std::string>& vSpecs, tJotID nSelfID,
                             std::vector<tJotID>& outLinks, std::vector<std::string>& outPending);

    // When a jot takes a name, any jot that was waiting on that slug gets its link promoted.
    void Locked_PromotePendingLinks(const std::string& sName, tJotID nTargetID);

    void Locked_AddBacklinks(tJotID nFrom, const std::vector<tJotID>& vTo);
    void Locked_RemoveBacklinks(tJotID nFrom, const std::vector<tJotID>& vTo);

    void Locked_Flatten(const Jot& jot, FlatJot& outFlat) const;
    void Locked_JournalPut(const Jot& jot);

    // Content equality over the fields a patch can touch - everything except id, updated, slot and
    // the derived index lengths. An exact comparison rather than a hash: "these are the same
    // record" has to be exactly right, and the strings are already in cache from applying them.
    static bool Locked_SameContent(const Jot& a, const Jot& b);
    std::error_code Locked_LoadBatch(std::vector<Jot>& vJots, size_t& outLoaded, bool bJournal);

    std::error_code Locked_Validate(const JotInput& input) const;

    // Applies input onto jot, doing all interning and index bookkeeping. bCreating suppresses the
    // unindex half, since there is nothing to unindex yet.
    std::error_code Locked_Apply(Jot& jot, const JotInput& input, bool bCreating,
                                 std::vector<TagSuggestion>& outSuggestions);

    // --- search internals ---
    bool Locked_BuildCandidates(const Query& q, std::vector<tJotID>& outCandidates,
                                bool& outbUnbounded) const;
    // Accumulates into a flat per-slot array and returns the touched slots, rather than filling a
    // hash map keyed by jot id. Same reason as the Posting layout above: one hash insert per
    // posting was a large share of query time, and postings vastly outnumber results.
    void Locked_ScoreText(const Query& q, const std::vector<tJotID>& vCandidates,
                          bool bUnbounded, std::vector<SearchHit>& outHits) const;
    float Locked_RecencyMultiplier(tJotID id, int64_t nNowUS) const;

    //----------------------------------------------------------------------------------------

    mutable FairSharedMutex mLock;

    IJournalSink*             mpJournal = nullptr;

    StoreConfig               mConfig;
    std::atomic<int64_t>      mnLastID{ 0 };
    uint64_t                  mnMutations = 0;

    std::unordered_map<tJotID, Jot>        mJots;
    std::unordered_map<std::string, tJotID> mNameIndex;

    // Live ids in ascending order. Because ids are allocated monotonically this stays sorted by
    // appending, which is what makes a date range two binary searches and no separate index.
    std::vector<tJotID>                    mChrono;

    // Dense slot space. A slot is a small integer handle for a live jot, stable for that record's
    // lifetime and recycled after a delete. It exists so scoring can index a flat array instead of
    // hashing an int64 id for every posting it touches.
    std::vector<tJotID>                    mSlotToID;    // slot -> id, kInvalidJotID when free
    std::vector<uint32_t>                  mFreeSlots;

    uint32_t Locked_AllocSlot(tJotID id);
    void     Locked_FreeSlot(uint32_t nSlot);

    TagRegistry                            mTags;
    Interner                               mEditors;
    Interner                               mTerms;

    // mPostings[field][termID]. Dense vectors indexed by term id: term ids are handed out densely
    // so this is a direct index, no hashing on the hot path. Each list stays sorted by jot id,
    // which makes intersection a linear merge and removal a binary search.
    std::vector<tPostings>                 mPostings[static_cast<size_t>(eField::kCount)];

    // Term ids ordered lexicographically by their string. Backs prefix expansion for the
    // live-as-you-type path. Kept sorted by insertion (lower_bound + insert): O(n) memmove on a
    // new term only, and new distinct terms become rare very quickly as a corpus grows.
    std::vector<tTermID>                   mTermsByLex;

    std::vector<std::vector<tJotID>>       mTagPostings;      // by tTagID, sorted
    std::vector<std::vector<tJotID>>       mEditorPostings;   // by tEditorID, sorted

    std::unordered_map<tJotID, std::vector<tJotID>>      mBacklinks;
    std::unordered_map<std::string, std::vector<tJotID>> mPendingLinks;   // slug -> waiting jots

    // BM25 length normalization needs the mean document length per field.
    uint64_t mnTotalLen[static_cast<size_t>(eField::kCount)] = { 0, 0 };
};
