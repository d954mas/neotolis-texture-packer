#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h> /* R5b-1: time(NULL) for the recovery-journal metadata timestamp (libc header FIRST -- macOS include-order) */

#include "gui_scan.h"
#include "gui_session_adapter.h"

#include "tp_core/tp_id.h"             /* tp_rng_os + id128 generate/nil for op ids */
#include "tp_core/tp_operation.h"      /* the typed op the GUI mutators build */
#include "tp_core/tp_identity.h"       /* canonical paths + exact saved-file fingerprints */
#include "tp_core/tp_recovery.h"       /* shared bounded orphan store + OS-backed claims */
#include "tp_core/tp_source_plan.h"    /* one source identity/dedupe owner */
#ifdef NTPACKER_GUI_SELFTEST
#include "tp_recovery_internal.h"
#include "tp_session_internal.h"
#endif

/* F2-05b-ii-A (docs/decisions/0015): GUI Undo/Redo and every mutation run through
 * tp_session. The session owns the model/project, history, save admission, and
 * recovery acknowledgement gate; GUI reads only cached immutable snapshot DTOs.
 *
 * TRANSACTION-LEVEL COALESCING (b-ii-A crux): the F2-03 history has no built-in coalescing (one
 * commit = one undo step), so a raw slider drag would revert one tick at a time. A field-precise
 * debounce buffer holds ONE pending transaction; consecutive same-key edits replace its value
 * (latest wins), a different key flushes it first. The FLUSH TRIGGER is GESTURE-SCOPED (owner
 * decision, ADR 0015): the view layer commits one transaction per interaction on the widget's
 * gesture boundary (slider release / field Enter+blur / discrete click) via
 * gui_project_flush_pending; the 0.30 s time window is only a gated fallback
 * (gui_project_flush_elapsed). See the pending-buffer region below and decision 0015. */

// #region state
static tp_session *s_session; /* sole owner of the live model and project */
static tp_session_snapshot *s_snapshot; /* one cached immutable GUI read view */
static uint64_t s_snapshot_lifetime_generation;
static uint64_t s_txn_seq; /* monotonic transaction-id source (unique per commit) */
static bool s_preview_stale;
static char s_name[256]; /* cached basename for the menu bar */
static double s_now;     /* coalescing clock (seconds), fed each frame by gui_project_tick */

/* Set only by the explicit Exit -> Discard confirmation. A raw OS close never sets it, so shutdown
 * preserves a dirty journal instead of silently treating X/Alt+F4 as confirmed data loss. */
static bool s_discard_recovery_on_shutdown;

/* Pending transaction REJECT (core rejected the op(s); model left byte-unchanged).
 * Surfaced once by the UI (gui_project_take_op_error) to the soft-error channel. */
static bool s_op_error;
static char s_op_error_msg[256];

/* Host policy only. Recovery core creates/destroys stores and claims per call;
 * a successful live attach transfers its concrete owner to tp_session. */
static char s_recovery_root[TP_IDENTITY_PATH_MAX];
#ifdef NTPACKER_GUI_SELFTEST
/* Legacy deterministic-path fixtures; production always lets core name slots. */
static char s_recovery_test_slot[TP_IDENTITY_PATH_MAX];
#endif
/* A one-shot startup notice explaining why recovery is unavailable. This deliberately covers both
 * contention and setup/storage failures: the editor remains usable, but silently losing crash
 * durability would be a data-safety bug. */
static bool s_recovery_setup_notice_pending;
static char s_recovery_setup_notice[256];
// #endregion

// #region transaction-level coalescing buffer (b-ii-A crux, decision 0015)
/* One pending transaction, keyed by (edit kind + atlas + EXACT target). Same key within the
 * window -> replace value (latest wins); different key / elapsed -> flush pending FIRST, then
 * buffer the new edit. Structural ops are non-coalescable (they flush pending, then commit
 * immediately). Field-precise keying is REQUIRED for correctness: keying slice9 by COMPONENT
 * makes a different-component edit flush the pending BEFORE its read-modify-write seed reads the
 * model, so each RMW read sees all prior edits committed (never merges two components against a
 * stale model -> no lost edit). */
#define GUI_COALESCE_SECS 0.30

typedef enum {
    CK_ATLAS_SETTING = 1,
    CK_SPRITE_ORIGIN,
    CK_SPRITE_SLICE9,   /* keyed by component (field = lrtb index) -- RMW correctness */
    CK_SPRITE_OVERRIDE,
    CK_ANIM_FPS,
    CK_ANIM_PLAYBACK,
    CK_ANIM_FLIP,
    CK_TARGET_OUTPATH   /* keyed by target index (field = index); H/G3 -- out-path text field coalesces */
} coalesce_kind;

typedef struct {
    coalesce_kind kind;
    tp_id128 atlas_id; /* structural identity for session-owned atlas intents */
    tp_id128 source_id;
    int field;         /* atlas field / sprite-override which / slice9 component; -1 if n/a */
    char sprite[256];  /* sprite export key; "" if n/a */
} coalesce_key;

static bool s_pending_valid;        /* a coalescable edit is buffered (uncommitted) */
static coalesce_key s_pending_key;
static tp_operation s_pending_op;   /* owns its arms until committed/replaced/discarded */
static double s_pending_time;       /* time of the last replace (coalesce window anchor) */
static int64_t s_pending_expected_revision; /* captured with the atlas snapshot intent */
static bool s_pending_preview_stale_before;
// #endregion

// #region helpers
static void note_session_reject(tp_status status, const tp_error *err);

static void snapshot_drop(void) {
    if (s_snapshot) {
        s_snapshot_lifetime_generation++;
    }
    tp_session_snapshot_destroy(s_snapshot);
    s_snapshot = NULL;
}

const tp_session_snapshot *gui_project_snapshot(void) {
    if (!s_snapshot && s_session) {
        tp_error err = {0};
        if (tp_session_snapshot_create(s_session, &s_snapshot, &err) != TP_STATUS_OK) {
            s_snapshot = NULL;
        }
    }
    return s_snapshot;
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return s_snapshot_lifetime_generation;
}

tp_session *gui_project_session_for_jobs(void) { return s_session; }

void gui_project_invalidate_sources(void) {
    gui_scan_invalidate_all();
    if (!s_session) {
        return;
    }
    tp_error err = {0};
    const tp_status status = tp_session_invalidate_sources(s_session, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
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
        (void)snprintf(s_name, sizeof s_name, "untitled");
        return;
    }
    const char *base = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    (void)snprintf(s_name, sizeof s_name, "%s", base);
}

/* Recovery helpers are defined further down with the rest of the recovery plumbing. */
#ifdef NTPACKER_GUI_SELFTEST
static bool recovery_active(void);
#endif
static void note_recovery_degraded(const char *msg);

/* Unix-seconds clock for recovery metadata. The selftest override makes list ordering and recovery
 * classification deterministic despite time()'s one-second resolution; production always uses time(). */
#ifdef NTPACKER_GUI_SELFTEST
static int64_t s_test_recovery_now = -1; /* >= 0 overrides time(NULL) */
#endif
static int64_t recovery_now(void) {
#ifdef NTPACKER_GUI_SELFTEST
    if (s_test_recovery_now >= 0) {
        return s_test_recovery_now;
    }
#endif
    return (int64_t)time(NULL);
}

/* Assign a random persistent ID to any structural entity that lacks one -- nil (a freshly
 * created project/atlas/anim/target) OR loader-synthesized for a migrated legacy file (§5.5). A
 * real loaded ID (v3/v4) is preserved. Idempotent after the first call. Returns the promote
 * status. */
/* Record a void-context id-promotion failure so the UI can surface it. */
bool gui_project_take_op_error(char *out, size_t cap) {
    if (!s_op_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_op_error_msg);
    }
    s_op_error = false;
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

/* Seeds a fresh atlas with the default target (core helper owns the exporter id +
 * "out/<name>" path). LIFECYCLE, not a mutation op (the exporter id is core's, never a
 * frontend literal); mirrors the CLI do_new. Only a target-free atlas is seeded. */
/* F2-05b-ii-B: build/app-stable recovery-journal key. A fixed 128-bit tag identifying THIS app +
 * journal format, checked on recover so a foreign / old-format sidecar is rejected (in ADDITION to
 * the journal header's magic + version guard) rather than misapplied. Bump on any journal-format
 * change so old slots self-invalidate to a clean fresh init. */
static tp_id128 recovery_key(void) {
    tp_id128 k;
    static const uint8_t b[16] = {'n', 't', 'p', 'k', '_', 'r', 'e', 'c', 'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    memcpy(k.bytes, b, sizeof b);
    return k;
}

/* Raise the status-bar channel for a degraded recovery notice. A live GUI host
 * requires recovery acknowledgement, so mutation stays blocked until a new
 * session can attach a healthy journal. Save/discard remain available. */
static void note_recovery_degraded(const char *msg) {
    s_op_error = true;
    (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "Recovery journal unavailable (%s) -- changes are blocked to preserve commit guarantees.",
                   msg ? msg : "unknown");
}

#ifdef NTPACKER_GUI_SELFTEST
static bool recovery_active(void) {
    return s_session && tp_session_recovery_available(s_session);
}
#endif

static bool recovery_configured(void) {
    return s_recovery_root[0] != '\0';
}

/* Attach one shared live recovery owner after the session identity is final.
 * The session owns the handle on every accepted attach path, including degraded
 * filesystem outcomes; GUI retains only configuration and presentation state. */
#ifdef NTPACKER_GUI_SELFTEST
static bool s_test_skip_recovery_reset = false;
#endif
static void attach_recovery_live(tp_session *session) {
    if (!session || !recovery_configured()) {
        return;
    }
    tp_error err = {0};
#ifdef NTPACKER_GUI_SELFTEST
    if (s_test_skip_recovery_reset) {
        s_test_skip_recovery_reset = false;
        (void)tp_session_require_recovery(session, &err);
        note_recovery_degraded("could not reset the recovery slot -- a stale journal is present");
        return;
    }
#endif
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
        .project_name = s_name,
        .file_fingerprint = has_saved_fingerprint ? &saved_fingerprint : NULL,
    };
    tp_status status;
#ifdef NTPACKER_GUI_SELFTEST
    if (s_recovery_test_slot[0] != '\0') {
        status = tp_recovery__test_session_attach_at(
            s_recovery_root, recovery_key(), s_recovery_test_slot, session,
            &metadata, &err);
    } else
#endif
    {
        tp_rng rng = tp_rng_os();
        status = tp_recovery_session_attach(
            s_recovery_root, recovery_key(), &rng, session, &metadata, &err);
    }
    if (status != TP_STATUS_OK && !tp_session_recovery_available(session)) {
        gui_project_note_recovery_setup_failure(
            status == TP_STATUS_RECOVERY_BUSY
                ? "another ntpacker window owns this recovery slot"
                : "the recovery storage lock could not be acquired");
        note_recovery_degraded(err.msg[0] ? err.msg
                                         : "could not checkpoint the recovery journal");
    }
}


static bool install_session(tp_session *next) {
    if (!next) {
        return false;
    }
    snapshot_drop();
    tp_session_destroy(s_session);
    s_session = next;
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
        note_session_reject(status, &err);
        return false;
    }
    return true;
}

static void next_transaction_id(char out[33]) {
    (void)snprintf(out, 33U, "%032llx", (unsigned long long)(s_txn_seq++));
}

static void note_session_reject(tp_status status, const tp_error *err) {
    const char *message = (err && err->msg[0]) ? err->msg : tp_status_str(status);
    if (status == TP_STATUS_JOURNAL_FAILED) {
        message = "Could not journal the edit -- disk full? Your change was not applied.";
    }
    s_op_error = true;
    (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "%s", message);
}

static bool refresh_after_session_commit(void) {
    /* Core is the sole semantic no-op owner. A no-change admission leaves the
     * revision unchanged, so keep the current projection and preview state. */
    if (s_snapshot &&
        tp_session_snapshot_revision(s_snapshot) == tp_session_revision(s_session)) {
        return false;
    }
    s_preview_stale = true;
    snapshot_drop();
    return true;
}

// #endregion

// #region pending-buffer primitives
static coalesce_key make_key(coalesce_kind kind, int field, const char *sprite) {
    coalesce_key k;
    memset(&k, 0, sizeof k);
    k.kind = kind;
    k.atlas_id = tp_id128_nil();
    k.source_id = tp_id128_nil();
    k.field = field;
    (void)snprintf(k.sprite, sizeof k.sprite, "%s", sprite ? sprite : "");
    return k;
}

static coalesce_key make_atlas_key(tp_id128 atlas_id, int field) {
    coalesce_key key = make_key(CK_ATLAS_SETTING, field, "");
    key.atlas_id = atlas_id;
    return key;
}

static coalesce_key make_sprite_key(coalesce_kind kind, const gui_sprite_ref *sprite,
                                    int field) {
    coalesce_key key = make_key(kind, field,
                                sprite ? sprite->source_key : "");
    if (sprite) {
        key.atlas_id = sprite->atlas_id;
        key.source_id = sprite->source_id;
    }
    return key;
}

static bool key_eq(const coalesce_key *a, const coalesce_key *b) {
    return a->kind == b->kind && a->field == b->field &&
           tp_id128_eq(a->atlas_id, b->atlas_id) &&
           tp_id128_eq(a->source_id, b->source_id) &&
           strcmp(a->sprite, b->sprite) == 0;
}

/* Discard the buffered edit WITHOUT committing (new/open replace the whole project). */
static void pending_discard(void) {
    if (s_pending_valid) {
        tp_operation_free(&s_pending_op);
        memset(&s_pending_op, 0, sizeof s_pending_op);
        s_pending_valid = false;
    }
}

/* fix [3]: returns FALSE iff a buffered gesture existed and its commit FAILED (e.g. a journal append
 * failure) -- i.e. an edit could NOT be made durable. Returns TRUE when nothing was pending, the
 * pending committed as a core-classified semantic no-op, or it committed OK. Callers that persist or discard on the strength of
 * "no pending left" (save/save-as, undo/redo) MUST check this and ABORT on false so a journal-failed
 * flush is never mistaken for a clean state (no false "saved", no wrong undo target). */
bool gui_project_flush_pending(void) {
    if (!s_pending_valid) {
        return true;
    }
    tp_operation op = s_pending_op; /* move: ownership of the arms transfers to the local */
    const int64_t expected_revision = s_pending_expected_revision;
    const bool preview_stale_before = s_pending_preview_stale_before;
    memset(&s_pending_op, 0, sizeof s_pending_op);
    s_pending_valid = false;
    if (op.kind == TP_OP_ATLAS_SETTINGS_SET) {
        char transaction_id[33];
        next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_atlas_settings(
            s_session, op.atlas_id, expected_revision, &op.u.atlas_settings,
            transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!refresh_after_session_commit()) {
                s_preview_stale = preview_stale_before;
            }
            return true;
        }
        note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_SPRITE_OVERRIDE_SET &&
        !tp_id128_is_nil(op.u.sprite_set.source_id)) {
        char transaction_id[33];
        next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_sprite_override(
            s_session, op.atlas_id, op.u.sprite_set.source_id,
            op.u.sprite_set.src_key, expected_revision,
            &op.u.sprite_set, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!refresh_after_session_commit()) {
                s_preview_stale = preview_stale_before;
            }
            return true;
        }
        note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_ANIMATION_SETTINGS_SET) {
        char transaction_id[33];
        next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_animation_settings(
            s_session, op.atlas_id, op.u.anim_settings.anim_id,
            expected_revision, &op.u.anim_settings, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!refresh_after_session_commit()) {
                s_preview_stale = preview_stale_before;
            }
            return true;
        }
        note_session_reject(status, &err);
        return false;
    }
    if (op.kind == TP_OP_TARGET_SET) {
        char transaction_id[33];
        next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_set_target(
            s_session, op.atlas_id, op.u.target_set.target_id,
            expected_revision, &op.u.target_set, transaction_id, &err);
        tp_operation_free(&op);
        if (status == TP_STATUS_OK) {
            if (!refresh_after_session_commit()) {
                s_preview_stale = preview_stale_before;
            }
            return true;
        }
        note_session_reject(status, &err);
        return false;
    }
    /* Every coalescable family is session-owned. Reaching this branch means a
     * new operation kind was added without an admission adapter: fail closed. */
    tp_operation_free(&op);
    tp_error err = {0};
    (void)tp_error_set(&err, TP_STATUS_INVALID_ARGUMENT,
                       "buffered operation has no session admission route");
    note_session_reject(TP_STATUS_INVALID_ARGUMENT, &err);
    return false;
}

/* Called AT THE TOP of a coalescable mutator with its key, BEFORE the mutator reads the model:
 * flush the buffered edit when the incoming key DIFFERS, so a) distinct edits never merge and
 * b) an RMW read sees all prior edits committed. GESTURE-SCOPED (owner decision, ADR 0015): the
 * flush trigger is the gesture boundary (slider release / field Enter+blur / discrete click),
 * NOT a timer -- so a SAME-key edit never flushes here regardless of how long the gesture takes
 * (a slow drag stays one transaction). The time window is a FALLBACK only (gui_project_flush_elapsed).
 * After this returns, s_pending_valid is true IFF a same-key pending remains (the replace target). */
static void pending_route(const coalesce_key *k) {
    if (s_pending_valid && !key_eq(k, &s_pending_key)) {
        /* fix2: the bool is intentionally IGNORED here. On a journal-failed flush the different-key
         * gesture is dropped WITH the op-error surfaced (commit_txn_now set it); the caller then only
         * BUFFERS a new (uncommitted) edit -- there is no persist/discard "proceed as clean" decision to
         * abort. gui_project_flush_elapsed (the timer fallback) is the same case. Audited fix2 [3]. */
        (void)gui_project_flush_pending();
    }
}

/* Buffer `op` (TAKES OWNERSHIP of the arms) under `k`. Precondition (pending_route ran): a still-
 * valid pending is same-key -> replace its value (latest wins). Preview goes stale immediately;
 * the commit (and model_ver bump) is deferred to the flush. Always returns true. */
static bool pending_offer(const coalesce_key *k, tp_operation *op) {
    if (!s_pending_valid) {
        s_pending_preview_stale_before = s_preview_stale;
    }
    if (s_pending_valid) {
        tp_operation_free(&s_pending_op); /* same key: replace the value */
    }
    s_pending_op = *op; /* shallow move; caller must not free `op` after this */
    s_pending_key = *k;
    s_pending_time = s_now;
    s_pending_valid = true;
    s_preview_stale = true; /* immediate stale feedback while the gesture buffers */
    return true;
}

/* FALLBACK ONLY (ADR 0015): commit a buffered gesture that never received a release/blur/discrete
 * boundary (e.g. a control that streams values with no pointer/focus edge). The caller (main.c)
 * gates this on NO active gesture (no held pointer, no focused input) so it can never split a live
 * drag or a mid-typing field. Primary commits are gesture-scoped; this only backstops a missed edge. */
void gui_project_flush_elapsed(void) {
    if (s_pending_valid && (s_now - s_pending_time) > GUI_COALESCE_SECS) {
        gui_project_flush_pending();
    }
}

/* F2-05b-ii-A #5: EFFECTIVE slice9 peek for the on-canvas guide lines. slice9 edits now BUFFER until
 * the gesture boundary, so the committed record is FROZEN mid-typing -- the guides would freeze with
 * it (regressing "typing in the Region panel moves the lines this same frame"). Returns true + fills
 * out_lrtb[4] with the BUFFERED slice9 when a slice9 gesture is buffered for this atlas+sprite (the
 * pending op already seeds all four components from the committed record, so it is the full effective
 * value); false otherwise, so the caller falls back to the committed record. No commit, read-only. */
bool gui_project_peek_pending_slice9(const gui_sprite_ref *sprite, int out_lrtb[4]) {
    if (!sprite || !out_lrtb || !s_pending_valid ||
        s_pending_key.kind != CK_SPRITE_SLICE9 ||
        !tp_id128_eq(s_pending_key.atlas_id, sprite->atlas_id) ||
        !tp_id128_eq(s_pending_key.source_id, sprite->source_id)) {
        return false;
    }
    if (strcmp(s_pending_key.sprite, sprite->source_key ? sprite->source_key : "") != 0) {
        return false;
    }
    for (int k = 0; k < 4; k++) {
        out_lrtb[k] = (int)s_pending_op.u.sprite_set.slice9[k];
    }
    return true;
}
// #endregion

// #region lifecycle
/* Install a fresh clean untitled session. Attaches a fresh recovery journal at
 * the live slot (no-op when recovery is off). */
static void fresh_init(void) {
    (void)install_fresh_session();
    recompute_name();
    attach_recovery_live(s_session);
    s_preview_stale = false;
    snapshot_drop();
}

void gui_project_init(void) {
    if (s_session) {
        return;
    }
    pending_discard();
    fresh_init();
}

void gui_project_shutdown(void) {
    /* The engine currently gives us no cancellable window-close callback. Flush a last buffered edit,
     * then keep the owned slot whenever the model is dirty or the flush failed. This turns X/Alt+F4
     * into a recoverable close. Only an explicit Exit -> Discard confirmation may remove dirty work. */
    (void)(!s_session || gui_project_flush_pending());
    pending_discard();
    snapshot_drop();
    if (s_session && s_discard_recovery_on_shutdown) {
        (void)tp_session_discard(s_session, NULL);
    }
    tp_session_destroy(s_session); /* frees the sole owned model/project/history/journal */
    s_session = NULL;
    s_recovery_root[0] = '\0';
#ifdef NTPACKER_GUI_SELFTEST
    s_recovery_test_slot[0] = '\0';
#endif
    s_discard_recovery_on_shutdown = false;
}

void gui_project_discard_recovery_on_shutdown(void) { s_discard_recovery_on_shutdown = true; }

/* Configure crash recovery from the host app-data root. Core generates the
 * per-process slot identity and owns all liveness and exclusion mechanics. */
void gui_project_enable_recovery(const char *root) {
    s_recovery_root[0] = '\0';
#ifdef NTPACKER_GUI_SELFTEST
    s_recovery_test_slot[0] = '\0';
#endif
    if (!root || root[0] == '\0') {
        return;
    }
    tp_error err = {0};
    tp_status status;
#ifdef NTPACKER_GUI_SELFTEST
    /* Legacy recovery tests name exact fixture journals. Keep that capability
     * behind a test-only core seam; production never accepts a slot path. */
    const char *base = root;
    for (const char *p = root; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    const size_t root_len = strlen(root);
    static const char suffix[] = ".ntpjournal";
    if (base != root && root_len >= sizeof suffix - 1U &&
        strcmp(root + root_len - (sizeof suffix - 1U), suffix) == 0) {
        size_t parent_len = (size_t)(base - root);
        while (parent_len > 0U &&
               (root[parent_len - 1U] == '/' || root[parent_len - 1U] == '\\')) {
            parent_len--;
        }
        char parent[TP_IDENTITY_PATH_MAX];
        if (parent_len == 0U || parent_len >= sizeof parent) {
            status = tp_error_set(&err, TP_STATUS_OUT_OF_BOUNDS,
                                  "recovery fixture path is too long");
        } else {
            memcpy(parent, root, parent_len);
            parent[parent_len] = '\0';
            status = tp_recovery_root_validate(parent, recovery_key(), &err);
            if (status == TP_STATUS_OK) {
                (void)snprintf(s_recovery_root, sizeof s_recovery_root, "%s",
                               parent);
                (void)snprintf(s_recovery_test_slot,
                               sizeof s_recovery_test_slot, "%s", root);
            }
        }
    } else
#endif
    {
        status = tp_recovery_root_validate(root, recovery_key(), &err);
        if (status == TP_STATUS_OK) {
            (void)snprintf(s_recovery_root, sizeof s_recovery_root, "%s",
                           root);
        }
    }
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure(
            err.msg[0] ? err.msg : "the recovery root is unavailable");
    }
}

/* Rebase only an intent that was current immediately before pending_route committed
 * a different GUI-owned gesture.  A ref that was already stale at entry must stay
 * stale so session admission rejects it instead of silently overwriting newer work. */
static int64_t revision_after_owned_route(int64_t captured_revision,
                                          int64_t revision_before_route) {
    const int64_t current_revision = tp_session_revision(s_session);
    return captured_revision == revision_before_route &&
                   current_revision != revision_before_route
               ? current_revision
               : captured_revision;
}

void gui_project_note_recovery_setup_failure(const char *reason) {
    s_recovery_setup_notice_pending = true;
    (void)snprintf(s_recovery_setup_notice, sizeof s_recovery_setup_notice,
                   "Crash recovery is off for this window (%s).",
                   (reason && reason[0] != '\0') ? reason : "startup setup failed");
}

static void note_recovery_scan_limited(void) {
    s_recovery_setup_notice_pending = true;
    (void)snprintf(s_recovery_setup_notice, sizeof s_recovery_setup_notice,
                   "Some previous recovery sessions were not scanned because the startup safety budget was reached.");
}

/* Drains the one-shot recovery-unavailable notice. */
bool gui_project_take_recovery_setup_notice(char *out, size_t cap) {
    if (!s_recovery_setup_notice_pending) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_recovery_setup_notice[0] != '\0'
                                             ? s_recovery_setup_notice
                                             : "Crash recovery is off for this window.");
    }
    s_recovery_setup_notice_pending = false;
    s_recovery_setup_notice[0] = '\0';
    return true;
}
// #endregion

// #region R6a recovery-resolution decision/action layer (headless-testable; the R6b modal calls this)
int gui_recovery_collect(gui_recovery_list *out) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!recovery_configured() || !out) {
        return 0;
    }
    tp_error err = {0};
    tp_recovery_candidates candidates;
    memset(&candidates, 0, sizeof candidates);
    tp_status status = tp_recovery_scan_root(
        s_recovery_root, recovery_key(), s_session, &candidates, &err);
    if (status != TP_STATUS_OK) {
        gui_project_note_recovery_setup_failure("the recovery directory could not be scanned");
        return 0;
    }
    out->has_more = candidates.has_more;
    out->count = (int)candidates.count;
    for (size_t i = 0U; i < candidates.count; ++i) {
        const tp_recovery_candidate *source = &candidates.items[i];
        gui_recovery_entry *target = &out->items[i];
        (void)snprintf(target->journal_path, sizeof target->journal_path,
                       "%s", source->journal_path);
        (void)snprintf(target->orig_path, sizeof target->orig_path, "%s", source->original_path);
        (void)snprintf(target->name, sizeof target->name, "%s", source->name);
        target->timestamp = source->timestamp;
        target->status = (int)source->status;
        target->adoptable = source->adoptable;
        target->file_fingerprint = source->file_fingerprint;
        target->has_file_fingerprint = source->has_file_fingerprint;
    }
    if (candidates.has_more) {
        note_recovery_scan_limited();
    }
    return out->count;
}

static void recovery_copy_error(char *out, size_t cap, tp_status status,
                                const tp_error *err) {
    if (out && cap) {
        (void)snprintf(out, cap, "%s",
                       err && err->msg[0] ? err->msg : tp_status_str(status));
    }
}

#ifdef NTPACKER_GUI_SELFTEST
tp_status
#else
static tp_status
#endif
gui_recovery_resolve(const char *journal_path, const char *orig_path,
                     gui_recovery_action action, const char *target_path,
                     char *err_out, size_t err_cap) {
    (void)orig_path; /* Candidate metadata is the sole Save Original authority. */
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
        s_recovery_root, recovery_key(), journal_path, s_session, core_action,
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
    return gui_recovery_resolve(entry->journal_path, entry->orig_path, action, target_path, err_out, err_cap);
}
// #endregion

// #region lifecycle dev seams (selftest only)
#ifdef NTPACKER_GUI_SELFTEST
bool gui_project__test_attach_memory_recovery(void) {
    tp_error err = {0};
    return tp_session__test_attach_memory_recovery(s_session, &err) ==
           TP_STATUS_OK;
}
void gui_project__test_fail_next_recovery_writes(int count) {
    tp_session__test_fail_next_recovery_writes(s_session, count);
}

/* Dev seam (selftest only, fix [1] regression): hold a FOREIGN single-instance lock on `slot` from a
 * SEPARATE handle, simulating another live editor, so a following gui_project_enable_recovery(slot)
 * sees the slot busy and runs journal-less. Returns true if the foreign lock was taken. */
bool gui_project__test_hold_foreign_lock(const char *slot) {
    gui_project__test_release_foreign_lock();
    if (!slot || slot[0] == '\0') {
        return false;
    }
    const char *base = slot;
    for (const char *p = slot; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    size_t parent_len = (size_t)(base - slot);
    while (parent_len > 0U &&
           (slot[parent_len - 1U] == '/' || slot[parent_len - 1U] == '\\')) {
        parent_len--;
    }
    char root[TP_IDENTITY_PATH_MAX];
    if (parent_len == 0U || parent_len >= sizeof root) {
        return false;
    }
    memcpy(root, slot, parent_len);
    root[parent_len] = '\0';
    return tp_recovery__test_hold_foreign_lock(root, recovery_key(), slot);
}
void gui_project__test_release_foreign_lock(void) {
    tp_recovery__test_release_foreign_lock();
}
/* True iff the current session owns the shared live recovery handle. */
bool gui_project__test_recovery_active(void) { return recovery_active(); }
/* Dev seam: simulate a failed live-slot reset before shared-owner attach. */
void gui_project__test_skip_next_recovery_reset(void) { s_test_skip_recovery_reset = true; }
/* Dev seam: pin the recovery-metadata clock for deterministic ordering/classification tests. */
void gui_project__test_set_recovery_now(int64_t t) { s_test_recovery_now = t; }
/* Dev seam (selftest only, R6a fix [2]): the REAL recovery-journal key. collect now KEY-FILTERS adoptable
 * orphans (a foreign-key journal is excluded), so a test that wants an orphan classified adoptable must craft
 * it with THIS key -- a synthetic repeated byte no longer peeks adoptable. */
tp_id128 gui_project__test_recovery_key(void) { return recovery_key(); }
/* Dev seam (selftest only, fix2 F2): drive the (adoptable desc, timestamp desc) cap eviction directly so the
 * regression guard is DETERMINISTIC -- independent of filesystem enumeration order (readdir on Linux is
 * unsorted, so a filesystem-crafted cap test passed even with the ordering reverted). */
void gui_project__test_recovery_insert(gui_recovery_list *out, const gui_recovery_entry *e) {
    tp_recovery_candidates candidates;
    memset(&candidates, 0, sizeof candidates);
    candidates.count = (size_t)out->count;
    candidates.has_more = out->has_more;
    for (int i = 0; i < out->count; ++i) {
        candidates.items[i].adoptable = out->items[i].adoptable;
        candidates.items[i].timestamp = out->items[i].timestamp;
    }
    tp_recovery_candidate candidate;
    memset(&candidate, 0, sizeof candidate);
    candidate.adoptable = e->adoptable;
    candidate.timestamp = e->timestamp;
    tp_recovery__test_candidate_insert(&candidates, &candidate);
    out->count = (int)candidates.count;
    out->has_more = candidates.has_more;
    for (int i = 0; i < out->count; ++i) {
        out->items[i].adoptable = candidates.items[i].adoptable;
        out->items[i].timestamp = candidates.items[i].timestamp;
    }
}
/* Dev seam (selftest only, fix2 F3): arm a one-shot failure of the resolve's post-save load-verify so the
 * verify+keep-journal backstop has a regression test (a real save-OK-but-reload-fail input is not cleanly
 * constructible). Consumed on use inside gui_recovery_resolve. */
void gui_project__test_fail_next_load_verify(void) {
    tp_recovery__test_fail_next_resolve_verify();
}
#endif
// #endregion

// #region accessors
const char *gui_project_path(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return tp_session_snapshot_canonical_path(snapshot);
}
const char *gui_project_display_name(void) { return s_name; }
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
bool gui_project_is_stale(void) { return s_preview_stale; }
// #endregion

// #region dirty/stale choke point
/* Post-commit choke point: a REAL committed mutation makes the preview stale and bumps the
 * session generation. Undo history + dirty are core-owned. `act` is vestigial (coalescing moved to the
 * transaction buffer) but kept for call-site clarity + the dev-seam signature. */
void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
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
    char out_path[576];
    const char *exporter_id = NULL;
    bool target_enabled = false;
    tp_error err = {0};
    const tp_status defaults_status = tp_session_snapshot_next_atlas_defaults(
        snapshot, name, sizeof name, out_path, sizeof out_path, &exporter_id,
        &target_enabled, &err);
    if (defaults_status != TP_STATUS_OK) {
        note_session_reject(defaults_status, &err);
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
    next_transaction_id(transaction_id);
    err = (tp_error){0};
    const tp_status status = gui_session_create_atlas(
        s_session, new_id, target_id, tp_session_snapshot_revision(snapshot), name,
        exporter_id, out_path, target_enabled, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return -1;
    }
    refresh_after_session_commit();
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
    const int64_t revision_before_flush = s_session ? tp_session_revision(s_session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (tp_id128_is_nil(atlas_id) || !s_session) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_atlas(
        s_session, atlas_id, expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
    refresh_after_session_commit();
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
 * Skips empty paths and any path already in the atlas OR already queued in THIS batch (both counted into
 * *out_dup), so the committed txn holds only DISTINCT new sources and never self-rejects on a duplicate.
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
    const int64_t revision_before_flush = s_session ? tp_session_revision(s_session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: a journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, atlas_id) ||
        n_paths <= 0 || !paths) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    tp_source_batch_plan plan = {0};
    tp_error plan_error = {0};
    const tp_status plan_status = tp_source_batch_plan_create(
        snapshot, atlas_id, paths, n_paths, &plan, &plan_error);
    if (plan_status != TP_STATUS_OK) {
        note_session_reject(plan_status, &plan_error);
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
        next_transaction_id(transaction_id);
        tp_error err = {0};
        const tp_status status = gui_session_add_sources(
            s_session, atlas_id, ids, distinct, m,
            (tp_snapshot_source_kind)kind,
            expected_revision, transaction_id, &err);
        ok = status == TP_STATUS_OK;
        if (!ok) {
            note_session_reject(status, &err);
        } else {
            gui_project_invalidate_sources();
            refresh_after_session_commit();
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
    const int64_t revision_before_flush = s_session ? tp_session_revision(s_session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_session || tp_id128_is_nil(atlas_id) || tp_id128_is_nil(source_id)) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_source(
        s_session, atlas_id, source_id, expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
    refresh_after_session_commit();
    return true;
}

bool gui_project_set_atlas_name(tp_id128 atlas_id, int64_t expected_revision, const char *name) {
    const int64_t revision_before_flush = s_session ? tp_session_revision(s_session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_session || !name) {
        return false;
    }
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_rename_atlas(
        s_session, atlas_id, expected_revision, name, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

tp_status gui_project_copy_atlas_name(tp_id128 atlas_id, char *out, size_t capacity,
                                      tp_error *err) {
    return gui_session_copy_atlas_name(gui_project_snapshot(), atlas_id, out, capacity, err);
}

bool gui_project_set_sprite_rename(const gui_sprite_ref *sprite, const char *rename) {
    const int64_t revision_before_flush = s_session ? tp_session_revision(s_session) : 0;
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    if (!s_session || !sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0') {
        return false;
    }
    int64_t expected_revision = sprite->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_set_sprite_name(
        s_session, sprite->atlas_id, sprite->source_id, sprite->source_key,
        expected_revision, rename, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
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
    if (!s_session || tp_id128_is_nil(atlas_id)) {
        return false;
    }
    coalesce_key ck = make_atlas_key(atlas_id, (int)field);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck); /* flush a different knob's pending BEFORE reading this atlas */
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
    s_pending_expected_revision = expected_revision;
    return pending_offer(&ck, &op);
}

/* Buffers a sprite.override.set at its canonical {source_id, source-local key}.
 * Core applies the masked fields on commit then prunes an all-default record. The
 * caller has already run pending_route(k). */
static bool sprite_override_offer(const gui_sprite_ref *sprite, tp_op_sprite_set payload,
                                  const coalesce_key *k) {
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
    s_pending_expected_revision = sprite->expected_revision;
    return pending_offer(k, &op);
}

bool gui_project_set_sprite_origin(const gui_sprite_ref *sprite, int axis, float value) {
    if (!s_session || !sprite || axis < 0 || axis > 1) {
        return false; /* 0 = Pivot X, 1 = Pivot Y */
    }
    /* Component-precise key (mirror slice9): X and Y are DIFFERENT keys, so editing the OTHER axis
     * flushes the buffered one FIRST -- then the read-modify-write seed below reads the COMMITTED
     * value of the non-edited component and the two components can never merge against a stale model.
     * (The pre-fix code keyed both axes the same AND seeded from a view-side committed read, so a
     * back-to-back X then Y replaced {x=new,y=old} with {x=old,y=new} and silently lost the X edit.) */
    coalesce_key ck = make_sprite_key(CK_SPRITE_ORIGIN, sprite, axis);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    if (!s_session || !sprite || lrtb_index < 0 || lrtb_index >= 4) {
        return false;
    }
    /* Field-precise key: the component index. A different-component edit therefore has a
     * different key, so pending_route flushes the prior component's pending BEFORE the RMW seed
     * below reads the model -> the seed carries the committed value of every OTHER component and
     * two components can never merge against a stale model (the RMW lost-edit is impossible). */
    coalesce_key ck = make_sprite_key(CK_SPRITE_SLICE9, sprite, lrtb_index);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    if (!s_session || !sprite) {
        return false;
    }
    coalesce_key ck = make_sprite_key(CK_SPRITE_OVERRIDE, sprite, (int)which);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    const int64_t revision_before_flush = tp_session_revision(s_session);
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
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    tp_id128 target_id;
    if (!gen_id(&target_id)) {
        return -1;
    }
    char out_path[576];
    const char *exporter_id = NULL;
    bool enabled = false;
    tp_error err = {0};
    const tp_status defaults_status = tp_session_snapshot_target_defaults(
        snapshot, atlas_id, &exporter_id, out_path, sizeof out_path, &enabled,
        &err);
    if (defaults_status != TP_STATUS_OK) {
        note_session_reject(defaults_status, &err);
        return -1;
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    const tp_status status = gui_session_create_target(
        s_session, atlas_id, target_id, expected_revision,
        exporter_id, out_path, enabled, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return -1;
    }
    refresh_after_session_commit();
    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    return atlas ? atlas->target_count - 1 : -1;
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_target(const gui_target_ref *target) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = target->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_target(
        s_session, target->atlas_id, target->target_id, expected_revision,
        transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

bool gui_project_set_target(const gui_target_ref *target, const char *exporter_id,
                            const char *out_path, bool enabled) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_session);
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
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    free(exp);
    free(outp);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

/* H/G3 + C1 mask: COALESCABLE out-path-only setter for the export-target path text field. Discrete target
 * edits build their own single-field MASKED ops (enabled checkbox -> TP_TF_ENABLED, exporter dropdown ->
 * TP_TF_EXPORTER, below); browse still uses the full-replace gui_project_set_target. The free-text out-path
 * field, however, fired one gui_project_set_target per keystroke -> one committed TP_OP_TARGET_SET per
 * keystroke = undo spam. Buffering it under a per-target key (field = index) makes
 * the field's existing Enter/blur gesture-commit flush the whole edit as ONE undo step -- mirrors the
 * atlas-settings path (gui_project_set_atlas_setting). The op is MASKED to TP_TF_OUT_PATH: it carries ONLY
 * out_path, so exporter_id + enabled are left untouched by apply -- no RMW-seed, and no way for this edit to
 * clobber a concurrently-changed sibling field (C1 mask). Switching to a different target index is a
 * different key -> pending_route flushes -> a correct one-undo-per-target boundary. */
bool gui_project_set_target_out_path(const gui_target_ref *target,
                                     const char *out_path) {
    if (!target) return false;
    coalesce_key ck = make_key(CK_TARGET_OUTPATH, -1, "");
    ck.atlas_id = target->atlas_id;
    ck.source_id = target->target_id;
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck); /* flush a DIFFERENT key (other target / other knob) BEFORE reading this target */
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
    s_pending_expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_route);
    return pending_offer(&ck, &op);
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
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) { /* commit any buffered out-path gesture FIRST (gesture boundary) */
        return false;
    }
    tp_op_target_set settings = {0};
    settings.mask = TP_TF_ENABLED;
    settings.enabled = enabled;
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

bool gui_project_set_target_exporter(const gui_target_ref *target,
                                     const char *exporter_id) {
    if (!target) return false;
    const int64_t revision_before_flush = tp_session_revision(s_session);
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
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const int64_t expected_revision = revision_after_owned_route(
        target->expected_revision, revision_before_flush);
    const tp_status status = gui_session_set_target(
        s_session, target->atlas_id, target->target_id,
        expected_revision, &settings, transaction_id, &err);
    free(exp);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
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
    const int64_t revision_before_flush = tp_session_revision(s_session);
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
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char id[128];
    tp_error naming_error = {0};
    const tp_status naming_status = tp_session_snapshot_next_animation_name(
        snapshot, atlas_id, base, id, sizeof id, &naming_error);
    if (naming_status != TP_STATUS_OK) {
        note_session_reject(naming_status, &naming_error);
        return -1;
    }
    tp_id128 anim_id;
    if (!gen_id(&anim_id)) {
        return -1;
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_create_animation(
        s_session, atlas_id, anim_id, expected_revision, id, frames,
        frame_count, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return -1;
    }
    refresh_after_session_commit();
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
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false;
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation(
        s_session, animation->atlas_id, animation->animation_id,
        expected_revision, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

bool gui_project_set_anim_id(const gui_animation_ref *animation, const char *new_id) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_rename_animation(
        s_session, animation->atlas_id, animation->animation_id,
        expected_revision, new_id, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

/* Buffers one animation.settings.set under `k` (the caller has run pending_route). */
static coalesce_key make_animation_key(coalesce_kind kind,
                                       const gui_animation_ref *animation) {
    coalesce_key key = make_key(kind, -1, "");
    if (animation) {
        key.atlas_id = animation->atlas_id;
        key.source_id = animation->animation_id;
    }
    return key;
}

static bool anim_settings_offer(const gui_animation_ref *animation,
                                const tp_op_anim_settings *settings,
                                const coalesce_key *key) {
    if (!animation || !settings) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = animation->atlas_id;
    op.u.anim_settings = *settings;
    op.u.anim_settings.anim_id = animation->animation_id;
    s_pending_expected_revision = animation->expected_revision;
    return pending_offer(key, &op);
}

bool gui_project_set_anim_fps(const gui_animation_ref *animation, float fps) {
    if (!animation) {
        return false;
    }
    coalesce_key ck = make_animation_key(CK_ANIM_FPS, animation);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    coalesce_key ck = make_animation_key(CK_ANIM_PLAYBACK, animation);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    coalesce_key ck = make_animation_key(CK_ANIM_FLIP, animation);
    const int64_t revision_before_route = tp_session_revision(s_session);
    pending_route(&ck);
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
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    if (!frames || count <= 0) {
        return false;
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_add_animation_frames(
        s_session, animation->atlas_id, animation->animation_id,
        expected_revision, frames, count, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

bool gui_project_anim_remove_frame(const gui_animation_ref *animation,
                                   int frame_index) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation_frame(
        s_session, animation->atlas_id, animation->animation_id,
        expected_revision, frame_index, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}

bool gui_project_anim_move_frame(const gui_animation_ref *animation,
                                 int frame_index, int delta) {
    if (!animation) {
        return false;
    }
    const int64_t revision_before_flush = tp_session_revision(s_session);
    if (!gui_project_flush_pending()) {
        return false; /* fix2 [3]: journal-failed flush dropped the gesture -> abort (op-error surfaced) */
    }
    int64_t expected_revision = animation->expected_revision;
    if (expected_revision == revision_before_flush &&
        tp_session_revision(s_session) != revision_before_flush) {
        expected_revision = tp_session_revision(s_session);
    }
    const int to = frame_index + delta;
    if (to == frame_index) {
        return true; /* no-op move (edge button): skip commit, as before */
    }
    char transaction_id[33];
    next_transaction_id(transaction_id);
    tp_error err = {0};
    const tp_status status = gui_session_move_animation_frame(
        s_session, animation->atlas_id, animation->animation_id,
        expected_revision, frame_index, to, transaction_id, &err);
    if (status != TP_STATUS_OK) {
        note_session_reject(status, &err);
        return false;
    }
    refresh_after_session_commit();
    return true;
}
// #endregion

// #region undo / redo (F2-03 diff history)
/* A pending buffered edit counts as undoable (undo flushes it into a step, then reverts it). */
bool gui_project_can_undo(void) { return s_pending_valid || tp_session_can_undo(s_session); }
bool gui_project_can_redo(void) { return tp_session_can_redo(s_session); }
int gui_project_undo_depth(void) { return tp_session_undo_depth(s_session); }
int gui_project_redo_depth(void) { return tp_session_redo_depth(s_session); }

/* Undo/Redo is journal-gated inside core. Record a rejected history transition on
 * the same structured soft-error channel as a rejected transaction; no GUI-side
 * durability compensation is needed (and no false "success then degrade" is possible). */
static void note_history_reject(const char *verb, tp_status st, const tp_error *err) {
    const char *detail = (err && err->msg[0]) ? err->msg : tp_status_str(st);
    s_op_error = true;
    if (st == TP_STATUS_JOURNAL_FAILED) {
        (void)snprintf(s_op_error_msg, sizeof s_op_error_msg,
                       "Could not journal the %s -- disk full? Nothing was changed.", verb);
    } else {
        (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "%s rejected: %s", verb, detail);
    }
}

/* Undo reverses the most recent committed transaction via its captured semantic diff.
 * A buffered gesture is committed FIRST (its own step) so Ctrl+Z reverts the in-flight drag.
 * Dirty is identity-derived, so an undo back to the saved baseline reads clean. */
bool gui_project_undo(void) {
    if (!gui_project_flush_pending()) {
        return false; /* fix [3]: the buffered gesture could not commit (journal failed) -- do NOT
                       * then undo a DIFFERENT (older) step; the op-error is already surfaced. */
    }
    tp_error e = {0};
    tp_status st = tp_session_undo(s_session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("undo", st, &e);
        }
        return false;
    }
    snapshot_drop();
    s_preview_stale = true;             /* restored model != last-packed; packing is blocked -> always stale */
    gui_project_invalidate_sources();
    return true;
}

bool gui_project_redo(void) {
    if (!gui_project_flush_pending()) {
        return false; /* fix [3]: a journal-failed flush must not silently proceed into a redo */
    }
    tp_error e = {0};
    tp_status st = tp_session_redo(s_session, &e);
    if (st != TP_STATUS_OK) {
        if (st != TP_STATUS_NOT_FOUND) {
            note_history_reject("redo", st, &e);
        }
        return false;
    }
    snapshot_drop();
    s_preview_stale = true;
    gui_project_invalidate_sources();
    return true;
}
// #endregion

// #region file operations
bool gui_project_new(void) {
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    if (!install_fresh_session()) {
        return false;
    }
    recompute_name();
    attach_recovery_live(s_session);
    s_preview_stale = false;
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
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
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
    attach_recovery_live(s_session);
    s_preview_stale = true; /* nothing packed this session yet */
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
    const tp_status st = tp_session_save(s_session, &result, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    snapshot_drop();
    recompute_name();
    if (result.recovery_degraded) {
        note_recovery_degraded("recovery checkpoint compaction failed");
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
    /* fix [3]: the buffered gesture must be DURABLY committed before we persist. If its commit fails
     * (e.g. a journal append failure -- full disk), the edit is NOT in the model, so writing the file
     * + mark_saved would produce a file missing the edit AND a false "saved"/clean state (silent data
     * loss). ABORT: do not save, do not mark_saved; surface the reason. The model stays dirty. */
    if (!gui_project_flush_pending()) {
        gui_project_flush_error(err_out, err_cap); /* fix3 [2]: shared neutral wording (save/pack/gate) */
        return TP_STATUS_JOURNAL_FAILED;
    }
    err = (tp_error){0};
    tp_session_save_result result;
    const tp_status st = tp_session_save_as(s_session, canonical_path, &result, &err);
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
        note_recovery_degraded("recovery checkpoint compaction failed");
    }
    return TP_STATUS_OK;
}
// #endregion
