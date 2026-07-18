#include "gui_project_internal.h"

#include <string.h>

#include "gui_session_adapter.h"

/* One buffered value edit. Same-target edits replace the value; a different
 * target or an explicit gesture boundary commits the current operation first. */
#define GUI_COALESCE_SECS 0.30
static bool key_eq(const gui_coalesce_key *a, const gui_coalesce_key *b) {
    return a->kind == b->kind && a->field == b->field &&
           tp_id128_eq(a->atlas_id, b->atlas_id) &&
           tp_id128_eq(a->source_id, b->source_id) &&
           strcmp(a->sprite, b->sprite) == 0;
}

/* Discard the buffered edit WITHOUT committing (new/open replace the whole project). */
void gui_project_pending_discard(void) {
    if (s_project.pending_valid) {
        tp_operation_free(&s_project.pending_op);
        memset(&s_project.pending_op, 0, sizeof s_project.pending_op);
        s_project.pending_valid = false;
    }
}

/* False means the pending gesture could not be committed. Save and history
 * callers must stop rather than act on an older committed state. */
bool gui_project_flush_pending(void) {
    if (!s_project.pending_valid) {
        return true;
    }
    tp_operation op = s_project.pending_op; /* move: ownership of the arms transfers to the local */
    const int64_t expected_revision = s_project.pending_expected_revision;
    const bool preview_stale_before = s_project.pending_preview_stale_before;
    memset(&s_project.pending_op, 0, sizeof s_project.pending_op);
    s_project.pending_valid = false;
    if (op.kind == TP_OP_ATLAS_SETTINGS_SET) {
        char transaction_id[33];
        gui_project__next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_atlas_settings(
            s_project.session, op.atlas_id, expected_revision, &op.u.atlas_settings,
            transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!gui_project__refresh_after_session_commit()) {
                s_project.preview_stale = preview_stale_before;
            }
            return true;
        }
        gui_project__note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_SPRITE_OVERRIDE_SET &&
        !tp_id128_is_nil(op.u.sprite_set.source_id)) {
        char transaction_id[33];
        gui_project__next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_sprite_override(
            s_project.session, op.atlas_id, op.u.sprite_set.source_id,
            op.u.sprite_set.src_key, expected_revision,
            &op.u.sprite_set, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!gui_project__refresh_after_session_commit()) {
                s_project.preview_stale = preview_stale_before;
            }
            return true;
        }
        gui_project__note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_ANIMATION_SETTINGS_SET) {
        char transaction_id[33];
        gui_project__next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_animation_settings(
            s_project.session, op.atlas_id, op.u.anim_settings.anim_id,
            expected_revision, &op.u.anim_settings, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!gui_project__refresh_after_session_commit()) {
                s_project.preview_stale = preview_stale_before;
            }
            return true;
        }
        gui_project__note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_TARGET_SET) {
        char transaction_id[33];
        gui_project__next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_target(
            s_project.session, op.atlas_id, op.u.target_set.target_id,
            expected_revision, &op.u.target_set, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!gui_project__refresh_after_session_commit()) {
                s_project.preview_stale = preview_stale_before;
            }
            return true;
        }
        gui_project__note_session_reject(status, &err);
        return false;
    }
    /* Every coalescable family is session-owned. Reaching this branch means a
     * new operation kind was added without an admission adapter: fail closed. */
    tp_operation_free(&op);
    tp_error err = {0};
    (void)tp_error_set(&err, TP_STATUS_INVALID_ARGUMENT,
                       "buffered operation has no session admission route");
    gui_project__note_session_reject(TP_STATUS_INVALID_ARGUMENT, &err);
    return false;
}

/* Called AT THE TOP of a coalescable mutator with its key, BEFORE the mutator reads the model:
 * flush the buffered edit when the incoming key DIFFERS, so a) distinct edits never merge and
 * b) an RMW read sees all prior edits committed. GESTURE-SCOPED (owner decision, ADR 0015): the
 * flush trigger is the gesture boundary (slider release / field Enter+blur / discrete click),
 * NOT a timer -- so a SAME-key edit never flushes here regardless of how long the gesture takes
 * (a slow drag stays one transaction). The time window is a FALLBACK only (gui_project_flush_elapsed).
 * After this returns, s_project.pending_valid is true IFF a same-key pending remains (the replace target). */
void gui_project_pending_route(const gui_coalesce_key *k) {
    if (s_project.pending_valid && !key_eq(k, &s_project.pending_key)) {
        /* fix2: the bool is intentionally IGNORED here. On a journal-failed flush the different-key
         * gesture is dropped WITH the op-error surfaced (commit_txn_now set it); the caller then only
         * BUFFERS a new (uncommitted) edit -- there is no persist/discard "proceed as clean" decision to
         * abort. gui_project_flush_elapsed (the timer fallback) is the same case. Audited fix2 [3]. */
        (void)gui_project_flush_pending();
    }
}

/* Buffer `op` (TAKES OWNERSHIP of the arms) under `k`. Precondition (gui_project_pending_route ran): a still-
 * valid pending is same-key -> replace its value (latest wins). Preview goes stale immediately;
 * the commit (and model_ver bump) is deferred to the flush. */
bool gui_project_pending_offer(const gui_coalesce_key *k, tp_operation *op) {
    if (!tp_session_recovery_available(s_project.session)) {
        tp_operation_free(op);
        gui_project__note_recovery_degraded("mutation is unavailable");
        return false;
    }
    if (!s_project.pending_valid) {
        s_project.pending_preview_stale_before = s_project.preview_stale;
    }
    if (s_project.pending_valid) {
        tp_operation_free(&s_project.pending_op); /* same key: replace the value */
    }
    s_project.pending_op = *op; /* shallow move; caller must not free `op` after this */
    s_project.pending_key = *k;
    s_project.pending_time = s_project.now;
    s_project.pending_valid = true;
    s_project.preview_stale = true; /* immediate stale feedback while the gesture buffers */
    return true;
}

/* Fallback for a control that missed its gesture boundary. main.c calls this
 * only when no pointer gesture or text input remains active. */
void gui_project_flush_elapsed(void) {
    if (s_project.pending_valid && (s_project.now - s_project.pending_time) > GUI_COALESCE_SECS) {
        gui_project_flush_pending();
    }
}

/* Exposes the full buffered slice9 value so canvas guides update before the
 * gesture commits. False asks the caller to use the committed snapshot. */
bool gui_project_peek_pending_slice9(const gui_sprite_ref *sprite, int out_lrtb[4]) {
    if (!sprite || !out_lrtb || !s_project.pending_valid ||
        s_project.pending_key.kind != CK_SPRITE_SLICE9 ||
        !tp_id128_eq(s_project.pending_key.atlas_id, sprite->atlas_id) ||
        !tp_id128_eq(s_project.pending_key.source_id, sprite->source_id)) {
        return false;
    }
    if (strcmp(s_project.pending_key.sprite, sprite->source_key ? sprite->source_key : "") != 0) {
        return false;
    }
    for (int k = 0; k < 4; k++) {
        out_lrtb[k] = (int)s_project.pending_op.u.sprite_set.slice9[k];
    }
    return true;
}
// #endregion
