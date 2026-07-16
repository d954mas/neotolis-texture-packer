#ifndef TP_CORE_TP_PROJECT_LEASE_H
#define TP_CORE_TP_PROJECT_LEASE_H

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_project_lease tp_project_lease;

/* Acquires the exclusive, non-transferable writer lease for the canonical
 * identity of `project_path`. The project may already exist or may be a
 * not-yet-created destination whose parent exists. The companion lock file is
 * permanent; release closes only the pinned OS handle and never unlinks it. */
tp_status tp_project_lease_acquire(const char *project_path,
                                   tp_project_lease **out,
                                   tp_error *err);

void tp_project_lease_release(tp_project_lease *lease);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PROJECT_LEASE_H */
