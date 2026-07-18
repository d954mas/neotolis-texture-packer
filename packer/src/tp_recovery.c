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


static tp_recovery_store *s_test_foreign_store;
static tp_recovery_claim *s_test_foreign_claim;
static bool s_test_fail_next_resolve_verify;













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
