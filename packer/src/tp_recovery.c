#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#endif

#include "tp_core/tp_recovery.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_transaction.h"
#include "tp_fs_internal.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_recovery_backend_types_internal.h"
#include "tp_recovery_internal.h"
#include "tp_session_internal.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>
#endif

#define TP_RECOVERY_LOCK_SUFFIX ".lock"
#define TP_RECOVERY_LOCK_PATH_MAX (TP_IDENTITY_PATH_MAX + sizeof(TP_RECOVERY_LOCK_SUFFIX))
#define TP_RECOVERY_SCAN_MAX_FILES 256U
#define TP_RECOVERY_SCAN_MAX_BYTES ((uint64_t)TP_JOURNAL_MAX_FILE_BYTES * 2U)

// #region types & state
static tp_recovery_store *s_test_foreign_store;
static tp_recovery_claim *s_test_foreign_claim;
static bool s_test_fail_next_resolve_verify;
static bool s_test_fail_next_live_retire_cleanup;
#ifndef _WIN32
static bool s_test_fail_next_quarantine_unlink;
#endif

struct tp_recovery_store {
    char root[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
};

struct tp_recovery_claim {
    tp_recovery_lock_pin lock;
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
    tp_recovery_owned_candidate *candidate;
    tp_recovery_resolution *resolution;
};

struct tp_recovery_owned_candidate {
    tp_recovery_claim *owner;
    tp_project *project;
    tp_journal_meta metadata;
    tp_id128 recovery_token;
    bool has_metadata;
    bool has_recovery_token;
    tp_recovery_file_pin journal_pin;
};

struct tp_recovery_resolution {
    tp_recovery_owned_candidate *candidate;
    tp_session *session;
    tp_project_lease *project_lease;
    tp_session_save_result last_receipt;
    bool has_receipt;
};

struct tp_recovery_live {
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_id128 journal_key;
    tp_recovery_lock_pin lock;
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    tp_recovery_file_pin journal_pin;
    tp_model *attached_model;
    int64_t metadata_timestamp;
    char metadata_name[256];
    bool healthy;
    bool finished;
    tp_status terminal_status;
};
// #endregion

// #region store paths
static bool has_journal_suffix(const char *name) {
    static const char suffix[] = ".ntpjournal";
    const size_t len = name ? strlen(name) : 0U;
    return len >= sizeof suffix - 1U &&
           strcmp(name + len - (sizeof suffix - 1U), suffix) == 0;
}

static const char *path_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static tp_status store_journal_path(const tp_recovery_store *store, const char *input,
                                    char *journal, size_t journal_cap, tp_error *err) {
    if (!store || !input || input[0] == '\0' || !journal || journal_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal path is required");
    }
    const char *base = path_basename(input);
    if (base == input || base[0] == '\0' || !has_journal_suffix(base)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal must be an .ntpjournal file under the store root");
    }
    size_t parent_len = (size_t)(base - input);
    while (parent_len > 0U && (input[parent_len - 1U] == '/' || input[parent_len - 1U] == '\\')) {
        parent_len--;
    }
    if (parent_len == 0U || parent_len >= TP_IDENTITY_PATH_MAX) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal parent path is invalid");
    }
    char parent[TP_IDENTITY_PATH_MAX];
    memcpy(parent, input, parent_len);
    parent[parent_len] = '\0';
    char canonical_parent[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(parent, canonical_parent,
                                                  sizeof canonical_parent, err);
    if (status != TP_STATUS_OK || !tp_identity_path_equal(canonical_parent, store->root)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal is outside the store root");
    }
    const int written = snprintf(journal, journal_cap, "%s/%s", store->root, base);
    if (written < 0 || (size_t)written >= journal_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery journal path is too long");
    }
    return TP_STATUS_OK;
}

static tp_status lock_path_for(const tp_recovery_store *store, const char *journal_input,
                               char *journal, size_t journal_cap,
                               char *lock, size_t lock_cap, tp_error *err) {
    tp_status status = store_journal_path(store, journal_input, journal, journal_cap, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const int written = snprintf(lock, lock_cap, "%s%s", journal, TP_RECOVERY_LOCK_SUFFIX);
    if (written < 0 || (size_t)written >= lock_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery claim lock path is too long");
    }
    return TP_STATUS_OK;
}

/* Recovery intentionally visits every directory entry, not only regular files:
 * a directory/reparse node with an .ntpjournal suffix must surface as a
 * structured unreadable-candidate diagnostic instead of disappearing. */
static bool recovery_visit_journals_utf8(const char *dir,
                                         tp_scan_name_visitor visit, void *ctx) {
    if (!dir || dir[0] == '\0' || !visit) {
        return false;
    }
    tp_fs_dir *stream = tp_fs_dir_open(dir);
    if (!stream) {
        return false;
    }
    tp_fs_dir_entry entry;
    tp_fs_dir_result next;
    while ((next = tp_fs_dir_next(stream, &entry)) == TP_FS_DIR_ENTRY) {
        const uint64_t size = entry.info.kind == TP_FS_KIND_REGULAR ? entry.info.size : 0U;
        if (!visit(ctx, entry.name, size)) {
            tp_fs_dir_close(stream);
            return true;
        }
    }
    tp_fs_dir_close(stream);
    return next == TP_FS_DIR_END;
}
// #endregion

#ifdef _WIN32
// #region OS file backend win32
static bool recovery_root_is_dir(const char *path) {
    tp_fs_info info;
    return tp_fs_stat(path, &info) && info.kind == TP_FS_KIND_DIRECTORY && !info.reparse;
}

static bool recovery_visit_journals(const char *dir,
                                     tp_scan_name_visitor visit, void *ctx) {
    return recovery_visit_journals_utf8(dir, visit, ctx);
}

static bool handle_is_regular_nofollow(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(handle, &info) &&
           (info.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
}

static tp_journal_io journal_read_nofollow(const char *path) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    WCHAR *wide = tp_fs_win32_path_alloc(path);
    if (!wide) {
        return io;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        return io;
    }
    if (!handle_is_regular_nofollow(handle)) {
        (void)CloseHandle(handle);
        return io;
    }
    const int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDONLY);
    if (fd < 0) {
        (void)CloseHandle(handle);
        return io;
    }
    return tp_journal_io_file_adopt_fd_read(fd);
}

static tp_journal_io live_io_create_new(const char *journal_path,
                                        tp_recovery_file_pin *pin,
                                        tp_status *status_out, tp_error *err) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    *status_out = TP_STATUS_JOURNAL_FAILED;
    WCHAR *wide = tp_fs_win32_path_alloc(journal_path);
    if (!wide) {
        (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                           "recovery live journal path conversion failed");
        return io;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, NULL, CREATE_NEW,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    const DWORD open_code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = open_code;
        if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS) {
            *status_out = TP_STATUS_RECOVERY_BUSY;
            (void)tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                               "recovery live journal already exists");
        } else {
            (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                               "recovery live journal create failed (error %lu)",
                               (unsigned long)code);
        }
        return io;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!handle_is_regular_nofollow(handle) ||
        !GetFileInformationByHandle(handle, &info)) {
        (void)CloseHandle(handle);
        (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                           "recovery live journal is not a regular file");
        return io;
    }
    const int fd = _open_osfhandle((intptr_t)handle, _O_BINARY | _O_RDWR);
    if (fd < 0) {
        (void)CloseHandle(handle);
        (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                           "recovery live journal descriptor conversion failed");
        return io;
    }
    pin->identity_high = info.dwVolumeSerialNumber;
    pin->identity_low = ((uint64_t)info.nFileIndexHigh << 32U) |
                        (uint64_t)info.nFileIndexLow;
    pin->has_identity = true;
    io = tp_journal_io_file_adopt_fd(fd);
    if (!io.ctx) {
        pin->identity_high = 0U;
        pin->identity_low = 0U;
        pin->has_identity = false;
        *status_out = TP_STATUS_OOM;
        (void)tp_error_set(err, TP_STATUS_OOM,
                           "recovery live journal I/O allocation failed");
    }
    return io;
}

static void live_close_pin(tp_recovery_file_pin *pin) {
    (void)pin;
}

static tp_status live_delete_pin(const char *journal_path,
                                 tp_recovery_file_pin *pin, tp_error *err) {
    if (!journal_path || !pin || !pin->has_identity) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal pin is unavailable");
    }
    WCHAR *wide = tp_fs_win32_path_alloc(journal_path);
    if (!wide) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal path conversion failed");
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | DELETE, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    const DWORD open_code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal could not be pinned for deletion (error %lu)",
                            (unsigned long)open_code);
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!handle_is_regular_nofollow(handle) ||
        !GetFileInformationByHandle(handle, &info) ||
        (uint64_t)info.dwVolumeSerialNumber != pin->identity_high ||
        (((uint64_t)info.nFileIndexHigh << 32U) |
         (uint64_t)info.nFileIndexLow) != pin->identity_low) {
        (void)CloseHandle(handle);
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal path no longer names the pinned slot");
    }
    FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
    if (!SetFileInformationByHandle(handle,
                                    FileDispositionInfo, &disposition,
                                    sizeof disposition)) {
        const DWORD code = GetLastError();
        (void)CloseHandle(handle);
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal could not be deleted (error %lu)",
                            (unsigned long)code);
    }
    (void)CloseHandle(handle);
    return TP_STATUS_OK;
}

static tp_status lock_open(tp_recovery_lock_pin *lock, const char *lock_path,
                           tp_error *err) {
    WCHAR *wide = tp_fs_win32_path_alloc(lock_path);
    if (!wide) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock path conversion failed");
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    const DWORD open_code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = open_code;
        if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
            return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                                "recovery journal is claimed by another process");
        }
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock open failed (error %lu)",
                            (unsigned long)code);
    }
    if (!handle_is_regular_nofollow(handle)) {
        (void)CloseHandle(handle);
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock path is not a regular file");
    }
    lock->native_handle = (intptr_t)handle;
    return TP_STATUS_OK;
}

static void lock_release(tp_recovery_lock_pin *lock) {
    HANDLE handle = (HANDLE)lock->native_handle;
    if (handle && handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
        lock->native_handle = -1;
    }
}

static bool lock_is_unowned(const char *lock_path) {
    WCHAR *wide = tp_fs_win32_path_alloc(lock_path);
    if (!wide) {
        return false;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    const DWORD open_code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD code = open_code;
        return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }
    const bool unowned = handle_is_regular_nofollow(handle);
    (void)CloseHandle(handle);
    return unowned;
}

static tp_journal_io candidate_pin(tp_recovery_file_pin *pin,
                                   const char *path, tp_status *status_out,
                                   tp_error *err) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    *status_out = TP_STATUS_BAD_PROJECT;
    WCHAR *wide = tp_fs_win32_path_alloc(path);
    if (!wide) {
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal path conversion failed");
        return io;
    }
    HANDLE handle = CreateFileW(wide, GENERIC_READ | DELETE, FILE_SHARE_READ,
                                NULL, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                                NULL);
    const DWORD open_code = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal could not be pinned (error %lu)",
                           (unsigned long)open_code);
        return io;
    }
    BY_HANDLE_FILE_INFORMATION info;
    if (!handle_is_regular_nofollow(handle) ||
        !GetFileInformationByHandle(handle, &info)) {
        (void)CloseHandle(handle);
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal is not a regular file");
        return io;
    }
    HANDLE duplicate = INVALID_HANDLE_VALUE;
    if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                         &duplicate, GENERIC_READ, FALSE, 0)) {
        (void)CloseHandle(handle);
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal read handle could not be duplicated");
        return io;
    }
    const int fd = _open_osfhandle((intptr_t)duplicate, _O_BINARY | _O_RDONLY);
    if (fd < 0) {
        (void)CloseHandle(duplicate);
        (void)CloseHandle(handle);
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal read descriptor conversion failed");
        return io;
    }
    io = tp_journal_io_file_adopt_fd_read(fd);
    if (!io.ctx) {
        (void)CloseHandle(handle);
        *status_out = TP_STATUS_OOM;
        (void)tp_error_set(err, TP_STATUS_OOM,
                           "recovery journal read I/O allocation failed");
        return io;
    }
    pin->native_handle = (intptr_t)handle;
    pin->identity_high = info.dwVolumeSerialNumber;
    pin->identity_low = ((uint64_t)info.nFileIndexHigh << 32U) |
                        (uint64_t)info.nFileIndexLow;
    pin->has_identity = true;
    return io;
}

static void candidate_close_pin(tp_recovery_file_pin *pin) {
    HANDLE handle = (HANDLE)pin->native_handle;
    if (handle && handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
        pin->native_handle = -1;
    }
}

static tp_status candidate_delete_pin(tp_recovery_file_pin *pin,
                                      const char *journal_path,
                                      tp_error *err) {
    (void)journal_path;
    if (!pin || pin->native_handle == -1) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal pin is unavailable");
    }
    FILE_DISPOSITION_INFO disposition = {.DeleteFile = TRUE};
    if (!SetFileInformationByHandle((HANDLE)pin->native_handle,
                                    FileDispositionInfo, &disposition,
                                    sizeof disposition)) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal could not be deleted (error %lu)",
                            (unsigned long)GetLastError());
    }
    candidate_close_pin(pin);
    return TP_STATUS_OK;
}
// #endregion
#else
// #region OS file backend posix
static bool recovery_root_is_dir(const char *path) {
    tp_fs_info info;
    return tp_fs_stat(path, &info) && info.kind == TP_FS_KIND_DIRECTORY && !info.reparse;
}

static bool recovery_visit_journals(const char *dir,
                                     tp_scan_name_visitor visit, void *ctx) {
    return recovery_visit_journals_utf8(dir, visit, ctx);
}

static int rename_exclusive(const char *from, const char *to) {
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U)
#endif
    const long result = syscall(SYS_renameat2, AT_FDCWD, from,
                                AT_FDCWD, to, RENAME_NOREPLACE);
    return result == 0L ? 0 : -1;
#elif defined(__APPLE__)
    return renamex_np(from, to, RENAME_EXCL);
#else
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

/* POSIX has no portable unlink-by-fd. Move the public name atomically into a
 * private, no-replace quarantine first; only the moved inode is then compared
 * with the held pin. A mismatching replacement is restored when possible and
 * is never unlinked. The quarantine name is unique to pid+inode+attempt, which
 * leaves only an adversarial race on an unpublished private name. */
static tp_status delete_pinned_path(const char *path, int fd, dev_t device,
                                    ino_t inode, tp_error *err) {
    if (!path || fd < 0) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal pin is unavailable");
    }
    const char *slash = strrchr(path, '/');
    if (!slash) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal parent path is unavailable");
    }
    const size_t parent_len = (size_t)(slash - path) + 1U;
    char quarantine[TP_IDENTITY_PATH_MAX];
    bool moved = false;
    int move_error = ENOSPC;
    for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
        const int written = snprintf(
            quarantine, sizeof quarantine,
            "%.*s.ntpq-%ld-%llu-%u.ntpjournal", (int)parent_len, path,
            (long)getpid(), (unsigned long long)inode, attempt);
        if (written < 0 || (size_t)written >= sizeof quarantine) {
            return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                                "recovery quarantine path is too long");
        }
        if (rename_exclusive(path, quarantine) == 0) {
            moved = true;
            break;
        }
        move_error = errno;
        if (move_error != EEXIST) {
            break;
        }
    }
    if (!moved) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal could not enter quarantine (error %d)",
                            move_error);
    }

    struct stat moved_info;
    if (lstat(quarantine, &moved_info) != 0 ||
        !S_ISREG(moved_info.st_mode) || moved_info.st_dev != device ||
        moved_info.st_ino != inode) {
        const int inspect_error = errno;
        /* Restore only into an absent public name. Exclusive rename can never
         * overwrite a concurrently-created path; on failure the replacement is
         * retained under quarantine for manual recovery. */
        (void)rename_exclusive(quarantine, path);
        return tp_error_set(
            err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
            "recovery journal replacement was retained during cleanup (error %d)",
            inspect_error);
    }
    int unlink_result;
    if (s_test_fail_next_quarantine_unlink) {
        s_test_fail_next_quarantine_unlink = false;
        errno = EACCES;
        unlink_result = -1;
    } else {
        unlink_result = unlink(quarantine);
    }
    if (unlink_result != 0) {
        const int unlink_error = errno;
        /* A failed cleanup must not make the last durable journal disappear
         * from recovery discovery. Prefer restoring its public name. If a
         * concurrent replacement occupies that name, the quarantine itself
         * deliberately retains the .ntpjournal suffix and is therefore a
         * normal scan/claim candidate. Never overwrite the replacement. */
        (void)rename_exclusive(quarantine, path);
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery quarantine could not be deleted (error %d)",
                            unlink_error);
    }
    return TP_STATUS_OK;
}

static tp_journal_io journal_read_nofollow(const char *path) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0) {
        return io;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        (void)close(fd);
        return io;
    }
    return tp_journal_io_file_adopt_fd_read(fd);
}

static tp_journal_io live_io_create_new(const char *journal_path,
                                        tp_recovery_file_pin *pin,
                                        tp_status *status_out, tp_error *err) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    *status_out = TP_STATUS_JOURNAL_FAILED;
    int flags = O_CREAT | O_EXCL | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(journal_path, flags, 0600);
    if (fd < 0) {
        if (errno == EEXIST) {
            *status_out = TP_STATUS_RECOVERY_BUSY;
            (void)tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                               "recovery live journal already exists");
        } else {
            (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                               "recovery live journal create failed (error %d)", errno);
        }
        return io;
    }
    const int pin_fd = dup(fd);
    if (pin_fd < 0) {
        const int code = errno;
        (void)close(fd);
        (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                           "recovery live journal descriptor could not be pinned (error %d)",
                           code);
        return io;
    }
    pin->native_handle = pin_fd;
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        const int code = errno;
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
        (void)close(fd);
        (void)tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                           "recovery live journal is not a regular file (error %d)", code);
        return io;
    }
    pin->identity_high = (uint64_t)info.st_dev;
    pin->identity_low = (uint64_t)info.st_ino;
    pin->has_identity = true;
    io = tp_journal_io_file_adopt_fd(fd);
    if (!io.ctx) {
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
        pin->identity_high = 0U;
        pin->identity_low = 0U;
        pin->has_identity = false;
        *status_out = TP_STATUS_OOM;
        (void)tp_error_set(err, TP_STATUS_OOM,
                           "recovery live journal I/O allocation failed");
    }
    return io;
}

static void live_close_pin(tp_recovery_file_pin *pin) {
    if (pin->native_handle >= 0) {
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
    }
}

static tp_status live_delete_pin(const char *journal_path,
                                 tp_recovery_file_pin *pin, tp_error *err) {
    if (!journal_path || !pin || pin->native_handle < 0) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal pin is unavailable");
    }
    const tp_status status = delete_pinned_path(
        journal_path, (int)pin->native_handle, (dev_t)pin->identity_high,
        (ino_t)pin->identity_low, err);
    if (status == TP_STATUS_OK) {
        live_close_pin(pin);
    }
    return status;
}

static tp_status lock_open(tp_recovery_lock_pin *lock, const char *lock_path,
                           tp_error *err) {
    int flags = O_CREAT | O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(lock_path, flags, 0600);
    if (fd < 0) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock open failed (error %d)", errno);
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        const int code = errno;
        (void)close(fd);
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock path is not a regular file (error %d)", code);
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        const int code = errno;
        (void)close(fd);
        if (code == EWOULDBLOCK || code == EAGAIN) {
            return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                                "recovery journal is claimed by another process");
        }
        return tp_error_set(err, TP_STATUS_RECOVERY_CLAIM_FAILED,
                            "recovery claim lock failed (error %d)", code);
    }
    lock->native_handle = fd;
    return TP_STATUS_OK;
}

static void lock_release(tp_recovery_lock_pin *lock) {
    if (lock->native_handle >= 0) {
        (void)flock((int)lock->native_handle, LOCK_UN);
        (void)close((int)lock->native_handle);
        lock->native_handle = -1;
    }
}

static bool lock_is_unowned(const char *lock_path) {
    int flags = O_RDWR;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(lock_path, flags);
    if (fd < 0) {
        return errno == ENOENT;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        (void)close(fd);
        return false;
    }
    const bool unowned = flock(fd, LOCK_EX | LOCK_NB) == 0;
    if (unowned) {
        (void)flock(fd, LOCK_UN);
    }
    (void)close(fd);
    return unowned;
}

static tp_journal_io candidate_pin(tp_recovery_file_pin *pin,
                                   const char *path, tp_status *status_out,
                                   tp_error *err) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    *status_out = TP_STATUS_BAD_PROJECT;
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    const int fd = open(path, flags);
    if (fd < 0) {
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal could not be pinned (error %d)", errno);
        return io;
    }
    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        const int code = errno;
        (void)close(fd);
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal is not a regular file (error %d)", code);
        return io;
    }
    const int read_fd = dup(fd);
    if (read_fd < 0) {
        (void)close(fd);
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "recovery journal read descriptor could not be duplicated");
        return io;
    }
    io = tp_journal_io_file_adopt_fd_read(read_fd);
    if (!io.ctx) {
        (void)close(fd);
        *status_out = TP_STATUS_OOM;
        (void)tp_error_set(err, TP_STATUS_OOM,
                           "recovery journal read I/O allocation failed");
        return io;
    }
    pin->native_handle = fd;
    pin->identity_high = (uint64_t)info.st_dev;
    pin->identity_low = (uint64_t)info.st_ino;
    pin->has_identity = true;
    return io;
}

static void candidate_close_pin(tp_recovery_file_pin *pin) {
    if (pin->native_handle >= 0) {
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
    }
}

static tp_status candidate_delete_pin(tp_recovery_file_pin *pin,
                                      const char *journal_path,
                                      tp_error *err) {
    if (!pin || pin->native_handle < 0) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery journal pin is unavailable");
    }
    const tp_status status = delete_pinned_path(
        journal_path, (int)pin->native_handle,
        (dev_t)pin->identity_high, (ino_t)pin->identity_low, err);
    if (status == TP_STATUS_OK) {
        candidate_close_pin(pin);
    }
    return status;
}
// #endregion
#endif

// #region store & live slot
tp_status tp_recovery_store_create(const char *root, tp_id128 journal_key,
                                   tp_recovery_store **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery store output is required");
    }
    *out = NULL;
    tp_recovery_store *store = (tp_recovery_store *)calloc(1, sizeof *store);
    if (!store) {
        return tp_error_set(err, TP_STATUS_OOM, "recovery store allocation failed");
    }
    tp_status status = tp_identity_path_canonical(root, store->root,
                                                  sizeof store->root, err);
    if (status != TP_STATUS_OK || !recovery_root_is_dir(store->root)) {
        free(store);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                  "recovery root is not a directory");
    }
    store->journal_key = journal_key;
    *out = store;
    return TP_STATUS_OK;
}

void tp_recovery_store_destroy(tp_recovery_store *store) {
    free(store);
}

tp_status tp_recovery_root_validate(const char *root, tp_id128 journal_key,
                                    tp_error *err) {
    tp_recovery_store *store = NULL;
    const tp_status status =
        tp_recovery_store_create(root, journal_key, &store, err);
    tp_recovery_store_destroy(store);
    return status;
}

static tp_status recovery_live_slot_generate(const tp_recovery_store *store,
                                             const tp_rng *rng, char *out,
                                             size_t out_cap, tp_error *err) {
    if (!store || !rng || !rng->fill || !out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live slot requires store, RNG, and output");
    }
    tp_id128 slot_id = tp_id128_nil();
    tp_status status = tp_id128_generate(rng, &slot_id, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char hex[33];
    for (size_t i = 0U; i < sizeof slot_id.bytes; ++i) {
        (void)snprintf(hex + i * 2U, 3U, "%02x", slot_id.bytes[i]);
    }
    const int written = snprintf(out, out_cap, "%s/%s.ntpjournal",
                                 store->root, hex);
    if (written < 0 || (size_t)written >= out_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery live-slot path is too long");
    }
    return TP_STATUS_OK;
}

tp_status tp_recovery_store_create_live(tp_recovery_store *store,
                                        const char *journal_path,
                                        tp_recovery_live **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live output is required");
    }
    *out = NULL;
    tp_recovery_live *live = (tp_recovery_live *)calloc(1, sizeof *live);
    if (!live) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery live allocation failed");
    }
    live->lock = TP_RECOVERY_LOCK_PIN_INIT;
    live->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status status = lock_path_for(store, journal_path, live->journal_path,
                                     sizeof live->journal_path,
                                     live->lock_path,
                                     sizeof live->lock_path, err);
    if (status == TP_STATUS_OK) {
        status = lock_open(&live->lock, live->lock_path, err);
    }
    if (status != TP_STATUS_OK) {
        free(live);
        return status;
    }
    live->journal_key = store->journal_key;
    live->healthy = true;
    *out = live;
    return TP_STATUS_OK;
}

static tp_status live_set_metadata(tp_recovery_live *live,
                                   const tp_recovery_metadata *metadata,
                                   tp_error *err) {
    const char *path = metadata->project_path ? metadata->project_path : "";
    const char *name = metadata->project_name ? metadata->project_name : "";
    tp_status status = tp_model_set_recovery_metadata_ex(
        live->attached_model, metadata->timestamp, path, name,
        metadata->file_fingerprint, err);
    if (status != TP_STATUS_OK) {
        tp_model_detach_journal(live->attached_model);
        live->attached_model = NULL;
        live->healthy = false;
    } else {
        live->metadata_timestamp = metadata->timestamp;
        (void)snprintf(live->metadata_name, sizeof live->metadata_name,
                       "%s", name);
    }
    return status;
}

tp_status tp_recovery_live__update_saved_identity(
    tp_recovery_live *live, const char *canonical_path,
    const tp_id128 *file_fingerprint, tp_error *err) {
    if (!live || !canonical_path || canonical_path[0] == '\0' ||
        !file_fingerprint) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "saved recovery identity requires live owner, path, and fingerprint");
    }
    const tp_recovery_metadata metadata = {
        .timestamp = live->metadata_timestamp,
        .project_path = canonical_path,
        .project_name = path_basename(canonical_path),
        .file_fingerprint = file_fingerprint,
    };
    return tp_recovery_live_update_metadata(live, &metadata, err);
}

tp_status tp_recovery_live_attach(tp_recovery_live *live, tp_model *model,
                                  const tp_recovery_metadata *metadata,
                                  tp_error *err) {
    if (!live || !model || !metadata || live->finished || live->attached_model) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live attach requires an open unattached live handle, model, and metadata");
    }
    tp_status io_status = TP_STATUS_JOURNAL_FAILED;
    tp_journal_io io = live_io_create_new(live->journal_path,
                                          &live->journal_pin,
                                          &io_status, err);
    if (!io.ctx) {
        live->healthy = false;
        return io_status;
    }
    if (!io.length || io.length(io.ctx) != 0U) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        live->healthy = false;
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "recovery live slot is not empty after reset");
    }
    tp_journal *journal = tp_journal_create(io, live->journal_key);
    if (!journal) {
        live->healthy = false;
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery live journal allocation failed");
    }
    tp_status status = tp_model_attach_journal(model, journal, err);
    if (status != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        live->healthy = false;
        return status;
    }
    live->attached_model = model;
    return live_set_metadata(live, metadata, err);
}

tp_status tp_recovery_live_update_metadata(tp_recovery_live *live,
                                           const tp_recovery_metadata *metadata,
                                           tp_error *err) {
    if (!live || !metadata || live->finished || !live->attached_model ||
        !live->healthy) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery metadata update requires a healthy attached live handle");
    }
    return live_set_metadata(live, metadata, err);
}

bool tp_recovery_live_healthy(const tp_recovery_live *live) {
    return live && live->healthy && !live->finished;
}

void tp_recovery_live__mark_degraded(tp_recovery_live *live) {
    if (live && !live->finished) {
        live->healthy = false;
    }
}

const char *tp_recovery_live_journal_path(const tp_recovery_live *live) {
    return live ? live->journal_path : NULL;
}

static tp_status live_finish(tp_recovery_live *live, bool preserve_journal,
                             bool retire_unhealthy, tp_error *err) {
    if (!live) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live handle is required");
    }
    if (live->finished) {
        return live->terminal_status;
    }
    if (live->attached_model) {
        tp_model_detach_journal(live->attached_model);
        live->attached_model = NULL;
    }
    tp_status status = TP_STATUS_OK;
    if (!preserve_journal && (live->healthy || retire_unhealthy)) {
        if (retire_unhealthy && s_test_fail_next_live_retire_cleanup) {
            s_test_fail_next_live_retire_cleanup = false;
            status = tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                                  "injected recovery live-retire cleanup failure");
        } else {
            status = live_delete_pin(live->journal_path,
                                     &live->journal_pin, err);
        }
    }
    live_close_pin(&live->journal_pin);
    lock_release(&live->lock);
    live->finished = true;
    live->terminal_status = status;
    return status;
}

tp_status tp_recovery_live_finish(tp_recovery_live *live,
                                  bool preserve_journal, tp_error *err) {
    return live_finish(live, preserve_journal, false, err);
}

tp_status tp_recovery_live_retire(tp_recovery_live *live, tp_error *err) {
    return live_finish(live, false, true, err);
}

void tp_recovery_live_destroy(tp_recovery_live *live) {
    if (!live) {
        return;
    }
    (void)tp_recovery_live_finish(live, true, NULL);
    free(live);
}
// #endregion

// #region claim & candidate
tp_status tp_recovery_store_claim(tp_recovery_store *store, const char *journal_path,
                                  tp_recovery_claim **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery claim output is required");
    }
    *out = NULL;
    tp_recovery_claim *claim = (tp_recovery_claim *)calloc(1, sizeof *claim);
    if (!claim) {
        return tp_error_set(err, TP_STATUS_OOM, "recovery claim allocation failed");
    }
    claim->lock = TP_RECOVERY_LOCK_PIN_INIT;
    char journal[TP_IDENTITY_PATH_MAX];
    tp_status status = lock_path_for(store, journal_path, journal, sizeof journal,
                                     claim->lock_path, sizeof claim->lock_path, err);
    if (status == TP_STATUS_OK) {
        status = lock_open(&claim->lock, claim->lock_path, err);
    }
    if (status != TP_STATUS_OK) {
        free(claim);
        return status;
    }
    (void)snprintf(claim->journal_path, sizeof claim->journal_path, "%s", journal);
    claim->journal_key = store->journal_key;
    *out = claim;
    return TP_STATUS_OK;
}

static void owned_candidate_destroy(tp_recovery_owned_candidate *candidate) {
    if (!candidate) {
        return;
    }
    candidate_close_pin(&candidate->journal_pin);
    tp_project_destroy(candidate->project);
    free(candidate->metadata.path);
    free(candidate->metadata.name);
    free(candidate);
}

void tp_recovery_claim_release(tp_recovery_claim *claim) {
    if (!claim) {
        return;
    }
    tp_recovery_resolution_destroy(claim->resolution);
    owned_candidate_destroy(claim->candidate);
    lock_release(&claim->lock);
    free(claim);
}

tp_status tp_recovery_claim_recover(tp_recovery_claim *claim,
                                    tp_recovery_owned_candidate **out,
                                    tp_error *err) {
    if (!claim || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery claim and candidate output are required");
    }
    *out = NULL;
    if (claim->candidate) {
        *out = claim->candidate;
        return TP_STATUS_OK;
    }
    tp_recovery_owned_candidate *candidate =
        (tp_recovery_owned_candidate *)calloc(1, sizeof *candidate);
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery candidate allocation failed");
    }
    candidate->owner = claim;
    candidate->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status pin_status = TP_STATUS_BAD_PROJECT;
    tp_journal_io io = candidate_pin(&candidate->journal_pin,
                                     claim->journal_path,
                                     &pin_status, err);
    if (!io.ctx) {
        owned_candidate_destroy(candidate);
        return pin_status;
    }
    tp_model *model = NULL;
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    tp_status status = tp_model_recover(io, claim->journal_key,
                                        &model, &recovery, err);
    if (status != TP_STATUS_OK || !model) {
        tp_journal_recovery_free(&recovery);
        owned_candidate_destroy(candidate);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "recovery journal has no recoverable state");
    }
    candidate->project = tp_project_clone(tp_model_project(model));
    if (recovery.has_metadata) {
        candidate->metadata = recovery.metadata;
        candidate->has_metadata = true;
        memset(&recovery.metadata, 0, sizeof recovery.metadata);
        recovery.has_metadata = false;
    }
    tp_model_destroy(model);
    tp_journal_recovery_free(&recovery);
    if (!candidate->project) {
        owned_candidate_destroy(candidate);
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovered project clone allocation failed");
    }
    claim->candidate = candidate;
    *out = candidate;
    return TP_STATUS_OK;
}
// #endregion

// #region resolution
tp_status tp_recovery_candidate_create_resolution(
    tp_recovery_owned_candidate *candidate, const tp_rng *rng,
    tp_recovery_resolution **out, tp_error *err) {
    if (!candidate || !candidate->project || !candidate->owner || !rng ||
        !rng->fill || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires candidate, RNG, and output");
    }
    *out = NULL;
    if (candidate->owner->resolution) {
        return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                            "recovery candidate already has an active resolution");
    }
    tp_project *project = tp_project_clone(candidate->project);
    if (!project) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "detached recovery project clone allocation failed");
    }
    tp_id128 token;
    tp_status status = tp_id128_generate(rng, &token, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return status;
    }
    tp_recovery_resolution *resolution =
        (tp_recovery_resolution *)calloc(1, sizeof *resolution);
    if (!resolution) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery resolution allocation failed");
    }
    status = tp_session_create_detached_recovery(project, rng, token,
                                                 &resolution->session, err);
    if (status != TP_STATUS_OK) {
        free(resolution);
        return status;
    }
    candidate->recovery_token = token;
    candidate->has_recovery_token = true;
    resolution->candidate = candidate;
    candidate->owner->resolution = resolution;
    *out = resolution;
    return status;
}

static tp_status resolution_save(tp_recovery_resolution *resolution,
                                 const char *target_path,
                                 const tp_id128 *expected_fingerprint,
                                 tp_session_save_result *receipt,
                                 tp_error *err) {
    if (!resolution || !resolution->session || !target_path ||
        target_path[0] == '\0' || !receipt) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "active recovery resolution, target, and receipt are required");
    }
    if (resolution->project_lease) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution already has an unfinalized save");
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(target_path, canonical,
                                                  sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (tp_identity_path_equal(canonical,
                               resolution->candidate->owner->journal_path)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovered project cannot be saved over its journal");
    }
    const tp_journal_meta *metadata = &resolution->candidate->metadata;
    if (!expected_fingerprint && resolution->candidate->has_metadata &&
        metadata->path && metadata->path[0] != '\0' &&
        tp_identity_path_equal(canonical, metadata->path)) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "Save As must not bypass the original-file fingerprint contract");
    }
    tp_project_lease *lease = NULL;
    status = tp_project_lease_acquire(canonical, &lease, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_session_save_detached_recovery(resolution->session, canonical,
                                               expected_fingerprint,
                                               receipt, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    resolution->project_lease = lease;
    resolution->last_receipt = *receipt;
    resolution->has_receipt = true;
    return TP_STATUS_OK;
}

tp_status tp_recovery_resolution_save_original(
    tp_recovery_resolution *resolution, tp_session_save_result *receipt,
    tp_error *err) {
    if (!resolution || !resolution->candidate ||
        !resolution->candidate->has_metadata ||
        !resolution->candidate->metadata.path ||
        resolution->candidate->metadata.path[0] == '\0' ||
        !resolution->candidate->metadata.has_file_fingerprint) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "recovery candidate has no exact original-file baseline; use Save As");
    }
    return resolution_save(resolution, resolution->candidate->metadata.path,
                           &resolution->candidate->metadata.file_fingerprint,
                           receipt, err);
}

tp_status tp_recovery_resolution_save_as(
    tp_recovery_resolution *resolution, const char *target_path,
    tp_session_save_result *receipt, tp_error *err) {
    return resolution_save(resolution, target_path, NULL, receipt, err);
}

static tp_status validate_save_receipt(const tp_recovery_claim *claim,
                                       const tp_recovery_owned_candidate *candidate,
                                       const tp_session_save_result *receipt,
                                       tp_error *err) {
    if (!receipt || !receipt->saved || !receipt->has_recovery_token ||
        !candidate->has_recovery_token ||
        !tp_id128_eq(receipt->recovery_token, candidate->recovery_token)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "save receipt is not bound to this recovery candidate");
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(receipt->target_path, canonical,
                                                  sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (tp_identity_path_equal(canonical, claim->journal_path)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovered project cannot be finalized over its journal");
    }
    tp_id128 current;
    status = tp_identity_file_fingerprint(canonical, &current, err);
    if (status != TP_STATUS_OK || !tp_id128_eq(current, receipt->file_fingerprint)) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "saved recovery target changed before finalize");
    }
    tp_project *verified = NULL;
    tp_id128 loaded_fingerprint;
    status = tp_project_load_with_fingerprint(canonical, &verified,
                                              &loaded_fingerprint, err);
    tp_project_destroy(verified);
    if (status != TP_STATUS_OK || !tp_id128_eq(loaded_fingerprint, current)) {
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "saved recovery target did not verify");
    }
    return TP_STATUS_OK;
}

static bool receipt_equals(const tp_session_save_result *a,
                           const tp_session_save_result *b) {
    return a && b && a->saved == b->saved &&
           a->has_recovery_token == b->has_recovery_token &&
           strcmp(a->target_path, b->target_path) == 0 &&
           tp_id128_eq(a->file_fingerprint, b->file_fingerprint) &&
           tp_id128_eq(a->recovery_token, b->recovery_token);
}

tp_status tp_recovery_resolution_finalize(
    tp_recovery_resolution *resolution,
    const tp_session_save_result *receipt, tp_error *err) {
    if (!resolution || !resolution->session || !resolution->project_lease ||
        !resolution->has_receipt ||
        !receipt_equals(receipt, &resolution->last_receipt)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "finalize requires the live resolution and its exact pending receipt");
    }
    tp_recovery_owned_candidate *candidate = resolution->candidate;
    tp_recovery_claim *claim = candidate->owner;
    tp_status status = validate_save_receipt(claim, candidate, receipt, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = candidate_delete_pin(&candidate->journal_pin,
                                  claim->journal_path, err);
    if (status == TP_STATUS_OK) {
        tp_project_lease_release(resolution->project_lease);
        resolution->project_lease = NULL;
        resolution->has_receipt = false;
    }
    return status;
}

void tp_recovery_resolution_cancel(tp_recovery_resolution *resolution) {
    if (!resolution) {
        return;
    }
    tp_project_lease_release(resolution->project_lease);
    resolution->project_lease = NULL;
    tp_session_destroy(resolution->session);
    resolution->session = NULL;
    resolution->has_receipt = false;
}

void tp_recovery_resolution_destroy(tp_recovery_resolution *resolution) {
    if (!resolution) {
        return;
    }
    tp_recovery_claim *claim = resolution->candidate
                                   ? resolution->candidate->owner
                                   : NULL;
    tp_recovery_resolution_cancel(resolution);
    if (claim && claim->resolution == resolution) {
        claim->resolution = NULL;
    }
    free(resolution);
}
// #endregion

// #region discard
tp_status tp_recovery_claim_discard(tp_recovery_claim *claim, tp_error *err) {
    if (!claim) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "discard requires a recovery claim");
    }
    if (claim->resolution) {
        return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                            "discard cannot race an active recovery resolution");
    }
    if (claim->candidate) {
        return candidate_delete_pin(&claim->candidate->journal_pin,
                                    claim->journal_path, err);
    }
    tp_recovery_owned_candidate *candidate =
        (tp_recovery_owned_candidate *)calloc(1, sizeof *candidate);
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "discard candidate allocation failed");
    }
    candidate->owner = claim;
    candidate->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status pin_status = TP_STATUS_BAD_PROJECT;
    tp_journal_io io = candidate_pin(&candidate->journal_pin,
                                     claim->journal_path,
                                     &pin_status, err);
    if (!io.ctx) {
        owned_candidate_destroy(candidate);
        return pin_status;
    }
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_status status = tp_journal_peek(io, &peek, err);
    const bool valid_domain = status == TP_STATUS_OK &&
                              memcmp(peek.key, claim->journal_key.bytes,
                                     sizeof peek.key) == 0 &&
                              peek.status != TP_JOURNAL_RECOVERY_EMPTY &&
                              peek.status != TP_JOURNAL_RECOVERY_BAD_MAGIC;
    tp_journal_peek_free(&peek);
    if (!valid_domain) {
        owned_candidate_destroy(candidate);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "discard requires a journal from this recovery domain");
    }
    status = candidate_delete_pin(&candidate->journal_pin,
                                  claim->journal_path, err);
    owned_candidate_destroy(candidate);
    return status;
}
// #endregion

// #region candidate ranking
static bool candidate_outranks(const tp_recovery_candidate *kept,
                               const tp_recovery_candidate *candidate) {
    if (kept->adoptable != candidate->adoptable) {
        return kept->adoptable;
    }
    if (kept->timestamp != candidate->timestamp) {
        return kept->timestamp > candidate->timestamp;
    }
    /* Directory enumeration order is not a contract and differs by platform.
     * The canonical journal path is unique within one root and gives equal-time
     * candidates (including cap eviction) a total deterministic order. */
    return strcmp(kept->journal_path, candidate->journal_path) <= 0;
}

static void candidate_insert(tp_recovery_candidates *out,
                             const tp_recovery_candidate *candidate) {
    size_t index = 0U;
    while (index < out->count && candidate_outranks(&out->items[index], candidate)) {
        index++;
    }
    if (out->count >= TP_RECOVERY_MAX_CANDIDATES) {
        out->has_more = true;
    }
    if (index >= TP_RECOVERY_MAX_CANDIDATES) {
        return;
    }
    size_t last = out->count < TP_RECOVERY_MAX_CANDIDATES
                      ? out->count
                      : TP_RECOVERY_MAX_CANDIDATES - 1U;
    while (last > index) {
        out->items[last] = out->items[last - 1U];
        last--;
    }
    out->items[index] = *candidate;
    if (out->count < TP_RECOVERY_MAX_CANDIDATES) {
        out->count++;
    }
}

static void scan_diagnostic_insert(tp_recovery_candidates *out,
                                   const char *journal_path,
                                   tp_status status) {
    size_t index = 0U;
    while (index < out->diagnostic_count &&
           strcmp(out->diagnostics[index].journal_path, journal_path) <= 0) {
        index++;
    }
    if (out->diagnostic_count >= TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        out->has_more = true;
    }
    if (index >= TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        return;
    }
    size_t last = out->diagnostic_count < TP_RECOVERY_MAX_SCAN_DIAGNOSTICS
                      ? out->diagnostic_count
                      : TP_RECOVERY_MAX_SCAN_DIAGNOSTICS - 1U;
    while (last > index) {
        out->diagnostics[last] = out->diagnostics[last - 1U];
        last--;
    }
    tp_recovery_scan_diagnostic *diagnostic = &out->diagnostics[index];
    memset(diagnostic, 0, sizeof *diagnostic);
    (void)snprintf(diagnostic->journal_path,
                   sizeof diagnostic->journal_path, "%s", journal_path);
    diagnostic->status = status;
    if (out->diagnostic_count < TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        out->diagnostic_count++;
    }
}

void tp_recovery__test_candidate_insert(tp_recovery_candidates *out,
                                        const tp_recovery_candidate *candidate) {
    if (out && candidate) {
        candidate_insert(out, candidate);
    }
}
// #endregion

// #region test seams
bool tp_recovery__test_hold_foreign_lock(
    const char *root, tp_id128 journal_key, const char *journal_path) {
    tp_recovery__test_release_foreign_lock();
    tp_error err = {{0}};
    return tp_recovery_store_create(root, journal_key, &s_test_foreign_store,
                                    &err) == TP_STATUS_OK &&
           tp_recovery_store_claim(s_test_foreign_store, journal_path,
                                   &s_test_foreign_claim, &err) == TP_STATUS_OK;
}

void tp_recovery__test_release_foreign_lock(void) {
    tp_recovery_claim_release(s_test_foreign_claim);
    s_test_foreign_claim = NULL;
    tp_recovery_store_destroy(s_test_foreign_store);
    s_test_foreign_store = NULL;
}

void tp_recovery__test_fail_next_resolve_verify(void) {
    s_test_fail_next_resolve_verify = true;
}

void tp_recovery__test_fail_next_live_retire_cleanup(void) {
    s_test_fail_next_live_retire_cleanup = true;
}

#ifndef _WIN32
void tp_recovery__test_fail_next_quarantine_unlink(void) {
    s_test_fail_next_quarantine_unlink = true;
}
#endif

tp_status tp_recovery__test_craft_metadata_journal(
    const char *path, tp_id128 key, int64_t timestamp,
    const char *project_path, const char *project_name, tp_error *err) {
    tp_journal_io io = tp_journal_io_file(path);
    if (!io.ctx) {
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "test journal could not be created");
    }
    tp_journal *journal = tp_journal_create(io, key);
    if (!journal) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "test journal allocation failed");
    }
    static const uint8_t snapshot[4] = {'r', '6', 'a', '!'};
    tp_status status = tp_journal_init_checkpoint(
        journal, snapshot, sizeof snapshot, 0, err);
    if (status == TP_STATUS_OK) {
        status = tp_journal_append_txn(
            journal, "6a0000000000000000000000000000ff", 1,
            snapshot, sizeof snapshot, err);
    }
    if (status == TP_STATUS_OK) {
        status = tp_journal_set_metadata(journal, timestamp,
                                         project_path, project_name, err);
    }
    tp_journal_destroy(journal);
    return status;
}

tp_status tp_recovery__test_peek_candidate(
    const char *path, tp_recovery_candidate *out, tp_error *err) {
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "test recovery peek requires path and output");
    }
    memset(out, 0, sizeof *out);
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_status status = tp_journal_peek(tp_journal_io_file_read(path),
                                       &peek, err);
    if (status == TP_STATUS_OK) {
        (void)snprintf(out->journal_path, sizeof out->journal_path, "%s", path);
        (void)snprintf(out->original_path, sizeof out->original_path, "%s",
                       peek.has_meta && peek.meta.path ? peek.meta.path : "");
        (void)snprintf(out->name, sizeof out->name, "%s",
                       peek.has_meta && peek.meta.name ? peek.meta.name : "");
        out->timestamp = peek.has_meta ? peek.meta.timestamp : 0;
        out->status = peek.status;
        if (peek.has_meta && peek.meta.has_file_fingerprint) {
            out->file_fingerprint = peek.meta.file_fingerprint;
            out->has_file_fingerprint = true;
        }
    }
    tp_journal_peek_free(&peek);
    return status;
}
// #endregion

// #region scan
typedef struct recovery_scan_context {
    tp_recovery_store *store;
    const char *live_basename;
    tp_recovery_candidates *out;
    uint64_t bytes_examined;
    unsigned files_examined;
    bool limit_reached;
} recovery_scan_context;

static bool scan_candidate(void *context, const char *name, uint64_t size) {
    recovery_scan_context *scan = (recovery_scan_context *)context;
    if (!name || name[0] == '\0') {
        return true;
    }
    /* Budget every entry the directory iterator examines, before suffix/live
     * filtering. Otherwise a root full of unrelated names, directories, or
     * symlinks makes startup work unbounded while consuming zero scan budget. */
    if (scan->files_examined >= TP_RECOVERY_SCAN_MAX_FILES ||
        size > TP_RECOVERY_SCAN_MAX_BYTES - scan->bytes_examined) {
        scan->limit_reached = true;
        scan->out->has_more = true;
        return false;
    }
    scan->files_examined++;
    scan->bytes_examined += size;
    if (!has_journal_suffix(name) ||
        (scan->live_basename[0] != '\0' && strcmp(name, scan->live_basename) == 0)) {
        return true;
    }

    char journal[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(journal, sizeof journal, "%s/%s", scan->store->root, name);
    if (written < 0 || (size_t)written >= sizeof journal) {
        scan_diagnostic_insert(scan->out, name, TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    const int lock_written = snprintf(lock_path, sizeof lock_path, "%s%s", journal,
                                      TP_RECOVERY_LOCK_SUFFIX);
    if (lock_written < 0 || (size_t)lock_written >= sizeof lock_path) {
        scan_diagnostic_insert(scan->out, journal,
                               TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    if (!lock_is_unowned(lock_path)) {
        return true;
    }
    tp_journal_io io = journal_read_nofollow(journal);
    if (!io.ctx) {
        scan_diagnostic_insert(scan->out, journal,
                               TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_error peek_err = {{0}};
    const tp_status peek_status = tp_journal_peek(io, &peek, &peek_err);
    if (peek_status != TP_STATUS_OK) {
        scan_diagnostic_insert(scan->out, journal, peek_status);
        tp_journal_peek_free(&peek);
        return true;
    }

    static const uint8_t empty_key[16] = {0};
    const bool has_header_key =
        memcmp(peek.key, empty_key, sizeof peek.key) != 0;
    if (peek.status == TP_JOURNAL_RECOVERY_BAD_MAGIC ||
        (!has_header_key &&
         (peek.status == TP_JOURNAL_RECOVERY_EMPTY ||
          peek.status == TP_JOURNAL_RECOVERY_TRUNCATED))) {
        scan_diagnostic_insert(scan->out, journal, TP_STATUS_BAD_PROJECT);
        tp_journal_peek_free(&peek);
        return true;
    }

    const bool key_matches = memcmp(peek.key, scan->store->journal_key.bytes,
                                    sizeof peek.key) == 0;
    const bool adoptable = key_matches && peek.has_checkpoint && peek.record_count > 1 &&
                           (peek.status == TP_JOURNAL_RECOVERY_OK ||
                            peek.status == TP_JOURNAL_RECOVERY_TRUNCATED ||
                            peek.status == TP_JOURNAL_RECOVERY_CORRUPT);
    const bool version_mismatch = key_matches &&
                                  peek.status == TP_JOURNAL_RECOVERY_VERSION_MISMATCH;
    if (key_matches && !adoptable && !version_mismatch) {
        scan_diagnostic_insert(scan->out, journal, TP_STATUS_BAD_PROJECT);
    }
    if (adoptable || version_mismatch) {
        tp_recovery_candidate candidate;
        memset(&candidate, 0, sizeof candidate);
        (void)snprintf(candidate.journal_path, sizeof candidate.journal_path, "%s", journal);
        const char *meta_path = peek.has_meta && peek.meta.path ? peek.meta.path : "";
        (void)snprintf(candidate.original_path, sizeof candidate.original_path, "%s", meta_path);
        if (peek.has_meta && peek.meta.name && peek.meta.name[0] != '\0') {
            (void)snprintf(candidate.name, sizeof candidate.name, "%s", peek.meta.name);
        } else if (meta_path[0] != '\0') {
            (void)snprintf(candidate.name, sizeof candidate.name, "%s", path_basename(meta_path));
        } else {
            (void)snprintf(candidate.name, sizeof candidate.name, "untitled");
        }
        candidate.timestamp = peek.has_meta ? peek.meta.timestamp : 0;
        candidate.status = peek.status;
        candidate.adoptable = adoptable;
        if (peek.has_meta && peek.meta.has_file_fingerprint) {
            candidate.file_fingerprint = peek.meta.file_fingerprint;
            candidate.has_file_fingerprint = true;
        }
        candidate_insert(scan->out, &candidate);
    }
    tp_journal_peek_free(&peek);
    return true;
}

tp_status tp_recovery_store_scan(tp_recovery_store *store, const char *live_slot,
                                 tp_recovery_candidates *out, tp_error *err) {
    if (!store || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery store and candidate output are required");
    }
    memset(out, 0, sizeof *out);
    const char *live_basename = path_basename(live_slot);
    recovery_scan_context scan = {
        .store = store,
        .live_basename = live_basename,
        .out = out,
    };
    if (!recovery_visit_journals(store->root, scan_candidate, &scan) &&
        !scan.limit_reached) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "recovery root could not be scanned");
    }
    return TP_STATUS_OK;
}
// #endregion

// #region session attach & resolve
static tp_status recovery_session_attach_store(
    tp_recovery_store *store, const char *journal_path, tp_session *session,
    const tp_recovery_metadata *metadata, tp_error *err) {
    if (!store || !journal_path || journal_path[0] == '\0' || !session ||
        !metadata) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery session attach requires session and metadata");
    }
    if (tp_session__has_recovery_owner(session)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    tp_status status = tp_session_require_recovery(session, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_recovery_live *live = NULL;
    status = tp_recovery_store_create_live(store, journal_path, &live, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_session_attach_recovery_live(session, live, metadata, err);
    /* The session accepts ownership before live I/O begins. Invalid
     * preconditions reject before transfer, so release that unaccepted owner. */
    if (status != TP_STATUS_OK &&
        !tp_session__owns_recovery_live(session, live)) {
        tp_recovery_live_destroy(live);
    }
    return status;
}

tp_status tp_recovery_session_attach(
    const char *root, tp_id128 journal_key, const tp_rng *rng,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err) {
    if (!session || !metadata || !rng || !rng->fill) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery session attach requires session, metadata, and RNG");
    }
    if (tp_session__has_recovery_owner(session)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    char journal_path[TP_IDENTITY_PATH_MAX];
    if (status == TP_STATUS_OK) {
        status = recovery_live_slot_generate(store, rng, journal_path,
                                             sizeof journal_path, err);
    }
    if (status == TP_STATUS_OK) {
        status = recovery_session_attach_store(store, journal_path, session,
                                               metadata, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

tp_status tp_recovery__test_session_attach_at(
    const char *root, tp_id128 journal_key, const char *journal_path,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err) {
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    char canonical_journal[TP_IDENTITY_PATH_MAX];
    if (status == TP_STATUS_OK) {
        status = store_journal_path(store, journal_path, canonical_journal,
                                    sizeof canonical_journal, err);
    }
    if (status == TP_STATUS_OK) {
        status = recovery_session_attach_store(store, canonical_journal,
                                               session, metadata, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

tp_status tp_recovery_scan_root(const char *root, tp_id128 journal_key,
                                const tp_session *live_session,
                                tp_recovery_candidates *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery scan output is required");
    }
    memset(out, 0, sizeof *out);
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_store_scan(
            store, tp_session__recovery_journal_path(live_session), out, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

static tp_status recovery_resolve_store(
    tp_recovery_store *store, const char *protected_live,
    const char *journal_path, tp_recovery_action action,
    const char *target_path, const tp_rng *rng,
    tp_recovery_resolve_result *out, tp_error *err) {
    if (!store || !out || !journal_path ||
        journal_path[0] == '\0' ||
        action < TP_RECOVERY_ACTION_DISCARD ||
        action > TP_RECOVERY_ACTION_SAVE_AS) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires journal, action, and output");
    }
    memset(out, 0, sizeof *out);
    tp_status status;
    char canonical_journal[TP_IDENTITY_PATH_MAX];
    status = store_journal_path(store, journal_path, canonical_journal,
                                sizeof canonical_journal, err);
    if (status == TP_STATUS_OK && protected_live && protected_live[0] != '\0') {
        char canonical_live[TP_IDENTITY_PATH_MAX];
        status = store_journal_path(store, protected_live, canonical_live,
                                    sizeof canonical_live, err);
        if (status == TP_STATUS_OK &&
            tp_identity_path_equal(canonical_journal, canonical_live)) {
            status = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "cannot resolve the live recovery journal of an open session");
        }
    }

    tp_recovery_claim *claim = NULL;
    tp_recovery_resolution *resolution = NULL;
    tp_session_save_result receipt;
    memset(&receipt, 0, sizeof receipt);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_store_claim(store, canonical_journal, &claim, err);
    }
    if (status == TP_STATUS_OK && action == TP_RECOVERY_ACTION_DISCARD) {
        status = tp_recovery_claim_discard(claim, err);
    } else if (status == TP_STATUS_OK) {
        if (!rng || !rng->fill) {
            status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "recovery save requires an RNG");
        }
        tp_recovery_owned_candidate *candidate = NULL;
        if (status == TP_STATUS_OK) {
            status = tp_recovery_claim_recover(claim, &candidate, err);
        }
        if (status == TP_STATUS_OK) {
            status = tp_recovery_candidate_create_resolution(candidate, rng,
                                                              &resolution, err);
        }
        if (status == TP_STATUS_OK) {
            status = action == TP_RECOVERY_ACTION_SAVE_ORIGINAL
                         ? tp_recovery_resolution_save_original(resolution,
                                                                &receipt, err)
                         : tp_recovery_resolution_save_as(resolution, target_path,
                                                          &receipt, err);
        }
        if (status == TP_STATUS_OK && s_test_fail_next_resolve_verify) {
            s_test_fail_next_resolve_verify = false;
            static const char invalid_project[] = "injected invalid project";
            (void)tp_fs_write_file(receipt.target_path, invalid_project, sizeof invalid_project - 1U);
        }
        if (status == TP_STATUS_OK) {
            status = tp_recovery_resolution_finalize(resolution, &receipt, err);
        }
    }
    if (status == TP_STATUS_OK) {
        out->journal_deleted = true;
        out->project_saved = action != TP_RECOVERY_ACTION_DISCARD;
        if (out->project_saved) {
            (void)snprintf(out->target_path, sizeof out->target_path, "%s",
                           receipt.target_path);
            out->file_fingerprint = receipt.file_fingerprint;
            out->has_file_fingerprint = true;
        }
    }
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    return status;
}

tp_status tp_recovery_resolve_journal(
    const char *root, tp_id128 journal_key, const char *journal_path,
    const tp_session *live_session, tp_recovery_action action,
    const char *target_path, const tp_rng *rng,
    tp_recovery_resolve_result *out, tp_error *err) {
    if (!out || !journal_path || journal_path[0] == '\0' ||
        action < TP_RECOVERY_ACTION_DISCARD ||
        action > TP_RECOVERY_ACTION_SAVE_AS ||
        (action != TP_RECOVERY_ACTION_DISCARD && (!rng || !rng->fill))) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires journal, action, output, and save RNG");
    }
    memset(out, 0, sizeof *out);
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    if (status == TP_STATUS_OK) {
        status = recovery_resolve_store(
            store, tp_session__recovery_journal_path(live_session),
            journal_path, action, target_path, rng, out, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}
// #endregion
