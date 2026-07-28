#include "tp_fs_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"

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

static bool fs_is_sep(char c) {
    return c == '/' || c == '\\';
}

/* Thread-local so a redirect brackets exactly one writer invocation on its own
 * thread; the strings are borrowed and must outlive the armed window. */
static _Thread_local const char *s_stage_final_dir;
static _Thread_local const char *s_stage_dir;

void tp_fs_stage_redirect_begin(const char *final_dir_utf8,
                                const char *staging_dir_utf8) {
    NT_ASSERT(final_dir_utf8 != NULL && staging_dir_utf8 != NULL);
    NT_ASSERT(s_stage_final_dir == NULL && s_stage_dir == NULL);
    s_stage_final_dir = final_dir_utf8;
    s_stage_dir = staging_dir_utf8;
}

void tp_fs_stage_redirect_end(void) {
    s_stage_final_dir = NULL;
    s_stage_dir = NULL;
}

bool tp_fs_stage_child_path(const char *final_dir_utf8,
                            const char *staging_dir_utf8,
                            const char *path_utf8, char *out, size_t out_cap) {
    if (!final_dir_utf8 || !staging_dir_utf8 || !path_utf8 || !out) {
        errno = EINVAL;
        return false;
    }
    const size_t dir_len = strlen(final_dir_utf8);
    NT_ASSERT(dir_len == 0U || fs_is_sep(final_dir_utf8[dir_len - 1U]));
    if (strncmp(path_utf8, final_dir_utf8, dir_len) != 0) {
        return false;
    }
    const char *name = path_utf8 + dir_len;
    if (name[0] == '\0') {
        return false;
    }
    for (const char *c = name; *c; c++) {
        if (fs_is_sep(*c)) {
            return false; /* deeper than a direct child: not covered */
        }
    }
    const int n = snprintf(out, out_cap, "%s/%s", staging_dir_utf8, name);
    if (n <= 0 || (size_t)n >= out_cap) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

/* The serial keeps two staged sets in one process apart; the pid keeps two
 * processes exporting into a shared output directory apart. A leftover or
 * foreign name is skipped, never adopted: the exclusive create is the single
 * atomic step that decides it, so there is no check-then-create window a
 * concurrent exporter could slip into. */
bool tp_fs_stage_dir_create(const char *parent_utf8, char *out, size_t out_cap) {
    if (!parent_utf8 || !out) {
        errno = EINVAL;
        return false;
    }
    const size_t parent_len = strlen(parent_utf8);
    NT_ASSERT(parent_len == 0U || fs_is_sep(parent_utf8[parent_len - 1U]));
    static _Atomic uint32_t counter;
    const unsigned long pid = (unsigned long)tp_fs_getpid();
    for (unsigned int attempt = 0U; attempt < 256U; attempt++) {
        const uint32_t serial =
            atomic_fetch_add_explicit(&counter, 1U, memory_order_relaxed) + 1U;
        const int n = snprintf(out, out_cap, "%s.tp-stage-%08lx-%08lx",
                               parent_utf8, pid & 0xffffffffUL,
                               (unsigned long)serial);
        if (n <= 0 || (size_t)n >= out_cap) {
            errno = ENAMETOOLONG;
            return false;
        }
        const tp_fs_create_dir_result created = tp_fs_create_dir_exclusive(out);
        if (created == TP_FS_CREATE_DIR_OK) {
            return true;
        }
        if (created == TP_FS_CREATE_DIR_EXISTS) {
            continue; /* leftover or a lost race: try the next serial */
        }
        return false; /* unwritable parent / real error: fail closed */
    }
    return false;
}

void tp_fs_remove_tree(const char *path_utf8) {
    tp_fs_dir *dir = tp_fs_dir_open(path_utf8);
    if (dir) {
        tp_fs_dir_entry entry;
        for (;;) {
            if (tp_fs_dir_next(dir, &entry) != TP_FS_DIR_ENTRY) {
                break;
            }
            char child[TP_FS_ATOMIC_TEMP_PATH_MAX];
            if ((size_t)snprintf(child, sizeof child, "%s/%s", path_utf8,
                                 entry.name) >= sizeof child) {
                continue;
            }
            if (entry.info.kind == TP_FS_KIND_DIRECTORY && !entry.info.reparse) {
                tp_fs_remove_tree(child);
            } else if (entry.info.kind == TP_FS_KIND_DIRECTORY) {
                /* Junction / dir-symlink: remove the link itself, never descend
                 * into and delete its target's contents. */
                (void)tp_fs_remove_dir(child);
            } else {
                (void)tp_fs_remove_file(child);
            }
        }
        tp_fs_dir_close(dir);
    }
    (void)tp_fs_remove_dir(path_utf8);
}

/* Sibling temp + one replace. The pid keeps two processes exporting to a shared
 * output directory off each other's temp; within one process the writers are
 * sequential, so the name is simply reused. An armed stage redirect (above)
 * takes precedence for direct children of its final dir: those bytes go to the
 * staging sibling as one plain write, and publication becomes the caller's
 * whole-set replace pass. */
bool tp_fs_write_file_atomic(const char *path_utf8, const void *data,
                             size_t size) {
    if (!path_utf8) {
        errno = EINVAL;
        return false;
    }
    if (s_stage_final_dir) {
        char staged[TP_FS_ATOMIC_TEMP_PATH_MAX];
        if (tp_fs_stage_child_path(s_stage_final_dir, s_stage_dir, path_utf8,
                                   staged, sizeof staged)) {
            return tp_fs_write_file(staged, data, size);
        }
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
