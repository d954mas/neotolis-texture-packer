#ifndef TP_CORE_TP_SOURCE_PLAN_H
#define TP_CORE_TP_SOURCE_PLAN_H

#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One accepted source request. `path` is the owned absolute lexical spelling
 * submitted to the operation layer; input_index maps it back to the request.
 * Callers borrow both fields until tp_source_batch_plan_free(). */
typedef struct tp_source_batch_item {
    int input_index;
    const char *path;
} tp_source_batch_item;

typedef struct tp_source_plan_storage tp_source_plan_storage;

typedef struct tp_source_batch_plan {
    tp_source_batch_item *items;
    int count;
    int duplicate_count;
    tp_source_plan_storage *storage;
} tp_source_batch_plan;

/* Plans one atomic add batch against an immutable snapshot. Empty inputs are
 * ignored. Existing sources and earlier accepted inputs are compared by one
 * native path identity: absolute lexical spelling first, then filesystem
 * canonical identity when both paths can be resolved. Thus ./, ../ and
 * symlink/reparse aliases collapse without requiring frontend policy. */
tp_status tp_source_batch_plan_create(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *const *inputs, int input_count, tp_source_batch_plan *out,
    tp_error *err);
void tp_source_batch_plan_free(tp_source_batch_plan *plan);

/* Resolves one request path through the same identity owner and returns the
 * matching immutable source DTO. */
tp_status tp_source_snapshot_find(const tp_session_snapshot *snapshot,
                                  tp_id128 atlas_id, const char *input,
                                  const tp_snapshot_source **out,
                                  tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SOURCE_PLAN_H */
