#include "gui_project.h"
#include "gui_project_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "gui_scan.h"
#include "gui_session_adapter.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_source_plan.h"
#include "tp_core/tp_srckey.h"
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

static char *dupstr(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
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

/* Generates a fresh non-nil structural id via the OS RNG; false on an RNG fault. */
static bool gen_id(tp_id128 *out) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    const tp_status status = tp_id128_generate(&rng, out, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
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

// #region pending-buffer primitives
static gui_coalesce_key make_key(gui_coalesce_kind kind, int field) {
    gui_coalesce_key k;
    memset(&k, 0, sizeof k);
    k.kind = kind;
    k.atlas_id = tp_id128_nil();
    k.source_id = tp_id128_nil();
    k.field = field;
    return k;
}

static gui_coalesce_key make_atlas_key(tp_id128 atlas_id, int field) {
    gui_coalesce_key key = make_key(CK_ATLAS_SETTING, field);
    key.atlas_id = atlas_id;
    return key;
}

static bool make_sprite_key(gui_coalesce_kind kind, const gui_sprite_ref *sprite,
                            int field, gui_coalesce_key *out) {
    if (!sprite || !sprite->source_key || !out) {
        return false;
    }
    const size_t length = strlen(sprite->source_key);
    if (length >= sizeof out->sprite) {
        tp_error error = {0};
        (void)tp_error_set(&error, TP_STATUS_OUT_OF_BOUNDS,
                           "sprite source key exceeds the supported limit");
        gui_project__note_session_reject(TP_STATUS_OUT_OF_BOUNDS, &error);
        return false;
    }
    *out = make_key(kind, field);
    out->atlas_id = sprite->atlas_id;
    out->source_id = sprite->source_id;
    memcpy(out->sprite, sprite->source_key, length + 1U);
    return true;
}



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

/* Rebase only an intent that was current immediately before gui_project_pending_route committed
 * a different GUI-owned gesture.  A ref that was already stale at entry must stay
 * stale so session admission rejects it instead of silently overwriting newer work. */
static int64_t revision_after_owned_route(int64_t captured_revision,
                                          int64_t revision_before_route) {
    const int64_t current_revision = tp_session_revision(s_project.session);
    return captured_revision == revision_before_route &&
                   current_revision != revision_before_route
               ? current_revision
               : captured_revision;
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

// #region mutation wrappers (each builds typed op(s) + commits through the model)
/* CONVENTION (UAF class, ADR 0015): these wrappers call gui_project_flush_pending() first, which may
 * commit a buffered gesture and invalidate the cached session snapshot. Any wrapper that then reads a
 * caller-supplied `const char *` sourced from snapshot DTO storage MUST duplicate it before the flush --
 * see gui_project_set_target / gui_project_remove_animation. */
int gui_project_add_atlas(void) {
    /* fix2 [3]: a journal-failed flush dropped the buffered gesture (op-error already surfaced) -> abort
     * this structural op too, never pair a silent lost edit with an unrelated committed change. */
    if (!gui_project_flush_pending()) {
        return -1;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot) {
        return -1;
    }
    char name[64];
    char out_path[TP_IDENTITY_PATH_MAX];
    const char *exporter_id = NULL;
    bool target_enabled = false;
    tp_error err = {0};
    const tp_status defaults_status = tp_session_snapshot_next_atlas_defaults(
        snapshot, name, sizeof name, out_path, sizeof out_path, &exporter_id,
        &target_enabled, &err);
    if (defaults_status != TP_STATUS_OK) {
        gui_project__note_session_reject(defaults_status, &err);
        return -1;
    }
    tp_id128 new_id;
    if (!gen_id(&new_id)) {
        return -1;
    }
    tp_id128 target_id;
    if (!gen_id(&target_id)) {
        return -1;
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    err = (tp_error){0};
    const tp_status status = gui_session_create_atlas(
        s_project.session, new_id, target_id, tp_session_snapshot_revision(snapshot), name,
        exporter_id, out_path, target_enabled, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return -1;
    }
    gui_project__refresh_after_session_commit();
    snapshot = gui_project_snapshot();
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; i++) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, new_id)) {
            return i;
        }
    }
    return -1;
}

/* fix3 [0]: returns TRUE iff the removal actually committed (false on the flush-fail abort, an
 * invalid index, or a commit reject) so the deferred handler shows "Removed X (Ctrl+Z)" + resets
 * selection ONLY on a real removal -- never a false "Removed" over a dropped gesture. */
bool gui_project_remove_atlas(tp_id128 atlas_id, int64_t expected_revision) {
    const int64_t revision_before_flush = s_project.session ? tp_session_revision(s_project.session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (tp_id128_is_nil(atlas_id) || !s_project.session) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_atlas(
        s_project.session, atlas_id, expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
    gui_project__refresh_after_session_commit();
    return true;
}

gui_add_status gui_project_add_source_kind(tp_id128 atlas_id,
                                           int64_t expected_revision,
                                           const char *path,
                                           tp_source_kind kind) {
    int added = 0;
    int duplicate = 0;
    if (!gui_project_add_sources(atlas_id, expected_revision, &path, 1, kind,
                                 &added, &duplicate)) {
        return GUI_ADD_FAILED;
    }
    return added > 0 ? GUI_ADD_ADDED : (duplicate > 0 ? GUI_ADD_DUPLICATE
                                                       : GUI_ADD_FAILED);
}

gui_add_status gui_project_add_source(tp_id128 atlas_id,
                                      int64_t expected_revision,
                                      const char *path) {
    return gui_project_add_source_kind(atlas_id, expected_revision, path,
                                       TP_SOURCE_KIND_FOLDER);
}

/* Batch-add multiple sources as ONE atomic transaction (H/P2-13) -- the "Add Files" multi-select path,
 * which previously committed one txn PER file (N undo steps + a mid-batch failure left a partial add).
 * The shared planner rejects invalid path elements and skips paths already in the atlas or queued in this
 * batch (reported through *out_dup), so the committed txn holds only distinct new sources.
 * Commits nothing when nothing is new. Returns true iff the txn committed (or was a clean no-op); false
 * on flush-fail / OOM / a core reject (the model is then byte-unchanged). Both out-counts are always set
 * (0 on early failure). One commit -> ONE undo step for the whole multi-select. */
bool gui_project_add_sources(tp_id128 atlas_id, int64_t expected_revision,
                             const char *const *paths, int n_paths, tp_source_kind kind,
                             int *out_added, int *out_dup) {
    if (out_added) {
        *out_added = 0;
    }
    if (out_dup) {
        *out_dup = 0;
    }
    const int64_t revision_before_flush = s_project.session ? tp_session_revision(s_project.session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: a journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, atlas_id) ||
        n_paths <= 0 || !paths) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    tp_source_batch_plan plan = {0};
    tp_error plan_error = {0};
    const tp_status plan_status = tp_source_batch_plan_create(
        snapshot, atlas_id, paths, n_paths, &plan, &plan_error);
    if (plan_status != TP_STATUS_OK) {
        gui_project__note_session_reject(plan_status, &plan_error);
        return false;
    }
    const int m = plan.count;
    const int dup = plan.duplicate_count;
    tp_id128 *ids = m > 0
                        ? (tp_id128 *)calloc((size_t)m, sizeof *ids)
                        : NULL;
    const char **distinct = m > 0
                                ? (const char **)calloc((size_t)m,
                                                        sizeof *distinct)
                                : NULL;
    if (m > 0 && (!ids || !distinct)) {
        free(ids);
        free(distinct);
        tp_source_batch_plan_free(&plan);
        return false;
    }
    for (int i = 0; i < m; i++) {
        if (!gen_id(&ids[i])) {
            free(ids);
            free(distinct);
            tp_source_batch_plan_free(&plan);
            if (out_dup) {
                *out_dup = dup; /* preserve the dup tally counted before this OOM/RNG fault */
            }
            return false;
        }
        distinct[i] = plan.items[i].path;
    }
    bool ok = true;
    if (m > 0) {
        char transaction_id[33];
        gui_project__next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_add_sources(
            s_project.session, atlas_id, ids, distinct, m,
            (tp_snapshot_source_kind)kind,
            expected_revision, transaction_id, &err);
        ok = status == TP_STATUS_OK;
        if (!ok) {
            gui_project__note_session_reject(status, &err);
        } else {
            gui_project_invalidate_sources();
            gui_project__refresh_after_session_commit();
        }
    }
    free(ids);
    free(distinct);
    tp_source_batch_plan_free(&plan);
    if (out_added) {
        *out_added = ok ? m : 0;
    }
    if (out_dup) {
        *out_dup = dup;
    }
    return ok;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_source(tp_id128 atlas_id, tp_id128 source_id,
                               int64_t expected_revision) {
    const int64_t revision_before_flush = s_project.session ? tp_session_revision(s_project.session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_project.session || tp_id128_is_nil(atlas_id) || tp_id128_is_nil(source_id)) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_source(
        s_project.session, atlas_id, source_id, expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_set_atlas_name(tp_id128 atlas_id, int64_t expected_revision, const char *name) {
    const int64_t revision_before_flush = s_project.session ? tp_session_revision(s_project.session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_project.session || !name) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_rename_atlas(
        s_project.session, atlas_id, expected_revision, name, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

tp_status gui_project_copy_atlas_name(tp_id128 atlas_id, char *out, size_t capacity,
                                      tp_error *err) {
    return gui_session_copy_atlas_name(gui_project_snapshot(), atlas_id, out, capacity, err);
}

bool gui_project_set_sprite_rename(const gui_sprite_ref *sprite, const char *rename) {
    const int64_t revision_before_flush = s_project.session ? tp_session_revision(s_project.session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_project.session || !sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0') {
        return false;
    }
    int64_t expected_revision = sprite->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_set_sprite_name(
        s_project.session, sprite->atlas_id, sprite->source_id, sprite->source_key,
        expected_revision, rename, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

/* Maps a gui_atlas_field to its op mask bit + fills the matching payload field. */
static bool fill_atlas_knob(tp_op_atlas_settings *s, gui_atlas_field f, int iv, float fv) {
    switch (f) {
        case GUI_ATLAS_MAX_SIZE: s->max_size = iv; s->mask = TP_AF_MAX_SIZE; return true;
        case GUI_ATLAS_PADDING: s->padding = iv; s->mask = TP_AF_PADDING; return true;
        case GUI_ATLAS_MARGIN: s->margin = iv; s->mask = TP_AF_MARGIN; return true;
        case GUI_ATLAS_EXTRUDE: s->extrude = iv; s->mask = TP_AF_EXTRUDE; return true;
        case GUI_ATLAS_ALPHA_THRESHOLD: s->alpha_threshold = iv; s->mask = TP_AF_ALPHA_THRESHOLD; return true;
        case GUI_ATLAS_MAX_VERTICES: s->max_vertices = iv; s->mask = TP_AF_MAX_VERTICES; return true;
        case GUI_ATLAS_SHAPE: s->shape = iv; s->mask = TP_AF_SHAPE; return true;
        case GUI_ATLAS_ALLOW_TRANSFORM: s->allow_transform = (iv != 0); s->mask = TP_AF_ALLOW_TRANSFORM; return true;
        case GUI_ATLAS_POWER_OF_TWO: s->power_of_two = (iv != 0); s->mask = TP_AF_POWER_OF_TWO; return true;
        case GUI_ATLAS_PIXELS_PER_UNIT: s->pixels_per_unit = fv; s->mask = TP_AF_PIXELS_PER_UNIT; return true;
    }
    return false;
}

bool gui_project_set_atlas_setting(tp_id128 atlas_id, int64_t expected_revision,
                                   gui_atlas_field field, int ivalue, float fvalue) {
    if (!s_project.session || tp_id128_is_nil(atlas_id)) {
        return false;
    }
    gui_coalesce_key ck = make_atlas_key(atlas_id, (int)field);
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck); /* flush a different knob's pending BEFORE reading this atlas */
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, atlas_id)) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = atlas_id;
    if (!fill_atlas_knob(&op.u.atlas_settings, field, ivalue, fvalue)) {
        tp_operation_free(&op);
        return false;
    }
    expected_revision = revision_after_owned_route(expected_revision,
                                                   revision_before_route);
    s_project.pending_expected_revision = expected_revision;
    return gui_project_pending_offer(&ck, &op);
}

/* Buffers a sprite.override.set at its canonical {source_id, source-local key}.
 * Core applies the masked fields on commit then prunes an all-default record. The
 * caller has already run gui_project_pending_route(k). */
static bool sprite_override_offer(const gui_sprite_ref *sprite, tp_op_sprite_set payload,
                                  const gui_coalesce_key *k) {
    if (!sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0') {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = sprite->atlas_id;
    op.u.sprite_set = payload;
    op.u.sprite_set.source_id = sprite->source_id;
    op.u.sprite_set.src_key = dupstr(sprite->source_key);
    if (!op.u.sprite_set.src_key) {
        tp_operation_free(&op);
        return false;
    }
    s_project.pending_expected_revision = sprite->expected_revision;
    return gui_project_pending_offer(k, &op);
}

bool gui_project_set_sprite_origin(const gui_sprite_ref *sprite, int axis, float value) {
    if (!s_project.session || !sprite || axis < 0 || axis > 1) {
        return false; /* 0 = Pivot X, 1 = Pivot Y */
    }
    /* Component-precise key (mirror slice9): X and Y are DIFFERENT keys, so editing the OTHER axis
     * flushes the buffered one FIRST -- then the read-modify-write seed below reads the COMMITTED
     * value of the non-edited component and the two components can never merge against a stale model.
     * (The pre-fix code keyed both axes the same AND seeded from a view-side committed read, so a
     * back-to-back X then Y replaced {x=new,y=old} with {x=old,y=new} and silently lost the X edit.) */
    gui_coalesce_key ck;
    if (!make_sprite_key(CK_SPRITE_ORIGIN, sprite, axis, &ck)) {
        return false;
    }
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_sprite_ref routed = *sprite;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *ov = snapshot ? tp_session_snapshot_sprite_by_key(
                                                  snapshot, sprite->atlas_id,
                                                  sprite->source_id,
                                                  sprite->source_key)
                                            : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_ORIGIN;
    p.origin_x = (axis == 0) ? value : (ov ? ov->origin_x : TP_PROJECT_ORIGIN_DEFAULT);
    p.origin_y = (axis == 1) ? value : (ov ? ov->origin_y : TP_PROJECT_ORIGIN_DEFAULT);
    return sprite_override_offer(&routed, p, &ck);
}

bool gui_project_set_sprite_slice9(const gui_sprite_ref *sprite, int lrtb_index, int value) {
    if (!s_project.session || !sprite || lrtb_index < 0 || lrtb_index >= 4) {
        return false;
    }
    /* Field-precise key: the component index. A different-component edit therefore has a
     * different key, so gui_project_pending_route flushes the prior component's pending BEFORE the RMW seed
     * below reads the model -> the seed carries the committed value of every OTHER component and
     * two components can never merge against a stale model (the RMW lost-edit is impossible). */
    gui_coalesce_key ck;
    if (!make_sprite_key(CK_SPRITE_SLICE9, sprite, lrtb_index, &ck)) {
        return false;
    }
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_sprite_ref routed = *sprite;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *ov = snapshot ? tp_session_snapshot_sprite_by_key(
                                                  snapshot, sprite->atlas_id,
                                                  sprite->source_id,
                                                  sprite->source_key)
                                            : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_SLICE9;
    for (int comp = 0; comp < 4; comp++) {
        p.slice9[comp] = ov ? ov->slice9_lrtb[comp] : 0;
    }
    p.slice9[lrtb_index] = value;
    return sprite_override_offer(&routed, p, &ck);
}

bool gui_project_set_sprite_override(const gui_sprite_ref *sprite, gui_sprite_ov which, int value) {
    if (!s_project.session || !sprite) {
        return false;
    }
    gui_coalesce_key ck;
    if (!make_sprite_key(CK_SPRITE_OVERRIDE, sprite, (int)which, &ck)) {
        return false;
    }
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_sprite_ref routed = *sprite;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    const int16_t v = (int16_t)value; /* value may be TP_PROJECT_OV_INHERIT to clear the field */
    switch (which) {
        case GUI_SPRITE_OV_SHAPE: p.mask = TP_SPF_SHAPE; p.ov_shape = v; break;
        case GUI_SPRITE_OV_ROTATE: p.mask = TP_SPF_ALLOW_ROTATE; p.ov_allow_rotate = v; break;
        case GUI_SPRITE_OV_MAXVERT: p.mask = TP_SPF_MAX_VERTICES; p.ov_max_vertices = v; break;
        case GUI_SPRITE_OV_MARGIN: p.mask = TP_SPF_MARGIN; p.ov_margin = v; break;
        case GUI_SPRITE_OV_EXTRUDE: p.mask = TP_SPF_EXTRUDE; p.ov_extrude = v; break;
        default: return false;
    }
    return sprite_override_offer(&routed, p, &ck);
}

int gui_project_add_target(tp_id128 atlas_id, int64_t expected_revision) {
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return -1; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    /* target.create op for the default json-neotolis target (mirrors seed_default_target's exporter +
     * "out/<name>" path). An OP (not the lifecycle seed) so the added target is captured in the diff
     * history and Undo removes exactly this target -- a direct seed leaves no undo step, so Ctrl+Z would
     * revert the WRONG (prior) edit (decision 0015). */
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
                                         : NULL;
    if (!atlas) {
        return -1;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    tp_id128 target_id;
    if (!gen_id(&target_id)) {
        return -1;
    }
    char out_path[TP_IDENTITY_PATH_MAX];
    const char *exporter_id = NULL;
    bool enabled = false;
    tp_error err = {0};
    const tp_status defaults_status = tp_session_snapshot_target_defaults(
        snapshot, atlas_id, &exporter_id, out_path, sizeof out_path, &enabled,
        &err);
    if (defaults_status != TP_STATUS_OK) {
        gui_project__note_session_reject(defaults_status, &err);
        return -1;
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    const tp_status status = gui_session_create_target(
        s_project.session, atlas_id, target_id, expected_revision,
        exporter_id, out_path, enabled, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return -1;
    }
    gui_project__refresh_after_session_commit();
    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    return atlas ? atlas->target_count - 1 : -1;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_target(const gui_target_ref *target) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = target->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_target(
        s_project.session, target->atlas_id, target->target_id, expected_revision,
        transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_set_target(const gui_target_ref *target, const char *exporter_id,
                            const char *out_path, bool enabled) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    /* Duplicate caller strings before flushing because exporter_id/out_path may
     * point into the cached snapshot that a successful flush invalidates. */
    char *exp = dupstr(exporter_id);
    char *outp = dupstr(out_path);
    /* fix2 [3]: same class as the other structural wrappers -- a journal-failed flush dropped the
     * buffered gesture (op-error surfaced), so abort THIS target edit too (freeing the pre-dup'd
     * strings) instead of pairing a lost edit with an unrelated target change. */
    if (!gui_project_flush_pending()) {
        free(exp);
        free(outp);
        return false;
    }
    if (!exp || !outp) {
        free(exp);
        free(outp);
        return false;
    }
    tp_op_target_set settings = {0};
    settings.mask = TP_TF_ALL;
    settings.enabled = enabled;
    settings.exporter_id = exp;
    settings.out_path = outp;
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_project.session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    free(exp);
    free(outp);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

/* H/G3 + C1 mask: COALESCABLE out-path-only setter for the export-target path text field. Discrete target
 * edits build their own single-field MASKED ops (enabled checkbox -> TP_TF_ENABLED, exporter dropdown ->
 * TP_TF_EXPORTER, below); browse calls this setter and flushes immediately. The free-text out-path
 * field, however, fired one gui_project_set_target per keystroke -> one committed TP_OP_TARGET_SET per
 * keystroke = undo spam. Buffering it under a per-target key (field = index) makes
 * the field's existing Enter/blur gesture-commit flush the whole edit as ONE undo step -- mirrors the
 * atlas-settings path (gui_project_set_atlas_setting). The op is MASKED to TP_TF_OUT_PATH: it carries ONLY
 * out_path, so exporter_id + enabled are left untouched by apply -- no RMW-seed, and no way for this edit to
 * clobber a concurrently-changed sibling field (C1 mask). Switching to a different target index is a
 * different key -> gui_project_pending_route flushes -> a correct one-undo-per-target boundary. */
bool gui_project_set_target_out_path(const gui_target_ref *target,
                                     const char *out_path) {
    if (!target) return false;
    gui_coalesce_key ck = make_key(CK_TARGET_OUTPATH, -1);
    ck.atlas_id = target->atlas_id;
    ck.source_id = target->target_id;
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck); /* flush a DIFFERENT key (other target / other knob) BEFORE reading this target */
    if (!out_path) {
        return false;
    }
    char *outp = dupstr(out_path);
    if (!outp) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = target->atlas_id;
    op.u.target_set.target_id = target->target_id;
    op.u.target_set.mask = TP_TF_OUT_PATH;     /* MASKED: only out_path -- exporter/enabled untouched (no RMW-seed) */
    op.u.target_set.out_path = outp;           /* ownership transfers to op */
    s_project.pending_expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_route);
    return gui_project_pending_offer(&ck, &op);
}

/* H/G3 + C1 mask: discrete target-field setters (enabled toggle / exporter change). IMMEDIATE (one undo step
 * each), and MASKED to the single field they edit -- so they never re-send exporter/out_path and can never
 * revert a concurrently-buffered out-path gesture (the hazard the pre-mask workaround had to RMW-seed around
 * is now impossible at the op level). They still flush any buffered out-path gesture FIRST: a discrete pick
 * is a gesture boundary, so the pending out-path commits as its own undo step before this one (clean
 * sequential history). An empty out_path is never buffered (gui_project_set_target_out_path guards it), so
 * that flush never rejects on emptiness -- only a genuine journal failure returns false, aborting this edit
 * too. */
bool gui_project_set_target_enabled(const gui_target_ref *target, bool enabled) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) { /* commit any buffered out-path gesture FIRST (gesture boundary) */
        return false;
    }
    tp_op_target_set settings = {0};
    settings.mask = TP_TF_ENABLED;
    settings.enabled = enabled;
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_project.session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_set_target_exporter(const gui_target_ref *target,
                                     const char *exporter_id) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false;
    }
    char *exp = dupstr(exporter_id);
    if (!exp) {
        return false;
    }
    tp_op_target_set settings = {0};
    settings.mask = TP_TF_EXPORTER;
    settings.exporter_id = exp;
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_project.session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    free(exp);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}
// #endregion

// #region animations
bool gui_project_animation_ref_at(int atlas_index, int animation_index,
                                  gui_animation_ref *out) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    const tp_snapshot_animation *animation = atlas
        ? tp_session_snapshot_animation_at(snapshot, atlas->id, animation_index)
        : NULL;
    if (!animation || !out) {
        return false;
    }
    *out = (gui_animation_ref){atlas->id, animation->id,
                               tp_session_snapshot_revision(snapshot)};
    return true;
}

bool gui_project_target_ref_at(int atlas_index, int target_index,
                               gui_target_ref *out) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    const tp_snapshot_target *target = atlas
        ? tp_session_snapshot_target_at(snapshot, atlas->id, target_index)
        : NULL;
    if (!target || !out) {
        return false;
    }
    *out = (gui_target_ref){atlas->id, target->id,
                            tp_session_snapshot_revision(snapshot)};
    return true;
}

int gui_project_create_animation(tp_id128 atlas_id, int64_t expected_revision,
                                 const char *base, const tp_op_sprite_ref *frames,
                                 int frame_count) {
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return -1; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
                                         : NULL;
    if (!atlas) {
        return -1;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char id[128];
    tp_error naming_error = {0};
    const tp_status naming_status = tp_session_snapshot_next_animation_name(
        snapshot, atlas_id, base, id, sizeof id, &naming_error);
    if (naming_status != TP_STATUS_OK) {
        gui_project__note_session_reject(naming_status, &naming_error);
        return -1;
    }
    tp_id128 anim_id;
    if (!gen_id(&anim_id)) {
        return -1;
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_create_animation(
        s_project.session, atlas_id, anim_id, expected_revision, id, frames,
        frame_count, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return -1;
    }
    gui_project__refresh_after_session_commit();
    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    for (int i = 0; atlas && i < atlas->animation_count; i++) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(snapshot, atlas_id, i);
        if (animation && tp_id128_eq(animation->id, anim_id)) {
            return i;
        }
    }
    return -1;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). The deferred
 * handler guards preview_stop + the s_sel_anim reset + "Removed" message on this. */
bool gui_project_remove_animation(const gui_animation_ref *animation) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false;
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation(
        s_project.session, animation->atlas_id, animation->animation_id,
        expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_set_anim_id(const gui_animation_ref *animation, const char *new_id) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_rename_animation(
        s_project.session, animation->atlas_id, animation->animation_id,
        expected_revision, new_id, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

/* Buffers one animation.settings.set under `k` (the caller has run gui_project_pending_route). */
static gui_coalesce_key make_animation_key(gui_coalesce_kind kind,
                                       const gui_animation_ref *animation) {
    gui_coalesce_key key = make_key(kind, -1);
    if (animation) {
        key.atlas_id = animation->atlas_id;
        key.source_id = animation->animation_id;
    }
    return key;
}

static bool anim_settings_offer(const gui_animation_ref *animation,
                                const tp_op_anim_settings *settings,
                                const gui_coalesce_key *key) {
    if (!animation || !settings) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = animation->atlas_id;
    op.u.anim_settings = *settings;
    op.u.anim_settings.anim_id = animation->animation_id;
    s_project.pending_expected_revision = animation->expected_revision;
    return gui_project_pending_offer(key, &op);
}

bool gui_project_set_anim_fps(const gui_animation_ref *animation, float fps) {
    if (!animation) {
        return false;
    }
    gui_coalesce_key ck = make_animation_key(CK_ANIM_FPS, animation);
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_animation_ref routed = *animation;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    tp_op_anim_settings settings = {0};
    settings.mask = TP_ANF_FPS;
    settings.fps = fps;
    return anim_settings_offer(&routed, &settings, &ck);
}

bool gui_project_set_anim_playback(const gui_animation_ref *animation, int playback) {
    if (!animation) {
        return false;
    }
    gui_coalesce_key ck = make_animation_key(CK_ANIM_PLAYBACK, animation);
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_animation_ref routed = *animation;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    tp_op_anim_settings settings = {0};
    settings.mask = TP_ANF_PLAYBACK;
    settings.playback = playback;
    return anim_settings_offer(&routed, &settings, &ck);
}

bool gui_project_set_anim_flip(const gui_animation_ref *animation, bool flip_h,
                               bool flip_v) {
    if (!animation) {
        return false;
    }
    gui_coalesce_key ck = make_animation_key(CK_ANIM_FLIP, animation);
    const int64_t revision_before_route = tp_session_revision(s_project.session);
    gui_project_pending_route(&ck);
    gui_animation_ref routed = *animation;
    routed.expected_revision = revision_after_owned_route(
        routed.expected_revision, revision_before_route);
    tp_op_anim_settings settings = {0};
    settings.mask = TP_ANF_FLIP_H | TP_ANF_FLIP_V;
    settings.flip_h = flip_h;
    settings.flip_v = flip_v;
    return anim_settings_offer(&routed, &settings, &ck);
}

bool gui_project_anim_add_frames(const gui_animation_ref *animation,
                                 const tp_op_sprite_ref *frames, int count) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    if (!frames || count <= 0) {
        return false;
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_add_animation_frames(
        s_project.session, animation->atlas_id, animation->animation_id,
        expected_revision, frames, count, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_anim_remove_frame(const gui_animation_ref *animation,
                                   int frame_index) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation_frame(
        s_project.session, animation->atlas_id, animation->animation_id,
        expected_revision, frame_index, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
    return true;
}

bool gui_project_anim_move_frame(const gui_animation_ref *animation,
                                 int frame_index, int delta) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_project.session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_project.session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_project.session);
    }
    const int to = frame_index + delta;
    if (to == frame_index) {
        return true; /* no-op move (edge button): skip commit, as before */
    }
    char transaction_id[33];
    gui_project__next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_move_animation_frame(
        s_project.session, animation->atlas_id, animation->animation_id,
        expected_revision, frame_index, to, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project__refresh_after_session_commit();
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
