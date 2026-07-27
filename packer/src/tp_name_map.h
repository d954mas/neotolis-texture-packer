#ifndef TP_CORE_TP_NAME_MAP_H
#define TP_CORE_TP_NAME_MAP_H

/* Reverse map hash(name) -> name (plan §2.8). NtAtlasRegion only stores
 * nt_hash64_str(sprite_name) -- the tool knows every input name up front, so
 * it builds this map once and resolves hashes back to names on parse-back.
 * Names are copied into the map's own storage (malloc-owned, independent of
 * any tp_arena). */

#include <stdint.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_name_map tp_name_map;

tp_name_map *tp_name_map_create(void);
void tp_name_map_destroy(tp_name_map *map);

/* Hashes `name` (nt_hash64_str) and inserts. Re-inserting an identical name is
 * a no-op (OK). Two distinct names that hash equal -> TP_STATUS_HASH_COLLISION. */
tp_status tp_name_map_insert(tp_name_map *map, const char *name);

/* Same insert logic but with a caller-supplied hash instead of hashing `name`
 * -- lets tests force a collision without finding two real xxh64-colliding
 * strings. Not for production use; production callers should use insert(). */
tp_status tp_name_map_insert_hashed(tp_name_map *map, uint64_t hash, const char *name);

/* NULL on miss. */
const char *tp_name_map_lookup(const tp_name_map *map, uint64_t hash);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_NAME_MAP_H */
