/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). Split out of main.c
 * (GUI decomposition step 2) as a pure move -- definitions relocated verbatim, no behavior change.
 * This TU is Clay-free AND nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_shell.h" /* gui_shell_reset_shown_result: kill the freed-pointer bind after a clear (P2) */
#include "tinyfiledialogs.h"

#include "tp_core/tp_export.h" /* tp_exporter_at -> the preview selector's exporter list */
#include "tp_core/tp_names.h"  /* tp_names_common_prefix (anim id from selection) */

#include "app/nt_app.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initial capacity of the preview resolved-frame scratch (update_preview); grows past it (P1 fix). */
#define PREVIEW_IDXS_INIT_CAP 512

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
bool s_pending_pack, s_pending_export;
bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
bool s_pending_commit_edit_enter; /* Enter in the inline editor -> commit it (deferred, non-force) */
bool s_pending_add_anim;    /* "+ Animation" -> append empty animation, select it */
bool s_pending_create_anim; /* "Create animation from selection" */
bool s_pending_open_preview;/* open the anim preview player on s_ctx_anim / s_sel_anim */
int s_pending_remove_atlas = -1;
int s_pending_remove_source = -1;
int s_pending_remove_anim = -1; /* animation index to remove */
bool s_pending_add_target;
int s_pending_remove_target = -1;
int s_pending_browse_target = -1;        /* target whose out-path "..." dialog is queued */
int s_pending_export_browse_atlas = -1;  /* Export dialog: atlas of the queued out-path browse */
int s_pending_export_browse_target = -1; /* Export dialog: target index of the queued browse */
int s_pending_preview_target = -1;       /* strip preview-selector pick (-1 none; 0 Native; k = exporter k-1) */
int s_after_confirm;
bool s_confirm_open;
int s_modal_action;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
int s_last_pack_atlas = -1; /* which atlas that timing belongs to */

// #region deferred model-edit queue (F2-05b-i, decision 0015)
/* A commit clone-swaps the model + frees the old project, so declare_* render fns must not
 * commit while holding a live atlas/sprite/anim/target pointer. They ENQUEUE the edit here;
 * drain_edits() (run at frame top from apply_pending, no live pointer held) replays each via
 * the self-contained gui_project_* setters. The queue grows (never a fixed slot) so no edit is
 * ever dropped if two land in one frame; typically it holds 0 or 1.
 *
 * String args that can be LONG carry a HEAP copy, not a fixed slot: out_path is up to
 * TP_PATH_MAX (4096) so a 256-byte slot silently truncated + persisted a corrupted export
 * path on a mere target toggle (F2); add-frames carries a variable-length list of COPIED
 * sprite keys so "Add frames" can defer instead of committing synchronously mid-declare (F1).
 * edit_dispose frees that heap payload after the edit drains (or if a queue-OOM drops it). */
typedef enum {
    GEDIT_ATLAS = 0, /* one atlas.settings.set knob: field in i0, ivalue in i1, fvalue in f0 (F7) */
    GEDIT_SPRITE_ORIGIN,
    GEDIT_SPRITE_SLICE9,
    GEDIT_SPRITE_OVERRIDE,
    GEDIT_ANIM_FPS,
    GEDIT_ANIM_PLAYBACK,
    GEDIT_ANIM_FLIP,
    GEDIT_ANIM_FRAME_REMOVE,
    GEDIT_ANIM_FRAME_MOVE,
    GEDIT_ANIM_ADD_FRAMES, /* append COPIED selection keys to anim i0 (F1: was a sync commit -> UAF) */
    GEDIT_TARGET
} gui_edit_kind;

typedef struct {
    gui_edit_kind kind;
    int atlas;
    int i0, i1, i2; /* field/which/anim_index/target_index; ivalue/frame_index/playback; delta */
    float f0, f1;
    bool b0, b1;
    char s0[256];   /* sprite name / exporter id -- short + bounded (registry id / GUI-wide 256 key) */
    char *out_path; /* HEAP: target out_path (up to TP_PATH_MAX); freed after drain -- F2 (no 255 cap) */
    char **keys;    /* HEAP: add-frames sprite keys, copied at enqueue; freed after drain -- F1 */
    int key_count;
} gui_edit;

static gui_edit *s_edits;
static int s_edit_count;
static int s_edit_cap;

/* Local heap strdup (POSIX strdup is not ISO C17). NULL treated as ""; NULL on OOM. */
static char *edit_strdup(const char *s) {
    if (!s) {
        s = "";
    }
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
}

/* Frees an edit's heap payload (out_path + copied keys). Safe on a zeroed/partially-built edit. */
static void edit_dispose(gui_edit *e) {
    free(e->out_path);
    e->out_path = NULL;
    if (e->keys) {
        for (int i = 0; i < e->key_count; i++) {
            free(e->keys[i]);
        }
        free(e->keys);
        e->keys = NULL;
    }
    e->key_count = 0;
}

/* Appends `e` to the queue (shallow copy -> the queue TAKES OWNERSHIP of e's heap out_path/keys;
 * the caller must not free them afterward, on success OR failure). On queue-realloc OOM the edit
 * is DROPPED: its heap payload is freed (no leak) and a status-bar error is raised so the drop is
 * visible -- the widget already returned "committed", so without this the value silently reverts
 * next frame with no explanation (F5). Returns true iff queued. */
static bool edit_push(gui_edit *e) {
    if (s_edit_count == s_edit_cap) {
        int nc = s_edit_cap ? s_edit_cap * 2 : 8;
        gui_edit *ne = (gui_edit *)realloc(s_edits, (size_t)nc * sizeof *ne);
        if (!ne) {
            edit_dispose(e);
            set_status_ex(STATUS_ERROR, "Out of memory: this edit could not be queued (change not applied).");
            return false;
        }
        s_edits = ne;
        s_edit_cap = nc;
    }
    s_edits[s_edit_count++] = *e;
    return true;
}

void gui_edit_atlas_int(int atlas, gui_atlas_field field, int value) {
    gui_edit e = {0};
    e.kind = GEDIT_ATLAS;
    e.atlas = atlas;
    e.i0 = (int)field;
    e.i1 = value;
    (void)edit_push(&e);
}
void gui_edit_atlas_bool(int atlas, gui_atlas_field field, bool value) {
    gui_edit e = {0};
    e.kind = GEDIT_ATLAS;
    e.atlas = atlas;
    e.i0 = (int)field;
    e.i1 = value ? 1 : 0; /* bool == ivalue 0/1 (F7) */
    (void)edit_push(&e);
}
void gui_edit_atlas_float(int atlas, gui_atlas_field field, float value) {
    gui_edit e = {0};
    e.kind = GEDIT_ATLAS;
    e.atlas = atlas;
    e.i0 = (int)field;
    e.f0 = value;
    (void)edit_push(&e);
}
void gui_edit_sprite_origin(int atlas, const char *sprite_name, int axis, float value) {
    gui_edit e = {0};
    e.kind = GEDIT_SPRITE_ORIGIN;
    e.atlas = atlas;
    e.i0 = axis; /* 0 = Pivot X, 1 = Pivot Y -- component-keyed (F2-05b-ii-A #2) */
    e.f0 = value;
    (void)snprintf(e.s0, sizeof e.s0, "%s", sprite_name ? sprite_name : "");
    (void)edit_push(&e);
}
void gui_edit_sprite_slice9(int atlas, const char *sprite_name, int lrtb_index, int value) {
    gui_edit e = {0};
    e.kind = GEDIT_SPRITE_SLICE9;
    e.atlas = atlas;
    e.i0 = lrtb_index;
    e.i1 = value;
    (void)snprintf(e.s0, sizeof e.s0, "%s", sprite_name ? sprite_name : "");
    (void)edit_push(&e);
}
void gui_edit_sprite_override(int atlas, const char *sprite_name, gui_sprite_ov which, int value) {
    gui_edit e = {0};
    e.kind = GEDIT_SPRITE_OVERRIDE;
    e.atlas = atlas;
    e.i0 = (int)which;
    e.i1 = value;
    (void)snprintf(e.s0, sizeof e.s0, "%s", sprite_name ? sprite_name : "");
    (void)edit_push(&e);
}
void gui_edit_anim_fps(int atlas, int anim_index, float fps) {
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_FPS;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.f0 = fps;
    (void)edit_push(&e);
}
void gui_edit_anim_playback(int atlas, int anim_index, int playback) {
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_PLAYBACK;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.i1 = playback;
    (void)edit_push(&e);
}
void gui_edit_anim_flip(int atlas, int anim_index, bool flip_h, bool flip_v) {
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_FLIP;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.b0 = flip_h;
    e.b1 = flip_v;
    (void)edit_push(&e);
}
void gui_edit_anim_frame_remove(int atlas, int anim_index, int frame_index) {
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_FRAME_REMOVE;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.i1 = frame_index;
    (void)edit_push(&e);
}
void gui_edit_anim_frame_move(int atlas, int anim_index, int frame_index, int delta) {
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_FRAME_MOVE;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.i1 = frame_index;
    e.i2 = delta;
    (void)edit_push(&e);
}
void gui_edit_target(int atlas, int index, const char *exporter_id, const char *out_path, bool enabled) {
    gui_edit e = {0};
    e.kind = GEDIT_TARGET;
    e.atlas = atlas;
    e.i0 = index;
    e.b0 = enabled;
    (void)snprintf(e.s0, sizeof e.s0, "%s", exporter_id ? exporter_id : "");
    e.out_path = edit_strdup(out_path); /* HEAP full path -- a >255 out_path must not truncate (F2) */
    if (!e.out_path) {
        set_status_ex(STATUS_ERROR, "Out of memory: export target edit not applied.");
        return;
    }
    (void)edit_push(&e);
}

/* Enqueue an "add frames" edit carrying a COPY of the selection keys (F1). "Add frames" used to
 * commit synchronously from inside declare_animation_editor, which clone-swaps + frees the project
 * under the live `an`/`a` the same declare invocation keeps dereferencing (frame_count, frames[].
 * name) -> a use-after-free on an ordinary click. Deferring it (drain replays via
 * gui_project_anim_add_frames at frame top, no live pointer held) closes that last synchronous
 * commit; the keys are copied NOW so a selection change before the drain cannot alter what lands. */
void gui_edit_anim_add_frames(int atlas, int anim_index, const char *const *keys, int count) {
    if (count <= 0) {
        return;
    }
    gui_edit e = {0};
    e.kind = GEDIT_ANIM_ADD_FRAMES;
    e.atlas = atlas;
    e.i0 = anim_index;
    e.keys = (char **)calloc((size_t)count, sizeof *e.keys);
    if (!e.keys) {
        set_status_ex(STATUS_ERROR, "Out of memory: add-frames not applied.");
        return;
    }
    int w = 0;
    bool oom = false;
    for (int i = 0; i < count; i++) {
        if (!keys[i] || keys[i][0] == '\0') {
            continue; /* skip empties (the setter would too) */
        }
        e.keys[w] = edit_strdup(keys[i]);
        if (!e.keys[w]) {
            oom = true;
            break;
        }
        w++;
    }
    e.key_count = w;
    if (oom || w == 0) {
        edit_dispose(&e);
        if (oom) {
            set_status_ex(STATUS_ERROR, "Out of memory: add-frames not applied.");
        }
        return;
    }
    (void)edit_push(&e);
}

/* Replays every queued edit through the committing setters, then clears the queue. Runs at
 * frame top (apply_pending) with NO live declare-fn pointer held, so the per-edit clone-swap
 * is safe. Each setter re-fetches by index/name internally. */
static void drain_edits(void) {
    for (int i = 0; i < s_edit_count; i++) {
        gui_edit *e = &s_edits[i];
        switch (e->kind) {
            case GEDIT_ATLAS: /* int/bool -> ivalue (i1); float -> fvalue (f0); the other is 0 (F7) */
                (void)gui_project_set_atlas_setting(e->atlas, (gui_atlas_field)e->i0, e->i1, e->f0);
                break;
            case GEDIT_SPRITE_ORIGIN:
                (void)gui_project_set_sprite_origin(e->atlas, e->s0, e->i0, e->f0);
                break;
            case GEDIT_SPRITE_SLICE9:
                (void)gui_project_set_sprite_slice9(e->atlas, e->s0, e->i0, e->i1);
                break;
            case GEDIT_SPRITE_OVERRIDE:
                (void)gui_project_set_sprite_override(e->atlas, e->s0, (gui_sprite_ov)e->i0, e->i1);
                break;
            case GEDIT_ANIM_FPS:
                (void)gui_project_set_anim_fps(e->atlas, e->i0, e->f0);
                break;
            case GEDIT_ANIM_PLAYBACK:
                (void)gui_project_set_anim_playback(e->atlas, e->i0, e->i1);
                break;
            case GEDIT_ANIM_FLIP:
                (void)gui_project_set_anim_flip(e->atlas, e->i0, e->b0, e->b1);
                break;
            case GEDIT_ANIM_FRAME_REMOVE:
                (void)gui_project_anim_remove_frame(e->atlas, e->i0, e->i1);
                break;
            case GEDIT_ANIM_FRAME_MOVE:
                (void)gui_project_anim_move_frame(e->atlas, e->i0, e->i1, e->i2);
                break;
            case GEDIT_ANIM_ADD_FRAMES:
                (void)gui_project_anim_add_frames(e->atlas, e->i0, (const char *const *)e->keys, e->key_count);
                break;
            case GEDIT_TARGET:
                (void)gui_project_set_target(e->atlas, e->i0, e->s0, e->out_path ? e->out_path : "", e->b0);
                break;
        }
        edit_dispose(e); /* free this edit's heap payload (out_path / copied keys) after it replays */
    }
    s_edit_count = 0;
}

/* Set by a view widget the frame its edit GESTURE ENDS (slider release / field Enter+blur / a
 * discrete dropdown/checkbox pick). apply_pending flushes gui_project's pending transaction AFTER
 * drain_edits buffers this frame's value, so the whole gesture commits as ONE undo step
 * (F2-05b-ii-A, decision 0015). One shared flag suffices: pending_route already flushes a prior
 * gesture when a different-key edit arrives, so the flag always targets the latest buffered edit. */
static bool s_gesture_commit;
void gui_request_gesture_commit(void) { s_gesture_commit = true; }
// #endregion

/* Refresh-vs-pack honesty latch (P2): a Refresh dirties the sources on DISK without touching the model,
 * so model_changed_since (which only diffs the serialized model) can't see it. If a Refresh lands while
 * an async pack is in flight, the just-landed result reflects the PRE-refresh disk -- so it must NOT
 * clear stale. do_refresh bumps s_refresh_epoch; do_pack snapshots it at start; poll_async clears stale
 * only when the model is unchanged AND no Refresh happened since the pack started. */
static unsigned s_refresh_epoch;
static unsigned s_pack_start_refresh_epoch;

/* True (and raises a status) when an async pack/export is running: the destructive ops (new/open/exit/
 * undo/redo) refuse while busy. Centralizes the guard the request_* fns had copy-pasted, and closes the
 * gap where undo/redo skipped it (P2 -- undo mid-pack then a pre-undo result landing was confusing). */
static bool busy_block(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Wait for the pack/export to finish (or Cancel) first.");
        return true;
    }
    return false;
}

// #region selection / edit helpers
/* Stops the animation preview player and restores the canvas to its atlas/source view. */
void preview_stop(void) {
    s_preview_active = false;
    s_preview_playing = false;
    s_preview_finished = false;
    s_preview_time = 0.0;
    s_canvas.anim_sprite = -1;
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ANIM) {
        s_canvas.mode = gui_canvas_has_atlas(&s_canvas) ? GUI_CANVAS_ATLAS : GUI_CANVAS_SOURCE;
    }
}

void reset_selection(void) {
    s_sel_src = -1;
    s_sel_child = -1;
    s_sel_abs[0] = '\0';
    s_sel_missing = false;
    multi_sel_clear();
    s_sel_anchor_row = -1;
    s_sel_anim = -1;
    s_sel_anim_frame = -1;
    preview_stop();
    preview_target_reset(); /* export-target preview is bound to the selection/atlas -> drop it on any reset */
}

void cancel_edit(void) {
    s_edit_kind = EDIT_NONE;
    s_edit_atlas = -1;
    s_edit_sprite[0] = '\0';
    s_edit_buf[0] = '\0';
}

/* --- start-edit entry points: the entry side of the same edit lifecycle as the inline-rename commit
 * path below (commit_active_edit, which inlines the atlas + animation rename and delegates the sprite
 * rename to commit_sprite_rename). Moved here in step 4 (they are Clay-free) so gui_view_lists and
 * gui_view_settings -- both of which start edits -- share one home instead of step 5 re-deciding. --- */
void start_atlas_edit(int i) {
    tp_project *p = gui_project_get();
    if (!p || i < 0 || i >= p->atlas_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ATLAS;
    s_edit_atlas = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", p->atlases[i].name);
    set_status("Rename atlas: type, Enter to commit, Esc to cancel.");
}
void start_anim_edit(int i) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || i < 0 || i >= a->animation_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ANIM;
    s_edit_anim = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", a->animations[i].name);
    set_status("Rename animation: type, Enter to commit, Esc to cancel.");
}
void start_sprite_edit_named(const char *sprite_name) {
    if (!sprite_name || sprite_name[0] == '\0') {
        return;
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    cancel_edit();
    s_edit_kind = EDIT_SPRITE;
    (void)snprintf(s_edit_sprite, sizeof s_edit_sprite, "%s", sprite_name);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    /* Seed with the CURRENT name: the rename override if set, else the file-derived final name
     * (sprite_name is the ext-stripped atlas-relative key = the default export name). The input
     * string is game-owned (nt_ui_input edits s_edit_buf in place), so seeding it here is the fix
     * for the "field opens empty" bug -- previously it seeded the (empty) override. */
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", (ov && ov->rename) ? ov->rename : sprite_name);
    set_status("Rename region: type, Enter to commit, Esc clears/cancels.");
}
void start_sprite_edit(const sprite_row *row) {
    if (!row || row->is_folder || row->missing || row->sprite_name[0] == '\0') {
        return;
    }
    start_sprite_edit_named(row->sprite_name);
}

/* Atlas-name validation (F1): non-empty, unique among atlases, and normalization-safe
 * (no path separators, not dots-only). Fills `err` on failure. */
static bool atlas_name_valid(const char *name, int self_idx, char *err, size_t cap) {
    if (!name || name[0] == '\0') {
        (void)snprintf(err, cap, "Atlas name cannot be empty");
        return false;
    }
    bool only_dots = true;
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\') {
            (void)snprintf(err, cap, "Atlas name cannot contain / or \\");
            return false;
        }
        if (*c != '.') {
            only_dots = false;
        }
    }
    if (only_dots) {
        (void)snprintf(err, cap, "Atlas name cannot be dots-only");
        return false;
    }
    tp_project *p = gui_project_get();
    for (int i = 0; p && i < p->atlas_count; i++) {
        if (i != self_idx && strcmp(p->atlases[i].name, name) == 0) {
            (void)snprintf(err, cap, "Atlas '%s' already exists", name);
            return false;
        }
    }
    return true;
}

void clamp_selection(void) {
    tp_project *p = gui_project_get();
    if (!p || p->atlas_count == 0) {
        s_sel_atlas = 0;
        reset_selection();
        return;
    }
    if (s_sel_atlas >= p->atlas_count) {
        s_sel_atlas = p->atlas_count - 1;
    }
    if (s_sel_atlas < 0) {
        s_sel_atlas = 0;
    }
}
// #endregion

// #region animation + preview actions (ux.md §3.7b)
/* The selected animation of the selected atlas, or NULL. */
tp_project_anim *current_anim(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || s_sel_anim < 0 || s_sel_anim >= a->animation_count) {
        return NULL;
    }
    return &a->animations[s_sel_anim];
}

/* Copies the multi-selection into the shared buffers, natural-sorted; returns the count (0 on OOM,
 * with a status set -- the caller then does nothing, never truncates). */
static int build_sorted_selection(void) {
    const int n = s_multi_sel_count;
    if (!sel_sort_reserve(n)) { /* grow the sort scratch WITH the selection (P1 fix, step 7) */
        set_status_ex(STATUS_ERROR, "Out of memory: could not sort the selection.");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        (void)snprintf(s_sel_sort_buf[i], sizeof s_sel_sort_buf[0], "%s", s_multi_sel[i]);
    }
    qsort(s_sel_sort_buf, (size_t)n, sizeof s_sel_sort_buf[0], nat_cmp_qsort);
    for (int i = 0; i < n; i++) {
        s_sel_sort_ptr[i] = s_sel_sort_buf[i];
    }
    return n;
}

/* Creates an animation from the current multi-selection: frames natural-sorted, id from the common
 * prefix (auto "animN" when there is none). Selects the new animation (opens its editor). */
int create_animation_from_selection(void) {
    if (s_multi_sel_count <= 0) {
        return -1;
    }
    const int n = build_sorted_selection();
    if (n <= 0) {
        return -1; /* OOM in the sort scratch (status already set) -- do nothing rather than truncate */
    }
    char base[192];
    tp_names_common_prefix(s_sel_sort_buf, n, base, sizeof base);
    const int idx = gui_project_create_animation(s_sel_atlas, base[0] ? base : NULL, s_sel_sort_ptr, n);
    if (idx >= 0) {
        s_sel_anim = idx;
        s_sel_anim_frame = -1;
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).", a->animations[idx].name, n);
    }
    return idx;
}

/* Appends the current multi-selection (natural-sorted) as frames of animation `anim_index`.
 * DEFERRED (F2-05b-i F1): this is called from declare_animation_editor, which holds live
 * `a`/`an` pointers it keeps dereferencing AFTER this returns. A synchronous commit here would
 * clone-swap + free the project under those pointers -> use-after-free on a plain "Add frames"
 * click. So it builds the sorted selection (read-only) and ENQUEUES an add-frames edit carrying
 * COPIED keys; apply_pending drains it next frame with no live pointer held (benign one-frame
 * lag, consistent with every other panel edit). */
void add_selection_frames_to_anim(int anim_index) {
    if (s_multi_sel_count <= 0) {
        return;
    }
    const int n = build_sorted_selection();
    if (n <= 0) {
        return; /* OOM in the sort scratch (status already set) -- do nothing rather than truncate */
    }
    gui_edit_anim_add_frames(s_sel_atlas, anim_index, s_sel_sort_ptr, n);
    set_statusf("Adding %d frame(s) to the animation (Ctrl+Z to undo).", n); /* lands on the next drain */
}

/* Opens the preview player on animation `anim_index` (plays from the packed regions; if the atlas is
 * not packed yet, the canvas shows a "Pack to preview" hint). */
void open_preview(int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return;
    }
    cancel_edit();
    preview_target_reset(); /* the anim player owns the canvas -> never leave an export preview bound under it */
    s_sel_anim = anim_index;
    s_preview_active = true;
    s_preview_playing = true;
    s_preview_finished = false;
    s_preview_time = 0.0;
    if (!gui_pack_result(s_sel_atlas)) {
        set_status("Pack (Ctrl+P) to preview the animation on packed regions.");
    } else {
        set_statusf("Previewing '%s' \xE2\x80\x94 Space play/pause.", a->animations[anim_index].name);
    }
}

void preview_toggle_play(void) {
    if (!s_preview_active) {
        return;
    }
    if (s_preview_playing) {
        s_preview_playing = false;
    } else {
        if (s_preview_finished) {
            s_preview_time = 0.0;
            s_preview_finished = false;
        }
        s_preview_playing = true;
    }
}

/* Nudges the preview timeline by `delta` frame-ticks (pauses first). */
void preview_step(int delta) {
    if (!s_preview_active) {
        return;
    }
    const tp_project_anim *an = current_anim();
    const float fps = (an && an->fps >= 1.0F) ? an->fps : 1.0F;
    s_preview_playing = false;
    s_preview_finished = false;
    long step = (long)floor(s_preview_time * (double)fps);
    step += delta;
    if (step < 0) {
        step = 0;
    }
    s_preview_time = ((double)step + 0.5) / (double)fps;
}

/* Resolves the selected animation's frames to packed regions and pushes the current frame to the
 * canvas each frame. Advances the clock while playing. No-op (leaves the hint) without a pack result. */
void update_preview(void) {
    if (!s_preview_active) {
        return;
    }
    tp_project_anim *an = current_anim();
    const tp_result *pr = gui_pack_result(s_sel_atlas);
    s_canvas.anim_sprite = -1;
    s_preview_frame_count = 0;
    if (!an || !pr) {
        return; /* declare_canvas draws the "Pack to preview" hint */
    }
    /* Resolved-frame scratch: grows to hold every frame (realloc-keep-capacity; runs each frame while
     * previewing, so it never mallocs once large enough). The old fixed idxs[512] silently dropped
     * frames past 512 (P1 fix, step 7). On OOM we keep the old capacity and the loop truncates loudly. */
    static int *idxs;
    static int idxs_cap;
    if (an->frame_count > idxs_cap) {
        int newcap = idxs_cap ? idxs_cap : PREVIEW_IDXS_INIT_CAP;
        while (newcap < an->frame_count) {
            newcap *= 2;
        }
        int *grown = realloc(idxs, (size_t)newcap * sizeof *idxs);
        if (grown) {
            idxs = grown;
            idxs_cap = newcap;
        } else {
            set_status_ex(STATUS_ERROR, "Out of memory: preview frames truncated.");
        }
    }
    int n = 0;
    int rw = 1;
    int rh = 1;
    for (int i = 0; i < an->frame_count && n < idxs_cap; i++) {
        const int si = gui_pack_find_sprite(s_sel_atlas, an->frames[i].name);
        if (si >= 0 && si < pr->sprite_count) {
            idxs[n++] = si;
            if (pr->sprites[si].sourceSize.w > rw) {
                rw = pr->sprites[si].sourceSize.w;
            }
            if (pr->sprites[si].sourceSize.h > rh) {
                rh = pr->sprites[si].sourceSize.h;
            }
        }
    }
    s_preview_frame_count = n;
    if (n == 0) {
        return;
    }
    const float fps = (an->fps >= 1.0F) ? an->fps : 1.0F;
    if (s_preview_playing) {
        s_preview_time += (double)g_nt_app.dt;
    }
    bool finished = false;
    int cur = gui_canvas_anim_frame_at(s_preview_time, fps, an->playback, n, &finished);
    if (finished && s_preview_playing) {
        s_preview_playing = false;
    }
    s_preview_finished = finished;
    if (cur < 0) {
        cur = 0;
    }
    if (cur >= n) {
        cur = n - 1;
    }
    s_preview_cur = cur;
    s_canvas.mode = GUI_CANVAS_ANIM;
    s_canvas.anim_sprite = idxs[cur];
    s_canvas.anim_ref_w = rw;
    s_canvas.anim_ref_h = rh;
    s_canvas.anim_flip_h = an->flip_h;
    s_canvas.anim_flip_v = an->flip_v;
}
// #endregion

// #region file dialogs (tinyfiledialogs)
static void ensure_project_ext(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    const char *base = path_last(out);
    if (strrchr(base, '.') == NULL) { /* boundary-ok: project FILENAME ext, not a sprite key */
        size_t len = strlen(out);
        (void)snprintf(out + len, cap - len, ".ntpacker_project");
    }
}

static void do_open(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *path = tinyfd_openFileDialog("Open Project", "", 1, filt, "ntpacker project", 0);
    if (!path) {
        return;
    }
    if (!gui_scan_exists(path)) {
        set_statusf_ex(STATUS_WARNING, "project not found: %s", path); /* never fatal (F6b) */
        return;
    }
    char err[256];
    if (gui_project_open(path, err, sizeof err) == TP_STATUS_OK) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_statusf("Opened %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Open failed: %s", err);
    }
}

static void do_save_as(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *def = gui_project_has_path() ? gui_project_path() : "untitled.ntpacker_project";
    const char *path = tinyfd_saveFileDialog("Save Project As", def, 1, filt, "ntpacker project");
    if (!path) {
        return;
    }
    char full[600];
    ensure_project_ext(path, full, sizeof full);
    char err[256];
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

static void do_save(void) {
    if (!gui_project_has_path()) {
        do_save_as();
        return;
    }
    char err[256];
    if (gui_project_save(err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

static void do_add_files(void) {
    static const char *filt[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"};
    const char *res = tinyfd_openFileDialog("Add Image Files", "", 5, filt, "image files", 1);
    if (!res) {
        return;
    }
    /* tinyfd returns a '|'-joined path list in its OWN fixed static buffer, capped at 32 paths of
     * <= 1024 bytes each (MAX_MULTIPLE_FILES x MAX_PATH_OR_CMD). Parse it IN PLACE, one segment at a
     * time into a single-path buffer, so nothing truncates: the old `char buf[8192]` silently dropped
     * the tail of a large multi-select mid-path (P2). Any per-path overflow (a path longer than the OS
     * max -- pathological) is dropped LOUDLY with a count. */
    int added = 0;
    int dup = 0;
    int dropped = 0;
    const char *start = res;
    for (;;) {
        const char *bar = strchr(start, '|');
        const size_t seg = bar ? (size_t)(bar - start) : strlen(start);
        if (seg > 0) {
            char path[1024]; /* MAX_PATH_OR_CMD: one tinyfd path never exceeds this */
            if (seg < sizeof path) {
                memcpy(path, start, seg);
                path[seg] = '\0';
                normalize_slashes(path);
                /* These come from the file-picker dialog: record the true kind. */
                const gui_add_status r = gui_project_add_source_kind(s_sel_atlas, path, TP_SOURCE_KIND_FILE);
                if (r == GUI_ADD_ADDED) {
                    added++;
                } else if (r == GUI_ADD_DUPLICATE) {
                    dup++;
                }
            } else {
                dropped++;
            }
        }
        if (!bar) {
            break;
        }
        start = bar + 1;
    }
    if (dropped > 0) {
        set_statusf_ex(STATUS_WARNING, "Added %d file source(s); %d dropped (path too long), %d already added", added,
                       dropped, dup);
    } else if (dup > 0) {
        set_statusf("Added %d file source(s); %d already added", added, dup);
    } else {
        set_statusf("Added %d file source(s)", added);
    }
}

/* Best-effort relativize `abs` against the project dir (targets travel like sources).
 * Absolute paths outside the project dir are kept as-is (usable, save leaves them). */
static void relativize_to_project(const char *abs, char *out, size_t cap) {
    tp_project *p = gui_project_get();
    const char *dir = p ? p->project_dir : NULL;
    if (dir && dir[0] != '\0') {
        const size_t dl = strlen(dir);
        if (strncmp(abs, dir, dl) == 0 && (abs[dl] == '/' || abs[dl] == '\\')) {
            (void)snprintf(out, cap, "%s", abs + dl + 1);
            normalize_slashes(out);
            return;
        }
    }
    (void)snprintf(out, cap, "%s", abs);
    normalize_slashes(out);
}

/* Save dialog for a target's output path, relativized to the project like sources. Atlas-explicit so
 * the Export dialog (which spans all atlases) can browse any target, not just the selected atlas's. */
static void do_browse_target_at(int atlas, int ti) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), atlas);
    if (!a || ti < 0 || ti >= a->target_count) {
        return;
    }
    const tp_project_target *t = &a->targets[ti];
    const char *path = tinyfd_saveFileDialog("Export output path", t->out_path, 0, NULL, NULL);
    if (!path) {
        return;
    }
    char rel[600];
    relativize_to_project(path, rel, sizeof rel);
    if (gui_project_set_target(atlas, ti, t->exporter_id, rel, t->enabled)) {
        set_statusf("Output path: %s", rel);
    }
}
static void do_browse_target(int ti) { do_browse_target_at(s_sel_atlas, ti); }

static void do_add_folder(void) {
    const char *dir = tinyfd_selectFolderDialog("Add Folder", "");
    if (!dir) {
        return;
    }
    char norm[600];
    (void)snprintf(norm, sizeof norm, "%s", dir);
    normalize_slashes(norm);
    const gui_add_status r = gui_project_add_source(s_sel_atlas, norm);
    if (r == GUI_ADD_ADDED) {
        set_statusf("Added folder %s", path_last(norm));
    } else if (r == GUI_ADD_DUPLICATE) {
        set_statusf_ex(STATUS_WARNING, "already added: %s", path_last(norm));
    } else {
        set_status_ex(STATUS_ERROR, "Add folder failed.");
    }
}
// #endregion

// #region new/exit confirm flow
/* fix2 [0]/[1]: a buffered gesture's flush-commit failed (journal append failure) -> the edit is gone
 * AND the model is NOT what the caller expects (clean-or-current). Surface the reason (the op-error is
 * already set by the failed commit) and tell the caller to ABORT, so a destructive gate (New/Open/Exit)
 * never silently discards the project over a lost edit and a pack/export never runs on a stale model.
 * Returns true when the flush FAILED (the caller must abort); false when it is safe to proceed. */
static bool flush_failed(void) {
    if (gui_project_flush_pending()) {
        return false; /* nothing pending / net-zero no-op / committed OK -> proceed */
    }
    char m[256];
    gui_project_flush_error(m, sizeof m); /* fix3 [2]: shared neutral wording (save/pack/gate) */
    set_status_ex(STATUS_ERROR, m);
    return true;
}

void request_new(void) {
    if (busy_block()) {
        return;
    }
    if (flush_failed()) {
        return; /* fix2 [0]: journal-failed flush dropped the edit -> abort (never discard over a silent loss) */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_NEW;
        s_confirm_open = true;
    } else if (gui_project_new()) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    } else {
        set_status_ex(STATUS_ERROR, "Out of memory: could not create a new project (current project kept)."); /* F3 */
    }
}
void request_exit(void) {
    if (busy_block()) {
        return;
    }
    if (flush_failed()) {
        return; /* fix2 [0]: journal-failed flush dropped the edit -> abort the quit (never quit over a silent loss) */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_EXIT;
        s_confirm_open = true;
    } else {
        nt_app_quit();
    }
}
/* Open routes through the same unsaved-changes confirm as New/Exit (no silent discard). The actual
 * OS open dialog runs via s_pending_open, either now (clean) or after the modal resolves. */
void request_open(void) {
    if (busy_block()) {
        return;
    }
    if (flush_failed()) {
        return; /* fix2 [0]: journal-failed flush dropped the edit -> abort the open (never switch over a silent loss) */
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_OPEN;
        s_confirm_open = true;
    } else {
        s_pending_open = true;
    }
}
static void confirm_perform(void) {
    if (s_after_confirm == AFTER_NEW) {
        if (gui_project_new()) {
            gui_pack_clear(-1);
            gui_shell_reset_shown_result();
            cancel_edit();
            clamp_selection();
            reset_selection();
            set_status("New project.");
        } else {
            set_status_ex(STATUS_ERROR, "Out of memory: could not create a new project (current project kept)."); /* F3 */
        }
    } else if (s_after_confirm == AFTER_EXIT) {
        nt_app_quit();
    } else if (s_after_confirm == AFTER_OPEN) {
        s_pending_open = true; /* runs the open dialog next frame */
    }
    s_after_confirm = AFTER_NONE;
}
// #endregion

// #region undo/redo + refresh actions
void do_undo(void) {
    if (busy_block()) {
        return; /* same async-busy guard as new/open/exit -- undo mid-pack then a pre-undo land is confusing */
    }
    if (flush_failed()) {
        return; /* fix4 [2]: a buffered gesture could not be journaled -> abort (error surfaced), never
                 * undo a DIFFERENT step. After this, gui_project_undo()'s false means empty-history only
                 * (undo does NOT journal), so "Nothing to undo." below is correct again. */
    }
    if (gui_project_undo()) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Undo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
void do_redo(void) {
    if (busy_block()) {
        return;
    }
    if (flush_failed()) {
        return; /* fix4 [2]: a journal-failed flush is not "Nothing to redo" -- abort (error surfaced) */
    }
    if (gui_project_redo()) {
        gui_pack_clear(-1);
        gui_shell_reset_shown_result();
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Redo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

/* Fingerprint every source (folders expand to their scanned children, files stat
 * directly) so a Refresh can diff added/removed/changed. Missing entries carry
 * size==-1 so a vanish/restore reads as removed/added. */
typedef struct fp_entry {
    char abs[512];
    long long size;
    long long mtime;
} fp_entry;

static void fp_collect(fp_entry **arr, int *count, int *cap) {
    tp_project *p = gui_project_get();
    for (int ai = 0; p && ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char abs[512];
            if (tp_project_resolve_path(p, a->sources[si].path, abs, sizeof abs) != TP_STATUS_OK) {
                continue;
            }
            if (gui_scan_is_dir(abs)) {
                const gui_scan_result *sc = gui_scan_get(abs);
                for (int ci = 0; ci < sc->count; ci++) {
                    if (*count == *cap) {
                        int nc = *cap ? *cap * 2 : 64;
                        fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                        if (!ne) {
                            return;
                        }
                        *arr = ne;
                        *cap = nc;
                    }
                    (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", sc->entries[ci].abs);
                    (*arr)[*count].size = sc->entries[ci].size;
                    (*arr)[*count].mtime = sc->entries[ci].mtime;
                    (*count)++;
                }
            } else {
                if (*count == *cap) {
                    int nc = *cap ? *cap * 2 : 64;
                    fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                    if (!ne) {
                        return;
                    }
                    *arr = ne;
                    *cap = nc;
                }
                long long sz = -1;
                long long mt = -1;
                (void)gui_scan_stat(abs, &sz, &mt);
                (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", abs);
                (*arr)[*count].size = sz;
                (*arr)[*count].mtime = mt;
                (*count)++;
            }
        }
    }
}

static const fp_entry *fp_find(const fp_entry *arr, int n, const char *abs) {
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].abs, abs) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

/* F4: rescan all sources, diff, evict the canvas cache, mark preview stale (NOT dirty). */
static void do_refresh(void) {
    fp_entry *before = NULL;
    int bn = 0;
    int bc = 0;
    fp_collect(&before, &bn, &bc);

    gui_scan_invalidate_all(); /* drop per-dir caches so fp_collect below rescans disk */

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    fp_collect(&after, &an, &ac);

    int added = 0;
    int removed = 0;
    int changed = 0;
    for (int i = 0; i < an; i++) {
        const fp_entry *b = fp_find(before, bn, after[i].abs);
        if (!b) {
            added++;
        } else if (b->size != after[i].size || b->mtime != after[i].mtime) {
            changed++;
        }
    }
    for (int i = 0; i < bn; i++) {
        if (!fp_find(after, an, before[i].abs)) {
            removed++;
        }
    }
    free(before);
    free(after);

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    s_refresh_epoch++;                /* an in-flight pack landing later must NOT clear this stale (P2) */
    set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed, changed);
}

/* Ctrl+P / Pack: pack the selected atlas in-process (gui_pack -> tp_pack). On success clear the
 * preview-stale bit and upload the packed pages to the canvas (atlas-page view); on failure the
 * previous result + the "outdated" tag stay (ux.md §3.3b). */
/* Blocking pack of the selected atlas (deterministic path for the selftest + --shot). Interactive
 * Pack (do_pack) runs this on a worker thread; the result lands via poll_async. */
void do_pack_blocking(void) {
    if (flush_failed()) {
        return; /* fix2 [1]: a journal-failed flush dropped the edit -> abort (never pack a stale model + report success) */
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
        return;
    }
    char err[256] = {0};
    char note[128] = {0};
    double ms = 0.0;
    if (gui_pack_atlas(s_sel_atlas, &ms, err, sizeof err, note, sizeof note)) {
        gui_project_mark_packed(); /* clears preview_stale for the current model */
        s_last_pack_ms = ms;
        s_last_pack_atlas = s_sel_atlas;
        /* the per-frame canvas<->atlas sync (frame()) picks up the new result pointer and uploads. */
        const tp_result *r = gui_pack_result(s_sel_atlas);
        const double shown_ms = s_status_fixed_time ? 0.0 : ms;
        if (note[0] != '\0') {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)", r->sprite_count, r->page_count, shown_ms, note);
        } else {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms", r->sprite_count, r->page_count, shown_ms);
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Interactive Pack (Ctrl+P / strip / stale chip): starts the pack on a worker thread so the window
 * never freezes. Completion is applied at a frame boundary by poll_async (apply_pending). */
void do_pack(void) {
    if (flush_failed()) {
        return; /* fix2 [1]: a journal-failed flush dropped the edit -> abort (never pack a stale model + report success) */
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
        return;
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_async_start(s_sel_atlas, err, sizeof err)) {
        s_pack_start_refresh_epoch = s_refresh_epoch; /* a Refresh after this must keep stale when it lands (P2) */
        set_status_ex(STATUS_INFO, "Packing\xE2\x80\xA6");   /* result lands via poll_async */
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Ctrl+E / Export: starts an async export of every atlas with sources + >=1 enabled target on a
 * worker thread (per-atlas failures non-fatal, ux.md §3.5). Completion reported via poll_async. */
static void do_export(void) {
    if (flush_failed()) {
        return; /* fix2 [1]: a journal-failed flush dropped the edit -> abort (never export a stale model + report success) */
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_export_async_start(err, sizeof err)) {
        set_status_ex(STATUS_INFO, "Exporting\xE2\x80\xA6"); /* progress + result via poll_async */
    } else {
        set_status_ex(STATUS_WARNING, err);
    }
}

// #region export-target preview (packet EXP-PREVIEW)
/* Back to Native: drop the preview state + free gui_pack's preview slot. Idempotent. */
void preview_target_reset(void) {
    s_preview_target = 0;
    s_preview_ver = 0;
    gui_pack_preview_clear();
}

/* The result the canvas should BIND this frame: the export preview when one is selected, has landed, and
 * the strip selector is actually visible (single-row tier); otherwise the native session pack. The anim
 * player owns the canvas in its own mode, so it always falls back to native (its frame indices are into
 * the native result). Single source of truth -- used by main.c's canvas bind AND the canvas stats line. */
const tp_result *preview_target_result(void) {
    const tp_result *native = gui_pack_result(s_sel_atlas);
    if (s_preview_target == 0 || s_preview_active) {
        return native;
    }
    if (s_canvas_w < S(STRIP_PREVIEW_MIN_W)) {
        return native; /* the selector folded away (compact / narrow single-row) -> show the honest session pack */
    }
    const tp_result *pv = gui_pack_preview_result(s_sel_atlas);
    return pv ? pv : native; /* preview not landed yet (async) -> native until it does */
}

/* Starts the preview pack for a strip-selector pick. combo 0 (or a bad index) -> Native. */
static void preview_target_start(int combo_index) {
    if (combo_index <= 0) {
        preview_target_reset();
        return;
    }
    const tp_exporter *e = tp_exporter_at(combo_index - 1);
    if (!e) {
        preview_target_reset();
        return;
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "Add sources first to preview an export target.");
        preview_target_reset();
        return;
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- wait for the current pack or export to finish.");
        preview_target_reset();
        return;
    }
    char err[256] = {0};
    if (gui_pack_preview_async_start(s_sel_atlas, e->id, err, sizeof err)) {
        s_preview_target = combo_index;
        s_preview_ver = gui_project_model_version();
        set_statusf_ex(STATUS_INFO, "Preview: %s\xE2\x80\xA6", e->display_name ? e->display_name : e->id);
    } else {
        preview_target_reset();
        set_statusf_ex(STATUS_ERROR, "Preview failed: %s", err);
    }
}

/* Per-frame reconciliation: a model edit since the preview packed makes it stale -> drop to Native (never
 * show a silently-wrong preview). Atlas switch / undo / redo / open / new drop it via reset_selection. */
static void preview_target_sync(void) {
    if (s_preview_target != 0 && gui_project_model_version() != s_preview_ver) {
        preview_target_reset();
    }
}
// #endregion

/* Lands a finished async pack/export at a frame boundary: the pack slot swap is done inside
 * gui_pack_poll; here we recompute stale honestly (mark_packed only when the model still matches
 * the packed snapshot) and route the outcome through the severity status. Called from apply_pending. */
static void poll_async(void) {
    gui_pack_result_info info;
    switch (gui_pack_poll(&info)) {
        case GUI_PACK_DONE_PACK_OK: {
            /* Clear stale only when the model is unchanged since the packed snapshot AND no Refresh
             * dirtied the sources on disk since the pack started (model_changed_since can't see a
             * disk-only Refresh -- P2). Otherwise the just-landed result is honestly stale. */
            if (!info.model_changed && s_refresh_epoch == s_pack_start_refresh_epoch) {
                gui_project_mark_packed();
            }
            s_last_pack_ms = info.ms;
            s_last_pack_atlas = info.atlas_index;
            const tp_result *r = gui_pack_result(info.atlas_index);
            const char *stale = info.model_changed ? " -- model changed, stale" : "";
            if (r && info.note[0] != '\0') {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)%s", r->sprite_count, r->page_count, info.ms, info.note, stale);
            } else if (r) {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms%s", r->sprite_count, r->page_count, info.ms, stale);
            }
            break;
        }
        case GUI_PACK_DONE_PACK_FAIL:
            set_statusf_ex(STATUS_ERROR, "Pack failed: %s", info.err);
            break;
        case GUI_PACK_DONE_PACK_CANCELLED:
            set_status_ex(STATUS_INFO, "Pack cancelled.");
            break;
        case GUI_PACK_DONE_EXPORT_OK:
            set_statusf_ex(info.notices > 0 ? STATUS_WARNING : STATUS_SUCCESS, "Exported %d target(s)%s", info.targets,
                           info.notices > 0 ? " (metadata notices raised)" : "");
            break;
        case GUI_PACK_DONE_EXPORT_FAIL:
            set_statusf_ex(STATUS_ERROR, "Exported %d target(s); %d atlas(es) failed -- %s", info.targets, info.atlases_fail, info.err);
            break;
        case GUI_PACK_DONE_EXPORT_CANCELLED:
            set_status_ex(STATUS_INFO, "Export cancelled.");
            break;
        case GUI_PACK_DONE_PREVIEW_OK:
            if (s_preview_target == 0) {
                gui_pack_preview_clear(); /* selection reset/changed while packing -> drop the orphan slot */
            } else {
                /* The degradation chip is width-gated (STRIP_CHIP_MIN_W) and drops on common window
                 * sizes, so the pill also carries the summary -- it is width-independent. */
                const tp_exporter *pe = tp_exporter_at(s_preview_target - 1);
                if (pe) {
                    char chip[96] = {0};
                    char tip[256] = {0};
                    const int nd = gui_pack_preview_diff(s_sel_atlas, pe->id, chip, sizeof chip, tip, sizeof tip);
                    if (nd > 0) {
                        set_statusf_ex(STATUS_WARNING, "Previewing %s export: %s", pe->display_name ? pe->display_name : pe->id, chip);
                    } else {
                        set_statusf_ex(STATUS_SUCCESS, "Previewing %s export: same layout rules as native.",
                                       pe->display_name ? pe->display_name : pe->id);
                    }
                }
            }
            break;
        case GUI_PACK_DONE_PREVIEW_FAIL:
            preview_target_reset();
            set_statusf_ex(STATUS_ERROR, "Preview failed: %s", info.err);
            break;
        case GUI_PACK_DONE_PREVIEW_CANCELLED:
            preview_target_reset();
            set_status_ex(STATUS_INFO, "Preview cancelled.");
            break;
        case GUI_PACK_DONE_NONE:
        default:
            break;
    }
    /* Surface a pending id-promotion fault recorded by a snapshot/touch (OS-RNG
     * failure): the model kept its nil ids, so warn loudly instead of swallowing it. */
    char id_err[256];
    if (gui_project_take_id_error(id_err, sizeof id_err)) {
        set_statusf_ex(STATUS_ERROR, "Structural id assignment failed: %s", id_err);
    }
    /* Surface a pending transaction REJECT (core rejected a mutator's op -- out-of-range value
     * or bad reference): the model was left byte-unchanged, so report it as a soft error
     * (F2-05b-i). In practice the widgets clamp valid ranges, so this rarely fires. */
    char op_err[256];
    if (gui_project_take_op_error(op_err, sizeof op_err)) {
        set_statusf_ex(STATUS_WARNING, "Edit rejected: %s", op_err);
    }
}
// #endregion

// #region inline rename commit
void commit_sprite_rename(void) {
    /* empty input clears the override back to the file-derived name. (Reached only via commit_active_edit,
     * which flush-firsts, so set_sprite_rename's own flush is a no-op here.) */
    if (gui_project_set_sprite_rename(s_sel_atlas, s_edit_sprite, s_edit_buf)) {
        if (s_edit_buf[0] == '\0') {
            set_statusf("Cleared rename on '%s'", s_edit_sprite);
        } else {
            set_statusf("Renamed '%s' -> '%s'", s_edit_sprite, s_edit_buf);
        }
    }
    cancel_edit();
}

/* Commit the active inline edit as if Enter was pressed (click-outside / model-change path).
 * `force` = the editor is being dismissed involuntarily: an invalid atlas name CANCELS instead of
 * keeping a zombie editor (the validation message stays in the status bar). Sprite rename never
 * zombies (empty clears the override). No-op when nothing is being edited.
 *
 * fix4 (DEFINITIVE closure of the journal-failed-flush class): FLUSH-FIRST at the entry via
 * flush_failed(). Any buffered gesture is committed-or-aborted HERE; on a journal-failed flush the
 * neutral error is surfaced and we abort (keeping the editor open unless forced, so the user can retry
 * after freeing disk). After a successful flush, each rename op's OWN internal flush is a guaranteed
 * no-op, so its return is DOMAIN-ONLY: for the anim branch (set_anim_id is a DIRECT write, never a
 * journal append) a false is a genuine name collision, so the "must be unique" warning is correct again
 * and never silent -- no anim_id_exists heuristic (fix3's heuristic was wrong: it matched the anim's own
 * unchanged name). set_atlas_name / set_sprite_rename journal their rename op; on that op's OWN append
 * failure they return false -> no success message + the op-error surfaces via poll_async (not a false
 * success, not a wrong "already exists"). */
static void commit_active_edit(bool force) {
    if (s_edit_kind == EDIT_NONE) {
        return; /* nothing being edited */
    }
    if (flush_failed()) {
        /* a buffered gesture could not be journaled -> abort the rename commit (error already surfaced) */
        if (force) {
            cancel_edit();
        }
        return;
    }
    if (s_edit_kind == EDIT_ATLAS) {
        char err[128];
        if (!atlas_name_valid(s_edit_buf, s_edit_atlas, err, sizeof err)) {
            set_status_ex(STATUS_WARNING, err);
            if (force) {
                cancel_edit();
            }
            return;
        }
        if (gui_project_set_atlas_name(s_edit_atlas, s_edit_buf)) {
            set_statusf("Renamed atlas to '%s'", s_edit_buf);
        }
        cancel_edit();
    } else if (s_edit_kind == EDIT_SPRITE) {
        commit_sprite_rename();
    } else if (s_edit_kind == EDIT_ANIM) {
        if (s_edit_buf[0] == '\0') {
            set_status_ex(STATUS_WARNING, "Animation name cannot be empty.");
            if (force) {
                cancel_edit();
            }
            return;
        }
        if (!gui_project_set_anim_id(s_sel_atlas, s_edit_anim, s_edit_buf)) {
            /* Post-flush, set_anim_id's false is a genuine name collision (the typed name is held by
             * ANOTHER animation), OR a rare non-collision failure (OOM on the name dup, or a stale
             * out-of-range edit index). gui_project_anim_id_exists() distinguishes them cleanly HERE:
             * the own-name case returns true from set_anim_id (a no-op) so it never reaches this branch,
             * and the flush-fail confound that made this heuristic wrong in fix3 is gone (the entry
             * flush_failed() above handles the journal case). So report the collision as such, else a
             * real error rather than a misleading "must be unique". */
            if (gui_project_anim_id_exists(s_sel_atlas, s_edit_buf)) {
                set_status_ex(STATUS_WARNING, "Animation name must be unique.");
            } else {
                set_status_ex(STATUS_ERROR, "Could not rename the animation.");
            }
            if (force) {
                cancel_edit();
            }
            return;
        }
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    }
}
// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
void apply_pending(void) {
    poll_async(); /* land any finished async pack/export/preview before this frame's canvas pickup */
    preview_target_sync(); /* drop a preview the model has since outrun (after poll: an in-flight one lands first) */

    /* A press landed outside the active inline editor last frame -> commit it (desktop rename UX).
     * Also fires before any pending model change (remove/refresh/open/new) so no orphaned editor
     * survives a mutation. */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit) {
        commit_active_edit(true);
    }
    s_pending_commit_edit = false;
    /* Enter pressed in an inline editor last frame -> commit it here (deferred, non-force: an
     * invalid atlas/anim name keeps the editor open). Deferring the commit off the declare pass is
     * what keeps declare_left_panel / the anim editor from committing while holding proj/a/an
     * (F2-05b-i UAF fix). */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit_enter) {
        commit_active_edit(false);
    }
    s_pending_commit_edit_enter = false;

    /* Drain the deferred model-edit queue (settings / overrides / anim knobs / target edits the
     * declare fns enqueued last frame). Runs here, at frame top, with no live declare-fn pointer
     * held -- so the per-edit clone-swap can never dangle a panel's cached atlas/sprite/anim/target
     * pointer (decision 0015). */
    drain_edits();

    /* A gesture ended last frame (slider release / field Enter+blur / discrete pick): commit the
     * buffered transaction NOW that drain_edits has folded in this frame's final value, so one
     * interaction == one undo step (F2-05b-ii-A, decision 0015). */
    if (s_gesture_commit) {
        /* fix2: the bool is intentionally IGNORED -- this is the gesture-BOUNDARY commit (one interaction
         * = one undo step). A journal-failed flush here drops the gesture WITH the op-error surfaced
         * (poll_async shows it); there is no persist/discard "proceed as clean" decision after it to
         * abort (unlike save/new/pack). Audited fix2 [0]/[1]. */
        (void)gui_project_flush_pending();
        s_gesture_commit = false;
    }

    if (s_modal_action == MODAL_SAVE) {
        do_save();
        s_confirm_open = false;
        if (!gui_project_is_dirty()) {
            confirm_perform();
        } else {
            s_after_confirm = AFTER_NONE; /* save cancelled -> abort the pending action */
        }
    } else if (s_modal_action == MODAL_DISCARD) {
        s_confirm_open = false;
        confirm_perform();
    } else if (s_modal_action == MODAL_CANCEL) {
        s_confirm_open = false;
        s_after_confirm = AFTER_NONE;
    }
    s_modal_action = MODAL_NONE;

    if (s_pending_open) {
        do_open();
    }
    if (s_pending_save) {
        do_save();
    }
    if (s_pending_save_as) {
        do_save_as();
    }
    if (s_pending_add_files) {
        do_add_files();
    }
    if (s_pending_add_folder) {
        do_add_folder();
    }
    if (s_pending_add_atlas) {
        int idx = gui_project_add_atlas();
        if (idx >= 0) {
            s_sel_atlas = idx;
            reset_selection();
            set_statusf("Added atlas '%s'", tp_project_get_atlas(gui_project_get(), idx)->name);
        }
    }
    if (s_pending_remove_source >= 0) {
        /* fix3 [0]: side-effects + "Removed" run ONLY on a real removal -- a journal-failed flush
         * aborts the wrapper (returns false), so no false "Removed" / bad Ctrl+Z (op-error surfaced). */
        if (gui_project_remove_source(s_sel_atlas, s_pending_remove_source)) {
            reset_selection();
            set_status("Removed source (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_atlas >= 0) {
        if (gui_project_remove_atlas(s_pending_remove_atlas)) {
            clamp_selection();
            reset_selection();
            set_status("Removed atlas (Ctrl+Z to undo).");
        }
    }
    if (s_pending_add_target) {
        const int ti = gui_project_add_target(s_sel_atlas);
        if (ti >= 0) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_target >= 0) {
        if (gui_project_remove_target(s_sel_atlas, s_pending_remove_target)) {
            set_status("Removed export target (Ctrl+Z to undo).");
        }
    }
    if (s_pending_export_browse_atlas >= 0 && s_pending_export_browse_target >= 0) {
        do_browse_target_at(s_pending_export_browse_atlas, s_pending_export_browse_target);
    }
    if (s_pending_browse_target >= 0) {
        do_browse_target(s_pending_browse_target);
    }
    if (s_pending_add_anim) {
        const int idx = gui_project_create_animation(s_sel_atlas, NULL, NULL, 0);
        if (idx >= 0) {
            s_sel_anim = idx;
            s_sel_anim_frame = -1;
            set_statusf("Added animation '%s' (Ctrl+Z to undo).",
                        tp_project_get_atlas(gui_project_get(), s_sel_atlas)->animations[idx].name);
        }
    }
    if (s_pending_create_anim) {
        (void)create_animation_from_selection();
    }
    if (s_pending_remove_anim >= 0) {
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        if (a && s_pending_remove_anim < a->animation_count) {
            /* fix3 [0]: preview_stop + the selection reset + "Removed" run ONLY on a real removal. A
             * journal-failed flush aborts the wrapper (returns false) -> the animation is still there,
             * so we must NOT stop its preview or clear the selection. (preview_stop only resets flags,
             * so running it AFTER the removal is safe -- no project deref.) */
            const bool was_previewing = (s_preview_active && s_sel_anim == s_pending_remove_anim);
            if (gui_project_remove_animation(s_sel_atlas, a->animations[s_pending_remove_anim].name)) {
                if (was_previewing) {
                    preview_stop();
                }
                s_sel_anim = -1;
                s_sel_anim_frame = -1;
                set_status("Removed animation (Ctrl+Z to undo).");
            }
        }
    }
    if (s_pending_open_preview) {
        open_preview(s_sel_anim);
    }
    if (s_pending_refresh) {
        do_refresh();
    }
    if (s_pending_pack) {
        do_pack();
    }
    if (s_pending_export) {
        do_export();
    }
    if (s_pending_preview_target >= 0) {
        preview_target_start(s_pending_preview_target);
    }

    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = s_pending_pack = s_pending_export = false;
    s_pending_add_target = false;
    s_pending_add_anim = s_pending_create_anim = s_pending_open_preview = false;
    s_pending_remove_source = -1;
    s_pending_remove_atlas = -1;
    s_pending_remove_target = -1;
    s_pending_export_browse_atlas = -1;
    s_pending_export_browse_target = -1;
    s_pending_remove_anim = -1;
    s_pending_browse_target = -1;
    s_pending_preview_target = -1;
}
// #endregion
