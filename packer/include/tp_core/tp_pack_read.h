#ifndef TP_CORE_TP_PACK_READ_H
#define TP_CORE_TP_PACK_READ_H

/* Parses the .ntpack the builder just wrote back into the canonical
 * tp_result model (plan §2). Implemented in Phase 1a task 4-5. */

#include <stddef.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;

tp_status tp_pack_read_file(const char *path, struct tp_arena *arena, struct tp_result **out, tp_error *err);
tp_status tp_pack_read_memory(const void *data, size_t size, struct tp_arena *arena, struct tp_result **out,
                               tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_READ_H */
