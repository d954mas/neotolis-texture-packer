#ifndef TP_SOURCE_RUNTIME_INTERNAL_H
#define TP_SOURCE_RUNTIME_INTERNAL_H

#include "tp_core/tp_session.h"
#include "tp_core/tp_source_runtime.h"

tp_status tp_source_runtime_build(
    const tp_session_snapshot *snapshot,
    tp_source_runtime_projection **out, tp_error *err);
tp_source_runtime_projection *tp_source_runtime_clone(
    const tp_source_runtime_projection *projection);
void tp_source_runtime_destroy(
    tp_source_runtime_projection *projection);
void tp_source_runtime_diff(
    const tp_source_runtime_projection *before,
    const tp_source_runtime_projection *after,
    int *out_added, int *out_removed, int *out_changed,
    int *out_unavailable);

#endif /* TP_SOURCE_RUNTIME_INTERNAL_H */
