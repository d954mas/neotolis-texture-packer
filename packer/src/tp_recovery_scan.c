#include "tp_recovery_state_internal.h"

#include <stdio.h>
#include <string.h>

#include "tp_core/tp_scan.h"
#include "tp_fs_internal.h"
#include "tp_journal_internal.h"

#define TP_RECOVERY_SCAN_MAX_FILES 256U
#define TP_RECOVERY_SCAN_MAX_BYTES ((uint64_t)TP_JOURNAL_MAX_FILE_BYTES * 2U)
// #region scan paths
/* Recovery intentionally visits every directory entry, not only regular files:
 * a directory/reparse node with an .ntpjournal suffix must surface as a
 * structured unreadable-candidate diagnostic instead of disappearing. */
static bool recovery_visit_journals_utf8(const char *dir,
                                         tp_scan_name_visitor visit, void *ctx) {
    if (!dir || dir[0] == '\0' || !visit) {
        return false;
    }
    tp_fs_dir *stream = tp_fs_dir_open(dir);
    if (!stream) {
        return false;
    }
    tp_fs_dir_entry entry;
    tp_fs_dir_result next;
    while ((next = tp_fs_dir_next(stream, &entry)) == TP_FS_DIR_ENTRY) {
        const uint64_t size = entry.info.kind == TP_FS_KIND_REGULAR ? entry.info.size : 0U;
        if (!visit(ctx, entry.name, size)) {
            tp_fs_dir_close(stream);
            return true;
        }
    }
    tp_fs_dir_close(stream);
    return next == TP_FS_DIR_END;
}

static bool recovery_visit_journals(const char *dir,
                                     tp_scan_name_visitor visit, void *ctx) {
    return recovery_visit_journals_utf8(dir, visit, ctx);
}
// #endregion

// #region candidate ranking
static bool candidate_outranks(const tp_recovery_candidate *kept,
                               const tp_recovery_candidate *candidate) {
    if (kept->adoptable != candidate->adoptable) {
        return kept->adoptable;
    }
    if (kept->timestamp != candidate->timestamp) {
        return kept->timestamp > candidate->timestamp;
    }
    /* Directory enumeration order is not a contract and differs by platform.
     * The canonical journal path is unique within one root and gives equal-time
     * candidates (including cap eviction) a total deterministic order. */
    return strcmp(kept->journal_path, candidate->journal_path) <= 0;
}

static void candidate_insert(tp_recovery_candidates *out,
                             const tp_recovery_candidate *candidate) {
    size_t index = 0U;
    while (index < out->count && candidate_outranks(&out->items[index], candidate)) {
        index++;
    }
    if (out->count >= TP_RECOVERY_MAX_CANDIDATES) {
        out->has_more = true;
    }
    if (index >= TP_RECOVERY_MAX_CANDIDATES) {
        return;
    }
    size_t last = out->count < TP_RECOVERY_MAX_CANDIDATES
                      ? out->count
                      : TP_RECOVERY_MAX_CANDIDATES - 1U;
    while (last > index) {
        out->items[last] = out->items[last - 1U];
        last--;
    }
    out->items[index] = *candidate;
    if (out->count < TP_RECOVERY_MAX_CANDIDATES) {
        out->count++;
    }
}

static void scan_diagnostic_insert(tp_recovery_candidates *out,
                                   const char *journal_path,
                                   tp_status status) {
    size_t index = 0U;
    while (index < out->diagnostic_count &&
           strcmp(out->diagnostics[index].journal_path, journal_path) <= 0) {
        index++;
    }
    if (out->diagnostic_count >= TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        out->has_more = true;
    }
    if (index >= TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        return;
    }
    size_t last = out->diagnostic_count < TP_RECOVERY_MAX_SCAN_DIAGNOSTICS
                      ? out->diagnostic_count
                      : TP_RECOVERY_MAX_SCAN_DIAGNOSTICS - 1U;
    while (last > index) {
        out->diagnostics[last] = out->diagnostics[last - 1U];
        last--;
    }
    tp_recovery_scan_diagnostic *diagnostic = &out->diagnostics[index];
    memset(diagnostic, 0, sizeof *diagnostic);
    (void)snprintf(diagnostic->journal_path,
                   sizeof diagnostic->journal_path, "%s", journal_path);
    diagnostic->status = status;
    if (out->diagnostic_count < TP_RECOVERY_MAX_SCAN_DIAGNOSTICS) {
        out->diagnostic_count++;
    }
}

void tp_recovery__test_candidate_insert(tp_recovery_candidates *out,
                                        const tp_recovery_candidate *candidate) {
    if (out && candidate) {
        candidate_insert(out, candidate);
    }
}
// #endregion

// #region scan
typedef struct recovery_scan_context {
    tp_recovery_store *store;
    const char *live_basename;
    tp_recovery_candidates *out;
    uint64_t bytes_examined;
    unsigned files_examined;
    bool limit_reached;
} recovery_scan_context;

static bool scan_candidate(void *context, const char *name, uint64_t size) {
    recovery_scan_context *scan = (recovery_scan_context *)context;
    if (!name || name[0] == '\0') {
        return true;
    }
    /* Budget every entry the directory iterator examines, before suffix/live
     * filtering. Otherwise a root full of unrelated names, directories, or
     * symlinks makes startup work unbounded while consuming zero scan budget. */
    if (scan->files_examined >= TP_RECOVERY_SCAN_MAX_FILES ||
        size > TP_RECOVERY_SCAN_MAX_BYTES - scan->bytes_examined) {
        scan->limit_reached = true;
        scan->out->has_more = true;
        return false;
    }
    scan->files_examined++;
    scan->bytes_examined += size;
    if (!tp_recovery__has_journal_suffix(name) ||
        (scan->live_basename[0] != '\0' && strcmp(name, scan->live_basename) == 0)) {
        return true;
    }

    char journal[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(journal, sizeof journal, "%s/%s", scan->store->root, name);
    if (written < 0 || (size_t)written >= sizeof journal) {
        scan_diagnostic_insert(scan->out, name, TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    char lock_path[TP_RECOVERY_LOCK_PATH_MAX];
    const int lock_written = snprintf(lock_path, sizeof lock_path, "%s%s", journal,
                                      TP_RECOVERY_LOCK_SUFFIX);
    if (lock_written < 0 || (size_t)lock_written >= sizeof lock_path) {
        scan_diagnostic_insert(scan->out, journal,
                               TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    if (!tp_recovery_backend_lock_is_unowned(lock_path)) {
        return true;
    }
    tp_journal_io io = tp_recovery_backend_journal_read(journal);
    if (!io.ctx) {
        scan_diagnostic_insert(scan->out, journal,
                               TP_STATUS_PATH_RESOLVE_FAILED);
        return true;
    }
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_error peek_err = {{0}};
    const tp_status peek_status = tp_journal_peek(io, &peek, &peek_err);
    if (peek_status != TP_STATUS_OK) {
        scan_diagnostic_insert(scan->out, journal, peek_status);
        tp_journal_peek_free(&peek);
        return true;
    }

    static const uint8_t empty_key[16] = {0};
    const bool has_header_key =
        memcmp(peek.key, empty_key, sizeof peek.key) != 0;
    if (peek.status == TP_JOURNAL_RECOVERY_BAD_MAGIC ||
        (!has_header_key &&
         (peek.status == TP_JOURNAL_RECOVERY_EMPTY ||
          peek.status == TP_JOURNAL_RECOVERY_TRUNCATED))) {
        scan_diagnostic_insert(scan->out, journal, TP_STATUS_BAD_PROJECT);
        tp_journal_peek_free(&peek);
        return true;
    }

    const bool key_matches = memcmp(peek.key, scan->store->journal_key.bytes,
                                    sizeof peek.key) == 0;
    const bool adoptable = key_matches && peek.has_checkpoint && peek.record_count > 1 &&
                           (peek.status == TP_JOURNAL_RECOVERY_OK ||
                            peek.status == TP_JOURNAL_RECOVERY_TRUNCATED ||
                            peek.status == TP_JOURNAL_RECOVERY_CORRUPT);
    const bool version_mismatch = key_matches &&
                                  peek.status == TP_JOURNAL_RECOVERY_VERSION_MISMATCH;
    if (key_matches && !adoptable && !version_mismatch) {
        scan_diagnostic_insert(scan->out, journal, TP_STATUS_BAD_PROJECT);
    }
    if (adoptable || version_mismatch) {
        tp_recovery_candidate candidate;
        memset(&candidate, 0, sizeof candidate);
        (void)snprintf(candidate.journal_path, sizeof candidate.journal_path, "%s", journal);
        const char *meta_path = peek.has_meta && peek.meta.path ? peek.meta.path : "";
        (void)snprintf(candidate.original_path, sizeof candidate.original_path, "%s", meta_path);
        if (peek.has_meta && peek.meta.name && peek.meta.name[0] != '\0') {
            (void)snprintf(candidate.name, sizeof candidate.name, "%s", peek.meta.name);
        } else if (meta_path[0] != '\0') {
            (void)snprintf(candidate.name, sizeof candidate.name, "%s", tp_recovery__path_basename(meta_path));
        } else {
            (void)snprintf(candidate.name, sizeof candidate.name, "untitled");
        }
        candidate.timestamp = peek.has_meta ? peek.meta.timestamp : 0;
        candidate.status = peek.status;
        candidate.adoptable = adoptable;
        if (peek.has_meta && peek.meta.has_file_fingerprint) {
            candidate.file_fingerprint = peek.meta.file_fingerprint;
            candidate.has_file_fingerprint = true;
        }
        candidate_insert(scan->out, &candidate);
    }
    tp_journal_peek_free(&peek);
    return true;
}

tp_status tp_recovery_store_scan(tp_recovery_store *store, const char *live_slot,
                                 tp_recovery_candidates *out, tp_error *err) {
    if (!store || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery store and candidate output are required");
    }
    memset(out, 0, sizeof *out);
    const char *live_basename = tp_recovery__path_basename(live_slot);
    recovery_scan_context scan = {
        .store = store,
        .live_basename = live_basename,
        .out = out,
    };
    if (!recovery_visit_journals(store->root, scan_candidate, &scan) &&
        !scan.limit_reached) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "recovery root could not be scanned");
    }
    return TP_STATUS_OK;
}
// #endregion
