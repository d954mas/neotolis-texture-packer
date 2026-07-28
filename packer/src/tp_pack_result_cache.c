#include "tp_core/tp_pack_result_cache.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_name_map.h"
#include "tp_pack_read.h"

/* One stored Pack result, in one of the two flavours the header describes.
 *
 * SERIALIZED (pin_release == NULL). INVARIANT: exactly the `active` entry (if
 * any) has a non-NULL `arena`; every other entry is inactive and holds only its
 * retained `ntpack` bytes + `names` (the inflate key list). `ntpack_size` is its
 * budget-accounted quantity (names/struct overhead are not counted, so the budget
 * number is an EXACT sum of inactive artifact sizes).
 *
 * RETAINED-PIN (pin_release != NULL). `arena` is ALWAYS NULL -- the arena belongs
 * to `pin_owner`, which the store only ever releases through `pin_release`, never
 * dismantles -- and `result` stays valid for the whole life of the entry, active
 * or not. It holds no `ntpack`/`names` because nothing is ever re-inflated, and
 * its budget-accounted quantity is the caller-supplied `retained_bytes`. */
typedef struct cache_entry {
    tp_id128 hash;
    uint8_t *ntpack;    /* owned copy of the serialized artifact */
    size_t ntpack_size; /* budget-accounted retained bytes (serialized) */
    char **names;       /* owned inflate name list (atlas name + sprite names) */
    int name_count;
    uint64_t sequence; /* monotonic completion sequence */
    uint64_t touch;    /* LRU clock: larger = more recently used */
    struct tp_arena *arena;    /* serialized: non-NULL only while ACTIVE */
    struct tp_result *result;  /* borrowed from `arena` / from `pin_owner` */
    void *pin_owner;                     /* retained-pin: opaque receipt */
    void (*pin_release)(void *owner);    /* retained-pin: flavour discriminator */
    uint64_t retained_bytes;             /* retained-pin: budget-accounted */
} cache_entry;

struct tp_pack_result_cache {
    cache_entry **entries;
    int count;
    int cap;
    cache_entry *active;
    bool has_selection;
    tp_id128 selected_hash;
    uint64_t budget;
    uint64_t inactive_bytes;
    uint64_t seq_clock;
    uint64_t touch_clock;
    uint64_t evicted;
    uint64_t dropped_corrupt;
};

static char *dup_cstr(const char *s) {
    const char *src = s ? s : "";
    const size_t n = strlen(src) + 1U;
    char *copy = malloc(n);
    if (copy) {
        memcpy(copy, src, n);
    }
    return copy;
}

static void names_free(char **names, int count) {
    if (!names) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

/* atlas name + every sprite name -- the reverse-map keys tp_pack_read_memory
 * needs to resolve region name hashes when this entry is re-inflated. */
static tp_status collect_names(const tp_result *result, char ***out_names,
                               int *out_count) {
    *out_names = NULL;
    *out_count = 0;
    const int count = 1 + (result->sprite_count > 0 ? result->sprite_count : 0);
    char **names = calloc((size_t)count, sizeof *names);
    if (!names) {
        return TP_STATUS_OOM;
    }
    int n = 0;
    names[n] = dup_cstr(result->atlas_name);
    if (!names[n]) {
        names_free(names, n);
        return TP_STATUS_OOM;
    }
    n++;
    for (int i = 0; i < result->sprite_count; i++) {
        names[n] = dup_cstr(result->sprites[i].name);
        if (!names[n]) {
            names_free(names, n);
            return TP_STATUS_OOM;
        }
        n++;
    }
    *out_names = names;
    *out_count = n;
    return TP_STATUS_OK;
}

static bool entry_is_retained(const cache_entry *entry) {
    return entry->pin_release != NULL;
}

/* The quantity the byte-budget LRU counts for this entry while it is inactive. */
static uint64_t entry_budget_bytes(const cache_entry *entry) {
    return entry_is_retained(entry) ? entry->retained_bytes
                                    : (uint64_t)entry->ntpack_size;
}

/* Gives up the entry's decompressed residency for good: the arena it owns
 * (serialized) or the caller's receipt (retained pin), exactly once. NOT the
 * demotion path -- a retained entry keeps its pin across demotion. */
static void entry_release_pin(cache_entry *entry) {
    if (entry_is_retained(entry)) {
        entry->pin_release(entry->pin_owner);
        entry->pin_owner = NULL;
        entry->pin_release = NULL;
        entry->retained_bytes = 0U;
    } else {
        tp_arena_destroy(entry->arena);
    }
    entry->arena = NULL;
    entry->result = NULL;
}

static void entry_free(cache_entry *entry) {
    if (!entry) {
        return;
    }
    free(entry->ntpack);
    names_free(entry->names, entry->name_count);
    entry_release_pin(entry);
    free(entry);
}

static cache_entry *find_entry(const tp_pack_result_cache *cache,
                               tp_id128 hash) {
    for (int i = 0; i < cache->count; i++) {
        if (tp_id128_eq(cache->entries[i]->hash, hash)) {
            return cache->entries[i];
        }
    }
    return NULL;
}

static bool entries_push(tp_pack_result_cache *cache, cache_entry *entry) {
    if (cache->count == cache->cap) {
        const int new_cap = cache->cap ? cache->cap * 2 : 8;
        cache_entry **grown =
            realloc(cache->entries, (size_t)new_cap * sizeof *grown);
        if (!grown) {
            return false;
        }
        cache->entries = grown;
        cache->cap = new_cap;
    }
    cache->entries[cache->count++] = entry;
    return true;
}

/* Removes `entry` from the store and frees it. Accounting for inactive bytes is
 * the caller's responsibility (it knows the entry's active/inactive state). */
static void remove_entry(tp_pack_result_cache *cache, cache_entry *entry) {
    int idx = -1;
    for (int i = 0; i < cache->count; i++) {
        if (cache->entries[i] == entry) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    if (cache->active == entry) {
        cache->active = NULL;
    }
    entry_free(entry);
    for (int i = idx; i < cache->count - 1; i++) {
        cache->entries[i] = cache->entries[i + 1];
    }
    cache->count--;
}

/* The entry holding the maximum completion sequence -- the one authoritative()
 * resolves to absent an explicit selection (decision 0004). Ties keep the
 * earliest-stored entry, exactly as resolve_target() does. */
static cache_entry *max_sequence_entry(const tp_pack_result_cache *cache) {
    cache_entry *best = NULL;
    for (int i = 0; i < cache->count; i++) {
        cache_entry *e = cache->entries[i];
        if (!best || e->sequence > best->sequence) {
            best = e;
        }
    }
    return best;
}

static void evict_over_budget(tp_pack_result_cache *cache) {
    /* The highest-sequence entry must stay resolvable regardless of budget size
     * (store contract, decision 0004): an out-of-order store can demote it to
     * inactive, but authoritative() must still return it, never a stale lower
     * sequence. So it is exempt from eviction unconditionally, the same way the
     * pinned active entry is -- even at budget 0. Its retained bytes stay counted
     * in inactive_bytes, so inactive_bytes may exceed the budget by at most this
     * one entry when it is inactive. */
    cache_entry *keep = max_sequence_entry(cache);
    while (cache->inactive_bytes > cache->budget) {
        cache_entry *victim = NULL;
        for (int i = 0; i < cache->count; i++) {
            cache_entry *e = cache->entries[i];
            /* Active pin + highest sequence are never evicted. The pin test is
             * `is the active entry`, not `holds an arena`: a retained-pin entry
             * has no arena of its own even while it is the active one. */
            if (e == cache->active || e == keep) {
                continue;
            }
            if (!victim || e->touch < victim->touch) {
                victim = e;
            }
        }
        if (!victim) {
            break; /* only the active + highest-sequence entries remain */
        }
        cache->inactive_bytes -= entry_budget_bytes(victim);
        remove_entry(cache, victim);
        cache->evicted++;
    }
}

/* Demotes the currently active entry into the byte-budget LRU. A serialized
 * entry gives up its decompressed arena and falls back to its retained bytes; a
 * retained-pin entry keeps its receipt (there is nothing cheaper to fall back
 * to) and only starts being counted against the budget. */
static void demote_active(tp_pack_result_cache *cache) {
    cache_entry *prev = cache->active;
    if (!prev) {
        return;
    }
    if (!entry_is_retained(prev)) {
        tp_arena_destroy(prev->arena);
        prev->arena = NULL;
        prev->result = NULL;
    }
    prev->touch = ++cache->touch_clock;
    cache->inactive_bytes += entry_budget_bytes(prev);
    cache->active = NULL;
}

static tp_status inflate_entry(cache_entry *entry, tp_error *err) {
    tp_name_map *map = tp_name_map_create();
    if (!map) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_pack_result_cache: name map alloc failed");
    }
    for (int i = 0; i < entry->name_count; i++) {
        tp_status st = tp_name_map_insert(map, entry->names[i]);
        if (st != TP_STATUS_OK) {
            tp_name_map_destroy(map);
            return tp_error_set(err, st,
                                "tp_pack_result_cache: inflate name map failed");
        }
    }
    tp_arena *arena = tp_arena_create(0);
    if (!arena) {
        tp_name_map_destroy(map);
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_pack_result_cache: inflate arena alloc failed");
    }
    tp_result **results = NULL;
    int count = 0;
    tp_status st = tp_pack_read_memory(entry->ntpack, entry->ntpack_size, map,
                                       arena, &results, &count, err);
    tp_name_map_destroy(map);
    if (st != TP_STATUS_OK) {
        tp_arena_destroy(arena);
        return st;
    }
    if (count != 1 || !results || !results[0]) {
        tp_arena_destroy(arena);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "tp_pack_result_cache: artifact held %d atlases, "
                            "expected 1",
                            count);
    }
    entry->arena = arena;
    entry->result = results[0];
    return TP_STATUS_OK;
}

tp_pack_result_cache *tp_pack_result_cache_create(uint64_t byte_budget) {
    tp_pack_result_cache *cache = calloc(1U, sizeof *cache);
    if (!cache) {
        return NULL;
    }
    cache->budget = byte_budget;
    return cache;
}

void tp_pack_result_cache_destroy(tp_pack_result_cache *cache) {
    if (!cache) {
        return;
    }
    for (int i = 0; i < cache->count; i++) {
        entry_free(cache->entries[i]);
    }
    free(cache->entries);
    free(cache);
}

tp_status tp_pack_result_cache_store(tp_pack_result_cache *cache, tp_id128 hash,
                                     uint64_t sequence,
                                     const void *ntpack_bytes,
                                     size_t ntpack_size,
                                     struct tp_arena *result_arena,
                                     const struct tp_result *result,
                                     tp_error *err) {
    if (!cache || !ntpack_bytes || ntpack_size == 0U || !result_arena ||
        !result) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack_result_cache: store requires cache, bytes, "
                            "arena, and result");
    }

    /* Build every owned copy up front; adopt `result_arena` only once nothing
     * else can fail, so an early error leaves ownership with the caller. */
    uint8_t *payload = malloc(ntpack_size);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_pack_result_cache: artifact copy alloc failed");
    }
    memcpy(payload, ntpack_bytes, ntpack_size);

    char **names = NULL;
    int name_count = 0;
    tp_status st = collect_names(result, &names, &name_count);
    if (st != TP_STATUS_OK) {
        free(payload);
        return tp_error_set(err, st,
                            "tp_pack_result_cache: name capture failed");
    }

    cache_entry *entry = find_entry(cache, hash);
    if (entry) {
        /* Refresh in place. If it was inactive its retained bytes were counted;
         * it is about to become active, so drop that contribution. */
        if (entry != cache->active) {
            cache->inactive_bytes -= entry_budget_bytes(entry);
        }
        free(entry->ntpack);
        names_free(entry->names, entry->name_count);
        entry_release_pin(entry);
        entry->ntpack = payload;
        entry->ntpack_size = ntpack_size;
        entry->names = names;
        entry->name_count = name_count;
    } else {
        entry = calloc(1U, sizeof *entry);
        if (!entry) {
            free(payload);
            names_free(names, name_count);
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry alloc failed");
        }
        entry->hash = hash;
        entry->ntpack = payload;
        entry->ntpack_size = ntpack_size;
        entry->names = names;
        entry->name_count = name_count;
        if (!entries_push(cache, entry)) {
            entry_free(entry);
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry table grow failed");
        }
    }

    entry->sequence = sequence;
    if (sequence > cache->seq_clock) {
        cache->seq_clock = sequence;
    }
    entry->touch = ++cache->touch_clock;
    entry->arena = result_arena; /* ADOPT: ownership transferred */
    entry->result = (tp_result *)result;

    if (cache->active != entry) {
        demote_active(cache);
    }
    cache->active = entry;

    evict_over_budget(cache);
    return TP_STATUS_OK;
}

tp_status tp_pack_result_cache_store_retained(
    tp_pack_result_cache *cache, tp_id128 hash, uint64_t sequence,
    const struct tp_result *result, uint64_t retained_bytes, void *pin_owner,
    void (*pin_release)(void *pin_owner), tp_error *err) {
    if (!cache || !result || !pin_release) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "tp_pack_result_cache: retained store requires cache, result, and a "
            "release hook");
    }

    /* Adopt the pin only once nothing else can fail: on every error return above
     * and below, the caller still owns `pin_owner` and must release it. */
    cache_entry *entry = find_entry(cache, hash);
    if (entry) {
        if (entry != cache->active) {
            cache->inactive_bytes -= entry_budget_bytes(entry);
        }
        free(entry->ntpack);
        entry->ntpack = NULL;
        entry->ntpack_size = 0U;
        names_free(entry->names, entry->name_count);
        entry->names = NULL;
        entry->name_count = 0;
        entry_release_pin(entry); /* releases the SUPERSEDED pin, not this one */
    } else {
        entry = calloc(1U, sizeof *entry);
        if (!entry) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry alloc failed");
        }
        entry->hash = hash;
        if (!entries_push(cache, entry)) {
            free(entry);
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_pack_result_cache: entry table grow failed");
        }
    }

    entry->sequence = sequence;
    if (sequence > cache->seq_clock) {
        cache->seq_clock = sequence;
    }
    entry->touch = ++cache->touch_clock;
    entry->pin_owner = pin_owner; /* ADOPT: released exactly once from now on */
    entry->pin_release = pin_release;
    entry->retained_bytes = retained_bytes;
    entry->result = (tp_result *)result;

    if (cache->active != entry) {
        demote_active(cache);
    }
    cache->active = entry;

    evict_over_budget(cache);
    return TP_STATUS_OK;
}

void tp_pack_result_cache_forget(tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return;
    }
    cache_entry *entry = find_entry(cache, hash);
    if (!entry) {
        return;
    }
    if (entry != cache->active) {
        cache->inactive_bytes -= entry_budget_bytes(entry);
    }
    if (cache->has_selection && tp_id128_eq(cache->selected_hash, hash)) {
        cache->has_selection = false;
    }
    remove_entry(cache, entry);
}

bool tp_pack_result_cache_contains(const tp_pack_result_cache *cache,
                                   tp_id128 hash) {
    if (!cache) {
        return false;
    }
    return find_entry(cache, hash) != NULL;
}

void tp_pack_result_cache_select(tp_pack_result_cache *cache, tp_id128 hash) {
    if (!cache) {
        return;
    }
    if (tp_id128_is_nil(hash) || !find_entry(cache, hash)) {
        cache->has_selection = false;
        return;
    }
    cache->has_selection = true;
    cache->selected_hash = hash;
}

static cache_entry *resolve_target(tp_pack_result_cache *cache) {
    if (cache->has_selection) {
        cache_entry *selected = find_entry(cache, cache->selected_hash);
        if (selected) {
            return selected;
        }
        /* selection lost (dropped/corrupt) -> fall back to latest sequence */
    }
    cache_entry *best = NULL;
    for (int i = 0; i < cache->count; i++) {
        if (!best || cache->entries[i]->sequence > best->sequence) {
            best = cache->entries[i];
        }
    }
    return best;
}

tp_status tp_pack_result_cache_authoritative(tp_pack_result_cache *cache,
                                             tp_id128 *out_hash,
                                             const struct tp_result **out_result,
                                             uint64_t *out_sequence,
                                             tp_error *err) {
    if (!cache) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack_result_cache: cache is required");
    }
    for (;;) {
        cache_entry *target = resolve_target(cache);
        if (!target) {
            return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                "tp_pack_result_cache: no cached result");
        }
        if (target != cache->active) {
            /* A retained-pin entry never gave up its decompressed result, so
             * there is nothing to inflate and nothing that can be corrupt. */
            if (!entry_is_retained(target)) {
                tp_status st = inflate_entry(target, err);
                if (st != TP_STATUS_OK) {
                    /* Contained failure: drop the corrupt entry and retry. */
                    cache->dropped_corrupt++;
                    if (cache->has_selection &&
                        tp_id128_eq(cache->selected_hash, target->hash)) {
                        cache->has_selection = false;
                    }
                    cache->inactive_bytes -= entry_budget_bytes(target);
                    remove_entry(cache, target);
                    continue;
                }
            }
            cache->inactive_bytes -= entry_budget_bytes(target);
            demote_active(cache);
            cache->active = target;
            target->touch = ++cache->touch_clock;
            evict_over_budget(cache);
        }
        if (out_hash) {
            *out_hash = target->hash;
        }
        if (out_result) {
            *out_result = target->result;
        }
        if (out_sequence) {
            *out_sequence = target->sequence;
        }
        return TP_STATUS_OK;
    }
}

void tp_pack_result_cache_stats_get(const tp_pack_result_cache *cache,
                                    tp_pack_result_cache_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (!cache) {
        return;
    }
    out->entry_count = cache->count;
    out->has_active = cache->active != NULL;
    if (cache->active) {
        out->active_hash = cache->active->hash;
    }
    out->inactive_bytes = cache->inactive_bytes;
    out->byte_budget = cache->budget;
    out->last_sequence = cache->seq_clock;
    out->evicted = cache->evicted;
    out->dropped_corrupt = cache->dropped_corrupt;
}
