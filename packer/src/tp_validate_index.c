#include "tp_validate_index_internal.h"

#include <stdlib.h>
#include <string.h>

#include "hash/nt_hash.h"

_Thread_local size_t tp_validate_work_probes;

/* Validation-local borrowed-string index. Slots own no strings and live only for
 * one atlas validation; open addressing keeps the lookup owner here rather than
 * adding a generic map or changing tp_sprite_index's public contract. */
bool str_index_init(str_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (str_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

void str_index_free(str_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}

str_slot *str_index_find(const str_index *index, const char *key) {
    if (!index || index->cap == 0U || !key) {
        return NULL;
    }
    const size_t start = (size_t)nt_hash64_str(key).value & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        tp_validate_work_probes++;
        str_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->key || strcmp(slot->key, key) == 0) {
            return slot;
        }
    }
    return NULL;
}

bool str_index_add(str_index *index, const char *key, int value_index) {
    str_slot *slot = str_index_find(index, key);
    if (!slot) {
        return false;
    }
    if (!slot->key) {
        slot->key = key;
        slot->first_index = value_index;
    }
    slot->last_index = value_index;
    slot->count++;
    return true;
}

bool str_index_build(str_index *index, const char *const *values, int count) {
    if (!str_index_init(index, count)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (!str_index_add(index, values[i], i)) {
            str_index_free(index);
            return false;
        }
    }
    return true;
}

bool id_index_init(id_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (id_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

static id_slot *id_index_find(const id_index *index, tp_id128 id) {
    if (!index || index->cap == 0U) {
        return NULL;
    }
    const size_t start = (size_t)tp_id128_bucket(id) & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        tp_validate_work_probes++;
        id_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->occupied || tp_id128_eq(slot->id, id)) {
            return slot;
        }
    }
    return NULL;
}

bool id_index_add(id_index *index, tp_id128 id) {
    id_slot *slot = id_index_find(index, id);
    if (!slot) {
        return false;
    }
    slot->id = id;
    slot->occupied = true;
    return true;
}

bool id_index_contains(const id_index *index, tp_id128 id) {
    const id_slot *slot = id_index_find(index, id);
    return slot && slot->occupied;
}

void id_index_free(id_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}

bool id_key_index_init(id_key_index *index, int expected) {
    memset(index, 0, sizeof *index);
    if (expected <= 0) {
        return true;
    }
    size_t cap = 16U;
    const size_t need = (size_t)expected * 2U;
    while (cap < need) {
        if (cap > SIZE_MAX / 2U) {
            return false;
        }
        cap *= 2U;
    }
    index->slots = (id_key_slot *)calloc(cap, sizeof *index->slots);
    if (!index->slots) {
        return false;
    }
    index->cap = cap;
    return true;
}

static id_key_slot *id_key_index_find(const id_key_index *index, tp_id128 id, const char *key) {
    if (!index || index->cap == 0U || !key) {
        return NULL;
    }
    const uint64_t hash = tp_id128_bucket(id) ^ (nt_hash64_str(key).value * UINT64_C(0x9e3779b97f4a7c15));
    const size_t start = (size_t)hash & (index->cap - 1U);
    for (size_t i = 0; i < index->cap; i++) {
        tp_validate_work_probes++;
        id_key_slot *slot = &index->slots[(start + i) & (index->cap - 1U)];
        if (!slot->key || (tp_id128_eq(slot->id, id) && strcmp(slot->key, key) == 0)) {
            return slot;
        }
    }
    return NULL;
}

bool id_key_index_add(id_key_index *index, tp_id128 id, const char *key, bool *already_present) {
    id_key_slot *slot = id_key_index_find(index, id, key);
    if (!slot) {
        return false;
    }
    *already_present = slot->key != NULL;
    if (!slot->key) {
        slot->id = id;
        slot->key = key;
    }
    return true;
}

bool id_key_index_contains(const id_key_index *index, tp_id128 id, const char *key) {
    const id_key_slot *slot = id_key_index_find(index, id, key);
    return slot && slot->key;
}

void id_key_index_free(id_key_index *index) {
    free(index->slots);
    memset(index, 0, sizeof *index);
}
