#include "tp_fs_internal.h"

#include <errno.h>

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

bool tp_fs_exists(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info);
}

bool tp_fs_is_dir(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info) && info.kind == TP_FS_KIND_DIRECTORY;
}
