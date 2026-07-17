#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#endif

#include "tp_core/tp_project_lease.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_fs_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define TP_PROJECT_LEASE_SUFFIX ".ntpacker.lock"
#define TP_PROJECT_LEASE_PATH_MAX (TP_IDENTITY_PATH_MAX + sizeof(TP_PROJECT_LEASE_SUFFIX))

struct tp_project_lease {
    char identity[TP_IDENTITY_PATH_MAX];
    char lock_path[TP_PROJECT_LEASE_PATH_MAX];
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

static tp_status lease_storage_error(tp_error *err, const char *operation,
                                     unsigned long code) {
    return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                        "project lease %s failed for the lock file (error %lu)",
                        operation, code);
}

#ifdef _WIN32
static tp_status lock_open(tp_project_lease *lease, tp_error *err) {
    wchar_t *wide = tp_fs_win32_path_alloc(lease->lock_path);
    if (!wide) {
        return lease_storage_error(err, "path conversion", (unsigned long)errno);
    }

    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                0, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    const DWORD open_error =
        handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = open_error;
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
            return tp_error_set(err, TP_STATUS_PROJECT_LIVE,
                                "project '%s' is already leased by another writer",
                                lease->identity);
        }
        return lease_storage_error(err, "open", code);
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD code = GetLastError();
        (void)CloseHandle(handle);
        return lease_storage_error(err, "inspect", code);
    }
    if ((info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        (void)CloseHandle(handle);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "project lease lock path is not a regular file");
    }

    lease->handle = handle;
    return TP_STATUS_OK;
}
#else
static tp_status lock_open(tp_project_lease *lease, tp_error *err) {
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(lease->lock_path, flags, 0600);
    if (fd < 0) {
        return lease_storage_error(err, "open", (unsigned long)errno);
    }

    struct stat info;
    if (fstat(fd, &info) != 0) {
        const int code = errno;
        (void)close(fd);
        return lease_storage_error(err, "inspect", (unsigned long)code);
    }
    if (!S_ISREG(info.st_mode)) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "project lease lock path is not a regular file");
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        (void)close(fd);
        if (code == EWOULDBLOCK || code == EAGAIN) {
            return tp_error_set(err, TP_STATUS_PROJECT_LIVE,
                                "project '%s' is already leased by another writer",
                                lease->identity);
        }
        return lease_storage_error(err, "lock", (unsigned long)code);
    }

    lease->fd = fd;
    return TP_STATUS_OK;
}
#endif

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
#ifndef _WIN32
    lease->fd = -1;
#endif
    (void)snprintf(lease->identity, sizeof lease->identity, "%s", identity);
    const int written = snprintf(lease->lock_path, sizeof lease->lock_path,
                                 "%s%s", identity, TP_PROJECT_LEASE_SUFFIX);
    if (written < 0 || (size_t)written >= sizeof lease->lock_path) {
        free(lease);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "project lease lock path is too long");
    }

    status = lock_open(lease, err);
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
#ifdef _WIN32
    if (lease->handle && lease->handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(lease->handle);
    }
#else
    if (lease->fd >= 0) {
        (void)flock(lease->fd, LOCK_UN);
        (void)close(lease->fd);
    }
#endif
    free(lease);
}
