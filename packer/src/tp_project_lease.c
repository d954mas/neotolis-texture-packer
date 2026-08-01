#include "tp_project_lease.h"

#include <stdio.h>
#include <stdlib.h>

#include "tp_core/tp_identity.h"
#include "tp_file_lease.h"

#define TP_PROJECT_LEASE_SUFFIX ".ntpacker.lock"
#define TP_PROJECT_LEASE_PATH_MAX (TP_IDENTITY_PATH_MAX + sizeof(TP_PROJECT_LEASE_SUFFIX))

struct tp_project_lease {
    tp_file_lease *file;
};

tp_status tp_project_lease_acquire(const char *project_path,
                                   tp_project_lease **out,
                                   tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "project lease output is required");
    }
    *out = NULL;

    char identity[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(project_path, identity,
                                                  sizeof identity, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    tp_project_lease *lease = (tp_project_lease *)calloc(1, sizeof *lease);
    if (!lease) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "project lease allocation failed");
    }
    char lock_path[TP_PROJECT_LEASE_PATH_MAX];
    const int written = snprintf(lock_path, sizeof lock_path,
                                 "%s%s", identity, TP_PROJECT_LEASE_SUFFIX);
    if (written < 0 || (size_t)written >= sizeof lock_path) {
        free(lease);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "project lease lock path is too long");
    }

    status = tp_file_lease_acquire(lock_path, TP_STATUS_PROJECT_LIVE,
                                   "project", identity, &lease->file, err);
    if (status != TP_STATUS_OK) {
        free(lease);
        return status;
    }
    *out = lease;
    return TP_STATUS_OK;
}

void tp_project_lease_release(tp_project_lease *lease) {
    if (!lease) {
        return;
    }
    tp_file_lease_release(lease->file);
    free(lease);
}
