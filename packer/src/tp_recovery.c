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






// #region claim & candidate
tp_status tp_recovery_store_claim(tp_recovery_store *store, const char *journal_path,
                                  tp_recovery_claim **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery claim output is required");
    }
    *out = NULL;
    tp_recovery_claim *claim = (tp_recovery_claim *)calloc(1, sizeof *claim);
    if (!claim) {
        return tp_error_set(err, TP_STATUS_OOM, "recovery claim allocation failed");
    }
    claim->lock = TP_RECOVERY_LOCK_PIN_INIT;
    char journal[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_recovery__lock_path_for(store, journal_path, journal, sizeof journal,
                                     claim->lock_path, sizeof claim->lock_path, err);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_backend_lock_open(&claim->lock, claim->lock_path, err);
    }
    if (status != TP_STATUS_OK) {
        free(claim);
        return status;
    }
    (void)snprintf(claim->journal_path, sizeof claim->journal_path, "%s", journal);
    claim->journal_key = store->journal_key;
    *out = claim;
    return TP_STATUS_OK;
}

static void owned_candidate_destroy(tp_recovery_owned_candidate *candidate) {
    if (!candidate) {
        return;
    }
    tp_recovery_backend_candidate_close(&candidate->journal_pin);
    tp_project_destroy(candidate->project);
    free(candidate->metadata.path);
    free(candidate->metadata.name);
    free(candidate);
}

void tp_recovery_claim_release(tp_recovery_claim *claim) {
    if (!claim) {
        return;
    }
    tp_recovery_resolution_destroy(claim->resolution);
    owned_candidate_destroy(claim->candidate);
    tp_recovery_backend_lock_release(&claim->lock);
    free(claim);
}

tp_status tp_recovery_claim_recover(tp_recovery_claim *claim,
                                    tp_recovery_owned_candidate **out,
                                    tp_error *err) {
    if (!claim || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery claim and candidate output are required");
    }
    *out = NULL;
    if (claim->candidate) {
        *out = claim->candidate;
        return TP_STATUS_OK;
    }
    tp_recovery_owned_candidate *candidate =
        (tp_recovery_owned_candidate *)calloc(1, sizeof *candidate);
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery candidate allocation failed");
    }
    candidate->owner = claim;
    candidate->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status pin_status = TP_STATUS_BAD_PROJECT;
    tp_journal_io io = tp_recovery_backend_candidate_pin(&candidate->journal_pin,
                                     claim->journal_path,
                                     &pin_status, err);
    if (!io.ctx) {
        owned_candidate_destroy(candidate);
        return pin_status;
    }
    tp_model *model = NULL;
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    tp_status status = tp_model_recover(io, claim->journal_key,
                                        &model, &recovery, err);
    if (status != TP_STATUS_OK || !model) {
        tp_journal_recovery_free(&recovery);
        owned_candidate_destroy(candidate);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "recovery journal has no recoverable state");
    }
    candidate->project = tp_project_clone(tp_model_project(model));
    if (recovery.has_metadata) {
        candidate->metadata = recovery.metadata;
        candidate->has_metadata = true;
        memset(&recovery.metadata, 0, sizeof recovery.metadata);
        recovery.has_metadata = false;
    }
    tp_model_destroy(model);
    tp_journal_recovery_free(&recovery);
    if (!candidate->project) {
        owned_candidate_destroy(candidate);
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovered project clone allocation failed");
    }
    claim->candidate = candidate;
    *out = candidate;
    return TP_STATUS_OK;
}
// #endregion

// #region resolution
tp_status tp_recovery_candidate_create_resolution(
    tp_recovery_owned_candidate *candidate, const tp_rng *rng,
    tp_recovery_resolution **out, tp_error *err) {
    if (!candidate || !candidate->project || !candidate->owner || !rng ||
        !rng->fill || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution requires candidate, RNG, and output");
    }
    *out = NULL;
    if (candidate->owner->resolution) {
        return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                            "recovery candidate already has an active resolution");
    }
    tp_project *project = tp_project_clone(candidate->project);
    if (!project) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "detached recovery project clone allocation failed");
    }
    tp_id128 token;
    tp_status status = tp_id128_generate(rng, &token, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(project);
        return status;
    }
    tp_recovery_resolution *resolution =
        (tp_recovery_resolution *)calloc(1, sizeof *resolution);
    if (!resolution) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery resolution allocation failed");
    }
    status = tp_session_create_detached_recovery(project, rng, token,
                                                 &resolution->session, err);
    if (status != TP_STATUS_OK) {
        free(resolution);
        return status;
    }
    candidate->recovery_token = token;
    candidate->has_recovery_token = true;
    resolution->candidate = candidate;
    candidate->owner->resolution = resolution;
    *out = resolution;
    return status;
}

static tp_status resolution_save(tp_recovery_resolution *resolution,
                                 const char *target_path,
                                 const tp_id128 *expected_fingerprint,
                                 tp_session_save_result *receipt,
                                 tp_error *err) {
    if (!resolution || !resolution->session || !target_path ||
        target_path[0] == '\0' || !receipt) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "active recovery resolution, target, and receipt are required");
    }
    if (resolution->project_lease) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery resolution already has an unfinalized save");
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(target_path, canonical,
                                                  sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (tp_identity_path_equal(canonical,
                               resolution->candidate->owner->journal_path)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovered project cannot be saved over its journal");
    }
    const tp_journal_meta *metadata = &resolution->candidate->metadata;
    if (!expected_fingerprint && resolution->candidate->has_metadata &&
        metadata->path && metadata->path[0] != '\0' &&
        tp_identity_path_equal(canonical, metadata->path)) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "Save As must not bypass the original-file fingerprint contract");
    }
    tp_project_lease *lease = NULL;
    status = tp_project_lease_acquire(canonical, &lease, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_session_save_detached_recovery(resolution->session, canonical,
                                               expected_fingerprint,
                                               receipt, err);
    if (status != TP_STATUS_OK) {
        tp_project_lease_release(lease);
        return status;
    }
    resolution->project_lease = lease;
    resolution->last_receipt = *receipt;
    resolution->has_receipt = true;
    return TP_STATUS_OK;
}

tp_status tp_recovery_resolution_save_original(
    tp_recovery_resolution *resolution, tp_session_save_result *receipt,
    tp_error *err) {
    if (!resolution || !resolution->candidate ||
        !resolution->candidate->has_metadata ||
        !resolution->candidate->metadata.path ||
        resolution->candidate->metadata.path[0] == '\0' ||
        !resolution->candidate->metadata.has_file_fingerprint) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "recovery candidate has no exact original-file baseline; use Save As");
    }
    return resolution_save(resolution, resolution->candidate->metadata.path,
                           &resolution->candidate->metadata.file_fingerprint,
                           receipt, err);
}

tp_status tp_recovery_resolution_save_as(
    tp_recovery_resolution *resolution, const char *target_path,
    tp_session_save_result *receipt, tp_error *err) {
    return resolution_save(resolution, target_path, NULL, receipt, err);
}

static tp_status validate_save_receipt(const tp_recovery_claim *claim,
                                       const tp_recovery_owned_candidate *candidate,
                                       const tp_session_save_result *receipt,
                                       tp_error *err) {
    if (!receipt || !receipt->saved || !receipt->has_recovery_token ||
        !candidate->has_recovery_token ||
        !tp_id128_eq(receipt->recovery_token, candidate->recovery_token)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "save receipt is not bound to this recovery candidate");
    }
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(receipt->target_path, canonical,
                                                  sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (tp_identity_path_equal(canonical, claim->journal_path)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovered project cannot be finalized over its journal");
    }
    tp_id128 current;
    status = tp_identity_file_fingerprint(canonical, &current, err);
    if (status != TP_STATUS_OK || !tp_id128_eq(current, receipt->file_fingerprint)) {
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "saved recovery target changed before finalize");
    }
    tp_project *verified = NULL;
    tp_id128 loaded_fingerprint;
    status = tp_project_load_with_fingerprint(canonical, &verified,
                                              &loaded_fingerprint, err);
    tp_project_destroy(verified);
    if (status != TP_STATUS_OK || !tp_id128_eq(loaded_fingerprint, current)) {
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "saved recovery target did not verify");
    }
    return TP_STATUS_OK;
}

static bool receipt_equals(const tp_session_save_result *a,
                           const tp_session_save_result *b) {
    return a && b && a->saved == b->saved &&
           a->has_recovery_token == b->has_recovery_token &&
           strcmp(a->target_path, b->target_path) == 0 &&
           tp_id128_eq(a->file_fingerprint, b->file_fingerprint) &&
           tp_id128_eq(a->recovery_token, b->recovery_token);
}

tp_status tp_recovery_resolution_finalize(
    tp_recovery_resolution *resolution,
    const tp_session_save_result *receipt, tp_error *err) {
    if (!resolution || !resolution->session || !resolution->project_lease ||
        !resolution->has_receipt ||
        !receipt_equals(receipt, &resolution->last_receipt)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "finalize requires the live resolution and its exact pending receipt");
    }
    tp_recovery_owned_candidate *candidate = resolution->candidate;
    tp_recovery_claim *claim = candidate->owner;
    tp_status status = validate_save_receipt(claim, candidate, receipt, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_recovery_backend_candidate_delete(&candidate->journal_pin,
                                  claim->journal_path, err);
    if (status == TP_STATUS_OK) {
        tp_project_lease_release(resolution->project_lease);
        resolution->project_lease = NULL;
        resolution->has_receipt = false;
    }
    return status;
}

void tp_recovery_resolution_cancel(tp_recovery_resolution *resolution) {
    if (!resolution) {
        return;
    }
    tp_project_lease_release(resolution->project_lease);
    resolution->project_lease = NULL;
    tp_session_destroy(resolution->session);
    resolution->session = NULL;
    resolution->has_receipt = false;
}

void tp_recovery_resolution_destroy(tp_recovery_resolution *resolution) {
    if (!resolution) {
        return;
    }
    tp_recovery_claim *claim = resolution->candidate
                                   ? resolution->candidate->owner
                                   : NULL;
    tp_recovery_resolution_cancel(resolution);
    if (claim && claim->resolution == resolution) {
        claim->resolution = NULL;
    }
    free(resolution);
}
// #endregion

// #region discard
tp_status tp_recovery_claim_discard(tp_recovery_claim *claim, tp_error *err) {
    if (!claim) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "discard requires a recovery claim");
    }
    if (claim->resolution) {
        return tp_error_set(err, TP_STATUS_RECOVERY_BUSY,
                            "discard cannot race an active recovery resolution");
    }
    if (claim->candidate) {
        return tp_recovery_backend_candidate_delete(&claim->candidate->journal_pin,
                                    claim->journal_path, err);
    }
    tp_recovery_owned_candidate *candidate =
        (tp_recovery_owned_candidate *)calloc(1, sizeof *candidate);
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "discard candidate allocation failed");
    }
    candidate->owner = claim;
    candidate->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status pin_status = TP_STATUS_BAD_PROJECT;
    tp_journal_io io = tp_recovery_backend_candidate_pin(&candidate->journal_pin,
                                     claim->journal_path,
                                     &pin_status, err);
    if (!io.ctx) {
        owned_candidate_destroy(candidate);
        return pin_status;
    }
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_status status = tp_journal_peek(io, &peek, err);
    const bool valid_domain = status == TP_STATUS_OK &&
                              memcmp(peek.key, claim->journal_key.bytes,
                                     sizeof peek.key) == 0 &&
                              peek.status != TP_JOURNAL_RECOVERY_EMPTY &&
                              peek.status != TP_JOURNAL_RECOVERY_BAD_MAGIC;
    tp_journal_peek_free(&peek);
    if (!valid_domain) {
        owned_candidate_destroy(candidate);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                  "discard requires a journal from this recovery domain");
    }
    status = tp_recovery_backend_candidate_delete(&candidate->journal_pin,
                                  claim->journal_path, err);
    owned_candidate_destroy(candidate);
    return status;
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
