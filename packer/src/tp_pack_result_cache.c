#include "tp_core/tp_pack_result_cache.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_model.h"
#include "tp_core/tp_name_map.h"
#include "tp_core/tp_pack_read.h"

/* One stored Pack result. INVARIANT: exactly the `active` entry (if any) has a
 * non-NULL `arena`; every other entry is inactive and holds only its retained
 * `ntpack` bytes + `names` (the inflate key list). `ntpack_size` is the only
 * byte-budget-accounted quantity (names/struct overhead are not counted, so the
 * budget number is an EXACT sum of inactive artifact sizes). */
typedef struct cache_entry {
    tp_id128 hash;
    uint8_t *ntpack;    /* owned copy of the serialized artifact */
    size_t ntpack_size; /* budget-accounted retained bytes */
    char **names;       /* owned inflate name list (atlas name + sprite names) */
    int name_count;
    uint64_t sequence; /* monotonic completion sequence */
    uint64_t touch;    /* LRU clock: larger = more recently used */
    struct tp_arena *arena;    /* non-NULL only while ACTIVE (pinned) */
    struct tp_result *result;  /* borrowed into `arena` while ACTIVE */
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

static void entry_free(cache_entry *entry) {
    if (!entry) {
        return;
    }
    free(entry->ntpack);
    names_free(entry->names, entry->name_count);
    tp_arena_destroy(entry->arena);
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

static void evict_over_budget(tp_pack_result_cache *cache) {
    while (cache->inactive_bytes > cache->budget) {
        cache_entry *victim = NULL;
        for (int i = 0; i < cache->count; i++) {
            cache_entry *e = cache->entries[i];
            if (e->arena != NULL) {
                continue; /* active pin is never evicted */
            }
            if (!victim || e->touch < victim->touch) {
                victim = e;
            }
        }
        if (!victim) {
            break; /* only the active entry remains */
        }
        cache->inactive_bytes -= victim->ntpack_size;
        remove_entry(cache, victim);
        cache->evicted++;
    }
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
            cache->inactive_bytes -= entry->ntpack_size;
        }
        free(entry->ntpack);
        names_free(entry->names, entry->name_count);
        tp_arena_destroy(entry->arena);
        entry->arena = NULL;
        entry->result = NULL;
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

    if (cache->active && cache->active != entry) {
        cache_entry *prev = cache->active;
        tp_arena_destroy(prev->arena);
        prev->arena = NULL;
        prev->result = NULL;
        prev->touch = ++cache->touch_clock;
        cache->inactive_bytes += prev->ntpack_size;
    }
    cache->active = entry;

    evict_over_budget(cache);
    return TP_STATUS_OK;
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
            tp_status st = inflate_entry(target, err);
            if (st != TP_STATUS_OK) {
                /* Contained failure: drop the corrupt entry and retry. */
                cache->dropped_corrupt++;
                if (cache->has_selection &&
                    tp_id128_eq(cache->selected_hash, target->hash)) {
                    cache->has_selection = false;
                }
                cache->inactive_bytes -= target->ntpack_size;
                remove_entry(cache, target);
                continue;
            }
            cache->inactive_bytes -= target->ntpack_size;
            if (cache->active) {
                cache_entry *prev = cache->active;
                tp_arena_destroy(prev->arena);
                prev->arena = NULL;
                prev->result = NULL;
                prev->touch = ++cache->touch_clock;
                cache->inactive_bytes += prev->ntpack_size;
            }
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
