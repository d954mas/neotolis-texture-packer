#include "nt_utf8_fs.h"

#if defined(_WIN32)

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_fs.h"

#define NT_WIN32_PATH_CAP 32767U

static wchar_t *path_alloc(const char *path_utf8) {
    wchar_t *path = (wchar_t *)malloc(NT_WIN32_PATH_CAP * sizeof *path);
    if (!path) {
        errno = ENOMEM;
        return NULL;
    }
    if (!tp_fs_win32_path_copy(path_utf8, path, NT_WIN32_PATH_CAP)) {
        free(path);
        return NULL;
    }
    return path;
}

bool nt_utf8_path_to_utf16(const char *path_utf8, wchar_t *out,
                           size_t output_capacity) {
    return tp_fs_win32_path_copy(path_utf8, out, output_capacity);
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
