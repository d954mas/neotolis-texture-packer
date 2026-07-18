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

#ifndef _WIN32

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

#ifndef _WIN32
bool tp_fs_sync(FILE *file) {
    if (!tp_fs_flush(file)) {
        return false;
    }
    int fd = fileno(file);
    return fd >= 0 && fsync(fd) == 0;
}
#endif

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

#ifndef _WIN32
bool tp_fs_sync_parent(const char *path_utf8) {
    if (!tp_fs_path_is_valid_utf8(path_utf8)) {
        return false;
    }
    if (path_utf8[0] == '\0') {
        errno = EINVAL;
        return false;
    }
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
}
#endif
