#ifndef TP_C0_CACHE_H
#define TP_C0_CACHE_H

/*
 * C0-03 task 4: the memory-only Pack-result cache INTERFACE, keyed by
 * pack_input_hash, with a configurable byte budget parameter and a reference
 * in-memory implementation.
 *
 * Master spec §10.3 (all successful results may enter the cache; selection is by
 * hash), §10.4 (memory-only for the live session; the active result is PINNED
 * and may hold decompressed/GPU resources; inactive results use a separate
 * byte-budget LRU), §52.3 (memory-only compressed Pack-result LRU). Concrete
 * byte budgets, the compressed representation, and GPU residency thresholds are
 * implementation policy and stay OPEN per §60 item 3 -- so the budget is a
 * CONSTRUCTOR PARAMETER here, never a baked default value.
 *
 * Keying by pack_input_hash (a 128-bit content-address, §10.2) is what lets
 * explicit selection and an Undo cache-hit pick a result BY HASH, not by
 * completion time (the pack-supersession policy, task 5, is built on this get).
 *
 * The reference impl copies blobs in and frees them internally (no cross-CRT
 * malloc/free handoff to callers, mirroring the C0-01 srckey/utf8proc rule): a
 * pointer returned by get is cache-owned and valid until the entry is
 * evicted/replaced or the cache is destroyed.
 */

#include <stdbool.h>
#include <stddef.h>

#include "tp_c0/tp_c0_error.h"
#include "tp_c0/tp_c0_id.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Spike fixed capacity on distinct entries (deterministic, no unbounded heap
 * growth); a production cache sizes by the byte budget alone. */
#define TP_C0_CACHE_MAX_ENTRIES 64

typedef struct tp_c0_cache tp_c0_cache; /* opaque reference in-memory impl */

/* Create a memory cache with `byte_budget` governing the UNPINNED (inactive) LRU
 * bytes. The budget is a caller parameter -- no production default is chosen
 * here (§60 item 3). A budget of 0 keeps no unpinned entries (every unpinned
 * entry is evicted unless it is the sole survivor). Returns NULL on OOM. */
tp_c0_cache *tp_c0_cache_mem_create(size_t byte_budget);
void tp_c0_cache_destroy(tp_c0_cache *c);

/* Insert/replace the result blob for `key` (copied in). The entry becomes the
 * most-recently-used and starts UNPINNED. After insert, unpinned LRU entries are
 * evicted while unpinned bytes exceed the budget and more than one unpinned
 * entry remains (a single over-budget item is retained -- soft cap). Faults:
 * id_nil (nil key), null_arg (NULL blob with len>0), empty (len==0),
 * oom (blob copy failed), buffer_too_small (entry table full, all pinned). */
tp_c0_detail tp_c0_cache_put(tp_c0_cache *c, tp_c0_id128 key, const void *blob, size_t len, tp_error *err);

/* Look up by hash. On a hit, PINS the entry and returns the cache-owned blob
 * pointer (and *out_len if non-NULL); on a miss returns NULL and pins nothing.
 * Auto-pinning makes the returned pointer safe to hold across a later put that
 * would otherwise evict+free it (§10.4: the active result is pinned) -- so the
 * caller MUST tp_c0_cache_unpin(key) when finished reading (else the entry is held
 * out of the budget forever). Selection is BY HASH, independent of insertion/
 * completion order. Use tp_c0_cache_contains for a membership check that does not
 * pin. */
const void *tp_c0_cache_get(tp_c0_cache *c, tp_c0_id128 key, size_t *out_len);

bool tp_c0_cache_contains(const tp_c0_cache *c, tp_c0_id128 key);

/* Evict `key` (also unpins it). Returns true if it was present. */
bool tp_c0_cache_evict(tp_c0_cache *c, tp_c0_id128 key);

/* Pin/unpin the ACTIVE result: a pinned entry is never evicted and its bytes do
 * not count against the unpinned byte budget (§10.4). Returns false if `key` is
 * absent. Unpinning re-subjects the entry to the budget (may trigger eviction of
 * OTHER unpinned entries on the next put, not immediately). */
bool tp_c0_cache_pin(tp_c0_cache *c, tp_c0_id128 key);
bool tp_c0_cache_unpin(tp_c0_cache *c, tp_c0_id128 key);
bool tp_c0_cache_is_pinned(const tp_c0_cache *c, tp_c0_id128 key);

/* Introspection (for policy tests). */
size_t tp_c0_cache_budget(const tp_c0_cache *c);
size_t tp_c0_cache_unpinned_bytes(const tp_c0_cache *c); /* bytes counted against the budget */
size_t tp_c0_cache_total_bytes(const tp_c0_cache *c);    /* pinned + unpinned */
int tp_c0_cache_count(const tp_c0_cache *c);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_CACHE_H */
