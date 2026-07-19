#include "tp_recovery_state_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_session_internal.h"

static tp_recovery_store *s_test_foreign_store;
static tp_recovery_claim *s_test_foreign_claim;

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
    candidate->recovery_status = recovery.status;
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
    const tp_session_save_result *receipt, bool *journal_deleted,
    tp_error *err) {
    if (journal_deleted) {
        *journal_deleted = false;
    }
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
    const bool preserve_original =
        candidate->recovery_status == TP_JOURNAL_RECOVERY_CORRUPT ||
        candidate->recovery_status == TP_JOURNAL_RECOVERY_TRUNCATED;
    if (!preserve_original) {
        status = tp_recovery_backend_candidate_delete(&candidate->journal_pin,
                                                       claim->journal_path, err);
    }
    if (status == TP_STATUS_OK) {
        if (journal_deleted) {
            *journal_deleted = !preserve_original;
        }
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
