#include "tp_core/tp_recovery.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_project_lease.h"
#include "tp_core/tp_transaction.h"
#include "tp_fs_internal.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_recovery_backend_types_internal.h"
#include "tp_recovery_internal.h"
#include "tp_recovery_state_internal.h"
#include "tp_session_internal.h"

#define TP_RECOVERY_SCAN_MAX_FILES 256U
#define TP_RECOVERY_SCAN_MAX_BYTES ((uint64_t)TP_JOURNAL_MAX_FILE_BYTES * 2U)

static tp_recovery_store *s_test_foreign_store;
static tp_recovery_claim *s_test_foreign_claim;
static bool s_test_fail_next_resolve_verify;


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

// #region test seams
bool tp_recovery__test_hold_foreign_lock(
    const char *root, tp_id128 journal_key, const char *journal_path) {
    tp_recovery__test_release_foreign_lock();
    tp_error err = {{0}};
    return tp_recovery_store_create(root, journal_key, &s_test_foreign_store,
                                    &err) == TP_STATUS_OK &&
           tp_recovery_store_claim(s_test_foreign_store, journal_path,
                                   &s_test_foreign_claim, &err) == TP_STATUS_OK;
}

void tp_recovery__test_release_foreign_lock(void) {
    tp_recovery_claim_release(s_test_foreign_claim);
    s_test_foreign_claim = NULL;
    tp_recovery_store_destroy(s_test_foreign_store);
    s_test_foreign_store = NULL;
}

void tp_recovery__test_fail_next_resolve_verify(void) {
    s_test_fail_next_resolve_verify = true;
}

void tp_recovery__test_fail_next_live_retire_cleanup(void) {
    tp_recovery__live_test_fail_next_retire_cleanup();
}

#ifndef _WIN32
void tp_recovery__test_fail_next_quarantine_unlink(void) {
    tp_recovery_backend_test_fail_next_quarantine_unlink();
}
#endif

tp_status tp_recovery__test_craft_metadata_journal(
    const char *path, tp_id128 key, int64_t timestamp,
    const char *project_path, const char *project_name, tp_error *err) {
    tp_journal_io io = tp_journal_io_file(path);
    if (!io.ctx) {
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "test journal could not be created");
    }
    tp_journal *journal = tp_journal_create(io, key);
    if (!journal) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "test journal allocation failed");
    }
    static const uint8_t snapshot[4] = {'r', '6', 'a', '!'};
    tp_status status = tp_journal_init_checkpoint(
        journal, snapshot, sizeof snapshot, 0, err);
    if (status == TP_STATUS_OK) {
        status = tp_journal_append_txn(
            journal, "6a0000000000000000000000000000ff", 1,
            snapshot, sizeof snapshot, err);
    }
    if (status == TP_STATUS_OK) {
        status = tp_journal_set_metadata(journal, timestamp,
                                         project_path, project_name, err);
    }
    tp_journal_destroy(journal);
    return status;
}

tp_status tp_recovery__test_peek_candidate(
    const char *path, tp_recovery_candidate *out, tp_error *err) {
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "test recovery peek requires path and output");
    }
    memset(out, 0, sizeof *out);
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_status status = tp_journal_peek(tp_journal_io_file_read(path),
                                       &peek, err);
    if (status == TP_STATUS_OK) {
        (void)snprintf(out->journal_path, sizeof out->journal_path, "%s", path);
        (void)snprintf(out->original_path, sizeof out->original_path, "%s",
                       peek.has_meta && peek.meta.path ? peek.meta.path : "");
        (void)snprintf(out->name, sizeof out->name, "%s",
                       peek.has_meta && peek.meta.name ? peek.meta.name : "");
        out->timestamp = peek.has_meta ? peek.meta.timestamp : 0;
        out->status = peek.status;
        if (peek.has_meta && peek.meta.has_file_fingerprint) {
            out->file_fingerprint = peek.meta.file_fingerprint;
            out->has_file_fingerprint = true;
        }
    }
    tp_journal_peek_free(&peek);
    return status;
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

// #region session attach & resolve
static tp_status recovery_session_attach_store(
    tp_recovery_store *store, const char *journal_path, tp_session *session,
    const tp_recovery_metadata *metadata, tp_error *err) {
    if (!store || !journal_path || journal_path[0] == '\0' || !session ||
        !metadata) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery session attach requires session and metadata");
    }
    if (tp_session__has_recovery_owner(session)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    tp_status status = tp_session_require_recovery(session, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_recovery_live *live = NULL;
    status = tp_recovery_store_create_live(store, journal_path, &live, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_session_attach_recovery_live(session, live, metadata, err);
    /* The session accepts ownership before live I/O begins. Invalid
     * preconditions reject before transfer, so release that unaccepted owner. */
    if (status != TP_STATUS_OK &&
        !tp_session__owns_recovery_live(session, live)) {
        tp_recovery_live_destroy(live);
    }
    return status;
}

tp_status tp_recovery_session_attach(
    const char *root, tp_id128 journal_key, const tp_rng *rng,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err) {
    if (!session || !metadata || !rng || !rng->fill) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery session attach requires session, metadata, and RNG");
    }
    if (tp_session__has_recovery_owner(session)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session already has recovery attached");
    }
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    char journal_path[TP_IDENTITY_PATH_MAX];
    if (status == TP_STATUS_OK) {
        status = tp_recovery__live_slot_generate(store, rng, journal_path,
                                             sizeof journal_path, err);
    }
    if (status == TP_STATUS_OK) {
        status = recovery_session_attach_store(store, journal_path, session,
                                               metadata, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

tp_status tp_recovery__test_session_attach_at(
    const char *root, tp_id128 journal_key, const char *journal_path,
    tp_session *session, const tp_recovery_metadata *metadata, tp_error *err) {
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    char canonical_journal[TP_IDENTITY_PATH_MAX];
    if (status == TP_STATUS_OK) {
        status = tp_recovery__store_journal_path(store, journal_path, canonical_journal,
                                    sizeof canonical_journal, err);
    }
    if (status == TP_STATUS_OK) {
        status = recovery_session_attach_store(store, canonical_journal,
                                               session, metadata, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

tp_status tp_recovery_scan_root(const char *root, tp_id128 journal_key,
                                const tp_session *live_session,
                                tp_recovery_candidates *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery scan output is required");
    }
    memset(out, 0, sizeof *out);
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_store_scan(
            store, tp_session__recovery_journal_path(live_session), out, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}

static tp_status recovery_resolve_store(
    tp_recovery_store *store, const char *protected_live,
    const char *journal_path, tp_recovery_action action,
    const char *target_path, const tp_rng *rng,
    tp_recovery_resolve_result *out, tp_error *err) {
    if (!store || !out || !journal_path ||
        journal_path[0] == '\0' ||
        action < TP_RECOVERY_ACTION_DISCARD ||
        action > TP_RECOVERY_ACTION_SAVE_AS) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires journal, action, and output");
    }
    memset(out, 0, sizeof *out);
    tp_status status;
    char canonical_journal[TP_IDENTITY_PATH_MAX];
    status = tp_recovery__store_journal_path(store, journal_path, canonical_journal,
                                sizeof canonical_journal, err);
    if (status == TP_STATUS_OK && protected_live && protected_live[0] != '\0') {
        char canonical_live[TP_IDENTITY_PATH_MAX];
        status = tp_recovery__store_journal_path(store, protected_live, canonical_live,
                                    sizeof canonical_live, err);
        if (status == TP_STATUS_OK &&
            tp_identity_path_equal(canonical_journal, canonical_live)) {
            status = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "cannot resolve the live recovery journal of an open session");
        }
    }

    tp_recovery_claim *claim = NULL;
    tp_recovery_resolution *resolution = NULL;
    tp_session_save_result receipt;
    memset(&receipt, 0, sizeof receipt);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_store_claim(store, canonical_journal, &claim, err);
    }
    if (status == TP_STATUS_OK && action == TP_RECOVERY_ACTION_DISCARD) {
        status = tp_recovery_claim_discard(claim, err);
    } else if (status == TP_STATUS_OK) {
        if (!rng || !rng->fill) {
            status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                  "recovery save requires an RNG");
        }
        tp_recovery_owned_candidate *candidate = NULL;
        if (status == TP_STATUS_OK) {
            status = tp_recovery_claim_recover(claim, &candidate, err);
        }
        if (status == TP_STATUS_OK) {
            status = tp_recovery_candidate_create_resolution(candidate, rng,
                                                              &resolution, err);
        }
        if (status == TP_STATUS_OK) {
            status = action == TP_RECOVERY_ACTION_SAVE_ORIGINAL
                         ? tp_recovery_resolution_save_original(resolution,
                                                                &receipt, err)
                         : tp_recovery_resolution_save_as(resolution, target_path,
                                                          &receipt, err);
        }
        if (status == TP_STATUS_OK && s_test_fail_next_resolve_verify) {
            s_test_fail_next_resolve_verify = false;
            static const char invalid_project[] = "injected invalid project";
            (void)tp_fs_write_file(receipt.target_path, invalid_project, sizeof invalid_project - 1U);
        }
        if (status == TP_STATUS_OK) {
            status = tp_recovery_resolution_finalize(resolution, &receipt, err);
        }
    }
    if (status == TP_STATUS_OK) {
        out->journal_deleted = true;
        out->project_saved = action != TP_RECOVERY_ACTION_DISCARD;
        if (out->project_saved) {
            (void)snprintf(out->target_path, sizeof out->target_path, "%s",
                           receipt.target_path);
            out->file_fingerprint = receipt.file_fingerprint;
            out->has_file_fingerprint = true;
        }
    }
    tp_recovery_resolution_destroy(resolution);
    tp_recovery_claim_release(claim);
    return status;
}

tp_status tp_recovery_resolve_journal(
    const char *root, tp_id128 journal_key, const char *journal_path,
    const tp_session *live_session, tp_recovery_action action,
    const char *target_path, const tp_rng *rng,
    tp_recovery_resolve_result *out, tp_error *err) {
    if (!out || !journal_path || journal_path[0] == '\0' ||
        action < TP_RECOVERY_ACTION_DISCARD ||
        action > TP_RECOVERY_ACTION_SAVE_AS ||
        (action != TP_RECOVERY_ACTION_DISCARD && (!rng || !rng->fill))) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires journal, action, output, and save RNG");
    }
    memset(out, 0, sizeof *out);
    tp_recovery_store *store = NULL;
    tp_status status = tp_recovery_store_create(root, journal_key, &store, err);
    if (status == TP_STATUS_OK) {
        status = recovery_resolve_store(
            store, tp_session__recovery_journal_path(live_session),
            journal_path, action, target_path, rng, out, err);
    }
    tp_recovery_store_destroy(store);
    return status;
}
// #endregion
