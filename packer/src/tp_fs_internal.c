#include "tp_fs_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tp_utf8_internal.h"

bool tp_fs_path_is_valid_utf8(const char *path_utf8) {
    if (!path_utf8) {
        errno = EINVAL;
        return false;
    }
    if (tp_utf8_validate_c_string(path_utf8, TP_STATUS_INVALID_UTF8,
                                  "filesystem path", NULL) != TP_STATUS_OK) {
        errno = EILSEQ;
        return false;
    }
    return true;
}

#ifdef _WIN32
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

bool tp_fs_replace(const char *source_utf8, const char *destination_utf8) {
    wchar_t *source = tp_fs_win32_path_alloc(source_utf8);
    wchar_t *destination = tp_fs_win32_path_alloc(destination_utf8);
    if (!source || !destination) {
        free(source);
        free(destination);
        return false;
    }
    BOOL moved = MoveFileExW(source, destination, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
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

#else

#include <dirent.h>
#include <fcntl.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

struct tp_fs_dir {
    DIR *handle;
    char *path;
};

static char *string_copy(const char *value) {
    size_t length = strlen(value);
    char *copy = (char *)malloc(length + 1U);
    if (copy) {
        memcpy(copy, value, length + 1U);
    }
    return copy;
}

FILE *tp_fs_fopen(const char *path_utf8, const char *mode) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return NULL;
    }
    if (!mode) {
        errno = EINVAL;
        return NULL;
    }
    return fopen(path_utf8, mode);
}

FILE *tp_fs_create_exclusive(const char *path_utf8, bool read_write) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return NULL;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    int flags = O_CREAT | O_EXCL | (read_write ? O_RDWR : O_WRONLY);
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    int fd = open(path_utf8, flags, 0600);
    if (fd < 0) {
        return NULL;
    }
    FILE *file = fdopen(fd, read_write ? "w+b" : "wb");
    if (!file) {
        (void)close(fd);
        (void)unlink(path_utf8);
    }
    return file;
}

static bool stat_to_info(const struct stat *value, tp_fs_info *out) {
    if (!value || !out) {
        errno = EINVAL;
        return false;
    }
    if (S_ISREG(value->st_mode)) {
        out->kind = TP_FS_KIND_REGULAR;
    } else if (S_ISDIR(value->st_mode)) {
        out->kind = TP_FS_KIND_DIRECTORY;
    } else {
        out->kind = TP_FS_KIND_OTHER;
    }
    out->size = value->st_size < 0 ? 0U : (uint64_t)value->st_size;
    out->mtime = (int64_t)value->st_mtime;
    out->reparse = S_ISLNK(value->st_mode);
    return true;
}

bool tp_fs_stat(const char *path_utf8, tp_fs_info *out) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    if (!out) {
        errno = EINVAL;
        return false;
    }
    memset(out, 0, sizeof *out);
    struct stat value;
    if (lstat(path_utf8, &value) != 0) {
        return false;
    }
    return stat_to_info(&value, out);
}

bool tp_fs_create_dir(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return false;
    }
    if (mkdir(path_utf8, 0755) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        tp_fs_info info;
        return tp_fs_stat(path_utf8, &info) && info.kind == TP_FS_KIND_DIRECTORY;
    }
    return false;
}

bool tp_fs_remove_file(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    return unlink(path_utf8) == 0;
}

bool tp_fs_replace(const char *source_utf8, const char *destination_utf8) {
    if (!tp_fs_path_is_valid_utf8(source_utf8) ||
        !tp_fs_path_is_valid_utf8(destination_utf8)) {
        return false;
    }
    return rename(source_utf8, destination_utf8) == 0;
}

tp_fs_move_result tp_fs_move_no_replace(const char *source_utf8, const char *destination_utf8) {
    if (!tp_fs_path_is_valid_utf8(source_utf8) ||
        !tp_fs_path_is_valid_utf8(destination_utf8)) {
        return TP_FS_MOVE_ERROR;
    }
#if defined(__linux__) && defined(SYS_renameat2)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE 1U
#endif
    long result = syscall(SYS_renameat2, AT_FDCWD, source_utf8, AT_FDCWD, destination_utf8, RENAME_NOREPLACE);
    if (result == 0L) {
        return TP_FS_MOVE_OK;
    }
#elif defined(__APPLE__)
    if (renamex_np(source_utf8, destination_utf8, RENAME_EXCL) == 0) {
        return TP_FS_MOVE_OK;
    }
#else
    errno = ENOTSUP;
    return TP_FS_MOVE_ERROR;
#endif
    return errno == EEXIST ? TP_FS_MOVE_DESTINATION_EXISTS : TP_FS_MOVE_ERROR;
}

tp_fs_dir *tp_fs_dir_open(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return NULL;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    DIR *handle = opendir(path_utf8);
    if (!handle) {
        return NULL;
    }
    tp_fs_dir *dir = (tp_fs_dir *)calloc(1U, sizeof *dir);
    if (!dir) {
        (void)closedir(handle);
        errno = ENOMEM;
        return NULL;
    }
    dir->path = string_copy(path_utf8);
    if (!dir->path) {
        (void)closedir(handle);
        free(dir);
        errno = ENOMEM;
        return NULL;
    }
    dir->handle = handle;
    return dir;
}

tp_fs_dir_result tp_fs_dir_next(tp_fs_dir *dir, tp_fs_dir_entry *out) {
    if (!dir || !out) {
        errno = EINVAL;
        return TP_FS_DIR_ERROR;
    }
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir->handle);
        if (!entry) {
            return errno == 0 ? TP_FS_DIR_END : TP_FS_DIR_ERROR;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t name_length = strlen(entry->d_name);
        if (!tp_fs_path_is_valid_utf8(entry->d_name)) {
            return TP_FS_DIR_ERROR;
        }
        if (name_length >= sizeof out->name) {
            errno = ENAMETOOLONG;
            return TP_FS_DIR_ERROR;
        }
        memcpy(out->name, entry->d_name, name_length + 1U);
        size_t dir_length = strlen(dir->path);
        bool separator = dir_length != 0U && dir->path[dir_length - 1U] == '/';
        if (dir_length > SIZE_MAX - name_length - 2U) {
            errno = ENAMETOOLONG;
            return TP_FS_DIR_ERROR;
        }
        size_t total = dir_length + (separator ? 0U : 1U) + name_length + 1U;
        char *child = (char *)malloc(total);
        if (!child) {
            errno = ENOMEM;
            return TP_FS_DIR_ERROR;
        }
        int written = snprintf(child, total, separator ? "%s%s" : "%s/%s", dir->path, entry->d_name);
        struct stat value;
        int status = written < 0 || (size_t)written >= total ? -1 : lstat(child, &value);
        free(child);
        if (status != 0) {
            if (errno == ENOENT) {
                continue; /* benign directory race */
            }
            return TP_FS_DIR_ERROR;
        }
        (void)stat_to_info(&value, &out->info);
        return TP_FS_DIR_ENTRY;
    }
}

void tp_fs_dir_close(tp_fs_dir *dir) {
    if (!dir) {
        return;
    }
    if (dir->handle) {
        (void)closedir(dir->handle);
    }
    free(dir->path);
    free(dir);
}

#endif

bool tp_fs_read_all(FILE *file, void *data, size_t size) {
    if (!file || (!data && size != 0U)) {
        errno = EINVAL;
        return false;
    }
    unsigned char *bytes = (unsigned char *)data;
    size_t read = 0U;
    while (read < size) {
        size_t amount = fread(bytes + read, 1U, size - read, file);
        if (amount == 0U) {
            return false;
        }
        read += amount;
    }
    return true;
}

bool tp_fs_write_all(FILE *file, const void *data, size_t size) {
    if (!file || (!data && size != 0U)) {
        errno = EINVAL;
        return false;
    }
    const unsigned char *bytes = (const unsigned char *)data;
    size_t written = 0U;
    while (written < size) {
        size_t amount = fwrite(bytes + written, 1U, size - written, file);
        if (amount == 0U) {
            return false;
        }
        written += amount;
    }
    return true;
}

bool tp_fs_flush(FILE *file) {
    if (!file) {
        errno = EINVAL;
        return false;
    }
    return fflush(file) == 0;
}

bool tp_fs_sync(FILE *file) {
    if (!tp_fs_flush(file)) {
        return false;
    }
#ifdef _WIN32
    int fd = _fileno(file);
    return fd >= 0 && _commit(fd) == 0;
#else
    int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0;
#endif
}

bool tp_fs_close(FILE *file) {
    if (!file) {
        errno = EINVAL;
        return false;
    }
    return fclose(file) == 0;
}

bool tp_fs_write_file(const char *path_utf8, const void *data, size_t size) {
    FILE *file = tp_fs_fopen(path_utf8, "wb");
    if (!file) {
        return false;
    }
    bool wrote = tp_fs_write_all(file, data, size);
    bool closed = tp_fs_close(file);
    return wrote && closed;
}

bool tp_fs_exists(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info);
}

bool tp_fs_is_dir(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info) && info.kind == TP_FS_KIND_DIRECTORY;
}

bool tp_fs_sync_parent(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return false;
    }
#ifdef _WIN32
    /* MoveFileExW(..., MOVEFILE_WRITE_THROUGH) is the Windows publication
     * durability primitive used above; Windows has no portable directory-fsync
     * equivalent for ordinary application handles. */
    return true;
#else
    size_t length = strlen(path_utf8);
    char *parent = (char *)malloc(length + 2U);
    if (!parent) {
        errno = ENOMEM;
        return false;
    }
    memcpy(parent, path_utf8, length + 1U);
    char *slash = strrchr(parent, '/');
    if (!slash) {
        memcpy(parent, ".", 2U);
    } else if (slash == parent) {
        parent[1] = '\0';
    } else {
        *slash = '\0';
    }
    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif
    int fd = open(parent, flags);
    free(parent);
    if (fd < 0) {
        return false;
    }
    bool ok = fsync(fd) == 0;
    int saved = errno;
    (void)close(fd);
    errno = saved;
    return ok;
#endif
}
