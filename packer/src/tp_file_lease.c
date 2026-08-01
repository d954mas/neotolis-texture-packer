#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#endif

#include "tp_file_lease.h"

#include <errno.h>
#include <stdlib.h>

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

struct tp_file_lease {
#ifdef _WIN32
    HANDLE handle;
#else
    int fd;
#endif
};

static tp_status lease_storage_error(tp_error *err, const char *operation,
                                     const char *path, unsigned long code) {
    return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                        "file lease %s failed for '%s' (error %lu)",
                        operation, path, code);
}

static tp_status lease_busy(tp_error *err, tp_status status,
                            const char *kind, const char *identity) {
    return tp_error_set(err, status,
                        "%s '%s' is already leased by another writer",
                        kind, identity);
}

#ifdef _WIN32
static tp_status lease_open(tp_file_lease *lease, const char *lock_path,
                            tp_status busy_status, const char *resource_kind,
                            const char *resource_identity, tp_error *err) {
    wchar_t *wide = tp_fs_win32_path_alloc(lock_path);
    if (!wide) {
        return lease_storage_error(err, "path conversion", lock_path,
                                   (unsigned long)errno);
    }

    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL |
                                    FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    const DWORD open_error =
        handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        if (open_error == ERROR_SHARING_VIOLATION ||
            open_error == ERROR_LOCK_VIOLATION) {
            return lease_busy(err, busy_status, resource_kind,
                              resource_identity);
        }
        return lease_storage_error(err, "open", lock_path,
                                   (unsigned long)open_error);
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info)) {
        const DWORD code = GetLastError();
        (void)CloseHandle(handle);
        return lease_storage_error(err, "inspect", lock_path,
                                   (unsigned long)code);
    }
    if ((info.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0U) {
        (void)CloseHandle(handle);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "file lease path is not a regular file: '%s'",
                            lock_path);
    }

    lease->handle = handle;
    return TP_STATUS_OK;
}
#else
static tp_status lease_open(tp_file_lease *lease, const char *lock_path,
                            tp_status busy_status, const char *resource_kind,
                            const char *resource_identity, tp_error *err) {
    if (!tp_fs_path_is_valid_utf8(lock_path) || lock_path[0] == '\0') {
        return lease_storage_error(err, "path validation", lock_path,
                                   (unsigned long)errno);
    }
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(lock_path, flags, 0600);
    if (fd < 0) {
        return lease_storage_error(err, "open", lock_path,
                                   (unsigned long)errno);
    }

    struct stat info;
    if (fstat(fd, &info) != 0) {
        const int code = errno;
        (void)close(fd);
        return lease_storage_error(err, "inspect", lock_path,
                                   (unsigned long)code);
    }
    if (!S_ISREG(info.st_mode)) {
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "file lease path is not a regular file: '%s'",
                            lock_path);
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        (void)close(fd);
        if (code == EWOULDBLOCK || code == EAGAIN) {
            return lease_busy(err, busy_status, resource_kind,
                              resource_identity);
        }
        return lease_storage_error(err, "lock", lock_path,
                                   (unsigned long)code);
    }

    lease->fd = fd;
    return TP_STATUS_OK;
}
#endif

tp_status tp_file_lease_acquire(const char *lock_path,
                                tp_status busy_status,
                                const char *resource_kind,
                                const char *resource_identity,
                                tp_file_lease **out,
                                tp_error *err) {
    if (!lock_path || !resource_kind || !resource_identity || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "file lease requires path, resource, and output");
    }
    *out = NULL;
    tp_file_lease *lease = (tp_file_lease *)calloc(1, sizeof *lease);
    if (!lease) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "file lease allocation failed");
    }
#ifndef _WIN32
    lease->fd = -1;
#endif
    const tp_status status =
        lease_open(lease, lock_path, busy_status, resource_kind,
                   resource_identity, err);
    if (status != TP_STATUS_OK) {
        free(lease);
        return status;
    }
    *out = lease;
    return TP_STATUS_OK;
}

void tp_file_lease_release(tp_file_lease *lease) {
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
