#include "tp_fs_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>

struct tp_fs_dir {
    HANDLE handle;
    WIN32_FIND_DATAW current;
    bool first;
};

static void win32_error_to_errno(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        errno = ENOENT;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        errno = EACCES;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        errno = EEXIST;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        errno = ENOSPC;
        break;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        errno = ENOMEM;
        break;
    case ERROR_FILENAME_EXCED_RANGE:
        errno = ENAMETOOLONG;
        break;
    case ERROR_DIR_NOT_EMPTY:
        errno = ENOTEMPTY;
        break;
    case ERROR_TOO_MANY_OPEN_FILES:
        errno = EMFILE;
        break;
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
}

static bool is_wide_separator(wchar_t c) {
    return c == L'/' || c == L'\\';
}

bool tp_fs_win32_utf8_to_utf16(const char *utf8, wchar_t *out, size_t cap) {
    if (!utf8 || !out || cap == 0U || cap > (size_t)INT_MAX) {
        errno = EINVAL;
        return false;
    }
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (required == 0) {
        errno = EILSEQ;
        return false;
    }
    if ((size_t)required > cap) {
        errno = ERANGE;
        return false;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, required) == 0) {
        errno = EILSEQ;
        return false;
    }
    return true;
}

bool tp_fs_win32_utf16_to_utf8(const wchar_t *wide, char *out, size_t cap) {
    if (!wide || !out || cap == 0U || cap > (size_t)INT_MAX) {
        errno = EINVAL;
        return false;
    }
    int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (required == 0) {
        errno = EILSEQ;
        return false;
    }
    if ((size_t)required > cap) {
        errno = ERANGE;
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, out, required, NULL, NULL) == 0) {
        errno = EILSEQ;
        return false;
    }
    return true;
}

wchar_t *tp_fs_win32_path_alloc(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return NULL;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8, -1, NULL, 0);
    if (count <= 0) {
        errno = EILSEQ;
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof *wide);
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8, -1, wide, count) == 0) {
        free(wide);
        errno = EILSEQ;
        return NULL;
    }

    size_t length = wcslen(wide);

    /* Raw device/verbatim namespaces are not accepted at this boundary. The
     * identity layer normalizes legitimate \\?\ drive/UNC aliases before I/O;
     * this backend itself generates a controlled extended prefix only for long
     * absolute paths below. */
    if (length >= 4U && is_wide_separator(wide[0]) && is_wide_separator(wide[1]) &&
        (wide[2] == L'.' || wide[2] == L'?') && is_wide_separator(wide[3])) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }

    if (length < (size_t)(MAX_PATH - 12)) {
        return wide;
    }

    /* Extended paths do not normalize '.'/'..'. Resolve those lexically with
     * GetFullPathNameW first, then add the controlled prefix. */
    DWORD needed = GetFullPathNameW(wide, 0U, NULL, NULL);
    if (needed == 0U) {
        DWORD error = GetLastError();
        free(wide);
        win32_error_to_errno(error);
        return NULL;
    }
    wchar_t *absolute = (wchar_t *)malloc((size_t)needed * sizeof *absolute);
    if (!absolute) {
        free(wide);
        errno = ENOMEM;
        return NULL;
    }
    DWORD copied = GetFullPathNameW(wide, needed, absolute, NULL);
    free(wide);
    if (copied == 0U || copied >= needed) {
        DWORD error = GetLastError();
        free(absolute);
        win32_error_to_errno(error);
        return NULL;
    }
    for (size_t i = 0; absolute[i] != L'\0'; i++) {
        if (absolute[i] == L'/') {
            absolute[i] = L'\\';
        }
    }
    length = wcslen(absolute);
    const bool resolved_unc = length >= 3U && absolute[0] == L'\\' && absolute[1] == L'\\';
    const bool resolved_drive = length >= 3U &&
                                ((absolute[0] >= L'A' && absolute[0] <= L'Z') ||
                                 (absolute[0] >= L'a' && absolute[0] <= L'z')) &&
                                absolute[1] == L':' && absolute[2] == L'\\';
    if (!resolved_unc && !resolved_drive) {
        free(absolute);
        errno = EINVAL;
        return NULL;
    }
    const wchar_t drive_prefix[] = L"\\\\?\\";
    const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
    size_t prefix_length = resolved_unc ? 8U : 4U;
    size_t skipped = resolved_unc ? 2U : 0U;
    if (length < skipped || length - skipped > SIZE_MAX - prefix_length - 1U) {
        free(absolute);
        errno = ENAMETOOLONG;
        return NULL;
    }
    size_t output_length = prefix_length + (length - skipped);
    if (output_length >= 32767U) {
        free(absolute);
        errno = ENAMETOOLONG;
        return NULL;
    }
    wchar_t *extended = (wchar_t *)malloc((output_length + 1U) * sizeof *extended);
    if (!extended) {
        free(absolute);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(extended, resolved_unc ? unc_prefix : drive_prefix, prefix_length * sizeof *extended);
    memcpy(extended + prefix_length, absolute + skipped, (length - skipped + 1U) * sizeof *extended);
    free(absolute);
    return extended;
}

bool tp_fs_win32_path_copy(const char *path_utf8, wchar_t *out, size_t cap) {
    if (!out || cap == 0U) {
        errno = EINVAL;
        return false;
    }
    out[0] = L'\0';
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return false;
    }
    const size_t required = wcslen(path) + 1U;
    if (required > cap) {
        free(path);
        errno = ERANGE;
        return false;
    }
    memcpy(out, path, required * sizeof *out);
    free(path);
    return true;
}

static bool mode_to_wide(const char *mode, wchar_t out[8]) {
    if (!mode) {
        errno = EINVAL;
        return false;
    }
    size_t length = strlen(mode);
    if (length == 0U || length >= 8U) {
        errno = EINVAL;
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)mode[i];
        if (c > 0x7FU) {
            errno = EINVAL;
            return false;
        }
        out[i] = (wchar_t)c;
    }
    out[length] = L'\0';
    return true;
}

FILE *tp_fs_fopen(const char *path_utf8, const char *mode) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    wchar_t wide_mode[8];
    if (!path || !mode_to_wide(mode, wide_mode)) {
        free(path);
        return NULL;
    }
    FILE *file = _wfopen(path, wide_mode);
    free(path);
    return file;
}

FILE *tp_fs_create_exclusive(const char *path_utf8, bool read_write) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return NULL;
    }
    int fd = -1;
    int flags = _O_CREAT | _O_EXCL | _O_BINARY | (read_write ? _O_RDWR : _O_WRONLY);
    errno_t opened = _wsopen_s(&fd, path, flags, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    free(path);
    if (opened != 0 || fd < 0) {
        errno = (int)opened;
        return NULL;
    }
    FILE *file = _fdopen(fd, read_write ? "w+b" : "wb");
    if (!file) {
        (void)_close(fd);
        (void)tp_fs_remove_file(path_utf8);
    }
    return file;
}

static bool win32_info_from_data(const WIN32_FILE_ATTRIBUTE_DATA *data, tp_fs_info *out) {
    if (!data || !out) {
        errno = EINVAL;
        return false;
    }
    out->reparse = (data->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U;
    if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
        out->kind = TP_FS_KIND_DIRECTORY;
    } else if ((data->dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0U) {
        out->kind = TP_FS_KIND_OTHER;
    } else {
        out->kind = TP_FS_KIND_REGULAR;
    }
    out->size = ((uint64_t)data->nFileSizeHigh << 32U) | (uint64_t)data->nFileSizeLow;
    uint64_t ticks = ((uint64_t)data->ftLastWriteTime.dwHighDateTime << 32U) |
                     (uint64_t)data->ftLastWriteTime.dwLowDateTime;
    out->mtime = ticks > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)ticks;
    return true;
}

bool tp_fs_stat(const char *path_utf8, tp_fs_info *out) {
    if (!out) {
        errno = EINVAL;
        return false;
    }
    memset(out, 0, sizeof *out);
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &data)) {
        DWORD error = GetLastError();
        free(path);
        win32_error_to_errno(error);
        return false;
    }
    free(path);
    return win32_info_from_data(&data, out);
}

bool tp_fs_create_dir(const char *path_utf8) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return false;
    }
    if (CreateDirectoryW(path, NULL)) {
        free(path);
        return true;
    }
    DWORD error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS) {
        DWORD attributes = GetFileAttributesW(path);
        free(path);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0U) {
            return true;
        }
    } else {
        free(path);
    }
    win32_error_to_errno(error);
    return false;
}

bool tp_fs_remove_file(const char *path_utf8) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return false;
    }
    if (DeleteFileW(path)) {
        free(path);
        return true;
    }
    DWORD error = GetLastError();
    free(path);
    win32_error_to_errno(error);
    return false;
}

bool tp_fs_remove_dir(const char *path_utf8) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return false;
    }
    if (RemoveDirectoryW(path)) {
        free(path);
        return true;
    }
    DWORD error = GetLastError();
    free(path);
    win32_error_to_errno(error);
    return false;
}

bool tp_fs_replace(const char *source_utf8, const char *destination_utf8) {
    wchar_t *source = tp_fs_win32_path_alloc(source_utf8);
    wchar_t *destination = tp_fs_win32_path_alloc(destination_utf8);
    if (!source || !destination) {
        free(source);
        free(destination);
        return false;
    }
    /* MOVEFILE_COPY_ALLOWED: a same-volume publish stays a pure atomic rename (the
     * flag is inert), but a degenerate cross-volume case (build-worker staging
     * relocated up an ancestor chain that a junction/mount point split off the
     * destination's volume) falls back to a WRITE_THROUGH copy + delete instead of
     * failing with ERROR_NOT_SAME_DEVICE and spuriously reporting builder_crashed. */
    BOOL moved = MoveFileExW(source, destination,
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH | MOVEFILE_COPY_ALLOWED);
    DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    free(source);
    free(destination);
    if (!moved) {
        win32_error_to_errno(error);
        return false;
    }
    return true;
}

tp_fs_move_result tp_fs_move_no_replace(const char *source_utf8, const char *destination_utf8) {
    wchar_t *source = tp_fs_win32_path_alloc(source_utf8);
    wchar_t *destination = tp_fs_win32_path_alloc(destination_utf8);
    if (!source || !destination) {
        free(source);
        free(destination);
        return TP_FS_MOVE_ERROR;
    }
    BOOL moved = MoveFileExW(source, destination, MOVEFILE_WRITE_THROUGH);
    DWORD error = moved ? ERROR_SUCCESS : GetLastError();
    free(source);
    free(destination);
    if (moved) {
        return TP_FS_MOVE_OK;
    }
    win32_error_to_errno(error);
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return TP_FS_MOVE_DESTINATION_EXISTS;
    }
    return TP_FS_MOVE_ERROR;
}

tp_fs_dir *tp_fs_dir_open(const char *path_utf8) {
    wchar_t *path = tp_fs_win32_path_alloc(path_utf8);
    if (!path) {
        return NULL;
    }
    size_t length = wcslen(path);
    bool has_separator = length != 0U && is_wide_separator(path[length - 1U]);
    size_t extra = has_separator ? 2U : 3U;
    if (length > SIZE_MAX - extra) {
        free(path);
        errno = ENAMETOOLONG;
        return NULL;
    }
    wchar_t *pattern = (wchar_t *)malloc((length + extra) * sizeof *pattern);
    if (!pattern) {
        free(path);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(pattern, path, length * sizeof *pattern);
    size_t pos = length;
    if (!has_separator) {
        pattern[pos++] = L'\\';
    }
    pattern[pos++] = L'*';
    pattern[pos] = L'\0';
    free(path);

    tp_fs_dir *dir = (tp_fs_dir *)calloc(1U, sizeof *dir);
    if (!dir) {
        free(pattern);
        errno = ENOMEM;
        return NULL;
    }
    dir->handle = FindFirstFileW(pattern, &dir->current);
    DWORD error = dir->handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    free(pattern);
    if (dir->handle == INVALID_HANDLE_VALUE) {
        free(dir);
        win32_error_to_errno(error);
        return NULL;
    }
    dir->first = true;
    return dir;
}

tp_fs_dir_result tp_fs_dir_next(tp_fs_dir *dir, tp_fs_dir_entry *out) {
    if (!dir || !out) {
        errno = EINVAL;
        return TP_FS_DIR_ERROR;
    }
    for (;;) {
        WIN32_FIND_DATAW *data = &dir->current;
        if (dir->first) {
            dir->first = false;
        } else if (!FindNextFileW(dir->handle, data)) {
            DWORD error = GetLastError();
            if (error == ERROR_NO_MORE_FILES) {
                return TP_FS_DIR_END;
            }
            win32_error_to_errno(error);
            return TP_FS_DIR_ERROR;
        }
        if ((data->cFileName[0] == L'.' && data->cFileName[1] == L'\0') ||
            (data->cFileName[0] == L'.' && data->cFileName[1] == L'.' && data->cFileName[2] == L'\0')) {
            continue;
        }
        if (!tp_fs_win32_utf16_to_utf8(data->cFileName, out->name, sizeof out->name)) {
            return TP_FS_DIR_ERROR;
        }
        WIN32_FILE_ATTRIBUTE_DATA attrs;
        attrs.dwFileAttributes = data->dwFileAttributes;
        attrs.ftCreationTime = data->ftCreationTime;
        attrs.ftLastAccessTime = data->ftLastAccessTime;
        attrs.ftLastWriteTime = data->ftLastWriteTime;
        attrs.nFileSizeHigh = data->nFileSizeHigh;
        attrs.nFileSizeLow = data->nFileSizeLow;
        (void)win32_info_from_data(&attrs, &out->info);
        return TP_FS_DIR_ENTRY;
    }
}

void tp_fs_dir_close(tp_fs_dir *dir) {
    if (!dir) {
        return;
    }
    if (dir->handle != INVALID_HANDLE_VALUE) {
        (void)FindClose(dir->handle);
    }
    free(dir);
}

bool tp_fs_sync(FILE *file) {
    if (!tp_fs_flush(file)) {
        return false;
    }
    int fd = _fileno(file);
    return fd >= 0 && _commit(fd) == 0;
}

bool tp_fs_sync_parent(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    /* MoveFileExW(..., MOVEFILE_WRITE_THROUGH) is the Windows publication
     * durability primitive used above; Windows has no portable directory-fsync
     * equivalent for ordinary application handles. */
    return true;
}
