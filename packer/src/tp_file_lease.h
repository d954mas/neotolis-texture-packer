#ifndef TP_CORE_TP_FILE_LEASE_H
#define TP_CORE_TP_FILE_LEASE_H

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_file_lease tp_file_lease;

/* Acquires a non-blocking, process-scoped lease on the permanent regular file
 * at `lock_path`. The file is never removed: ownership is the pinned OS handle,
 * so a process exit releases it without an unsafe stale-file cleanup race.
 * `resource_kind` and `resource_identity` only describe a busy diagnostic. */
tp_status tp_file_lease_acquire(const char *lock_path,
                                tp_status busy_status,
                                const char *resource_kind,
                                const char *resource_identity,
                                tp_file_lease **out,
                                tp_error *err);

void tp_file_lease_release(tp_file_lease *lease);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_FILE_LEASE_H */
