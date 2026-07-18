#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>

#include "tp_core/tp_identity.h"
static void recompute_name(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_session_identity identity = tp_session_snapshot_identity(snapshot);
    const char *path = identity.kind == TP_IDENTITY_SAVED ? identity.canonical_path : "";
    if (path[0] == '\0') {
        (void)snprintf(s_project.name, sizeof s_project.name, "untitled");
        return;
    }
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    (void)snprintf(s_project.name, sizeof s_project.name, "%s", base);
}

static bool install_session(tp_session *next) {
    if (!next) {
        return false;
    }
    gui_project__snapshot_drop();
    if (s_project.session) {
        (void)tp_session_discard(s_project.session, NULL);
    }
    tp_session_destroy(s_project.session);
    s_project.session = next;
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
    return true;
}

static bool install_fresh_session(void) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    tp_session *next = NULL;
    if (tp_session_create_default_project(&rng, &next, &err) != TP_STATUS_OK) {
        return false;
    }
    return install_session(next);
}

// #region lifecycle
/* Install a fresh clean untitled session. Attaches a fresh recovery journal at
 * the live slot (no-op when recovery is off). */
static void fresh_init(void) {
    (void)install_fresh_session();
    recompute_name();
    gui_project__attach_recovery_live(s_project.session);
    s_project.preview_stale = false;
    gui_project__snapshot_drop();
}

void gui_project_init(void) {
    if (s_project.session) {
        return;
    }
    gui_project_pending_discard();
    fresh_init();
}

void gui_project_shutdown(void) {
    /* The engine currently gives us no cancellable window-close callback. Flush a last buffered edit,
     * then keep the owned slot whenever the model is dirty or the flush failed. This turns X/Alt+F4
     * into a recoverable close. Only an explicit Exit -> Discard confirmation may remove dirty work. */
    (void)(!s_project.session || gui_project_flush_pending());
    gui_project_pending_discard();
    gui_project__snapshot_drop();
    if (s_project.session && s_project.discard_recovery_on_shutdown) {
        (void)tp_session_discard(s_project.session, NULL);
    }
    tp_session_destroy(s_project.session); /* frees the sole owned model/project/history/journal */
    s_project.session = NULL;
    s_project.recovery_root[0] = '\0';
    s_project.discard_recovery_on_shutdown = false;
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
}

void gui_project_discard_recovery_on_shutdown(void) { s_project.discard_recovery_on_shutdown = true; }

bool gui_project_take_save_notice(char *out, size_t cap) {
    if (!s_project.save_notice_pending) {
        return false;
    }
    if (out && cap > 0U) {
        (void)snprintf(out, cap, "%s", s_project.save_notice);
    }
    s_project.save_notice_pending = false;
    s_project.save_notice[0] = '\0';
    return true;
}
// #endregion

// #region undo / redo
/* A pending buffered edit counts as undoable (undo flushes it into a step, then reverts it). */
bool gui_project_can_undo(void) {
    return tp_session_recovery_available(s_project.session) &&
           (s_project.pending_valid || tp_session_can_undo(s_project.session));
}
bool gui_project_can_redo(void) { return tp_session_can_redo(s_project.session); }
int gui_project_undo_depth(void) { return tp_session_undo_depth(s_project.session); }
int gui_project_redo_depth(void) { return tp_session_redo_depth(s_project.session); }

/* Undo/Redo is journal-gated inside core. Record a rejected history transition on
 * the same structured soft-error channel as a rejected transaction; no GUI-side
 * durability compensation is needed (and no false "success then degrade" is possible). */
static void note_history_reject(const char *verb, tp_status st, const tp_error *err) {
    const char *detail = (err && err->msg[0]) ? err->msg : tp_status_str(st);
    s_project.op_error = true;
    if (st == TP_STATUS_JOURNAL_FAILED) {
        (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg,
                       "Could not journal the %s -- disk full? Nothing was changed.", verb);
    } else {
        (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg, "%s rejected: %s", verb, detail);
    }
}

/* Undo reverses the most recent committed transaction via its captured semantic diff.
 * A buffered gesture is committed FIRST (its own step) so Ctrl+Z reverts the in-flight drag.
 * Dirty is identity-derived, so an undo back to the saved baseline reads clean. */
bool gui_project_undo(void) {
    if (!gui_project_flush_pending()) {
        return false;
    }
    tp_error e = {0};
    tp_status st = tp_session_undo(s_project.session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("undo", st, &e);
        }
        return false;
    }
    gui_project__snapshot_drop();
    s_project.preview_stale = true;             /* restored model != last-packed; packing is blocked -> always stale */
    gui_project_invalidate_sources();
    return true;
}

bool gui_project_redo(void) {
    if (!gui_project_flush_pending()) {
        return false;
    }
    tp_error e = {0};
    tp_status st = tp_session_redo(s_project.session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("redo", st, &e);
        }
        return false;
    }
    gui_project__snapshot_drop();
    s_project.preview_stale = true;
    gui_project_invalidate_sources();
    return true;
}
// #endregion

// #region file operations
bool gui_project_new(void) {
    gui_project_pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    if (!install_fresh_session()) {
        return false;
    }
    recompute_name();
    gui_project__attach_recovery_live(s_project.session);
    s_project.preview_stale = false;
    gui_project_invalidate_sources();
    gui_project__snapshot_drop();
    return true;
}

tp_status gui_project_open(const char *path, char *err_out, size_t err_cap) {
    if (!path || strlen(path) >= TP_IDENTITY_PATH_MAX) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "project path exceeds the supported %zu-byte limit",
                           (size_t)TP_IDENTITY_PATH_MAX - 1U);
        }
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_error err = {0};
    char canonical_path[TP_IDENTITY_PATH_MAX];
    tp_status canonical = tp_identity_project_path_canonical(
        path, canonical_path, sizeof canonical_path, &err);
    if (canonical != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(canonical));
        }
        return canonical;
    }
    gui_project_pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    tp_rng rng = tp_rng_os();
    tp_session *opened = NULL;
    tp_status st = tp_session_open(canonical_path, &rng, &opened, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    if (!install_session(opened)) {
        tp_session_destroy(opened);
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : "could not install opened session");
        }
        return TP_STATUS_OOM;
    }
    recompute_name();
    gui_project__attach_recovery_live(s_project.session);
    s_project.preview_stale = true; /* nothing packed this session yet */
    gui_project_invalidate_sources();
    gui_project__snapshot_drop();
    return TP_STATUS_OK;
}

tp_status gui_project_save(char *err_out, size_t err_cap) {
    if (!gui_project_has_path()) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no path (use Save As)");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!gui_project_flush_pending()) {
        gui_project_flush_error(err_out, err_cap);
        return TP_STATUS_JOURNAL_FAILED;
    }
    tp_error err = {0};
    tp_session_save_result result;
    const tp_status st = tp_session_save(s_project.session, &result, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    gui_project__snapshot_drop();
    recompute_name();
    if (result.recovery_degraded) {
        gui_project__note_recovery_degraded("recovery checkpoint compaction failed");
    }
    if (result.file_durability_degraded) {
        s_project.save_notice_pending = true;
        (void)snprintf(
            s_project.save_notice, sizeof s_project.save_notice,
            "Saved, but storage durability could not be confirmed");
    }
    return TP_STATUS_OK;
}

/* Master spec 14.2: tp_session exclusively owns the exact-byte Open/Save
 * baseline and rejects an external replacement before publication. */

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    if (!path || strlen(path) >= TP_IDENTITY_PATH_MAX) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "project path exceeds the supported %zu-byte limit",
                           (size_t)TP_IDENTITY_PATH_MAX - 1U);
        }
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_error err = {0};
    char canonical_path[TP_IDENTITY_PATH_MAX];
    tp_status canonical = tp_identity_project_path_canonical(
        path, canonical_path, sizeof canonical_path, &err);
    if (canonical != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(canonical));
        }
        return canonical;
    }
    /* Never save an older snapshot when the pending edit failed to commit. */
    if (!gui_project_flush_pending()) {
        gui_project_flush_error(err_out, err_cap);
        return TP_STATUS_JOURNAL_FAILED;
    }
    err = (tp_error){0};
    tp_session_save_result result;
    const tp_status st = tp_session_save_as(s_project.session, canonical_path, &result, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s",
                           err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    gui_project__snapshot_drop();
    recompute_name();
    if (result.recovery_degraded) {
        gui_project__note_recovery_degraded("recovery checkpoint compaction failed");
    }
    if (result.file_durability_degraded) {
        s_project.save_notice_pending = true;
        (void)snprintf(
            s_project.save_notice, sizeof s_project.save_notice,
            "Saved, but storage durability could not be confirmed");
    }
    return TP_STATUS_OK;
}
// #endregion
