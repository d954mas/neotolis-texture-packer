#ifndef TP_CORE_TP_PACK_READ_H
#define TP_CORE_TP_PACK_READ_H

/* Parses the .ntpack the builder just wrote back into the canonical tp_result
 * model (plan §2). A single .ntpack may hold several atlases (owner decision:
 * project = multiple atlases), so both entry points return an array of
 * tp_result* -- one per NT_ASSET_ATLAS entry, in file order.
 *
 * `names` is the reverse map hash(name)->name (plan §2.8). Sprite region names
 * are resolved through it; a miss is a hard error (TP_STATUS_UNKNOWN_REGION).
 * Atlas display names are resolved the same way but fall back to a hex string
 * ("atlas_%016llx") on a miss instead of erroring (plan §2.8). May be NULL, in
 * which case every lookup misses.
 *
 * Every array/string/pixel buffer in the output is allocated from `arena`;
 * destroying the arena frees the whole result set. The input buffer / file is
 * NOT retained -- pixels and strings are copied into the arena.
 *
 * On success returns TP_STATUS_OK and writes *out_results / *out_count. On any
 * failure returns a tp_status and fills `err` (if non-NULL) with a precise
 * message; *out_results is set to NULL and *out_count to 0. Never asserts or
 * crashes on malformed input -- every offset is bounds-checked. */

#include <stddef.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;
struct tp_name_map;

tp_status tp_pack_read_memory(const void *data, size_t size, const struct tp_name_map *names, struct tp_arena *arena,
                              struct tp_result ***out_results, int *out_count, tp_error *err);

tp_status tp_pack_read_file(const char *path, const struct tp_name_map *names, struct tp_arena *arena,
                            struct tp_result ***out_results, int *out_count, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_READ_H */
