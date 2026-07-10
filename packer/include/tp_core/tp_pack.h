#ifndef TP_CORE_TP_PACK_H
#define TP_CORE_TP_PACK_H

/* Runs a pack through nt_builder and returns an owned tp_result (plan §4
 * task 9, ROADMAP 1b). Stub in Phase 1a so tp_core links; the settings input
 * and real implementation land in Phase 1b. */

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;
struct tp_pack_settings; /* defined in Phase 1b task 9 */

tp_status tp_pack(const struct tp_pack_settings *settings, struct tp_arena *arena, struct tp_result **out,
                   tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_H */
