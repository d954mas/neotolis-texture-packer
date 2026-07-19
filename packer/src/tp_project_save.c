#include "tp_core/tp_project.h"

#include <errno.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define tp_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define tp_getpid getpid
#endif

#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_utf8.h"
#include "tp_fs_internal.h"
#include "tp_project_internal.h"
#include "tp_project_path_internal.h"
#include "tp_strutil.h"

/* ======================================================================== */
/* staged save + durable publication                                        */
/* ======================================================================== */

typedef enum tp_temp_open_result {
    TP_TEMP_OPEN_OK = 0,
    TP_TEMP_OPEN_PATH_TOO_LONG,
    TP_TEMP_OPEN_FAILED,
} tp_temp_open_result;

static _Thread_local bool s_test_fail_next_temp_create;
static _Thread_local bool s_test_fail_next_file_sync;
static _Thread_local bool s_test_fail_next_parent_sync;
static _Thread_local tp_file_io_phase s_test_fail_next_save_io;
static bool s_test_save_max_bytes_armed;
static size_t s_test_save_max_bytes;

void tp_project__test_fail_next_temp_create(void) { s_test_fail_next_temp_create = true; }
void tp_project__test_fail_next_file_sync(void) { s_test_fail_next_file_sync = true; }
void tp_project__test_fail_next_parent_sync(void) {
    s_test_fail_next_parent_sync = true;
}
void tp_project__test_fail_next_save_io(tp_file_io_phase phase) {
    s_test_fail_next_save_io = phase;
}
void tp_project__test_set_save_max_bytes(size_t max_bytes) {
    s_test_save_max_bytes = max_bytes;
    s_test_save_max_bytes_armed = true;
}

/* Create a unique sibling temp atomically and keep that exact file open. There
 * is deliberately no remove-before-open: an existing name belongs to another
 * writer (or an interrupted save), so it is skipped rather than followed or
 * deleted. The returned FILE owns the underlying descriptor/handle. */
static tp_temp_open_result tp_open_save_temp(const char *path, char *tmp,
                                             size_t tmp_cap, FILE **out,
                                             int *out_native_code) {
    *out = NULL;
    *out_native_code = 0;
    if (s_test_fail_next_temp_create ||
        s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_OPEN) {
        s_test_fail_next_temp_create = false;
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        *out_native_code = EACCES;
        return TP_TEMP_OPEN_FAILED;
    }
    static _Atomic uint64_t counter;
    const unsigned long pid = (unsigned long)tp_getpid();
    for (unsigned int attempt = 0; attempt < 128U; attempt++) {
        const uint64_t serial =
            atomic_fetch_add_explicit(&counter, UINT64_C(1),
                                      memory_order_relaxed) +
            UINT64_C(1);
        int nt = snprintf(tmp, tmp_cap, "%s.savetmp.%08lx.%016llx", path,
                          pid, (unsigned long long)serial);
        if (nt <= 0 || (size_t)nt >= tmp_cap) {
            return TP_TEMP_OPEN_PATH_TOO_LONG;
        }
        FILE *f = tp_fs_create_exclusive(tmp, false);
        if (!f) {
            if (errno == EEXIST) {
                continue;
            }
            *out_native_code = errno != 0 ? errno : EIO;
            return TP_TEMP_OPEN_FAILED;
        }
#ifndef _WIN32
        /* Preserve collaborative permissions only for an existing regular
         * destination. A symlink is replaced, never followed; a new project
         * keeps the exclusive creator's private mode. */
        struct stat destination;
        if (lstat(path, &destination) == 0 &&
            S_ISREG(destination.st_mode) &&
            fchmod(fileno(f), destination.st_mode & 0777) != 0) {
            *out_native_code = errno != 0 ? errno : EIO;
            (void)tp_fs_close(f);
            (void)tp_fs_remove_file(tmp);
            return TP_TEMP_OPEN_FAILED;
        }
#endif
        *out = f;
        return TP_TEMP_OPEN_OK;
    }
    *out_native_code = EEXIST;
    return TP_TEMP_OPEN_FAILED;
}

typedef struct tp_save_path_restore_entry {
    char **slot;
    char *original;
} tp_save_path_restore_entry;

typedef struct tp_save_path_restore {
    tp_save_path_restore_entry *entries;
    size_t count;
    size_t capacity;
} tp_save_path_restore;

static tp_status tp_project_save_stage(tp_project *p, const char *path,
                                       tp_id128 *out_fingerprint,
                                       const tp_id128 *expected_fingerprint,
                                       bool create_only,
                                       tp_save_path_restore *restore,
                                       tp_error *err) {
    if (out_fingerprint) {
        memset(out_fingerprint, 0, sizeof *out_fingerprint);
    }
    if (!p || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save: NULL project or path");
    }
    if (!tp_utf8_is_valid_c_string(path)) {
        return tp_error_set(err, TP_STATUS_INVALID_UTF8,
                            "tp_project_save: path is not valid UTF-8");
    }

    char new_dir[TP_PATH_MAX];
    tp_status st = tp_abs_dir_of(path, new_dir, sizeof new_dir);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "tp_project_save: path too long: %s", path);
    }

    /* Resolve each source against its established source base, collapse lexical
     * aliases, then express that identity relative to the new project dir.
     * An unsaved project has no source base yet, so its relative spelling is
     * retained without silently binding it to the process CWD. */
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char norm[TP_PATH_MAX];
            if ((size_t)snprintf(norm, sizeof norm, "%s", a->sources[si].path) >= sizeof norm) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: source path too long");
            }
            tp_normalize_slashes(norm);
            char rel[TP_PATH_MAX];
            char resolved[TP_IDENTITY_PATH_MAX];
            st = tp_project_source_path_absolute_lexical(
                p, norm, resolved, sizeof resolved, err);
            if (st == TP_STATUS_OK) {
                st = tp_relativize(resolved, new_dir, rel, sizeof rel);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err, st,
                                        "tp_project_save: cannot relativize %s",
                                        resolved);
                }
            } else if (st == TP_STATUS_PATH_NOT_ABSOLUTE) {
                if (err) {
                    memset(err, 0, sizeof *err);
                }
                if ((size_t)snprintf(rel, sizeof rel, "%s", norm) >=
                    sizeof rel) {
                    return tp_error_set(
                        err, TP_STATUS_OUT_OF_BOUNDS,
                        "tp_project_save: source path too long");
                }
            } else {
                return tp_error_set(err, st,
                                    "tp_project_save: cannot resolve %s", norm);
            }
            char *copy = tp_strdup(rel);
            if (!copy) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
            }
            if (restore) {
                if (restore->count >= restore->capacity) {
                    free(copy);
                    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                        "tp_project_save: source restore capacity overflow");
                }
                tp_save_path_restore_entry *entry =
                    &restore->entries[restore->count++];
                entry->slot = &a->sources[si].path;
                entry->original = a->sources[si].path;
                a->sources[si].path = copy;
            } else {
                free(a->sources[si].path);
                a->sources[si].path = copy;
            }
        }
    }

    if (!p->source_base_dir) {
        p->source_base_dir = tp_strdup(new_dir);
        if (!p->source_base_dir) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_project_save: out of memory");
        }
    }

    /* Update the staged project dir (Save / Save-As). */
    char *dir_copy = tp_strdup(new_dir);
    if (!dir_copy) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
    }
    free(p->project_dir);
    p->project_dir = dir_copy;

    /* file-save = relativize (above) + canonical buffer + durable publish. */
    char *buf = NULL;
    size_t len = 0;
    tp_status bst = tp_project_save_buffer(p, &buf, &len, err);
    if (bst != TP_STATUS_OK) {
        return bst;
    }
    size_t save_max_bytes = (size_t)TP_IDENTITY_FILE_MAX_BYTES;
    if (s_test_save_max_bytes_armed) {
        save_max_bytes = s_test_save_max_bytes;
        s_test_save_max_bytes_armed = false;
    }
    if (len > save_max_bytes) {
        free(buf);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_project_save: serialized project exceeds the %zu-byte limit", save_max_bytes);
    }
    tp_id128 written_fingerprint = {{0}};
    if (out_fingerprint) {
        bst = tp_identity_bytes_fingerprint(buf, len, &written_fingerprint, err);
        if (bst != TP_STATUS_OK) {
            free(buf);
            return bst;
        }
    }

    /* Atomic durable write: serialize to a sibling temp, check a durable file
     * sync, close, atomically publish, then sync the containing directory. A
     * short write / file-sync / close failure leaves `path` untouched. A
     * directory-sync failure happens after publication and therefore returns
     * FILE_DURABILITY_UNCERTAIN: the saved bytes are authoritative and clients
     * must surface a warning rather than lie that Save had no side effect.
     *
     * ATOMIC-SAVE TRADEOFFS (accepted -- these are inherent to any atomic-rename save, as done by editors and
     * git): replacing the destination via rename/MoveFileEx swaps the inode. POSIX preserves an existing
     * regular destination's permission bits, while a new file keeps private mode 0600; owner and ACLs may
     * still change. On Windows the new file inherits the parent ACL. A `path`
     * that is a SYMLINK is replaced by a regular file rather than written through; a save into a READ-ONLY
     * containing directory fails (the sibling temp cannot be created) where an in-place truncate once succeeded.
     * For a `.ntpacker_project` JSON file these are immaterial, and every one fails CLOSED -- an error, never a
     * corrupt/partial file. */
    char tmp[TP_PATH_MAX];
    FILE *f = NULL;
    int native_code = 0;
    const tp_temp_open_result temp_rc =
        tp_open_save_temp(path, tmp, sizeof tmp, &f, &native_code);
    if (temp_rc == TP_TEMP_OPEN_PATH_TOO_LONG) {
        free(buf);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: path too long: %s", path);
    }
    if (temp_rc != TP_TEMP_OPEN_OK) {
        free(buf);
        return tp_error_set_file_io(
            err, TP_FILE_IO_PHASE_TEMP_OPEN, path, native_code,
            "tp_project_save: cannot create temporary file for %s", path);
    }
    tp_file_io_phase failed_phase = TP_FILE_IO_PHASE_NONE;
    bool wrote = false;
    if (s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_WRITE) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = ENOSPC;
        failed_phase = TP_FILE_IO_PHASE_TEMP_WRITE;
    } else {
        wrote = tp_fs_write_all(f, buf, len);
        if (!wrote) {
            native_code = errno != 0 ? errno : EIO;
            failed_phase = TP_FILE_IO_PHASE_TEMP_WRITE;
        }
    }
    bool synced = false;
    if (wrote) {
        if (s_test_fail_next_file_sync ||
            s_test_fail_next_save_io == TP_FILE_IO_PHASE_FILE_SYNC) {
            s_test_fail_next_file_sync = false;
            s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
            native_code = EIO;
            failed_phase = TP_FILE_IO_PHASE_FILE_SYNC;
        } else {
            synced = tp_fs_sync(f);
            if (!synced) {
                native_code = errno != 0 ? errno : EIO;
                failed_phase = TP_FILE_IO_PHASE_FILE_SYNC;
            }
        }
    }
    const bool close_result = tp_fs_close(f);
    bool closed = close_result;
    if (s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_CLOSE) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = EIO;
        failed_phase = TP_FILE_IO_PHASE_TEMP_CLOSE;
        closed = false;
    } else if (!close_result && failed_phase == TP_FILE_IO_PHASE_NONE) {
        native_code = errno != 0 ? errno : EIO;
        failed_phase = TP_FILE_IO_PHASE_TEMP_CLOSE;
    }
    free(buf);
    if (!wrote || !synced || !closed) {
        (void)tp_fs_remove_file(tmp);
        return tp_error_set_file_io(
            err, failed_phase, path, native_code,
            "tp_project_save: could not durably write temporary file for %s",
            path);
    }
    /* Optimistic concurrency is checked as late as portability allows: after the complete replacement
     * is durable in its sibling temp, immediately before the atomic promotion. This closes the large
     * serialize/write window left by GUI-only preflight checks. A non-cooperating writer can still race
     * the final fingerprint->rename instructions; no portable filesystem CAS exists for replacement. */
    if (expected_fingerprint) {
        tp_id128 current = {{0}};
        tp_status fps = tp_identity_file_fingerprint(path, &current, err);
        if (fps != TP_STATUS_OK || !tp_id128_eq(current, *expected_fingerprint)) {
            (void)tp_fs_remove_file(tmp);
            return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                                "tp_project_save: destination changed before publish: %s", path);
        }
    }
    /* Atomically publish the fully-written temp. Create-only publication must
     * fail if another writer won the destination after our earlier checks. */
    bool moved = false;
    bool destination_exists = false;
    const tp_file_io_phase publish_phase =
        create_only ? TP_FILE_IO_PHASE_ATOMIC_CREATE
                    : TP_FILE_IO_PHASE_ATOMIC_REPLACE;
    const bool inject_publish_failure =
        s_test_fail_next_save_io == publish_phase;
    if (inject_publish_failure) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = EACCES;
    }
    if (!inject_publish_failure && create_only) {
        const tp_fs_move_result move_result =
            tp_fs_move_no_replace(tmp, path);
        moved = move_result == TP_FS_MOVE_OK;
        destination_exists =
            move_result == TP_FS_MOVE_DESTINATION_EXISTS;
        if (!moved && !destination_exists) {
            native_code = errno != 0 ? errno : EIO;
        }
    } else if (!inject_publish_failure) {
        moved = tp_fs_replace(tmp, path);
        if (!moved) {
            native_code = errno != 0 ? errno : EIO;
        }
    }
    if (!moved) {
        (void)tp_fs_remove_file(tmp);
        if (destination_exists) {
            return tp_error_set(err, TP_STATUS_FILE_EXISTS,
                                "tp_project_save: destination already exists: %s",
                                path);
        }
        return tp_error_set_file_io(
            err, publish_phase, path, native_code,
            "tp_project_save: could not finalize save to %s", path);
    }
    if (out_fingerprint) {
        *out_fingerprint = written_fingerprint;
    }
    bool parent_synced = false;
    if (s_test_fail_next_parent_sync) {
        s_test_fail_next_parent_sync = false;
    } else {
        parent_synced = tp_fs_sync_parent(path);
    }
    if (!parent_synced) {
        return tp_error_set(
            err, TP_STATUS_FILE_DURABILITY_UNCERTAIN,
            "tp_project_save: %s was published, but directory durability could not be confirmed",
            path);
    }
    return TP_STATUS_OK;
}

/* The staged clone owns serialization-only path normalization. Publish the new
 * project-file directory while preserving an established live source base. */
static void tp_project_adopt_saved_dir(tp_project *dst, tp_project *stage) {
    free(dst->project_dir);
    dst->project_dir = stage->project_dir;
    stage->project_dir = NULL;
    if (!dst->source_base_dir) {
        dst->source_base_dir = stage->source_base_dir;
        stage->source_base_dir = NULL;
    }
}

static tp_status tp_project_save_staged(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                        const tp_id128 *expected_fingerprint,
                                        bool create_only, tp_error *err) {
    if (out_fingerprint) {
        memset(out_fingerprint, 0, sizeof *out_fingerprint);
    }
    if (!p || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save: NULL project or path");
    }
    tp_project *stage = tp_project_clone(p);
    if (!stage) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: could not stage project paths");
    }
    tp_status st = tp_project_save_stage(stage, path, out_fingerprint,
                                         expected_fingerprint, create_only,
                                         NULL, err);
    if (st == TP_STATUS_OK ||
        st == TP_STATUS_FILE_DURABILITY_UNCERTAIN) {
        tp_project_adopt_saved_dir(p, stage);
    }
    tp_project_destroy(stage);
    return st;
}

tp_status tp_project_save_candidate_with_fingerprint(
    tp_project *candidate, const char *path,
    const tp_id128 *expected_fingerprint, bool create_only,
    tp_id128 *out_fingerprint, tp_error *err) {
    if (!candidate || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_save: NULL candidate or path");
    }
    size_t source_count = 0U;
    for (int ai = 0; ai < candidate->atlas_count; ++ai) {
        const int count = candidate->atlases[ai].source_count;
        if (count < 0 || (size_t)count > SIZE_MAX - source_count) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_project_save: source restore count overflow");
        }
        source_count += (size_t)count;
    }
    tp_save_path_restore restore = {0};
    if (source_count > 0U) {
        restore.entries = calloc(source_count, sizeof *restore.entries);
        if (!restore.entries) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_project_save: source restore allocation failed");
        }
        restore.capacity = source_count;
    }
    const tp_status status = tp_project_save_stage(
        candidate, path, out_fingerprint, expected_fingerprint, create_only,
        &restore, err);
    for (size_t i = 0U; i < restore.count; ++i) {
        tp_save_path_restore_entry *entry = &restore.entries[i];
        char *normalized = *entry->slot;
        *entry->slot = entry->original;
        free(normalized);
    }
    free(restore.entries);
    return status;
}

tp_status tp_project_save_with_fingerprint(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                           tp_error *err) {
    return tp_project_save_staged(p, path, out_fingerprint, NULL, false, err);
}

tp_status tp_project_save_new_with_fingerprint(
    tp_project *p, const char *path, tp_id128 *out_fingerprint, tp_error *err) {
    return tp_project_save_staged(p, path, out_fingerprint, NULL, true, err);
}

tp_status tp_project_save_if_unchanged(tp_project *p, const char *path,
                                       const tp_id128 *expected_fingerprint,
                                       tp_id128 *out_fingerprint, tp_error *err) {
    if (!expected_fingerprint) {
        if (out_fingerprint) {
            memset(out_fingerprint, 0, sizeof *out_fingerprint);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_save_if_unchanged: NULL expected fingerprint");
    }
    return tp_project_save_staged(p, path, out_fingerprint,
                                  expected_fingerprint, false, err);
}

tp_status tp_project_save(tp_project *p, const char *path, tp_error *err) {
    return tp_project_save_with_fingerprint(p, path, NULL, err);
}
