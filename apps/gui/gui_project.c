#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gui_history.h"
#include "gui_scan.h"

#include "tp_core/tp_id.h"             /* tp_rng_os + id128 generate/nil for op ids */
#include "tp_core/tp_operation.h"      /* the typed op the GUI mutators build */
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"    /* tp_model + tp_model_apply (journal-less commit) */

/* F2-05b-i (docs/decisions/0015): every mutator below BUILDS typed tp_operation(s) and
 * commits them atomically through a journal-LESS tp_model (tp_model_wrap ->
 * tp_model_apply, the F2-01/F2-02 clone-swap engine) instead of hand-mutating tp_project
 * fields. The model OWNS the project; s_proj is a read view of tp_model_project(s_model),
 * refreshed after every committed transaction (a commit frees the old project). The
 * pre-cutover snapshot undo (gui_history.c) is KEPT unchanged -- because the model is
 * journal-less, undo just reloads a snapshot and re-wraps the model. Value-range
 * validation authority is core's now (the ops validate on commit); the GUI keeps only
 * widget parse/clamp + client naming policy (atlas/anim name uniqueness). */

// #region state
static tp_model *s_model;  /* owns s_proj; NULL until gui_project_init */
static tp_project *s_proj; /* == tp_model_project(s_model); refreshed after every commit */
static uint64_t s_txn_seq; /* monotonic transaction-id source (unique per commit) */
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_project_dirty;
static bool s_preview_stale;
static unsigned s_model_ver; /* bumped per real mutation (gui_project_model_version) */
static char s_name[256]; /* cached basename for the menu bar */

/* Snapshots (serialized project bytes) for undo/dirty recompute (ux.md §3.3c). */
static char *s_last_buf;   /* the CURRENT model, serialized (the pre-mutation snapshot the next touch pushes) */
static size_t s_last_len;
static char *s_saved_buf;  /* the last-SAVED model, serialized (dirty baseline) */
static size_t s_saved_len;
static double s_now;       /* history clock (seconds), fed each frame */

/* Pending id-promotion failure from a void-context ensure_ids() (a snapshot/touch):
 * OS-RNG failure would leave nil structural ids, which must never be silently
 * swallowed. Drained + surfaced once by the UI (gui_project_take_id_error). The
 * save path fails closed on its own, so it does NOT set this. */
static bool s_id_error;
static char s_id_error_msg[256];

/* Pending transaction REJECT (core rejected the op(s); model left byte-unchanged).
 * Surfaced once by the UI (gui_project_take_op_error) to the soft-error channel. */
static bool s_op_error;
static char s_op_error_msg[256];
// #endregion

// #region helpers
static char *dupbytes(const char *src, size_t len) {
    char *c = (char *)malloc(len ? len : 1U);
    if (c && len) {
        memcpy(c, src, len);
    }
    return c;
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

/* Serialize the live model; on OOM leaves *buf NULL. */
static void serialize_current(char **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (!s_proj) {
        return;
    }
    tp_error e = {0};
    char *b = NULL;
    size_t n = 0;
    if (tp_project_save_buffer(s_proj, &b, &n, &e) == TP_STATUS_OK) {
        *buf = b;
        *len = n;
    }
}

/* Assign a random persistent ID to any structural entity that lacks one -- nil (a
 * freshly created project/atlas/anim/target) OR loader-synthesized for a migrated
 * legacy file (§5.5). A real loaded ID (v3/v4) is preserved. Idempotent after the first
 * call. Since the cutover mints created-entity ids at op-build time, this is a no-op
 * after load-time promotion for a mutated session -- it stays as the load/save-time §5.5
 * promotion of nil/synthetic ids. Returns the promote status. */
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

static void set_last_from_current(void) {
    tp_error err = {0};
    tp_status st = ensure_ids(&err);
    if (st != TP_STATUS_OK) {
        note_id_error(st, &err); /* keep snapshotting: surfaces the fault, never crashes */
    }
    free(s_last_buf);
    serialize_current(&s_last_buf, &s_last_len);
}

/* Adopt the current model bytes as the last-SAVED baseline (dirty == 0 after). */
static void set_saved_baseline(void) {
    free(s_saved_buf);
    s_saved_buf = NULL;
    s_saved_len = 0;
    if (s_last_buf) {
        s_saved_buf = dupbytes(s_last_buf, s_last_len);
        s_saved_len = s_last_len;
    }
}

static void recompute_dirty(void) {
    s_project_dirty = !(s_last_buf && s_saved_buf && s_last_len == s_saved_len &&
                        memcmp(s_last_buf, s_saved_buf, s_last_len) == 0);
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
 * journal-less model would otherwise accumulate one 33-byte id per commit FOREVER (never
 * evicted) with an O(n) contains-scan each commit -> unbounded memory + O(n^2) CPU over a
 * long session (esp. one-commit-per-drag-frame). The commit path already NULL-guards the
 * idstore (`if (m->idstore && m->idstore->record)`), so a NULL idstore simply skips id
 * recording. Mirrors tp_model_destroy's idstore cleanup exactly. */
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

/* Wrap `p` (TAKES OWNERSHIP on success) in a fresh journal-less, idstore-less model and
 * make it the live model, refreshing the s_proj view. COMMIT-THEN-REPLACE (F2-05b-i F3,
 * mirrors the F1-00 Save-As rollback): the replacement is wrapped into a TEMP first and the
 * OLD model is destroyed only AFTER the wrap succeeds -- so an OOM in the wrap can never
 * lose the open project or leave s_proj NULL for the next frame to deref. Returns true when
 * `p` is installed; on failure `p` is freed (never leaked) and the CURRENT model+project are
 * kept intact. A NULL `p` (an upstream create/load OOM) leaves the current model untouched
 * and returns false. */
static bool wrap_model(tp_project *p) {
    if (!p) {
        return false; /* nothing to install -> keep the current model (an upstream OOM must not clear it) */
    }
    tp_model *nm = tp_model_wrap(p);
    if (!nm) {
        tp_project_destroy(p); /* wrap did not take ownership on failure */
        return false;          /* keep the current model+project intact (F3) */
    }
    drop_idstore(nm);          /* GUI carries NO idstore (F6) */
    tp_model_destroy(s_model); /* success: only now free the previous model + its owned project */
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

/* Commit `ops` as ONE atomic transaction on the persistent journal-less model. On
 * success refreshes the s_proj view (the clone-swap replaced m->project) and returns
 * true; the caller then runs any lifecycle follow-up and calls gui_project_touch(act)
 * to snapshot. On a reject the model is BYTE-UNCHANGED and the structured status is
 * recorded for the soft-error channel. ALWAYS frees the op arms (tp_operation_free) and
 * the result. Returns false on reject / no model. */
static bool commit_txn(tp_operation *ops, int nops) {
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
// #endregion

// #region lifecycle
void gui_project_init(void) {
    if (s_model) {
        return;
    }
    tp_project *p = tp_project_create();
    seed_default_target(p, 0); /* clean baseline includes it (I1) -- lifecycle, direct */
    (void)wrap_model(p);       /* first model: on OOM s_model/s_proj stay NULL (serialize is NULL-safe) */
    set_path("");
    s_project_dirty = false;
    s_preview_stale = false;
    gui_history_init();
    set_last_from_current();
    set_saved_baseline();
}

void gui_project_shutdown(void) {
    gui_history_shutdown();
    tp_model_destroy(s_model); /* frees the model + its owned project */
    s_model = NULL;
    s_proj = NULL;
    free(s_last_buf);
    s_last_buf = NULL;
    s_last_len = 0;
    free(s_saved_buf);
    s_saved_buf = NULL;
    s_saved_len = 0;
}
// #endregion

// #region accessors
tp_project *gui_project_get(void) { return s_proj; }
const char *gui_project_path(void) { return s_path; }
const char *gui_project_display_name(void) { return s_name; }
bool gui_project_has_path(void) { return s_path[0] != '\0'; }
bool gui_project_is_dirty(void) { return s_project_dirty; }
bool gui_project_is_stale(void) { return s_preview_stale; }
// #endregion

// #region dirty/stale choke point
void gui_project_touch(gui_action act) {
    s_preview_stale = true;
    tp_error id_err = {0};
    tp_status id_st = ensure_ids(&id_err); /* §5.5: promote any nil/synthetic id before this snapshot */
    if (id_st != TP_STATUS_OK) {
        note_id_error(id_st, &id_err); /* do not swallow an RNG failure */
    }
    char *nb = NULL;
    size_t nl = 0;
    serialize_current(&nb, &nl);
    if (!nb) {
        s_project_dirty = true; /* fallback: can't snapshot, assume changed */
        return;
    }
    if (s_last_buf && nl == s_last_len && memcmp(nb, s_last_buf, nl) == 0) {
        free(nb); /* memcmp dedup: no real change -> no history, no dirty flip */
        return;
    }
    if (s_last_buf) {
        gui_history_push(s_last_buf, s_last_len, (uint32_t)act, s_now); /* PRE-mutation snapshot */
    }
    free(s_last_buf);
    s_last_buf = nb;
    s_last_len = nl;
    s_model_ver++; /* a real change committed -> a view watching this drops its stale derived state */
    recompute_dirty();
}

void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
unsigned gui_project_model_version(void) { return s_model_ver; }
// #endregion

// #region mutation wrappers (each builds typed op(s) + commits through the model)
int gui_project_add_atlas(void) {
    if (!s_proj) {
        return -1;
    }
    char name[64];
    (void)snprintf(name, sizeof name, "atlas%d", s_proj->atlas_count + 1);
    tp_id128 new_id;
    if (!gen_id(&new_id)) {
        return -1;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = new_id;
    op.u.atlas_create.name = dupstr(name);
    if (!op.u.atlas_create.name) {
        tp_operation_free(&op);
        return -1;
    }
    if (!commit_txn(&op, 1)) {
        return -1;
    }
    int idx = tp_project_find_atlas_by_id(s_proj, new_id);
    if (idx >= 0) {
        seed_default_target(s_proj, idx); /* fresh atlas exports something (I1) -- lifecycle, direct */
    }
    gui_project_touch(GUI_ACT_ADD_ATLAS);
    return idx;
}

void gui_project_remove_atlas(int index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, index);
    if (!a) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a->id;
    if (commit_txn(&op, 1)) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_ATLAS);
    }
}

gui_add_status gui_project_add_source_kind(int atlas_index, const char *path, tp_source_kind kind) {
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
    if (!commit_txn(&op, 1)) {
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
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || source_index < 0 || source_index >= a->source_count) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a->id;
    op.u.source_ref.source_id = a->sources[source_index].id;
    if (commit_txn(&op, 1)) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_SOURCE);
    }
}

bool gui_project_set_atlas_name(int atlas_index, const char *name) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_ATLAS);
    return true;
}

bool gui_project_set_sprite_rename(int atlas_index, const char *sprite_name, const char *rename) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_SPRITE); /* touch dedups a no-op rename */
    return true;
}

void gui_project_touch_setting(void) { gui_project_touch(GUI_ACT_SET_SETTING); }

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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_SETTING);
    return true;
}

/* Commits a sprite.override.set on the PENDING (name-keyed) record for `sprite_name`:
 * nil source_id + verbatim key (== the export-key bridge, since sprite_name is
 * ext-stripped). Core applies the masked fields then prunes the record if it becomes
 * all-default (keeps storage sparse) -- byte-identical to the pre-cutover
 * add_sprite/set/prune. Touches through GUI_ACT_SET_SETTING (gesture coalescing). */
static bool sprite_override_commit(int atlas_index, const char *sprite_name, tp_op_sprite_set payload) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_SETTING);
    return true;
}

bool gui_project_set_sprite_origin(int atlas_index, const char *sprite_name, float ox, float oy) {
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_ORIGIN;
    p.origin_x = ox;
    p.origin_y = oy;
    return sprite_override_commit(atlas_index, sprite_name, p);
}

bool gui_project_set_sprite_slice9(int atlas_index, const char *sprite_name, int lrtb_index, int value) {
    if (lrtb_index < 0 || lrtb_index >= 4) {
        return false;
    }
    /* Read-modify-write: the op's SLICE9 mask sets all four components, but the widget
     * edits one at a time. Seed from the current record (absent -> all-zero). */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    tp_op_sprite_set p;
    memset(&p, 0, sizeof p);
    p.mask = TP_SPF_SLICE9;
    for (int k = 0; k < 4; k++) {
        p.slice9[k] = ov ? ov->slice9_lrtb[k] : 0;
    }
    p.slice9[lrtb_index] = (uint16_t)(value < 0 ? 0 : value);
    return sprite_override_commit(atlas_index, sprite_name, p);
}

bool gui_project_set_sprite_override(int atlas_index, const char *sprite_name, gui_sprite_ov which, int value) {
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
    return sprite_override_commit(atlas_index, sprite_name, p);
}

int gui_project_add_target(int atlas_index) {
    /* Seeds the default json-neotolis target (core owns the exporter id + path). LIFECYCLE
     * seeding, not a mutation op (a frontend must not name an exporter id literal); the
     * seed's target gets its id at the save/touch §5.5 promotion, exactly as before. */
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || tp_project_atlas_seed_default_target(s_proj, atlas_index) != TP_STATUS_OK) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_TARGET);
    a = tp_project_get_atlas(s_proj, atlas_index);
    return a ? a->target_count - 1 : -1;
}

void gui_project_remove_target(int atlas_index, int index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || index < 0 || index >= a->target_count) {
        return;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_REMOVE;
    op.atlas_id = a->id;
    op.u.target_ref.target_id = a->targets[index].id;
    if (commit_txn(&op, 1)) {
        gui_project_touch(GUI_ACT_REMOVE_TARGET);
    }
}

bool gui_project_set_target(int atlas_index, int index, const char *exporter_id, const char *out_path, bool enabled) {
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
    if (!commit_txn(&op, 1)) {
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

/* The atlas's animation named `name` (exact), or NULL. One home for the by-name scan the
 * exists-check / remove / rename-clash paths each open-coded (F2-05b-i F8). */
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
    if (!commit_txn(&op, 1)) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_ANIM);
    /* Re-fetch the (clone-swapped) atlas to report the appended animation's index. */
    a = tp_project_get_atlas(s_proj, atlas_index);
    return (a && tp_project_atlas_find_animation_by_id(a, anim_id)) ? (a->animation_count - 1) : -1;
}

void gui_project_remove_animation(int atlas_index, const char *id) {
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
    if (commit_txn(&op, 1)) {
        gui_project_touch(GUI_ACT_REMOVE_ANIM);
    }
}

bool gui_project_set_anim_id(int atlas_index, int anim_index, const char *new_id) {
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
    /* The F2-01 operation catalog has NO animation-rename op (the CLI has no anim rename
     * verb either -- animations are name-keyed; the structural id is stable). This is the
     * ONE mutator that cannot be expressed as an op in b-i; it stays a direct name write.
     * Coherent because the model is journal-less: the write mutates m->project in place and
     * touch() snapshots it; the next transaction clones it. Documented in decision 0015. */
    char *copy = dupstr(new_id);
    if (!copy) {
        return false;
    }
    free(an->name); /* boundary-ok: no ANIMATION_RENAME op exists (decision 0015) */
    an->name = copy; /* boundary-ok: no ANIMATION_RENAME op exists (decision 0015) */
    gui_project_touch(GUI_ACT_RENAME_ANIM);
    return true;
}

/* One animation.settings.set with `mask`/values; false on OOM / reject / no-op skip. */
static bool anim_settings_commit(int atlas_index, int anim_index, uint32_t mask, float fps, int playback, bool flip_h,
                                 bool flip_v) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_ANIM);
    return true;
}

bool gui_project_set_anim_fps(int atlas_index, int anim_index, float fps) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!(fps >= 1.0F)) {
        fps = 1.0F; /* widget parse-clamp (>=1); core also range-checks (>0 finite) on commit */
    }
    if (an->fps == fps) {
        return true; /* no-op: skip the commit (no history/dirty), exactly as before */
    }
    return anim_settings_commit(atlas_index, anim_index, TP_ANF_FPS, fps, an->playback, an->flip_h, an->flip_v);
}

bool gui_project_set_anim_playback(int atlas_index, int anim_index, int playback) {
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
    if (an->playback == playback) {
        return true;
    }
    return anim_settings_commit(atlas_index, anim_index, TP_ANF_PLAYBACK, an->fps, playback, an->flip_h, an->flip_v);
}

bool gui_project_set_anim_flip(int atlas_index, int anim_index, bool flip_h, bool flip_v) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (an->flip_h == flip_h && an->flip_v == flip_v) {
        return true;
    }
    return anim_settings_commit(atlas_index, anim_index, TP_ANF_FLIP_H | TP_ANF_FLIP_V, an->fps, an->playback, flip_h,
                                flip_v);
}

bool gui_project_anim_add_frames(int atlas_index, int anim_index, const char *const *frames, int count) {
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
    bool ok = commit_txn(ops, n); /* frees the op arms */
    free(ops);
    if (ok) {
        gui_project_touch(GUI_ACT_ANIM_FRAMES);
    }
    return ok;
}

bool gui_project_anim_remove_frame(int atlas_index, int anim_index, int frame_index) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}

bool gui_project_anim_move_frame(int atlas_index, int anim_index, int frame_index, int delta) {
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
    if (!commit_txn(&op, 1)) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}
// #endregion

// #region undo / redo
bool gui_project_can_undo(void) { return gui_history_can_undo(); }
bool gui_project_can_redo(void) { return gui_history_can_redo(); }

/* Loads `buf` (owned; adopted as the new last snapshot) into the live model and re-wraps
 * the journal-less model around the restored project (snapshot undo stays coherent
 * because the model is journal-less -- F2-05b-i). The file path is invariant across
 * undo/redo, so project_dir is carried over from the old model. */
static bool restore_from_buffer(char *buf, size_t len) {
    tp_project *np = NULL;
    tp_error e = {0};
    if (tp_project_load_buffer(buf, len, &np, &e) != TP_STATUS_OK) {
        return false;
    }
    np->project_dir = (s_proj && s_proj->project_dir) ? dupstr(s_proj->project_dir) : NULL;
    if (!wrap_model(np)) {
        return false; /* OOM re-wrapping: np freed + the CURRENT model kept intact (F3); undo aborts cleanly */
    }

    free(s_last_buf);
    s_last_buf = buf; /* the restored bytes ARE the current serialization */
    s_last_len = len;

    recompute_dirty();
    s_preview_stale = true; /* restored model != last-packed; since packing is blocked, always stale */
    gui_scan_invalidate_all();
    return true;
}

bool gui_project_undo(void) {
    char *out = NULL;
    size_t olen = 0;
    if (!gui_history_undo(s_last_buf, s_last_len, &out, &olen)) {
        return false;
    }
    if (!restore_from_buffer(out, olen)) {
        free(out);
        return false;
    }
    return true;
}

bool gui_project_redo(void) {
    char *out = NULL;
    size_t olen = 0;
    if (!gui_history_redo(s_last_buf, s_last_len, &out, &olen)) {
        return false;
    }
    if (!restore_from_buffer(out, olen)) {
        free(out);
        return false;
    }
    return true;
}
// #endregion

// #region file operations
bool gui_project_new(void) {
    tp_project *fresh = tp_project_create();
    if (!fresh) {
        return false;
    }
    seed_default_target(fresh, 0); /* fresh GUI project exports something (I1) -- lifecycle, direct */
    if (!wrap_model(fresh)) {
        return false; /* OOM: wrap_model freed fresh + kept the CURRENT project intact (F3) -- no data loss */
    }
    set_path("");
    s_project_dirty = false;
    s_preview_stale = false;
    gui_scan_invalidate_all();
    gui_history_reset();
    set_last_from_current();
    set_saved_baseline();
    return true;
}

tp_status gui_project_open(const char *path, char *err_out, size_t err_cap) {
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
    s_project_dirty = false;
    s_preview_stale = true; /* nothing packed this session yet */
    gui_scan_invalidate_all();
    gui_history_reset();
    set_last_from_current();
    set_saved_baseline();
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
    s_project_dirty = false;
    /* Save may have relativized absolute sources -> re-snapshot the on-disk form and
     * adopt it as the saved baseline (undo history is preserved). */
    set_last_from_current();
    set_saved_baseline();
    return TP_STATUS_OK;
}
// #endregion
