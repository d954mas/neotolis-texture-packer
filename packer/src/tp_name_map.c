#include "tp_core/tp_name_map.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "hash/nt_hash.h"

#define TP_NAME_MAP_INITIAL_CAPACITY 64U

typedef struct tp_name_entry {
    uint64_t hash;
    char *name; /* malloc-owned copy; NULL means the slot is empty */
} tp_name_entry;

struct tp_name_map {
    tp_name_entry *entries;
    size_t capacity;
    size_t count;
};

static char *tp_dup_str(const char *s) {
    const size_t len = strlen(s) + 1U;
    char *copy = (char *)malloc(len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

static bool tp_name_map_grow(tp_name_map *map) {
    const size_t new_capacity = (map->capacity == 0U) ? TP_NAME_MAP_INITIAL_CAPACITY : map->capacity * 2U;
    tp_name_entry *new_entries = (tp_name_entry *)calloc(new_capacity, sizeof(tp_name_entry));
    if (!new_entries) {
        return false;
    }

    for (size_t i = 0; i < map->capacity; i++) {
        tp_name_entry *e = &map->entries[i];
        if (!e->name) {
            continue;
        }
        size_t idx = (size_t)(e->hash % (uint64_t)new_capacity);
        for (size_t j = 0; j < new_capacity; j++) {
            size_t probe = (idx + j) % new_capacity;
            if (!new_entries[probe].name) {
                new_entries[probe] = *e;
                break;
            }
        }
    }

    free(map->entries);
    map->entries = new_entries;
    map->capacity = new_capacity;
    return true;
}

tp_name_map *tp_name_map_create(void) {
    tp_name_map *map = (tp_name_map *)malloc(sizeof(tp_name_map));
    if (!map) {
        return NULL;
    }
    map->entries = NULL;
    map->capacity = 0U;
    map->count = 0U;
    if (!tp_name_map_grow(map)) {
        free(map);
        return NULL;
    }
    return map;
}

void tp_name_map_destroy(tp_name_map *map) {
    if (!map) {
        return;
    }
    for (size_t i = 0; i < map->capacity; i++) {
        free(map->entries[i].name);
    }
    free(map->entries);
    free(map);
}

static tp_status tp_name_map_insert_impl(tp_name_map *map, uint64_t hash, const char *name) {
    if (!map || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }

    if (map->count * 2U >= map->capacity) {
        if (!tp_name_map_grow(map)) {
            return TP_STATUS_OOM;
        }
    }

    const size_t idx = (size_t)(hash % (uint64_t)map->capacity);
    for (size_t i = 0; i < map->capacity; i++) {
        const size_t probe = (idx + i) % map->capacity;
        tp_name_entry *e = &map->entries[probe];
        if (!e->name) {
            char *copy = tp_dup_str(name);
            if (!copy) {
                return TP_STATUS_OOM;
            }
            e->hash = hash;
            e->name = copy;
            map->count++;
            return TP_STATUS_OK;
        }
        if (e->hash == hash) {
            if (strcmp(e->name, name) == 0) {
                return TP_STATUS_OK; /* re-inserting the same name is idempotent */
            }
            return TP_STATUS_HASH_COLLISION;
        }
    }

    return TP_STATUS_OOM; /* unreachable: grow() above always leaves room */
}

tp_status tp_name_map_insert(tp_name_map *map, const char *name) {
    if (!name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return tp_name_map_insert_impl(map, nt_hash64_str(name).value, name);
}

tp_status tp_name_map_insert_hashed(tp_name_map *map, uint64_t hash, const char *name) {
    return tp_name_map_insert_impl(map, hash, name);
}

const char *tp_name_map_lookup(const tp_name_map *map, uint64_t hash) {
    if (!map || map->capacity == 0U) {
        return NULL;
    }
    const size_t idx = (size_t)(hash % (uint64_t)map->capacity);
    for (size_t i = 0; i < map->capacity; i++) {
        const size_t probe = (idx + i) % map->capacity;
        const tp_name_entry *e = &map->entries[probe];
        if (!e->name) {
            return NULL;
        }
        if (e->hash == hash) {
            return e->name;
        }
    }
    return NULL;
}
