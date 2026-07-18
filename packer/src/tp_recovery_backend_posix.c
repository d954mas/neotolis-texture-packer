#include "tp_recovery_backend_types_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include "tp_journal_internal.h"

static bool s_test_fail_next_quarantine_unlink;

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

tp_journal_io tp_recovery_backend_journal_read(const char *path) {
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

tp_journal_io tp_recovery_backend_live_create(const char *journal_path,
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

void tp_recovery_backend_live_close(tp_recovery_file_pin *pin) {
    if (pin->native_handle >= 0) {
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
    }
}

tp_status tp_recovery_backend_live_delete(const char *journal_path,
                                 tp_recovery_file_pin *pin, tp_error *err) {
    if (!journal_path || !pin || pin->native_handle < 0) {
        return tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                            "recovery live journal pin is unavailable");
    }
    const tp_status status = delete_pinned_path(
        journal_path, (int)pin->native_handle, (dev_t)pin->identity_high,
        (ino_t)pin->identity_low, err);
    if (status == TP_STATUS_OK) {
        tp_recovery_backend_live_close(pin);
    }
    return status;
}

tp_status tp_recovery_backend_lock_open(tp_recovery_lock_pin *lock, const char *lock_path,
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

void tp_recovery_backend_lock_release(tp_recovery_lock_pin *lock) {
    if (lock->native_handle >= 0) {
        (void)flock((int)lock->native_handle, LOCK_UN);
        (void)close((int)lock->native_handle);
        lock->native_handle = -1;
    }
}

bool tp_recovery_backend_lock_is_unowned(const char *lock_path) {
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

tp_journal_io tp_recovery_backend_candidate_pin(tp_recovery_file_pin *pin,
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

void tp_recovery_backend_candidate_close(tp_recovery_file_pin *pin) {
    if (pin->native_handle >= 0) {
        (void)close((int)pin->native_handle);
        pin->native_handle = -1;
    }
}

tp_status tp_recovery_backend_candidate_delete(tp_recovery_file_pin *pin,
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
        tp_recovery_backend_candidate_close(pin);
    }
    return status;
}

void tp_recovery_backend_test_fail_next_quarantine_unlink(void) {
    s_test_fail_next_quarantine_unlink = true;
}
