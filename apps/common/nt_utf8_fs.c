#include "nt_utf8_fs.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static bool is_separator(wchar_t value) {
    return value == L'/' || value == L'\\';
}

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
    case ERROR_INVALID_NAME:
    case ERROR_INVALID_PARAMETER:
        errno = EINVAL;
        break;
    default:
        errno = EIO;
        break;
    }
}

static wchar_t *path_alloc(const char *path_utf8) {
    if (!path_utf8 || path_utf8[0] == '\0') {
        errno = EINVAL;
        return NULL;
    }
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8, -1, NULL, 0);
    if (required <= 0) {
        errno = EILSEQ;
        return NULL;
    }
    wchar_t *wide = (wchar_t *)malloc((size_t)required * sizeof *wide);
    if (!wide) {
        errno = ENOMEM;
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path_utf8, -1,
                            wide, required) != required) {
        free(wide);
        errno = EILSEQ;
        return NULL;
    }

    size_t length = wcslen(wide);
    if (length >= 4U && is_separator(wide[0]) && is_separator(wide[1]) &&
        (wide[2] == L'.' || wide[2] == L'?') && is_separator(wide[3])) {
        free(wide);
        errno = EINVAL;
        return NULL;
    }
    if (length < (size_t)(MAX_PATH - 12)) {
        return wide;
    }

    /* Extended paths do not normalize relative components. Resolve first,
     * normalize separators, then add the one controlled namespace prefix. */
    const DWORD needed = GetFullPathNameW(wide, 0U, NULL, NULL);
    if (needed == 0U) {
        const DWORD error = GetLastError();
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
    const DWORD copied = GetFullPathNameW(wide, needed, absolute, NULL);
    free(wide);
    if (copied == 0U || copied >= needed) {
        const DWORD error = GetLastError();
        free(absolute);
        win32_error_to_errno(error);
        return NULL;
    }
    for (size_t i = 0; absolute[i] != L'\0'; ++i) {
        if (absolute[i] == L'/') {
            absolute[i] = L'\\';
        }
    }
    length = wcslen(absolute);
    const bool unc = length >= 3U && absolute[0] == L'\\' &&
                     absolute[1] == L'\\';
    const bool drive =
        length >= 3U &&
        ((absolute[0] >= L'A' && absolute[0] <= L'Z') ||
         (absolute[0] >= L'a' && absolute[0] <= L'z')) &&
        absolute[1] == L':' && absolute[2] == L'\\';
    if (!unc && !drive) {
        free(absolute);
        errno = EINVAL;
        return NULL;
    }

    static const wchar_t drive_prefix[] = L"\\\\?\\";
    static const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
    const size_t prefix_length = unc ? 8U : 4U;
    const size_t skipped = unc ? 2U : 0U;
    if (length < skipped || length - skipped >
                                (size_t)INT_MAX - prefix_length - 1U) {
        free(absolute);
        errno = ENAMETOOLONG;
        return NULL;
    }
    const size_t output_length = prefix_length + length - skipped;
    if (output_length >= 32767U) {
        free(absolute);
        errno = ENAMETOOLONG;
        return NULL;
    }
    wchar_t *extended =
        (wchar_t *)malloc((output_length + 1U) * sizeof *extended);
    if (!extended) {
        free(absolute);
        errno = ENOMEM;
        return NULL;
    }
    memcpy(extended, unc ? unc_prefix : drive_prefix,
           prefix_length * sizeof *extended);
    memcpy(extended + prefix_length, absolute + skipped,
           (length - skipped + 1U) * sizeof *extended);
    free(absolute);
    return extended;
}

bool nt_utf8_path_to_utf16(const char *path_utf8, wchar_t *out,
                           size_t output_capacity) {
    if (!out || output_capacity == 0U) {
        errno = EINVAL;
        return false;
    }
    out[0] = L'\0';
    wchar_t *wide = path_alloc(path_utf8);
    if (!wide) {
        return false;
    }
    const size_t required = wcslen(wide) + 1U;
    if (required > output_capacity) {
        free(wide);
        errno = ERANGE;
        return false;
    }
    memcpy(out, wide, required * sizeof *out);
    free(wide);
    return true;
}

static bool mode_to_utf16(const char *mode, wchar_t out[8]) {
    if (!mode) {
        errno = EINVAL;
        return false;
    }
    const size_t length = strlen(mode);
    if (length == 0U || length >= 8U) {
        errno = EINVAL;
        return false;
    }
    for (size_t i = 0; i < length; ++i) {
        const unsigned char value = (unsigned char)mode[i];
        if (value > 0x7FU) {
            errno = EINVAL;
            return false;
        }
        out[i] = (wchar_t)value;
    }
    out[length] = L'\0';
    return true;
}

FILE *nt_utf8_fopen(const char *path_utf8, const char *mode) {
    wchar_t *path = path_alloc(path_utf8);
    wchar_t wide_mode[8];
    if (!path || !mode_to_utf16(mode, wide_mode)) {
        free(path);
        return NULL;
    }
    FILE *file = _wfopen(path, wide_mode);
    free(path);
    return file;
}

int nt_utf8_remove(const char *path_utf8) {
    wchar_t *path = path_alloc(path_utf8);
    if (!path) {
        return -1;
    }
    const int result = _wremove(path);
    free(path);
    return result;
}

int nt_utf8_rename(const char *source_utf8, const char *destination_utf8) {
    wchar_t *source = path_alloc(source_utf8);
    wchar_t *destination = path_alloc(destination_utf8);
    if (!source || !destination) {
        free(source);
        free(destination);
        return -1;
    }
    const int result = _wrename(source, destination);
    free(source);
    free(destination);
    return result;
}

#else

FILE *nt_utf8_fopen(const char *path_utf8, const char *mode) {
    return fopen(path_utf8, mode);
}

int nt_utf8_remove(const char *path_utf8) { return remove(path_utf8); }

int nt_utf8_rename(const char *source_utf8, const char *destination_utf8) {
    return rename(source_utf8, destination_utf8);
}

#endif
