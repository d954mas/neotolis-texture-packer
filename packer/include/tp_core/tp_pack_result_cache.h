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
 * active entry is never counted). 0 is legal (every demoted entry is evicted
 * immediately, only the active entry survives). Returns NULL on OOM. */
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

/* True iff an entry with `hash` is present (active or inactive). This is the
 * Undo/Redo cache probe: a hit means the previous preview is available; a miss
 * means keep the current preview and mark it stale -- it never starts a Pack. */
bool tp_pack_result_cache_contains(const tp_pack_result_cache *cache,
                                   tp_id128 hash);

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
