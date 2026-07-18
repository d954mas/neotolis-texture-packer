#ifndef TP_VALIDATE_INDEX_INTERNAL_H
#define TP_VALIDATE_INDEX_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_id.h"

typedef struct str_slot {
    const char *key;
    size_t count;
    int first_index;
    int last_index;
} str_slot;

typedef struct str_index {
    str_slot *slots;
    size_t cap;
} str_index;

bool str_index_init(str_index *index, int expected);
void str_index_free(str_index *index);
str_slot *str_index_find(const str_index *index, const char *key);
bool str_index_add(str_index *index, const char *key, int value_index);
bool str_index_build(str_index *index, const char *const *values, int count);

typedef struct id_slot {
    tp_id128 id;
    bool occupied;
} id_slot;

typedef struct id_index {
    id_slot *slots;
    size_t cap;
} id_index;

bool id_index_init(id_index *index, int expected);
bool id_index_add(id_index *index, tp_id128 id);
bool id_index_contains(const id_index *index, tp_id128 id);
void id_index_free(id_index *index);

typedef struct id_key_slot {
    tp_id128 id;
    uint64_t key_hash;
    const char *key;
} id_key_slot;

typedef struct id_key_index {
    id_key_slot *slots;
    size_t cap;
} id_key_index;

bool id_key_index_init(id_key_index *index, int expected);
bool id_key_index_add(id_key_index *index, tp_id128 id, const char *key,
                      bool *already_present);
bool id_key_index_contains(const id_key_index *index, tp_id128 id,
                           const char *key);
void id_key_index_free(id_key_index *index);

extern _Thread_local size_t tp_validate_work_probes;

#endif /* TP_VALIDATE_INDEX_INTERNAL_H */
