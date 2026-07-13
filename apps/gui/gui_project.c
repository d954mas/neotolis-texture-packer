#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gui_scan.h"

#include "tp_core/tp_export.h"         /* TP_EXPORTER_ID_JSON_NEOTOLIS (default target exporter id) */
#include "tp_core/tp_id.h"             /* tp_rng_os + id128 generate/nil for op ids */
#include "tp_core/tp_operation.h"      /* the typed op the GUI mutators build */
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"    /* tp_model + tp_model_apply + tp_model_dirty/mark_saved */
#include "tp_core/tp_diff.h"           /* F2-03 undo/redo history (tp_model_enable_history/undo/redo) */

/* F2-05b-ii-A (docs/decisions/0015): GUI Undo/Redo now runs through the F2-03 diff history
 * (tp_model_enable_history + tp_model_undo/redo), NOT the retired 32 MB snapshot stack. Every
 * mutator BUILDS typed tp_operation(s) and commits them through a journal-LESS tp_model
 * (tp_model_wrap -> tp_model_apply, the F2-01/F2-02 clone-swap engine); the model OWNS the
 * project and captures one semantic diff per committed transaction (history is enabled at wrap).
 * s_proj is a read view of tp_model_project(s_model), refreshed after every committed
 * transaction and after every undo/redo (both clone-swap m->project). Dirty is IDENTITY-derived
 * (tp_model_dirty); a Save re-baselines via tp_model_mark_saved. The journal stays NULL (b-ii-B
 * owns the live journal + append-fail + recovery + D2).
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
static tp_model *s_model;  /* owns s_proj; NULL until gui_project_init */
static tp_project *s_proj; /* == tp_model_project(s_model); refreshed after every commit/undo/redo */
static uint64_t s_txn_seq; /* monotonic transaction-id source (unique per commit) */
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_preview_stale;
static unsigned s_model_ver; /* bumped per real committed mutation (gui_project_model_version) */
static char s_name[256]; /* cached basename for the menu bar */
static double s_now;     /* coalescing clock (seconds), fed each frame by gui_project_tick */

/* Pending id-promotion failure from a void-context ensure_ids() (init/new/open). OS-RNG failure
 * would leave nil structural ids, which must never be silently swallowed. Drained + surfaced
 * once by the UI (gui_project_take_id_error). The save path fails closed on its own. */
static bool s_id_error;
static char s_id_error_msg[256];

/* Pending transaction REJECT (core rejected the op(s); model left byte-unchanged).
 * Surfaced once by the UI (gui_project_take_op_error) to the soft-error channel. */
static bool s_op_error;
static char s_op_error_msg[256];
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
    CK_ANIM_FLIP
} coalesce_kind;

typedef struct {
    coalesce_kind kind;
    int atlas;         /* atlas index */
    int field;         /* atlas field / sprite-override which / slice9 component; -1 if n/a */
    int anim;          /* animation index; -1 if n/a */
    char sprite[256];  /* sprite export key; "" if n/a */
} coalesce_key;

static bool s_pending_valid;        /* a coalescable edit is buffered (uncommitted) */
static coalesce_key s_pending_key;
static tp_operation s_pending_op;   /* owns its arms until committed/replaced/discarded */
static gui_action s_pending_act;    /* the touch tag to raise when it commits */
static double s_pending_time;       /* time of the last replace (coalesce window anchor) */
// #endregion

// #region helpers
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
    if (s_path[0] == '\0') {
        (void)snprintf(s_name, sizeof s_name, "untitled");
        return;
    }
    const char *base = s_path;
    for (const char *p = s_path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    (void)snprintf(s_name, sizeof s_name, "%s", base);
}

static void set_path(const char *path) {
    (void)snprintf(s_path, sizeof s_path, "%s", path ? path : "");
    recompute_name();
}

/* Assign a random persistent ID to any structural entity that lacks one -- nil (a freshly
 * created project/atlas/anim/target) OR loader-synthesized for a migrated legacy file (§5.5). A
 * real loaded ID (v3/v4) is preserved. Idempotent after the first call. Returns the promote
 * status. */
static tp_status ensure_ids(tp_error *err) {
    if (!s_proj) {
        return TP_STATUS_OK;
    }
    tp_rng rng = tp_rng_os();
    return tp_project_promote_ids(s_proj, &rng, err);
}

/* Record a void-context id-promotion failure so the UI can surface it. */
static void note_id_error(tp_status st, const tp_error *err) {
    s_id_error = true;
    (void)snprintf(s_id_error_msg, sizeof s_id_error_msg, "%s", (err && err->msg[0]) ? err->msg : tp_status_str(st));
}

bool gui_project_take_id_error(char *out, size_t cap) {
    if (!s_id_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_id_error_msg);
    }
    s_id_error = false;
    return true;
}

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

/* Seeds a fresh atlas with the default target (core helper owns the exporter id +
 * "out/<name>" path). LIFECYCLE, not a mutation op (the exporter id is core's, never a
 * frontend literal); mirrors the CLI do_new. Only a target-free atlas is seeded. */
static void seed_default_target(tp_project *p, int atlas_index) {
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a || a->target_count > 0) {
        return;
    }
    (void)tp_project_atlas_seed_default_target(p, atlas_index);
}

/* Drop a model's idempotency retention store (F2-05b-i F6). Idempotency is MOOT for the
 * single-threaded interactive GUI (unique monotonic ids, no retries), and the long-lived
 * journal-less model would otherwise accumulate one 33-byte id per commit FOREVER. The commit
 * path NULL-guards the idstore, so a NULL idstore simply skips id recording. Mirrors
 * tp_model_destroy's idstore cleanup exactly. */
static void drop_idstore(tp_model *m) {
    if (!m || !m->idstore) {
        return;
    }
    if (m->owns_idstore && m->idstore->destroy) {
        m->idstore->destroy(m->idstore->ctx);
    }
    free(m->idstore);
    m->idstore = NULL;
    m->owns_idstore = false;
}

/* Wrap `p` (TAKES OWNERSHIP on success) in a fresh journal-less, idstore-less model with the
 * F2-03 undo/redo history enabled, and make it the live model, refreshing the s_proj view.
 * COMMIT-THEN-REPLACE (F2-05b-i F3): the replacement is wrapped into a TEMP first and the OLD
 * model is destroyed only AFTER the wrap succeeds -- so an OOM in the wrap can never lose the
 * open project or leave s_proj NULL. Returns true when `p` is installed; on failure `p` is freed
 * (never leaked) and the CURRENT model+project are kept intact. A NULL `p` returns false. */
static bool wrap_model(tp_project *p) {
    if (!p) {
        return false; /* nothing to install -> keep the current model (an upstream OOM must not clear it) */
    }
    tp_model *nm = tp_model_wrap(p);
    if (!nm) {
        tp_project_destroy(p); /* wrap did not take ownership on failure */
        return false;          /* keep the current model+project intact (F3) */
    }
    drop_idstore(nm);                  /* GUI carries NO idstore (F6) */
    (void)tp_model_enable_history(nm); /* F2-05b-ii-A: undo/redo runs through this diff history */
    tp_model_destroy(s_model);         /* success: only now free the previous model + its owned project */
    s_model = nm;
    s_proj = tp_model_project(s_model);
    return true;
}

/* Generates a fresh non-nil structural id via the OS RNG; false on an RNG fault. */
static bool gen_id(tp_id128 *out) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    return tp_id128_generate(&rng, out, &err) == TP_STATUS_OK;
}

static void ops_free(tp_operation *ops, int n) {
    for (int i = 0; i < n; i++) {
        tp_operation_free(&ops[i]);
    }
}

/* Commit `ops` as ONE atomic transaction on the persistent journal-less model NOW. On success
 * refreshes the s_proj view (the clone-swap replaced m->project; when history is enabled the
 * commit also captured a semantic diff and pushed one undo step, dropping any redo branch) and
 * returns true. On a reject the model is BYTE-UNCHANGED and the structured status is recorded for
 * the soft-error channel. ALWAYS frees the op arms and the result. Returns false on reject / no
 * model. */
static bool commit_txn_now(tp_operation *ops, int nops) {
    if (!s_model) {
        ops_free(ops, nops);
        return false;
    }
    tp_txn_request req;
    memset(&req, 0, sizeof req);
    req.schema = TP_TXN_SCHEMA;
    /* Unique 32-lowercase-hex per commit: the model persists across edits, so a fixed id
     * would trip idempotency (duplicate_id) on the second commit. A monotonic counter is
     * unique + never serialized. */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%032llx", (unsigned long long)(s_txn_seq++));
    req.expected_revision = tp_model_revision(s_model); /* single-threaded edits -> always matches */
    req.ops = ops;
    req.op_count = nops;

    tp_txn_result res;
    memset(&res, 0, sizeof res);
    tp_error err = {0};
    tp_status st = tp_model_apply(s_model, &req, &res, &err);

    bool ok = (st == TP_STATUS_OK);
    if (ok) {
        s_proj = tp_model_project(s_model); /* clone-swapped: old project freed, adopt the new view */
    } else {
        const char *msg = (err.msg[0]) ? err.msg : tp_status_str(st);
        if (res.error_count > 0 && res.errors[0].message[0]) {
            msg = res.errors[0].message;
        }
        s_op_error = true;
        (void)snprintf(s_op_error_msg, sizeof s_op_error_msg, "%s", msg);
    }
    tp_txn_result_free(&res);
    ops_free(ops, nops);
    return ok;
}

/* Re-baseline the dirty anchor + history to the freshly installed project (init/new/open):
 * promote §5.5 ids (a direct mutation, so it is NOT an undo step) and mark_saved AFTER the
 * promotion, because ids participate in the semantic identity -- a promotion changes the
 * identity, so the clean baseline must be captured post-promotion. */
static void promote_and_baseline(void) {
    tp_error e = {0};
    tp_status st = ensure_ids(&e);
    if (st != TP_STATUS_OK) {
        note_id_error(st, &e); /* surface the RNG fault; never crash */
    }
    tp_model_mark_saved(s_model); /* clean baseline == the promoted identity */
}
// #endregion

// #region pending-buffer primitives
static coalesce_key make_key(coalesce_kind kind, int atlas, int field, int anim, const char *sprite) {
    coalesce_key k;
    memset(&k, 0, sizeof k);
    k.kind = kind;
    k.atlas = atlas;
    k.field = field;
    k.anim = anim;
    (void)snprintf(k.sprite, sizeof k.sprite, "%s", sprite ? sprite : "");
    return k;
}

static bool key_eq(const coalesce_key *a, const coalesce_key *b) {
    return a->kind == b->kind && a->atlas == b->atlas && a->field == b->field && a->anim == b->anim &&
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

void gui_project_flush_pending(void) {
    if (!s_pending_valid) {
        return;
    }
    tp_operation op = s_pending_op; /* move: ownership of the arms transfers to the local */
    gui_action act = s_pending_act;
    memset(&s_pending_op, 0, sizeof s_pending_op);
    s_pending_valid = false;
    if (commit_txn_now(&op, 1)) { /* frees op's arms; refreshes s_proj + pushes one undo step */
        gui_project_touch(act);
    }
    /* on reject commit_txn_now still freed the arms + set the op-error; the edit is dropped */
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
        gui_project_flush_pending();
    }
}

/* Buffer `op` (TAKES OWNERSHIP of the arms) under `k`. Precondition (pending_route ran): a still-
 * valid pending is same-key -> replace its value (latest wins). Preview goes stale immediately;
 * the commit (and model_ver bump) is deferred to the flush. Always returns true. */
static bool pending_offer(const coalesce_key *k, tp_operation *op, gui_action act) {
    if (s_pending_valid) {
        tp_operation_free(&s_pending_op); /* same key: replace the value */
    }
    s_pending_op = *op; /* shallow move; caller must not free `op` after this */
    s_pending_key = *k;
    s_pending_act = act;
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
// #endregion

// #region lifecycle
void gui_project_init(void) {
    if (s_model) {
        return;
    }
    pending_discard();
    tp_project *p = tp_project_create();
    seed_default_target(p, 0); /* clean baseline includes it (I1) -- lifecycle, direct */
    (void)wrap_model(p);       /* first model: on OOM s_model/s_proj stay NULL */
    set_path("");
    s_preview_stale = false;
    promote_and_baseline();
}

void gui_project_shutdown(void) {
    pending_discard();
    tp_model_destroy(s_model); /* frees the model + its owned project + its history */
    s_model = NULL;
    s_proj = NULL;
}
// #endregion

// #region accessors
tp_project *gui_project_get(void) { return s_proj; }
const char *gui_project_path(void) { return s_path; }
const char *gui_project_display_name(void) { return s_name; }
bool gui_project_has_path(void) { return s_path[0] != '\0'; }
/* Identity-derived dirty (tp_model_dirty). A buffered-but-uncommitted edit is NOT yet in the
 * model identity; the destructive gates (new/open/exit) flush the pending buffer BEFORE calling
 * this (see gui_actions.c) so a pending edit can never be silently discarded. */
bool gui_project_is_dirty(void) { return tp_model_dirty(s_model); }
bool gui_project_is_stale(void) { return s_preview_stale; }
// #endregion

// #region dirty/stale choke point
/* Post-commit choke point: a REAL committed mutation makes the preview stale and bumps the
 * model-version counter. Undo history + dirty are core's now (F2-03 diff + identity), so this no
 * longer serializes a snapshot or recomputes dirty. `act` is vestigial (coalescing moved to the
 * transaction buffer) but kept for call-site clarity + the dev-seam signature. */
void gui_project_touch(gui_action act) {
    (void)act;
    s_preview_stale = true;
    s_model_ver++;
}

void gui_project_touch_setting(void) { gui_project_touch(GUI_ACT_SET_SETTING); }

void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
unsigned gui_project_model_version(void) { return s_model_ver; }
// #endregion

// #region mutation wrappers (each builds typed op(s) + commits through the model)
int gui_project_add_atlas(void) {
    gui_project_flush_pending(); /* structural: commit any buffered gesture as its own step first */
    if (!s_proj) {
        return -1;
    }
    char name[64];
    (void)snprintf(name, sizeof name, "atlas%d", s_proj->atlas_count + 1);
    tp_id128 new_id;
    tp_id128 tgt_id;
    if (!gen_id(&new_id) || !gen_id(&tgt_id)) {
        return -1;
    }
    char out_path[576];
    (void)snprintf(out_path, sizeof out_path, "out/%s", name);
    /* ONE transaction: create the atlas AND seed its default json-neotolis target (I1). Both ops go
     * through the diff history so undo removes the whole atlas and redo restores its target too -- the
     * old direct seed_default_target (a non-op mutation) would be LOST on redo (decision 0015). */
    tp_operation ops[2];
    memset(ops, 0, sizeof ops);
    ops[0].kind = TP_OP_ATLAS_CREATE;
    ops[0].atlas_id = new_id;
    ops[0].u.atlas_create.name = dupstr(name);
    ops[1].kind = TP_OP_TARGET_CREATE;
    ops[1].atlas_id = new_id;
    ops[1].u.target_create.target_id = tgt_id;
    ops[1].u.target_create.exporter_id = dupstr(TP_EXPORTER_ID_JSON_NEOTOLIS);
    ops[1].u.target_create.out_path = dupstr(out_path);
    ops[1].u.target_create.enabled = true;
    if (!ops[0].u.atlas_create.name || !ops[1].u.target_create.exporter_id || !ops[1].u.target_create.out_path) {
        ops_free(ops, 2);
        return -1;
    }
    if (!commit_txn_now(ops, 2)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_ATLAS);
    return tp_project_find_atlas_by_id(s_proj, new_id);
}

void gui_project_remove_atlas(int index) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, index);
    if (!a) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a->id;
    if (commit_txn_now(&op, 1)) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_ATLAS);
    }
}

gui_add_status gui_project_add_source_kind(int atlas_index, const char *path, tp_source_kind kind) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !path || path[0] == '\0') {
        return GUI_ADD_FAILED;
    }
    if (tp_project_atlas_has_source_path(a, path)) {
        return GUI_ADD_DUPLICATE; /* core rejects a dup path; catch it here (no op) -- no touch, no dirty */
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = a->id;
    op.u.source_add.kind = kind;
    op.u.source_add.key = dupstr(path);
    if (!op.u.source_add.key || !gen_id(&op.u.source_add.source_id)) {
        tp_operation_free(&op);
        return GUI_ADD_FAILED;
    }
    if (!commit_txn_now(&op, 1)) {
        return GUI_ADD_FAILED;
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_ADD_SOURCE);
    return GUI_ADD_ADDED;
}

gui_add_status gui_project_add_source(int atlas_index, const char *path) {
    /* Folder default: the "Add Folder" dialog and other kind-agnostic callers. The
     * "Add Files" dialog records TP_SOURCE_KIND_FILE via add_source_kind directly. */
    return gui_project_add_source_kind(atlas_index, path, TP_SOURCE_KIND_FOLDER);
}

void gui_project_remove_source(int atlas_index, int source_index) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || source_index < 0 || source_index >= a->source_count) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a->id;
    op.u.source_ref.source_id = a->sources[source_index].id;
    if (commit_txn_now(&op, 1)) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_SOURCE);
    }
}

bool gui_project_set_atlas_name(int atlas_index, const char *name) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !name || name[0] == '\0') {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a->id;
    op.u.atlas_rename.name = dupstr(name);
    if (!op.u.atlas_rename.name) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_ATLAS);
    return true;
}

bool gui_project_set_sprite_rename(int atlas_index, const char *sprite_name, const char *rename) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    /* PENDING (name-keyed) override: nil source_id + verbatim key, exactly the CLI's
     * source-less sprite path (decision 0014 F2). The GUI's sprite_name IS the ext-stripped
     * export key, so verbatim == the export-key bridge. An empty/NULL rename clears it. */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_NAME_SET;
    op.atlas_id = a->id;
    op.u.sprite_name.source_id = tp_id128_nil();
    op.u.sprite_name.src_key = dupstr(sprite_name);
    op.u.sprite_name.name = (rename && rename[0]) ? dupstr(rename) : NULL;
    if (!op.u.sprite_name.src_key || ((rename && rename[0]) && !op.u.sprite_name.name)) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_SPRITE);
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

bool gui_project_set_atlas_setting(int atlas_index, gui_atlas_field field, int ivalue, float fvalue) {
    coalesce_key ck = make_key(CK_ATLAS_SETTING, atlas_index, (int)field, -1, "");
    pending_route(&ck); /* flush a different knob's pending BEFORE reading this atlas */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a->id;
    if (!fill_atlas_knob(&op.u.atlas_settings, field, ivalue, fvalue)) {
        tp_operation_free(&op);
        return false;
    }
    return pending_offer(&ck, &op, GUI_ACT_SET_SETTING);
}

/* Buffers a sprite.override.set on the PENDING (name-keyed) record for `sprite_name`: nil
 * source_id + verbatim key (== the export-key bridge). Core applies the masked fields on commit
 * then prunes an all-default record. The caller has already run pending_route(k). */
static bool sprite_override_offer(int atlas_index, const char *sprite_name, tp_op_sprite_set payload,
                                  const coalesce_key *k) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = a->id;
    op.u.sprite_set = payload;
    op.u.sprite_set.source_id = tp_id128_nil();
    op.u.sprite_set.src_key = dupstr(sprite_name);
    if (!op.u.sprite_set.src_key) {
        tp_operation_free(&op);
        return false;
    }
    return pending_offer(k, &op, GUI_ACT_SET_SETTING);
}

bool gui_project_set_sprite_origin(int atlas_index, const char *sprite_name, float ox, float oy) {
    coalesce_key ck = make_key(CK_SPRITE_ORIGIN, atlas_index, -1, -1, sprite_name);
    pending_route(&ck);
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_ORIGIN;
    p.origin_x = ox;
    p.origin_y = oy;
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

bool gui_project_set_sprite_slice9(int atlas_index, const char *sprite_name, int lrtb_index, int value) {
    if (lrtb_index < 0 || lrtb_index >= 4) {
        return false;
    }
    /* Field-precise key: the component index. A different-component edit therefore has a
     * different key, so pending_route flushes the prior component's pending BEFORE the RMW seed
     * below reads the model -> the seed carries the committed value of every OTHER component and
     * two components can never merge against a stale model (the RMW lost-edit is impossible). */
    coalesce_key ck = make_key(CK_SPRITE_SLICE9, atlas_index, lrtb_index, -1, sprite_name);
    pending_route(&ck);
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_SLICE9;
    for (int comp = 0; comp < 4; comp++) {
        p.slice9[comp] = ov ? ov->slice9_lrtb[comp] : 0;
    }
    p.slice9[lrtb_index] = (uint16_t)(value < 0 ? 0 : value);
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

bool gui_project_set_sprite_override(int atlas_index, const char *sprite_name, gui_sprite_ov which, int value) {
    coalesce_key ck = make_key(CK_SPRITE_OVERRIDE, atlas_index, (int)which, -1, sprite_name);
    pending_route(&ck);
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
    return sprite_override_offer(atlas_index, sprite_name, p, &ck);
}

int gui_project_add_target(int atlas_index) {
    gui_project_flush_pending();
    /* target.create op for the default json-neotolis target (mirrors seed_default_target's exporter +
     * "out/<name>" path). An OP (not the lifecycle seed) so the added target is captured in the diff
     * history and Undo removes exactly this target -- a direct seed leaves no undo step, so Ctrl+Z would
     * revert the WRONG (prior) edit (decision 0015). */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return -1;
    }
    tp_id128 tgt_id;
    if (!gen_id(&tgt_id)) {
        return -1;
    }
    char out_path[576];
    (void)snprintf(out_path, sizeof out_path, "out/%s", a->name);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_CREATE;
    op.atlas_id = a->id;
    op.u.target_create.target_id = tgt_id;
    op.u.target_create.exporter_id = dupstr(TP_EXPORTER_ID_JSON_NEOTOLIS);
    op.u.target_create.out_path = dupstr(out_path);
    op.u.target_create.enabled = true;
    if (!op.u.target_create.exporter_id || !op.u.target_create.out_path) {
        tp_operation_free(&op);
        return -1;
    }
    if (!commit_txn_now(&op, 1)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_TARGET);
    a = tp_project_get_atlas(s_proj, atlas_index);
    return a ? a->target_count - 1 : -1;
}

void gui_project_remove_target(int atlas_index, int index) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_REMOVE;
    op.atlas_id = a->id;
    op.u.target_ref.target_id = a->targets[index].id;
    if (commit_txn_now(&op, 1)) {
        gui_project_touch(GUI_ACT_REMOVE_TARGET);
    }
}

bool gui_project_set_target(int atlas_index, int index, const char *exporter_id, const char *out_path, bool enabled) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return false;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = a->id;
    op.u.target_set.target_id = a->targets[index].id;
    op.u.target_set.enabled = enabled;
    op.u.target_set.exporter_id = dupstr(exporter_id);
    op.u.target_set.out_path = dupstr(out_path);
    if (!op.u.target_set.exporter_id || !op.u.target_set.out_path) {
        tp_operation_free(&op);
        return false;
    }
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_TARGET);
    return true;
}
// #endregion

// #region animations
static tp_project_anim *anim_at(int atlas_index, int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return NULL;
    }
    return &a->animations[anim_index];
}

/* The atlas's animation named `name` (exact), or NULL. */
static tp_project_anim *find_anim_by_name(tp_project_atlas *a, const char *name) {
    if (!a || !name) {
        return NULL;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (a->animations[i].name && strcmp(a->animations[i].name, name) == 0) {
            return &a->animations[i];
        }
    }
    return NULL;
}

bool gui_project_anim_id_exists(int atlas_index, const char *id) {
    return find_anim_by_name(tp_project_get_atlas(s_proj, atlas_index), id) != NULL;
}

int gui_project_create_animation(int atlas_index, const char *base, const char *const *frames, int frame_count) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return -1;
    }
    /* unique id (CLIENT naming policy): prefer `base` verbatim, else base"2"/"3"...; a
     * NULL/empty base auto-names "animN". Core has no dup-name rejection for animations. */
    char id[128];
    if (base && base[0]) {
        (void)snprintf(id, sizeof id, "%s", base);
        for (int n = 2; gui_project_anim_id_exists(atlas_index, id); n++) {
            (void)snprintf(id, sizeof id, "%s%d", base, n);
        }
    } else {
        for (int n = 1;; n++) {
            (void)snprintf(id, sizeof id, "anim%d", n);
            if (!gui_project_anim_id_exists(atlas_index, id)) {
                break;
            }
        }
    }
    tp_id128 anim_id;
    if (!gen_id(&anim_id)) {
        return -1;
    }
    int nframes = 0;
    for (int i = 0; frames && i < frame_count; i++) {
        if (frames[i] && frames[i][0]) {
            nframes++;
        }
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = a->id;
    op.u.anim_create.anim_id = anim_id;
    op.u.anim_create.name = dupstr(id);
    op.u.anim_create.fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    op.u.anim_create.playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    op.u.anim_create.flip_h = false;
    op.u.anim_create.flip_v = false;
    op.u.anim_create.frame_count = nframes;
    bool bad = (op.u.anim_create.name == NULL);
    if (!bad && nframes > 0) {
        op.u.anim_create.frames = (char **)calloc((size_t)nframes, sizeof(char *));
        if (!op.u.anim_create.frames) {
            bad = true;
        } else {
            int w = 0;
            for (int i = 0; frames && i < frame_count && !bad; i++) {
                if (frames[i] && frames[i][0]) {
                    op.u.anim_create.frames[w] = dupstr(frames[i]);
                    if (!op.u.anim_create.frames[w]) {
                        bad = true;
                    }
                    w++;
                }
            }
        }
    }
    if (bad) {
        tp_operation_free(&op);
        return -1;
    }
    if (!commit_txn_now(&op, 1)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_ANIM);
    /* Re-fetch the (clone-swapped) atlas to report the appended animation's index. */
    a = tp_project_get_atlas(s_proj, atlas_index);
    return (a && tp_project_atlas_find_animation_by_id(a, anim_id)) ? (a->animation_count - 1) : -1;
}

void gui_project_remove_animation(int atlas_index, const char *id) {
    gui_project_flush_pending();
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !id) {
        return;
    }
    tp_project_anim *an = find_anim_by_name(a, id);
    if (!an) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_REMOVE;
    op.atlas_id = a->id;
    op.u.anim_ref.anim_id = an->id;
    if (commit_txn_now(&op, 1)) {
        gui_project_touch(GUI_ACT_REMOVE_ANIM);
    }
}

bool gui_project_set_anim_id(int atlas_index, int anim_index, const char *new_id) {
    gui_project_flush_pending();
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !new_id || new_id[0] == '\0') {
        return false;
    }
    if (an->name && strcmp(an->name, new_id) == 0) {
        return true; /* no-op */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_project_anim *clash = find_anim_by_name(a, new_id);
    if (clash && clash != an) {
        return false; /* CLIENT policy: clashes with ANOTHER animation (core has no anim-rename) */
    }
    /* The F2-01 operation catalog has NO animation-rename op, so this is the ONE mutator that
     * cannot be expressed as a typed op and therefore CANNOT be captured by the F2-03 diff
     * history -- animation rename is NOT undoable in b-ii-A (a documented carry, not a
     * regression introduced here; adding a core ANIMATION_RENAME op is F2-01 scope). Coherent
     * because the model is journal-less: the direct write mutates m->project in place and the
     * next transaction clones it. Documented in decision 0015. */
    char *copy = dupstr(new_id);
    if (!copy) {
        return false;
    }
    free(an->name); /* boundary-ok: no ANIMATION_RENAME op exists (decision 0015) */
    an->name = copy; /* boundary-ok: no ANIMATION_RENAME op exists (decision 0015) */
    gui_project_touch(GUI_ACT_RENAME_ANIM);
    return true;
}

/* Buffers one animation.settings.set under `k` (the caller has run pending_route). */
static bool anim_settings_offer(int atlas_index, int anim_index, uint32_t mask, float fps, int playback, bool flip_h,
                                bool flip_v, const coalesce_key *k) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = a->id;
    op.u.anim_settings.anim_id = an->id;
    op.u.anim_settings.mask = mask;
    op.u.anim_settings.fps = fps;
    op.u.anim_settings.playback = playback;
    op.u.anim_settings.flip_h = flip_h;
    op.u.anim_settings.flip_v = flip_v;
    return pending_offer(k, &op, GUI_ACT_SET_ANIM);
}

bool gui_project_set_anim_fps(int atlas_index, int anim_index, float fps) {
    coalesce_key ck = make_key(CK_ANIM_FPS, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!(fps >= 1.0F)) {
        fps = 1.0F; /* widget parse-clamp (>=1); core also range-checks (>0 finite) on commit */
    }
    if (!s_pending_valid && an->fps == fps) {
        return true; /* safe no-op: nothing buffered for this key and the committed value matches */
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_FPS, fps, an->playback, an->flip_h, an->flip_v, &ck);
}

bool gui_project_set_anim_playback(int atlas_index, int anim_index, int playback) {
    coalesce_key ck = make_key(CK_ANIM_PLAYBACK, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (playback < 0) {
        playback = 0;
    }
    if (playback > 6) {
        playback = 6;
    }
    if (!s_pending_valid && an->playback == playback) {
        return true;
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_PLAYBACK, an->fps, playback, an->flip_h, an->flip_v, &ck);
}

bool gui_project_set_anim_flip(int atlas_index, int anim_index, bool flip_h, bool flip_v) {
    coalesce_key ck = make_key(CK_ANIM_FLIP, atlas_index, -1, anim_index, "");
    pending_route(&ck);
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!s_pending_valid && an->flip_h == flip_h && an->flip_v == flip_v) {
        return true;
    }
    return anim_settings_offer(atlas_index, anim_index, TP_ANF_FLIP_H | TP_ANF_FLIP_V, an->fps, an->playback, flip_h,
                               flip_v, &ck);
}

bool gui_project_anim_add_frames(int atlas_index, int anim_index, const char *const *frames, int count) {
    gui_project_flush_pending();
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !frames || count <= 0) {
        return false;
    }
    tp_id128 anim_id = an->id;
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_id128 aid = a->id;
    tp_operation *ops = (tp_operation *)calloc((size_t)count, sizeof *ops);
    if (!ops) {
        return false;
    }
    int n = 0;
    bool oom = false;
    for (int i = 0; i < count && !oom; i++) {
        if (!(frames[i] && frames[i][0])) {
            continue;
        }
        tp_operation *op = &ops[n];
        op->kind = TP_OP_ANIMATION_FRAME_ADD;
        op->atlas_id = aid;
        op->u.anim_frame_add.anim_id = anim_id;
        op->u.anim_frame_add.frame = dupstr(frames[i]);
        op->u.anim_frame_add.index = -1; /* append */
        if (!op->u.anim_frame_add.frame) {
            oom = true;
            break;
        }
        n++;
    }
    if (oom || n == 0) {
        ops_free(ops, n);
        free(ops);
        return false;
    }
    bool ok = commit_txn_now(ops, n); /* frees the op arms */
    free(ops);
    if (ok) {
        gui_project_touch(GUI_ACT_ANIM_FRAMES);
    }
    return ok;
}

bool gui_project_anim_remove_frame(int atlas_index, int anim_index, int frame_index) {
    gui_project_flush_pending();
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || frame_index < 0 || frame_index >= an->frame_count) {
        return false;
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = a->id;
    op.u.anim_frame_rm.anim_id = an->id;
    op.u.anim_frame_rm.index = frame_index;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}

bool gui_project_anim_move_frame(int atlas_index, int anim_index, int frame_index, int delta) {
    gui_project_flush_pending();
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || frame_index < 0 || frame_index >= an->frame_count) {
        return false;
    }
    int to = frame_index + delta; /* op addresses (from,to) absolute; core clamps `to` into range */
    if (to < 0) {
        to = 0;
    }
    if (to >= an->frame_count) {
        to = an->frame_count - 1;
    }
    if (to == frame_index) {
        return true; /* no-op move (edge button): skip commit, as before */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = a->id;
    op.u.anim_frame_move.anim_id = an->id;
    op.u.anim_frame_move.from_index = frame_index;
    op.u.anim_frame_move.to_index = to;
    if (!commit_txn_now(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}
// #endregion

// #region undo / redo (F2-03 diff history)
/* A pending buffered edit counts as undoable (undo flushes it into a step, then reverts it). */
bool gui_project_can_undo(void) { return s_pending_valid || tp_model_can_undo(s_model); }
bool gui_project_can_redo(void) { return tp_model_can_redo(s_model); }
int gui_project_undo_depth(void) { return tp_model_undo_depth(s_model); }
int gui_project_redo_depth(void) { return tp_model_redo_depth(s_model); }

/* Undo reverses the most recent committed transaction via its captured semantic diff; the model
 * clone-swaps m->project, so refresh s_proj + re-resolve cached pointers exactly as commit does.
 * A buffered gesture is committed FIRST (its own step) so Ctrl+Z reverts the in-flight drag.
 * Dirty is identity-derived, so an undo back to the saved baseline reads clean. */
bool gui_project_undo(void) {
    gui_project_flush_pending(); /* flush boundary: the drag becomes one step, then we revert it */
    tp_error e = {0};
    if (tp_model_undo(s_model, &e) != TP_STATUS_OK) {
        return false;
    }
    s_proj = tp_model_project(s_model); /* the model swapped its project on undo */
    s_preview_stale = true;             /* restored model != last-packed; packing is blocked -> always stale */
    gui_scan_invalidate_all();
    return true;
}

bool gui_project_redo(void) {
    gui_project_flush_pending(); /* a buffered new edit drops the redo branch on commit, as expected */
    tp_error e = {0};
    if (tp_model_redo(s_model, &e) != TP_STATUS_OK) {
        return false;
    }
    s_proj = tp_model_project(s_model);
    s_preview_stale = true;
    gui_scan_invalidate_all();
    return true;
}
// #endregion

// #region file operations
bool gui_project_new(void) {
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    tp_project *fresh = tp_project_create();
    if (!fresh) {
        return false;
    }
    seed_default_target(fresh, 0); /* fresh GUI project exports something (I1) -- lifecycle, direct */
    if (!wrap_model(fresh)) {
        return false; /* OOM: wrap_model freed fresh + kept the CURRENT project intact (F3) -- no data loss */
    }
    set_path("");
    s_preview_stale = false;
    gui_scan_invalidate_all();
    promote_and_baseline();
    return true;
}

tp_status gui_project_open(const char *path, char *err_out, size_t err_cap) {
    pending_discard(); /* the buffered edit belongs to the OUTGOING project -> discard */
    tp_error err = {0};
    tp_project *loaded = NULL;
    tp_status st = tp_project_load(path, &loaded, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    if (!wrap_model(loaded)) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "out of memory wrapping the loaded project");
        }
        return TP_STATUS_OOM;
    }
    set_path(path);
    s_preview_stale = true; /* nothing packed this session yet */
    gui_scan_invalidate_all();
    promote_and_baseline();
    return TP_STATUS_OK;
}

tp_status gui_project_save(char *err_out, size_t err_cap) {
    if (s_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no path (use Save As)");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return gui_project_save_as(s_path, err_out, err_cap);
}

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    gui_project_flush_pending(); /* flush boundary: persist the buffered edit, never a stale model */
    tp_error err = {0};
    /* Promote to final random ids BEFORE writing (§5.5); on OS-RNG failure promote returns
     * RNG_FAILED with every id left nil, so persisting now would write a nil-id file that
     * fails on reload. Fail closed -- report and return WITHOUT saving. */
    tp_status ids = ensure_ids(&err);
    if (ids != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(ids));
        }
        return ids;
    }
    tp_status st = tp_project_save(s_proj, path, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    set_path(path);
    /* Re-baseline the identity anchor to the just-saved (possibly relativized) in-memory form.
     * Save may relativize absolute sources in place -> that identity IS the clean baseline; the
     * revision + undo history are preserved (§8/§420). */
    tp_model_mark_saved(s_model);
    return TP_STATUS_OK;
}
// #endregion
