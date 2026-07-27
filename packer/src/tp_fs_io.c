#include "tp_fs_internal.h"

#include <errno.h>
#include <stdio.h>

#ifdef _WIN32
#include <process.h>
#define tp_fs_getpid _getpid
#else
#include <unistd.h>
#define tp_fs_getpid getpid
#endif

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

/* Sibling temp + one replace. The pid keeps two processes exporting to a shared
 * output directory off each other's temp; within one process the writers are
 * sequential, so the name is simply reused. */
bool tp_fs_write_file_atomic(const char *path_utf8, const void *data,
                             size_t size) {
    if (!path_utf8) {
        errno = EINVAL;
        return false;
    }
    char tmp[TP_FS_ATOMIC_TEMP_PATH_MAX];
    const int length = snprintf(tmp, sizeof tmp, "%s.tp-tmp-%08lx", path_utf8,
                                (unsigned long)tp_fs_getpid());
    if (length <= 0 || (size_t)length >= sizeof tmp) {
        errno = ENAMETOOLONG;
        return false;
    }
    if (!tp_fs_write_file(tmp, data, size)) {
        (void)tp_fs_remove_file(tmp);
        return false;
    }
    if (!tp_fs_replace(tmp, path_utf8)) {
        /* The destination is untouched -- everything so far happened in the temp. */
        (void)tp_fs_remove_file(tmp);
        return false;
    }
    return true;
}

bool tp_fs_exists(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info);
}

bool tp_fs_is_dir(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info) && info.kind == TP_FS_KIND_DIRECTORY;
}
