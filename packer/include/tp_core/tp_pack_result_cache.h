#ifndef TP_CORE_TP_PACK_RESULT_CACHE_H
#define TP_CORE_TP_PACK_RESULT_CACHE_H

/* Memory-only pack-result store keyed by pack_input_hash (master spec §10.3-10.4,
 * §59 items 21/25, decision 0004, packet F3-03 T4/T5).
 *
 * MODEL. Each completed Pack result is stored under its `pack_input_hash`. The
 * store keeps every result as its serialized .ntpack artifact bytes (the "normal
 * serialized runtime artifact", §10.4) plus the small name list needed to inflate
 * it. Exactly one entry is ACTIVE at a time: it is pinned, holds a decompressed
 * tp_result (arena-owned), and is exempt from the byte budget. Every other
 * (INACTIVE) entry holds only its retained bytes and is managed by a byte-budget
 * LRU. A hit re-inflates the retained bytes through tp_pack_read_memory.
 *
 * ENTRY FLAVOURS. An entry is stored one of two ways, and the two mix freely in
 * one store because the difference is purely how ONE entry owns its memory:
 *
 *   SERIALIZED (tp_pack_result_cache_store) -- the caller owns the .ntpack bytes
 *   and the result arena. Demotion frees the decompressed arena and keeps only
 *   the bytes; a later hit re-inflates them. This is the §10.4 shape and the
 *   cheapest inactive footprint.
 *
 *   RETAINED-PIN (tp_pack_result_cache_store_retained) -- the result lives behind
 *   an opaque, already-refcounted OWNER the store must not dismantle (the session
 *   Pack receipt, tp_session_job_result::_owner, which is what pins the Pack
 *   arena). The store never touches that arena directly: it releases the owner
 *   through the caller's release hook, exactly once, on eviction/refresh/destroy.
 *   A retained entry keeps its decompressed tp_result for its whole life, so
 *   demotion costs nothing and a hit needs no inflate -- it trades the smaller
 *   inactive footprint for zero restore work and stable result pointers.
 *   A live GUI has no serialized artifact to hand over (the host deletes the
 *   worker's .ntpack the moment it inflates it, §10.6) and does not own the
 *   arena, so a frontend uses this flavour.
 *
 * TRANSIENT PEAK. An active-entry swap (a store, or authoritative resolution
 * switching winners) holds TWO fully decompressed results at once for the span of
 * the swap: the incoming/inflated winner AND the outgoing active result before it
 * is demoted to retained bytes. Budget and OOM thinking must therefore size for
 * 2x one decompressed result, not 1x.
 *
 * SELECTION (§10.3, decision 0004). The authoritative result is the entry with
 * the latest completion SEQUENCE, unless an explicit selection points at a
 * specific cached hash (Undo/Redo and manual selection choose by hash, never by
 * wall-clock). Selection is deterministic: two completions that arrive in either
 * order resolve to the same winner.
 *
 * CONTAINMENT. A retained-bytes entry whose inflate fails (corrupt/truncated) is
 * dropped in place -- structured status, no crash, the store stays usable and
 * falls back to the next candidate.
 *
 * NON-GOALS. No disk persistence; the store lives only for the session. No GPU
 * textures or source-image copies are retained. Undo/Redo NEVER auto-pack (see
 * tp_pack_result_cache_contains, used by the F3-03 T6 probe). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;

typedef struct tp_pack_result_cache tp_pack_result_cache;

/* `byte_budget` bounds the total retained bytes of INACTIVE entries (the pinned
 * active entry is never counted). 0 is legal: every demoted entry is evicted
 * immediately EXCEPT the highest-sequence entry, which stays resolvable no matter
 * the budget (store contract, decision 0004) -- so the active entry and the
 * max-sequence entry survive, and inactive_bytes may exceed the budget by at most
 * that one retained entry. Returns NULL on OOM.
 *
 * SIZING. Retained bytes are the serialized .ntpack artifact, whose pages are RAW
 * RGBA8 (tp_pack_read requires RAW) -- so a budget is raw-pixel-sized, not
 * codec-compressed: a single 4096x4096 page retains ~64 MiB. Size `byte_budget`
 * against raw page area x retained-entry count, not a compressed estimate. */
tp_pack_result_cache *tp_pack_result_cache_create(uint64_t byte_budget);
void tp_pack_result_cache_destroy(tp_pack_result_cache *cache);

/* Stores a completed Pack result under `hash` and pins it ACTIVE.
 *
 * `sequence` is the caller's MONOTONIC completion order (the generalized
 * slot->version): the session assigns it per Pack REQUEST in strictly increasing
 * order and hands it back at completion. Selecting by this sequence -- not by
 * store/wall-clock order -- is what lets an earlier-requested job that finishes
 * LATE (lower sequence, stored later) never overwrite a newer preview (§10.3,
 * decision 0004). Two completions therefore resolve to the same winner (the
 * higher sequence) regardless of which one is stored first.
 *
 * `ntpack_bytes`/`ntpack_size` (the serialized artifact) are COPIED into the
 * store. On success `result_arena` ownership is TRANSFERRED to the store (freed
 * on eviction/destroy); on any error the caller retains it. `result` is a
 * borrowed pointer into `result_arena` that the store pins as the active
 * decompressed result and mines for the inflate name list (atlas name + sprite
 * names). Re-storing an existing hash refreshes its bytes and sequence.
 *
 * The previously-active entry becomes inactive and enters the byte-budget LRU;
 * over-budget inactive entries are then evicted. */
tp_status tp_pack_result_cache_store(tp_pack_result_cache *cache, tp_id128 hash,
                                     uint64_t sequence,
                                     const void *ntpack_bytes,
                                     size_t ntpack_size,
                                     struct tp_arena *result_arena,
                                     const struct tp_result *result,
                                     tp_error *err);

/* Stores a completed Pack result whose memory is owned by `pin_owner` -- an
 * opaque, already-refcounted receipt the store cannot adopt or take apart -- and
 * pins it ACTIVE. This is the frontend entry point (packet S18): a GUI holds the
 * session Pack receipt (`tp_session_job_result::_owner`), which is what keeps the
 * Pack arena alive, and has no serialized artifact to hand over.
 *
 * `result` is BORROWED for the whole life of the entry: it must stay valid until
 * `pin_release(pin_owner)` runs, and the store hands it straight back from
 * tp_pack_result_cache_authoritative without any inflate, so a caller's result
 * pointer stays stable while the entry is present. `pin_release` is required and
 * is invoked EXACTLY ONCE per successful store -- on eviction, on a re-store of
 * the same hash, on tp_pack_result_cache_forget, or on destroy. On any error the
 * caller keeps the pin and must release it itself.
 *
 * `retained_bytes` is the caller's measure of the memory this pin holds (for a
 * Pack receipt: the raw RGBA8 page bytes). It is what the byte-budget LRU counts
 * while the entry is inactive; the active pin is exempt exactly as it is for a
 * serialized entry. `sequence` has the same monotonic-completion meaning as in
 * tp_pack_result_cache_store. */
tp_status tp_pack_result_cache_store_retained(
    tp_pack_result_cache *cache, tp_id128 hash, uint64_t sequence,
    const struct tp_result *result, uint64_t retained_bytes, void *pin_owner,
    void (*pin_release)(void *pin_owner), tp_error *err);

/* True iff an entry with `hash` is present (active or inactive). This is the
 * Undo/Redo cache probe: a hit means the previous preview is available; a miss
 * means keep the current preview and mark it stale -- it never starts a Pack. */
bool tp_pack_result_cache_contains(const tp_pack_result_cache *cache,
                                   tp_id128 hash);

/* Drops `hash` if present, releasing its arena (serialized entry) or its owner
 * (retained-pin entry) exactly once, and clears an explicit selection that named
 * it. Used when the thing a cached result describes is gone for good -- the atlas
 * was deleted, or its presentation slot was cleared. Absent hash: no-op. */
void tp_pack_result_cache_forget(tp_pack_result_cache *cache, tp_id128 hash);

/* Explicit selection by hash (decision 0004). If `hash` is present, subsequent
 * authoritative resolution returns it; a nil hash or an absent hash clears the
 * selection and authoritative reverts to the latest completion sequence. */
void tp_pack_result_cache_select(tp_pack_result_cache *cache, tp_id128 hash);

/* Resolves the authoritative result: the explicit selection if present, else the
 * latest completion sequence. Ensures the winner is inflated and pinned ACTIVE
 * and returns a borrowed tp_result (valid until the next store/select/destroy),
 * its hash, and its completion sequence (each out param may be NULL).
 *
 * A corrupt retained-bytes entry is dropped (contained) and skipped in favour of
 * the next candidate. TP_STATUS_NOT_FOUND if the store holds no usable entry. */
tp_status tp_pack_result_cache_authoritative(tp_pack_result_cache *cache,
                                             tp_id128 *out_hash,
                                             const struct tp_result **out_result,
                                             uint64_t *out_sequence,
                                             tp_error *err);

typedef struct tp_pack_result_cache_stats {
    int entry_count;          /* active + inactive entries currently held */
    bool has_active;          /* an entry is pinned active */
    tp_id128 active_hash;     /* meaningful only when has_active */
    uint64_t inactive_bytes;  /* EXACT retained bytes of inactive entries */
    uint64_t byte_budget;     /* configured inactive budget */
    uint64_t last_sequence;   /* highest completion sequence stored so far */
    uint64_t evicted;         /* inactive entries removed by the LRU */
    uint64_t dropped_corrupt; /* entries dropped because inflate failed */
} tp_pack_result_cache_stats;

void tp_pack_result_cache_stats_get(const tp_pack_result_cache *cache,
                                    tp_pack_result_cache_stats *out);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_RESULT_CACHE_H */
