/* C0-03 task 4: reference in-memory Pack-result cache. Fixed-capacity entry
 * table + a byte-budget LRU over UNPINNED entries; the pinned (active) result is
 * never evicted and does not count against the budget (master spec §10.4). All
 * blob malloc/free stays inside this TU (no cross-CRT handoff). See
 * tp_c0_cache.h. */

#include "tp_c0/tp_c0_cache.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct cache_entry {
    bool used;
    bool pinned;
    tp_c0_id128 key;
    uint8_t *blob; /* owned */
    size_t len;
    uint64_t last_use; /* LRU tick */
} cache_entry;

struct tp_c0_cache {
    cache_entry entries[TP_C0_CACHE_MAX_ENTRIES];
    size_t budget;
    uint64_t next_tick;
};

tp_c0_cache *tp_c0_cache_mem_create(size_t byte_budget) {
    tp_c0_cache *c = (tp_c0_cache *)calloc(1, sizeof *c);
    if (!c) {
        return NULL;
    }
    c->budget = byte_budget;
    c->next_tick = 1;
    return c;
}

void tp_c0_cache_destroy(tp_c0_cache *c) {
    if (!c) {
        return;
    }
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        free(c->entries[i].blob);
    }
    free(c);
}

static cache_entry *find(tp_c0_cache *c, tp_c0_id128 key) {
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used && tp_c0_id128_eq(c->entries[i].key, key)) {
            return &c->entries[i];
        }
    }
    return NULL;
}

static const cache_entry *find_const(const tp_c0_cache *c, tp_c0_id128 key) {
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used && tp_c0_id128_eq(c->entries[i].key, key)) {
            return &c->entries[i];
        }
    }
    return NULL;
}

static void entry_clear(cache_entry *e) {
    free(e->blob);
    e->blob = NULL;
    e->len = 0;
    e->used = false;
    e->pinned = false;
    e->last_use = 0;
}

static size_t unpinned_bytes(const tp_c0_cache *c) {
    size_t sum = 0;
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used && !c->entries[i].pinned) {
            sum += c->entries[i].len;
        }
    }
    return sum;
}

static int unpinned_count(const tp_c0_cache *c) {
    int n = 0;
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used && !c->entries[i].pinned) {
            n++;
        }
    }
    return n;
}

/* Least-recently-used UNPINNED entry, or NULL if none. */
static cache_entry *lru_unpinned(tp_c0_cache *c) {
    cache_entry *best = NULL;
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        cache_entry *e = &c->entries[i];
        if (e->used && !e->pinned && (!best || e->last_use < best->last_use)) {
            best = e;
        }
    }
    return best;
}

/* Evict LRU unpinned entries while over budget and more than one unpinned entry
 * remains -- a single over-budget item is retained (soft spike cap). */
static void enforce_budget(tp_c0_cache *c) {
    while (unpinned_bytes(c) > c->budget && unpinned_count(c) > 1) {
        cache_entry *victim = lru_unpinned(c);
        if (!victim) {
            break;
        }
        entry_clear(victim);
    }
}

tp_c0_detail tp_c0_cache_put(tp_c0_cache *c, tp_c0_id128 key, const void *blob, size_t len, tp_error *err) {
    if (!c) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "cache put: cache is NULL");
    }
    if (tp_c0_id128_is_nil(key)) {
        return tp_c0_fail(err, TP_C0_ERR_ID_NIL, "cache put: nil pack_input_hash key");
    }
    if (len == 0) {
        return tp_c0_fail(err, TP_C0_ERR_EMPTY, "cache put: empty blob");
    }
    if (!blob) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "cache put: blob is NULL but len > 0");
    }

    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) {
        return tp_c0_fail(err, TP_C0_ERR_OOM, "cache put: blob copy alloc failed");
    }
    memcpy(copy, blob, len);

    cache_entry *e = find(c, key);
    if (!e) {
        /* New key: claim a free slot, else evict an unpinned LRU to make room. */
        for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
            if (!c->entries[i].used) {
                e = &c->entries[i];
                break;
            }
        }
        if (!e) {
            cache_entry *victim = lru_unpinned(c);
            if (!victim) {
                free(copy);
                return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "cache put: entry table full (all pinned)");
            }
            entry_clear(victim);
            e = victim;
        }
        e->used = true;
        e->pinned = false;
        e->key = key;
        e->blob = NULL;
    }
    /* Replace blob (existing or new); recency refreshed. Pin state preserved. */
    free(e->blob);
    e->blob = copy;
    e->len = len;
    e->last_use = c->next_tick++;
    enforce_budget(c);
    return TP_C0_OK;
}

const void *tp_c0_cache_get(tp_c0_cache *c, tp_c0_id128 key, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!c) {
        return NULL;
    }
    cache_entry *e = find(c, key);
    if (!e) {
        return NULL;
    }
    /* Auto-PIN the returned entry so the caller's pointer stays valid across a
     * later budget-busting put that would otherwise evict+free it (F5, §10.4: the
     * active result is pinned). The caller MUST tp_c0_cache_unpin(key) when done.
     * enforce_budget / the table-full path only ever evict UNPINNED entries, so a
     * pinned pointer is never dangled. */
    e->pinned = true;
    e->last_use = c->next_tick++;
    if (out_len) {
        *out_len = e->len;
    }
    return e->blob;
}

bool tp_c0_cache_contains(const tp_c0_cache *c, tp_c0_id128 key) { return c && find_const(c, key) != NULL; }

bool tp_c0_cache_evict(tp_c0_cache *c, tp_c0_id128 key) {
    if (!c) {
        return false;
    }
    cache_entry *e = find(c, key);
    if (!e) {
        return false;
    }
    entry_clear(e);
    return true;
}

bool tp_c0_cache_pin(tp_c0_cache *c, tp_c0_id128 key) {
    if (!c) {
        return false;
    }
    cache_entry *e = find(c, key);
    if (!e) {
        return false;
    }
    e->pinned = true;
    return true;
}

bool tp_c0_cache_unpin(tp_c0_cache *c, tp_c0_id128 key) {
    if (!c) {
        return false;
    }
    cache_entry *e = find(c, key);
    if (!e) {
        return false;
    }
    e->pinned = false;
    return true;
}

bool tp_c0_cache_is_pinned(const tp_c0_cache *c, tp_c0_id128 key) {
    const cache_entry *e = c ? find_const(c, key) : NULL;
    return e && e->pinned;
}

size_t tp_c0_cache_budget(const tp_c0_cache *c) { return c ? c->budget : 0; }

size_t tp_c0_cache_unpinned_bytes(const tp_c0_cache *c) { return c ? unpinned_bytes(c) : 0; }

size_t tp_c0_cache_total_bytes(const tp_c0_cache *c) {
    if (!c) {
        return 0;
    }
    size_t sum = 0;
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used) {
            sum += c->entries[i].len;
        }
    }
    return sum;
}

int tp_c0_cache_count(const tp_c0_cache *c) {
    if (!c) {
        return 0;
    }
    int n = 0;
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        if (c->entries[i].used) {
            n++;
        }
    }
    return n;
}
