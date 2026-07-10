#include "tp_core/tp_pack_read.h"

/* Implemented in Phase 1a task 4-5. */
tp_status tp_pack_read_file(const char *path, struct tp_arena *arena, struct tp_result **out, tp_error *err) {
    (void)path;
    (void)arena;
    if (out) {
        *out = NULL;
    }
    return tp_error_set(err, TP_STATUS_UNIMPLEMENTED, "tp_pack_read_file: not implemented (Phase 1a task 4-5)");
}

tp_status tp_pack_read_memory(const void *data, size_t size, struct tp_arena *arena, struct tp_result **out,
                               tp_error *err) {
    (void)data;
    (void)size;
    (void)arena;
    if (out) {
        *out = NULL;
    }
    return tp_error_set(err, TP_STATUS_UNIMPLEMENTED, "tp_pack_read_memory: not implemented (Phase 1a task 4-5)");
}
