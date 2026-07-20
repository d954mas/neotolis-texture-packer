#if defined(_WIN32)
#define _WIN32_WINNT 0x0601
#endif

#include "tp_recovery_backend_types_internal.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "tp_fs_internal.h"
#include "tp_journal_internal.h"

static bool handle_is_regular_nofollow(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info;
    return GetFileInformationByHandle(handle, &info) &&
           (info.dwFileAttributes &
            (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0U;
}

tp_journal_io tp_recovery_backend_journal_read(const char *path) {
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

tp_journal_io tp_recovery_backend_live_create(const char *journal_path,
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

void tp_recovery_backend_live_close(tp_recovery_file_pin *pin) {
    (void)pin;
}

tp_status tp_recovery_backend_live_delete(const char *journal_path,
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

tp_status tp_recovery_backend_lock_open(tp_recovery_lock_pin *lock, const char *lock_path,
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

void tp_recovery_backend_lock_release(tp_recovery_lock_pin *lock) {
    HANDLE handle = (HANDLE)lock->native_handle;
    if (handle && handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
        lock->native_handle = -1;
    }
}

bool tp_recovery_backend_lock_is_unowned(const char *lock_path) {
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

tp_journal_io tp_recovery_backend_candidate_pin(tp_recovery_file_pin *pin,
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

void tp_recovery_backend_candidate_close(tp_recovery_file_pin *pin) {
    HANDLE handle = (HANDLE)pin->native_handle;
    if (handle && handle != INVALID_HANDLE_VALUE) {
        (void)CloseHandle(handle);
        pin->native_handle = -1;
    }
}

tp_status tp_recovery_backend_candidate_delete(tp_recovery_file_pin *pin,
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
    tp_recovery_backend_candidate_close(pin);
    return TP_STATUS_OK;
}
