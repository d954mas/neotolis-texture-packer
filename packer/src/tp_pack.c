#include "tp_core/tp_pack.h"

/* Implemented in Phase 1b task 9. */
tp_status tp_pack(const struct tp_pack_settings *settings, struct tp_arena *arena, struct tp_result **out,
                   tp_error *err) {
    (void)settings;
    (void)arena;
    if (out) {
        *out = NULL;
    }
    return tp_error_set(err, TP_STATUS_UNIMPLEMENTED, "tp_pack: not implemented (Phase 1b task 9)");
}
