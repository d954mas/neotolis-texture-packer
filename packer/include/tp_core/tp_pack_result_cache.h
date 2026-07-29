#ifndef TP_CORE_TP_PACK_RESULT_CACHE_H
#define TP_CORE_TP_PACK_RESULT_CACHE_H

/* Memory-only pack-result store keyed by pack_input_hash, with a COMPRESSED COLD
 * TIER (master spec §10.3-10.4, §59 items 21/25, decision 0004, packets F3-03
 * T4/T5, S18, S28).
 *
 * MODEL. Each completed Pack result is stored under its `pack_input_hash` behind
 * an opaque, already-refcounted OWNER the store must not dismantle (the session
 * Pack receipt, tp_session_job_result::_owner, which is what pins the Pack
 * arena). Exactly one entry is ACTIVE at a time: it is pinned, holds a usable
 * tp_result, and is exempt from the byte budget. Every other (INACTIVE) entry is
 * managed by a byte-budget LRU.
 *
 * RESIDENCY STATES. One entry moves through three states, and only the store
 * decides which:
 *
 *   HOT      -- the entry still holds the caller's pin. `result` is the caller's
 *               own tp_result and its pages point into the pinned Pack arena.
 *               Budget-accounted at `retained_bytes` (RAW RGBA8 page bytes).
 *               Every entry starts here.
 *
 *   COLD     -- the entry holds a store-owned deep COPY of the result geometry
 *               (arena metadata: atlas/sprite/page records, names, hulls) plus
 *               one raw-deflate blob per page, and the caller's pin has been
 *               RELEASED -- the raw pages are gone. `result` is the metadata
 *               copy: every field is valid EXCEPT `pages[i].rgba`, which is
 *               NULL. Budget-accounted at the COMPRESSED byte total PLUS the
 *               geometry copy: both are store-owned allocations the entry is
 *               holding, so the budget counts what the entry actually occupies.
 *
 *               The geometry is not a rounding error at this tier. For a
 *               2000-sprite 4096^2 atlas it is 303 KiB (~155 bytes per sprite) --
 *               0.5% of the 64 MiB of raw pages it replaces, but at the measured
 *               142x compression of real art roughly 40% of what the cold entry
 *               then occupies (303 KiB of geometry beside 460 KiB of blobs). And
 *               cold entries accumulate in COUNT without a practical bound: the
 *               budget is what bounds them, so metadata outside the budget is
 *               unbounded real memory. tp_pack_result_cache_stats::cold_meta_bytes
 *               still reports the split, but it is now a component of
 *               inactive_bytes rather than a number beside it.
 *
 *               HOT entries keep the older rule: they are accounted at the
 *               caller's `retained_bytes` (raw page bytes), so the names and
 *               geometry inside the caller's own pinned arena stay uncounted.
 *               That memory belongs to the caller's receipt, not to this store,
 *               and one entry of it is bounded by the active pin -- changing it
 *               would churn every caller's byte measure to restate memory the
 *               store does not own.
 *
 *   COLD+RESIDENT -- a COLD entry that has been promoted back to ACTIVE: its
 *               pages have been decompressed into fresh store-owned buffers, so
 *               `pages[i].rgba` is valid again. Only the active entry is ever in
 *               this state, so it is always budget-exempt; demoting it frees the
 *               decompressed pixels (the blobs are kept, never re-encoded).
 *
 * ENCODE IS BACKGROUND. Demoting a HOT entry does not compress it inline. The
 * store enqueues a compression job on ONE low-priority background thread and the
 * entry stays HOT -- budget-counted RAW -- until the caller PUMPS the store and
 * the finished job is applied. This is why the budget counts compressed bytes for
 * cold entries and raw bytes for not-yet-compressed inactive ones: the accounting
 * always describes the memory the store is actually holding right now.
 *
 * DECODE IS PARALLEL. Promoting a COLD entry decompresses its pages on up to
 * TP_PACK_RESULT_CACHE_DECODE_THREADS threads (pages are independent). Measured
 * through THIS store on real CC0 sprite art (tp_bench_pack_blob `coldtier` rows,
 * native-release): a 64 MiB single-page atlas promotes in 36 ms and a 256 MiB
 * four-page atlas in 46 ms -- the fork-join is what keeps 4x the pixels at ~1.3x
 * the latency. Background encode of the same entries costs 122 ms and 462 ms off
 * the UI thread.
 *
 * RATIO FLOOR. After a compression job finishes, the store computes
 * raw_bytes / compressed_bytes. Below 1.5 the entry is NOT kept cold: pages that
 * do not compress buy almost no memory back while still charging the full
 * parallel-decode latency to the next atlas switch, so such an entry is EVICTED
 * instead (its pin released, the blobs thrown away). The two entries the store
 * contract may never evict -- the ACTIVE pin and the highest-SEQUENCE entry --
 * are the one exception: they stay HOT, and the store does not try to compress
 * them again.
 *
 * THREAD OWNERSHIP CONTRACT (packet S28). The store is a SINGLE-THREADED object:
 * every function here must be called from the one thread that owns it (the UI
 * thread in the GUI). The background encoder never touches the store:
 *
 *   - A compression job is a private SNAPSHOT struct. The owning thread fills it
 *     with borrowed page pointers/sizes read out of the pinned Pack arena and
 *     hands it to the queue; the worker allocates its OWN arena and buffers for
 *     the metadata copy and the blobs and writes only into that struct.
 *   - The worker never reads or writes a cache entry, the entry table, the
 *     budget, or any session object, and never frees anything the owning thread
 *     owns.
 *   - The owning thread guarantees that while a job is in flight, the pinned
 *     arena its page pointers describe stays alive. Every operation that would
 *     end that guarantee -- eviction, forget, a re-store of the same hash,
 *     re-activation, destroy -- first CANCELS AND JOINS the job. Cancellation is
 *     checked between fixed-size deflate chunks, so a join costs at most one
 *     chunk, and the discarded job's own allocations are freed by the joiner.
 *   - The only lock in the design is the mutex/condvar pair INSIDE the job queue.
 *     No cache structure is ever locked.
 *
 * SELECTION (§10.3, decision 0004). The authoritative result is the entry with
 * the latest completion SEQUENCE, unless an explicit selection points at a
 * specific cached hash (Undo/Redo and manual selection choose by hash, never by
 * wall-clock). Selection is deterministic: two completions that arrive in either
 * order resolve to the same winner.
 *
 * CONTAINMENT. A cold entry whose decode fails (OOM, or a blob that no longer
 * inflates to its page) is dropped in place -- structured status, no crash, the
 * store stays usable and falls back to the next candidate.
 *
 * NON-GOALS. No disk persistence; the store lives only for the session. No GPU
 * textures or source-image copies are retained. Undo/Redo NEVER auto-pack (see
 * tp_pack_result_cache_contains). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_result;

typedef struct tp_pack_result_cache tp_pack_result_cache;

/* Deflate level for cold page blobs. Measured (tp_bench_pack_blob phase-2 sweep,
 * 2026-07-29): across levels 1/3/6 the DECODE cost -- the number an atlas switch
 * actually pays -- moves ~7% while the ratio moves 87.9x -> 114.1x on real art,
 * so the read path is nearly indifferent to the level and the highest useful one
 * wins. Level 6 also beat every stb PNG configuration on ratio, encode AND
 * decode, which is why the cold tier stores raw deflate and not a PNG container. */
#define TP_PACK_RESULT_CACHE_DEFLATE_LEVEL 6

/* raw/compressed below this and the entry is not worth keeping cold (see the
 * RATIO FLOOR paragraph above). The pessimistic floor measured on deliberately
 * incompressible synthetic noise is 1.24x, i.e. exactly the case this rejects;
 * real sprite art measured 114x. */
#define TP_PACK_RESULT_CACHE_MIN_RATIO 1.5

/* Upper bound on the fork-join width of one cold promote. Pages are independent,
 * so the promote scales with page count up to this cap. */
#define TP_PACK_RESULT_CACHE_DECODE_THREADS 8

/* `byte_budget` bounds the total retained bytes of INACTIVE entries (the pinned
 * active entry is never counted). 0 is legal: every demoted entry is evicted
 * immediately EXCEPT the highest-sequence entry, which stays resolvable no matter
 * the budget (store contract, decision 0004) -- so the active entry and the
 * max-sequence entry survive, and inactive_bytes may exceed the budget by at most
 * that one entry. Returns NULL on OOM.
 *
 * SIZING. An inactive entry costs its RAW RGBA8 page bytes until its background
 * compression lands, and its COMPRESSED bytes plus its geometry copy afterwards.
 * A budget therefore
 * bounds the steady state, not the transient: a store that has just demoted N
 * atlases holds them raw until the next pump. Size the budget against the
 * compressed steady state and expect the raw transient to be the real peak. */
tp_pack_result_cache *tp_pack_result_cache_create(uint64_t byte_budget);
/* Cancels and joins every in-flight compression, shuts the encoder thread down,
 * and releases every pin exactly once. */
void tp_pack_result_cache_destroy(tp_pack_result_cache *cache);

/* Stores a completed Pack result whose memory is owned by `pin_owner` -- an
 * opaque, already-refcounted receipt the store cannot adopt or take apart -- and
 * pins it ACTIVE.
 *
 * `result` is BORROWED while the entry is HOT: it must stay valid until
 * `pin_release(pin_owner)` runs. `pin_release` is required and is invoked EXACTLY
 * ONCE per successful store -- on eviction, on a re-store of the same hash, on
 * tp_pack_result_cache_forget, on destroy, or when the entry goes COLD and the
 * store no longer needs the caller's pages. On any error the caller keeps the pin
 * and must release it itself.
 *
 * `retained_bytes` is the caller's measure of the memory this pin holds (for a
 * Pack receipt: the raw RGBA8 page bytes). It is what the byte-budget LRU counts
 * while the entry is inactive AND still HOT; once the entry is COLD the store
 * counts its own allocations instead -- the compressed pages and the geometry
 * copy it made -- and this number stops mattering for that entry. `sequence` is the caller's MONOTONIC
 * completion order (the generalized slot->version): the session assigns it per
 * Pack REQUEST in strictly increasing order and hands it back at completion.
 * Selecting by this sequence -- not by store/wall-clock order -- is what lets an
 * earlier-requested job that finishes LATE (lower sequence, stored later) never
 * overwrite a newer preview (§10.3, decision 0004).
 *
 * The previously-active entry becomes inactive: it is queued for background
 * compression if it is HOT, or gives up its decompressed pixels immediately if it
 * is COLD+RESIDENT. Over-budget inactive entries are then evicted. */
tp_status tp_pack_result_cache_store_retained(
    tp_pack_result_cache *cache, tp_id128 hash, uint64_t sequence,
    const struct tp_result *result, uint64_t retained_bytes, void *pin_owner,
    void (*pin_release)(void *pin_owner), tp_error *err);

/* True iff an entry with `hash` is present, in ANY residency state (active, hot
 * inactive, or cold). This is the Undo/Redo cache probe: a hit means the previous
 * preview is available; a miss means keep the current preview and mark it stale
 * -- it never starts a Pack. A merely COLD entry is present, so a caller that
 * uses this to decide "is the result gone for good" gets the honest answer. */
bool tp_pack_result_cache_contains(const tp_pack_result_cache *cache,
                                   tp_id128 hash);

/* READ-ONLY lookup of the result `hash` holds. Nothing about residency moves: no
 * selection, no promotion to active, no demotion of whoever is active, no LRU
 * touch, no eviction pass, no decompression. It is the read for a caller that
 * must observe a second entry WITHOUT taking the one active pin away from the
 * entry that owns the presentation.
 *
 * NULL only when the hash is ABSENT. A present entry always answers with a
 * tp_result, in EVERY residency state -- which is what keeps "the atlas is merely
 * cold" distinguishable from "the atlas is gone" at a caller that only has this
 * read.
 *
 * GEOMETRY ONLY. A peek promises the geometry, not the pixels: for a COLD entry
 * `pages[i].rgba` is NULL (the pixels are the compressed blobs, and inflating
 * them is a residency decision this read is defined not to make). A caller that
 * needs pixels calls tp_pack_result_cache_authoritative, which promotes.
 *
 * LIFETIME. The pointer is BORROWED and only as stable as the entry behind it:
 * any later store_retained, forget, authoritative, pump, settle, or destroy on
 * this cache may release that entry's pin, retire its metadata, and free the
 * result. Use it inside the operation that asked for it; never retain it across
 * one. (A pump is exactly what turns a HOT entry COLD, which frees the caller's
 * result -- so a peeked pointer does not survive a pump either.) */
const struct tp_result *tp_pack_result_cache_peek(
    const tp_pack_result_cache *cache, tp_id128 hash);

/* Drops `hash` if present -- cancelling and joining any in-flight compression,
 * releasing its pin exactly once, freeing any cold blobs -- and clears an
 * explicit selection that named it. Used when the thing a cached result describes
 * is gone for good (the atlas was deleted, or its presentation slot was cleared).
 * Absent hash: no-op. */
void tp_pack_result_cache_forget(tp_pack_result_cache *cache, tp_id128 hash);

/* Explicit selection by hash (decision 0004). If `hash` is present, subsequent
 * authoritative resolution returns it; a nil hash or an absent hash clears the
 * selection and authoritative reverts to the latest completion sequence. */
void tp_pack_result_cache_select(tp_pack_result_cache *cache, tp_id128 hash);

/* Resolves the authoritative result: the explicit selection if present, else the
 * latest completion sequence. Ensures the winner is pinned ACTIVE -- promoting it
 * from COLD (parallel decode into fresh page buffers) if needed -- and returns a
 * borrowed tp_result with valid page pixels (valid until the next residency
 * change), its hash, and its completion sequence (each out param may be NULL).
 *
 * A cold entry whose decode fails is dropped (contained) and skipped in favour of
 * the next candidate. TP_STATUS_NOT_FOUND if the store holds no usable entry. */
tp_status tp_pack_result_cache_authoritative(tp_pack_result_cache *cache,
                                             tp_id128 *out_hash,
                                             const struct tp_result **out_result,
                                             uint64_t *out_sequence,
                                             tp_error *err);

/* Applies every compression job that has FINISHED since the last call (a mutex
 * lock and a flag test per in-flight job -- nothing else), then runs the budget
 * sweep. Call once per frame from the owning thread.
 *
 * This is the ONLY place a HOT inactive entry becomes COLD, so a caller that
 * never pumps is still correct: it simply keeps every inactive result raw and
 * budget-accounted raw. Nothing here can touch the ACTIVE entry. */
void tp_pack_result_cache_pump(tp_pack_result_cache *cache);

/* Blocks until every in-flight compression has finished, then pumps. This is the
 * store's quiescence barrier: after it returns, no background work is
 * outstanding and the residency/budget numbers are final for the current entry
 * set. Tests use it to make the cold transition observable; production code needs
 * it only where a deterministic settled state matters, because destroy already
 * cancels and joins on its own. */
void tp_pack_result_cache_settle(tp_pack_result_cache *cache);

typedef struct tp_pack_result_cache_stats {
    int entry_count;          /* active + inactive entries currently held */
    bool has_active;          /* an entry is pinned active */
    tp_id128 active_hash;     /* meaningful only when has_active */
    uint64_t inactive_bytes;  /* EXACT accounted bytes of inactive entries: raw
                               * pages for HOT ones, cold_coded + cold_meta for
                               * COLD ones */
    uint64_t byte_budget;     /* configured inactive budget */
    uint64_t last_sequence;   /* highest completion sequence stored so far */
    uint64_t evicted;         /* inactive entries removed by the LRU */
    uint64_t dropped_corrupt; /* entries dropped because a cold decode failed */
    /* --- cold tier --- */
    int cold_entries;         /* entries currently holding compressed pages */
    int encoding;             /* compression jobs in flight right now */
    uint64_t cold_coded_bytes;/* compressed bytes those cold entries hold (in
                               * inactive_bytes when the entry is inactive) */
    uint64_t cold_raw_bytes;  /* raw page bytes those blobs stand for (NOT held) */
    uint64_t cold_meta_bytes; /* uncompressed geometry/metadata they also hold --
                               * budgeted alongside cold_coded_bytes */
    uint64_t encoded;         /* entries that reached COLD over this store's life */
    uint64_t encode_discarded;/* jobs cancelled by evict/forget/re-activate/destroy */
    uint64_t ratio_floor_evicted; /* entries dropped for failing the ratio floor */
    uint64_t promoted;        /* cold entries decompressed back to active */
} tp_pack_result_cache_stats;

void tp_pack_result_cache_stats_get(const tp_pack_result_cache *cache,
                                    tp_pack_result_cache_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_RESULT_CACHE_H */
