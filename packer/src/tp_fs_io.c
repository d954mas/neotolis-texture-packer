#include "tp_fs_internal.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h> /* strtoul: the reaper parses the pid out of a private name */
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

/* The two private names the staged-set publish creates. Both carry the owning
 * pid so the reaper can decide by liveness, and both are parsed back by
 * stage_parse_owner below, so each spelling lives in exactly one place. */
#define TP_FS_STAGE_DIR_PREFIX ".tp-stage-"
#define TP_FS_STAGE_OLD_INFIX ".tp-old-"

/* One counter behind every private name this module hands out, so a stage dir
 * and a displaced destination created back to back can never collide. */
static _Atomic uint32_t s_stage_serial;

static uint32_t stage_next_serial(void) {
    return atomic_fetch_add_explicit(&s_stage_serial, 1U,
                                     memory_order_relaxed) +
           1U;
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
    const unsigned long pid = (unsigned long)tp_fs_getpid();
    for (unsigned int attempt = 0U; attempt < 256U; attempt++) {
        const uint32_t serial = stage_next_serial();
        const int n = snprintf(out, out_cap, "%s" TP_FS_STAGE_DIR_PREFIX
                                             "%08lx-%08lx",
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

bool tp_fs_stage_old_path(const char *destination_utf8, char *out,
                          size_t out_cap) {
    if (!destination_utf8 || !out) {
        errno = EINVAL;
        return false;
    }
    const unsigned long pid = (unsigned long)tp_fs_getpid();
    const int n = snprintf(out, out_cap, "%s" TP_FS_STAGE_OLD_INFIX "%08lx-%08lx",
                           destination_utf8, pid & 0xffffffffUL,
                           (unsigned long)stage_next_serial());
    if (n <= 0 || (size_t)n >= out_cap) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

/* Parses the "<hexpid>-<hexserial>" tail our private names end with. Returns
 * false for anything that is not exactly that shape, so an unrelated name that
 * merely starts the same way is left alone. */
static bool stage_parse_owner(const char *tail, unsigned long *out_pid) {
    if (tail[0] == '\0') {
        return false;
    }
    char *end = NULL;
    const unsigned long pid = strtoul(tail, &end, 16);
    if (end == tail || *end != '-') {
        return false;
    }
    const char *serial = end + 1;
    if (serial[0] == '\0') {
        return false;
    }
    (void)strtoul(serial, &end, 16);
    if (*end != '\0') {
        return false;
    }
    *out_pid = pid;
    return true;
}

/* The owner pid of a ".tp-stage-<hexpid>-<hexserial>" directory name. */
static bool stage_dir_owner(const char *name, unsigned long *out_pid) {
    const size_t prefix_len = sizeof TP_FS_STAGE_DIR_PREFIX - 1U;
    if (strncmp(name, TP_FS_STAGE_DIR_PREFIX, prefix_len) != 0) {
        return false;
    }
    return stage_parse_owner(name + prefix_len, out_pid);
}

/* The owner pid of a "<destination>.tp-old-<hexpid>-<hexserial>" file name, plus
 * the length of the destination name it displaced. The LAST infix wins, so a
 * destination that legitimately contains ".tp-old-" still round-trips. */
static bool stage_old_owner(const char *name, unsigned long *out_pid,
                            size_t *out_destination_len) {
    const size_t infix_len = sizeof TP_FS_STAGE_OLD_INFIX - 1U;
    const char *at = NULL;
    for (const char *scan = name; (scan = strstr(scan, TP_FS_STAGE_OLD_INFIX)) != NULL;
         scan += infix_len) {
        at = scan;
    }
    if (!at || at == name) {
        return false; /* no infix, or nothing left to restore it to */
    }
    if (!stage_parse_owner(at + infix_len, out_pid)) {
        return false;
    }
    *out_destination_len = (size_t)(at - name);
    return true;
}

void tp_fs_stage_reap_orphans(const char *parent_utf8) {
    if (!parent_utf8) {
        return;
    }
    const size_t parent_len = strlen(parent_utf8);
    NT_ASSERT(parent_len == 0U || fs_is_sep(parent_utf8[parent_len - 1U]));
    tp_fs_dir *dir = tp_fs_dir_open(parent_len > 0U ? parent_utf8 : ".");
    if (!dir) {
        return;
    }
    tp_fs_dir_entry entry;
    while (tp_fs_dir_next(dir, &entry) == TP_FS_DIR_ENTRY) {
        unsigned long pid = 0UL;
        size_t destination_len = 0U;
        const bool is_stage = stage_dir_owner(entry.name, &pid);
        const bool is_old =
            !is_stage && stage_old_owner(entry.name, &pid, &destination_len);
        if (!is_stage && !is_old) {
            continue;
        }
        if (tp_fs_process_is_live(pid)) {
            continue; /* someone is still exporting through it */
        }
        char path[TP_FS_STAGE_PATH_MAX];
        const int n =
            snprintf(path, sizeof path, "%s%s", parent_utf8, entry.name);
        if (n <= 0 || (size_t)n >= sizeof path) {
            continue;
        }
        if (is_stage) {
            if (entry.info.kind == TP_FS_KIND_DIRECTORY && !entry.info.reparse) {
                tp_fs_remove_tree(path);
            }
            continue;
        }
        if (entry.info.kind != TP_FS_KIND_REGULAR || entry.info.reparse) {
            continue;
        }
        /* Crash mid-swap. The destination decides which half of the swap ran:
         * present => the staged file already landed and this copy is spent;
         * missing => the displace ran and the promote did not, so the old set
         * member is restored. */
        char destination[TP_FS_STAGE_PATH_MAX];
        const int dn = snprintf(destination, sizeof destination, "%s%.*s",
                                parent_utf8, (int)destination_len, entry.name);
        if (dn <= 0 || (size_t)dn >= sizeof destination) {
            continue;
        }
        if (tp_fs_exists(destination)) {
            (void)tp_fs_remove_file(path);
        } else {
            (void)tp_fs_replace(path, destination);
        }
    }
    tp_fs_dir_close(dir);
}

void tp_fs_remove_tree(const char *path_utf8) {
    tp_fs_dir *dir = tp_fs_dir_open(path_utf8);
    if (dir) {
        tp_fs_dir_entry entry;
        for (;;) {
            if (tp_fs_dir_next(dir, &entry) != TP_FS_DIR_ENTRY) {
                break;
            }
            char child[TP_FS_STAGE_PATH_MAX];
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

bool tp_fs_exists(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info);
}

bool tp_fs_is_dir(const char *path_utf8) {
    tp_fs_info info;
    return tp_fs_stat(path_utf8, &info) && info.kind == TP_FS_KIND_DIRECTORY;
}
