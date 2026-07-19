#include "tp_core/tp_recovery.h"

#include <stdio.h>
#include <string.h>

#include "tp_fs_internal.h"
#include "tp_recovery_internal.h"
#include "tp_recovery_state_internal.h"
#include "tp_session_internal.h"

static bool s_test_fail_next_resolve_verify;

void tp_recovery__test_fail_next_resolve_verify(void) {
    s_test_fail_next_resolve_verify = true;
}

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
    bool journal_deleted = false;
    tp_session_save_result receipt;
    memset(&receipt, 0, sizeof receipt);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_store_claim(store, canonical_journal, &claim, err);
    }
    if (status == TP_STATUS_OK && action == TP_RECOVERY_ACTION_DISCARD) {
        status = tp_recovery_claim_discard(claim, err);
        journal_deleted = status == TP_STATUS_OK;
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
            status = tp_recovery_resolution_finalize(
                resolution, &receipt, &journal_deleted, err);
        }
    }
    if (status == TP_STATUS_OK) {
        out->journal_deleted = journal_deleted;
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
