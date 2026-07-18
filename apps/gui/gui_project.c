#include "gui_project.h"
#include "gui_project_internal.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gui_scan.h"
#include "gui_session_adapter.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_recovery.h"
#ifdef NTPACKER_GUI_SELFTEST
#include "tp_session_internal.h"
#endif

/* GUI mutation and Undo/Redo run through tp_session; reads use one cached owned
 * snapshot. Field edits coalesce by exact target until the gesture boundary, so
 * one gesture produces one transaction and one Undo step. */

gui_project_state s_project;

// #region helpers
static void snapshot_drop(void) {
    if (s_project.snapshot) {
        s_project.snapshot_lifetime_generation++;
    }
    tp_session_snapshot_destroy(s_project.snapshot);
    s_project.snapshot = NULL;
}

const tp_session_snapshot *gui_project_snapshot(void) {
    if (!s_project.snapshot && s_project.session) {
        tp_error err = {0};
        if (tp_session_snapshot_create(s_project.session, &s_project.snapshot, &err) != TP_STATUS_OK) {
            s_project.snapshot = NULL;
        }
    }
    return s_project.snapshot;
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return s_project.snapshot_lifetime_generation;
}

tp_session *gui_project_session_for_jobs(void) { return s_project.session; }

void gui_project_invalidate_sources(void) {
    gui_scan_invalidate_all();
    if (!s_project.session) {
        return;
    }
    tp_error err = {0};
    const tp_status status = tp_session_invalidate_sources(s_project.session, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return;
    }
    snapshot_drop();
}

uint64_t gui_project_snapshot_model_generation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot ? tp_session_snapshot_model_generation(snapshot) : 0U;
}

tp_status gui_project_snapshot_serialize(char **out, size_t *out_len,
                                         tp_error *err) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot ? tp_session_snapshot_serialize(snapshot, out, out_len, err)
                    : tp_error_set(err, TP_STATUS_NOT_FOUND,
                                   "GUI session snapshot is unavailable");
}

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

static int64_t recovery_now(void) {
    return (int64_t)time(NULL);
}

/* Record a void-context id-promotion failure so the UI can surface it. */
bool gui_project_take_op_error(char *out, size_t cap) {
    if (!s_project.op_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_project.op_error_msg);
    }
    s_project.op_error = false;
    return true;
}

/* fix3 [2]: fill `out` with the reason a flush's commit failed -- the drained op-error, else a NEUTRAL
 * fallback that fits save AND pack AND the dirty gate (the flush-failure abort paths share one wording,
 * no "saved"-specific verb). Consumes the op-error like gui_project_take_op_error. NULL-safe. */
void gui_project_flush_error(char *out, size_t cap) {
    if (!out || !cap) {
        return;
    }
    char m[256] = {0};
    if (!gui_project_take_op_error(m, sizeof m)) {
        (void)snprintf(m, sizeof m, "Your last edit could not be committed (disk full?) -- resolve it and try again.");
    }
    (void)snprintf(out, cap, "%s", m);
}

/* Stable application key for recovery sidecars. Bump when the journal contract
 * changes incompatibly so old slots cannot be misapplied. */
static tp_id128 recovery_key(void) {
    tp_id128 k;
    static const uint8_t b[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(k.bytes, b, sizeof b);
    return k;
}

/* Raise the status-bar channel for a degraded recovery notice. A live GUI host
 * requires recovery acknowledgement, so mutation stays blocked until a new
 * session can attach a healthy journal. Save/discard remain available. */
void gui_project__note_recovery_degraded(const char *msg) {
    s_project.op_error = true;
    (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg, "Recovery journal unavailable (%s) -- changes are blocked to preserve commit guarantees.",
                   msg ? msg : "unknown");
}

static bool recovery_configured(void) {
    return s_project.recovery_root[0] != '\0';
}

/* Attach one shared live recovery owner after the session identity is final.
 * The session owns the handle on every accepted attach path, including degraded
 * filesystem outcomes; GUI retains only configuration and presentation state. */
static void attach_recovery_live(tp_session *session) {
    if (!session) {
        return;
    }
    tp_error err = {0};
    if (s_project.recovery_required &&
        tp_session_require_recovery(session, &err) != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure(
            err.msg[0] ? err.msg : "recovery could not be required");
        return;
    }
    if (!recovery_configured()) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_session_identity identity = tp_session_snapshot_identity(snapshot);
    tp_id128 saved_fingerprint = tp_id128_nil();
    const bool has_saved_fingerprint =
        tp_session_snapshot_saved_file_fingerprint(snapshot, &saved_fingerprint);
    const tp_recovery_metadata metadata = {
        .timestamp = recovery_now(),
        .project_path = identity.kind == TP_IDENTITY_SAVED
                            ? identity.canonical_path
                            : "",
        .project_name = s_project.name,
        .file_fingerprint = has_saved_fingerprint ? &saved_fingerprint : NULL,
    };
    tp_rng rng = tp_rng_os();
    const tp_status status = tp_recovery_session_attach(
        s_project.recovery_root, recovery_key(), &rng, session, &metadata, &err);
    if (status != TP_STATUS_OK && !tp_session_recovery_available(session)) {
        gui_project_note_recovery_setup_failure(
            status == TP_STATUS_RECOVERY_BUSY
                ? "another ntpacker window owns this recovery slot"
                : "the recovery storage lock could not be acquired");
        gui_project__note_recovery_degraded(err.msg[0] ? err.msg
                                         : "could not checkpoint the recovery journal");
    }
}


static bool install_session(tp_session *next) {
    if (!next) {
        return false;
    }
    snapshot_drop();
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

void gui_project__next_transaction_id(char out[33]) {
    (void)snprintf(out, 33U, "%032llx", (unsigned long long)(s_project.txn_seq++));
}

void gui_project__note_session_reject(tp_status status, const tp_error *err) {
    const char *message = (err && err->msg[0]) ? err->msg : tp_status_str(status);
    if (status == TP_STATUS_JOURNAL_FAILED) {
        message = "Could not journal the edit -- disk full? Your change was not applied.";
    }
    s_project.op_error = true;
    (void)snprintf(s_project.op_error_msg, sizeof s_project.op_error_msg, "%s", message);
}

bool gui_project__refresh_after_session_commit(void) {
    /* Core is the sole semantic no-op owner. A no-change admission leaves the
     * revision unchanged, so keep the current projection and preview state. */
    if (s_project.snapshot &&
        tp_session_snapshot_revision(s_project.snapshot) == tp_session_revision(s_project.session)) {
        return false;
    }
    s_project.preview_stale = true;
    snapshot_drop();
    return true;
}

// #endregion

// #region lifecycle
/* Install a fresh clean untitled session. Attaches a fresh recovery journal at
 * the live slot (no-op when recovery is off). */
static void fresh_init(void) {
    (void)install_fresh_session();
    recompute_name();
    attach_recovery_live(s_project.session);
    s_project.preview_stale = false;
    snapshot_drop();
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
    snapshot_drop();
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

void gui_project_require_recovery(void) { s_project.recovery_required = true; }

/* Configure crash recovery from the host app-data root. Core generates the
 * per-process slot identity and owns all liveness and exclusion mechanics. */
void gui_project_enable_recovery(const char *root) {
    s_project.recovery_root[0] = '\0';
    if (!root || root[0] == '\0') {
        return;
    }
    tp_error err = {0};
    const tp_status status = tp_recovery_root_validate(
        root, recovery_key(), &err);
    if (status == TP_STATUS_OK) {
        (void)snprintf(s_project.recovery_root, sizeof s_project.recovery_root, "%s", root);
    }
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure(
            err.msg[0] ? err.msg : "the recovery root is unavailable");
    }
}

void gui_project_note_recovery_setup_failure(const char *reason) {
    s_project.recovery_setup_notice_pending = true;
    (void)snprintf(s_project.recovery_setup_notice, sizeof s_project.recovery_setup_notice,
                   "Editing is unavailable because crash recovery could not start (%s).",
                   (reason && reason[0] != '\0') ? reason : "startup setup failed");
}

static void note_recovery_scan_limited(void) {
    s_project.recovery_setup_notice_pending = true;
    (void)snprintf(s_project.recovery_setup_notice, sizeof s_project.recovery_setup_notice,
                   "Some previous recovery sessions were not scanned because the startup safety budget was reached.");
}

/* Drains the one-shot recovery-unavailable notice. */
bool gui_project_take_recovery_setup_notice(char *out, size_t cap) {
    if (!s_project.recovery_setup_notice_pending) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_project.recovery_setup_notice[0] != '\0'
                                             ? s_project.recovery_setup_notice
                                             : "Editing is unavailable because crash recovery could not start.");
    }
    s_project.recovery_setup_notice_pending = false;
    s_project.recovery_setup_notice[0] = '\0';
    return true;
}

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

// #region recovery resolution
int gui_recovery_collect(gui_recovery_list *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!recovery_configured() || !out) {
        return 0;
    }
    tp_error err = {0};
    tp_status status = tp_recovery_scan_root(
        s_project.recovery_root, recovery_key(), s_project.session, out, &err);
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure("the recovery directory could not be scanned");
        return 0;
    }
    if (out->has_more) {
        note_recovery_scan_limited();
    }
    return (int)out->count;
}

static void recovery_copy_error(char *out, size_t cap, tp_status status,
                                const tp_error *err) {
    if (out && cap) {
        (void)snprintf(out, cap, "%s",
                       err && err->msg[0] ? err->msg : tp_status_str(status));
    }
}

static tp_status
gui_recovery_resolve(const char *journal_path, gui_recovery_action action,
                     const char *target_path,
                     char *err_out, size_t err_cap) {
    if (err_out && err_cap) {
        err_out[0] = '\0';
    }
    if (!journal_path || journal_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no recovery journal to resolve");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (action != GUI_RECOVERY_DISCARD &&
        action != GUI_RECOVERY_SAVE_ORIGINAL &&
        action != GUI_RECOVERY_SAVE_AS) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "unknown recovery action");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_error err = {0};
    if (!recovery_configured()) {
        const tp_status missing = tp_error_set(
            &err, TP_STATUS_INVALID_ARGUMENT,
            "recovery domain is not configured");
        recovery_copy_error(err_out, err_cap, missing, &err);
        return missing;
    }
    tp_recovery_action core_action = TP_RECOVERY_ACTION_DISCARD;
    if (action == GUI_RECOVERY_SAVE_ORIGINAL) {
        core_action = TP_RECOVERY_ACTION_SAVE_ORIGINAL;
    } else if (action == GUI_RECOVERY_SAVE_AS) {
        core_action = TP_RECOVERY_ACTION_SAVE_AS;
    }
    tp_rng rng = tp_rng_os();
    tp_recovery_resolve_result result;
    const tp_status status = tp_recovery_resolve_journal(
        s_project.recovery_root, recovery_key(), journal_path, s_project.session, core_action,
        target_path, &rng, &result, &err);
    if (status != TP_STATUS_OK) {
        recovery_copy_error(err_out, err_cap, status, &err);
    }
    return status;
}


tp_status gui_recovery_resolve_entry(const gui_recovery_entry *entry, gui_recovery_action action,
                                     const char *target_path, char *err_out, size_t err_cap) {
    if (!entry) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no recovery entry to resolve");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return gui_recovery_resolve(entry->journal_path, action, target_path,
                                err_out, err_cap);
}
// #endregion

// #region lifecycle dev seams (selftest only)
#ifdef NTPACKER_GUI_SELFTEST
tp_session *gui_project__test_session(void) { return s_project.session; }

#endif
// #endregion

// #region accessors
const char *gui_project_path(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return tp_session_snapshot_canonical_path(snapshot);
}
const char *gui_project_display_name(void) { return s_project.name; }
bool gui_project_has_path(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return tp_session_snapshot_identity(snapshot).kind == TP_IDENTITY_SAVED;
}
/* Dirty is a scalar captured in the cached immutable snapshot. The first read
 * after a commit refreshes the snapshot; unchanged frames only read the scalar. */
bool gui_project_is_dirty(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return snapshot && tp_session_snapshot_dirty(snapshot);
}
bool gui_project_is_stale(void) { return s_project.preview_stale; }
// #endregion

// #region dirty/stale choke point
/* Post-commit choke point: a REAL committed mutation makes the preview stale and bumps the
 * session generation. Undo history + dirty are core-owned. `act` is vestigial (coalescing moved to the
 * transaction buffer) but kept for call-site clarity + the dev-seam signature. */
void gui_project_mark_packed(void) { s_project.preview_stale = false; }
void gui_project_mark_stale(void) { s_project.preview_stale = true; }
void gui_project_tick(double now_seconds) { s_project.now = now_seconds; }
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
    snapshot_drop();
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
    snapshot_drop();
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
    attach_recovery_live(s_project.session);
    s_project.preview_stale = false;
    gui_project_invalidate_sources();
    snapshot_drop();
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
    attach_recovery_live(s_project.session);
    s_project.preview_stale = true; /* nothing packed this session yet */
    gui_project_invalidate_sources();
    snapshot_drop();
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
    snapshot_drop();
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
    snapshot_drop();
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
