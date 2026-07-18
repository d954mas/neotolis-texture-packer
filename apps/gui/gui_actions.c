/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "tp_core/tp_export.h" /* tp_exporter_at -> the preview selector's exporter list */
#include "tp_core/tp_names.h"  /* tp_names_common_prefix (anim id from selection) */

#include "app/nt_app.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Initial capacity of the grow-only active-preview frame map. */
#define PREVIEW_IDXS_INIT_CAP 512

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
bool s_pending_pack, s_pending_export;
bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
bool s_pending_commit_edit_enter; /* Enter in the inline editor -> commit it (deferred, non-force) */

/* Presentation-only mapping from the active stable animation to one Pack result. */

bool s_pending_remove_atlas;
tp_id128 s_pending_remove_atlas_id;
int64_t s_pending_remove_atlas_revision;
bool s_pending_remove_source;
tp_id128 s_pending_remove_source_atlas_id;
tp_id128 s_pending_remove_source_id;
int64_t s_pending_remove_source_revision;
int s_pending_preview_target = -1; /* boundary-ok: exporter option, not a target entity index */
int s_after_confirm;
bool s_confirm_open;
int s_modal_action;
/* R6b: startup crash-recovery modal glue. The orphan list lives here; the modal reads it via the
 * count/at accessors and requests a per-row action, deferred to apply_pending() (below) so the
 * Save-As dialog + disk-mutating gui_recovery_resolve run outside nt_ui_begin/end, like s_pending_save_as. */
bool s_recovery_open;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
int s_last_pack_atlas = -1; /* which atlas that timing belongs to */

gui_actions_state s_actions = {.recovery_pending_row = -1};

#ifdef NTPACKER_GUI_SELFTEST
void gui_preview_frame_work_reset(void) {
    memset(&s_actions.preview_frame_work, 0, sizeof s_actions.preview_frame_work);
}
gui_preview_frame_work gui_preview_frame_work_get(void) {
    return s_actions.preview_frame_work;
}
#endif


_Static_assert(sizeof s_actions.edit_sprite_source_key == TP_SRCKEY_MAX,
               "editor source-key buffer must match the canonical bound");

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
    memset(&s_actions.preview_animation_ref, 0, sizeof s_actions.preview_animation_ref);
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
    s_actions.edit_atlas_id = tp_id128_nil();
    s_actions.edit_atlas_revision = 0;
    s_actions.edit_anim_atlas_id = tp_id128_nil();
    s_actions.edit_anim_id = tp_id128_nil();
    s_actions.edit_anim_revision = 0;
    s_edit_sprite[0] = '\0';
    s_actions.edit_sprite_atlas_id = tp_id128_nil();
    s_actions.edit_sprite_source_id = tp_id128_nil();
    s_actions.edit_sprite_revision = 0;
    s_actions.edit_sprite_source_key[0] = '\0';
    s_edit_buf[0] = '\0';
}

/* --- start-edit entry points: the entry side of the same edit lifecycle as the inline-rename commit
 * path below (commit_active_edit, which inlines the atlas + animation rename and delegates the sprite
 * rename to commit_sprite_rename). They live here (Clay-free) so gui_view_lists and gui_view_settings
 * -- both of which start edits -- share one home. --- */
static bool edit_text_fits(const char *value, size_t capacity,
                           const char *entity) {
    if (value && strlen(value) < capacity) {
        return true;
    }
    set_statusf_ex(STATUS_ERROR,
                   "%s name exceeds the GUI edit limit; it was not changed.",
                   entity ? entity : "Item");
    return false;
}

void start_atlas_edit(int i) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, i)
                                         : NULL;
    if (!atlas) {
        return;
    }
    start_atlas_edit_ref(atlas->id, tp_session_snapshot_revision(snapshot));
}
void start_atlas_edit_ref(tp_id128 atlas_id, int64_t expected_revision) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
        ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
        : NULL;
    if (!atlas) {
        return;
    }
    if (!edit_text_fits(atlas->name, sizeof s_edit_buf, "Atlas")) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ATLAS;
    s_edit_atlas = -1;
    for (int i = 0; i < tp_session_snapshot_atlas_count(snapshot); ++i) {
        const tp_snapshot_atlas *candidate =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (candidate && tp_id128_eq(candidate->id, atlas_id)) {
            s_edit_atlas = i;
            break;
        }
    }
    s_actions.edit_atlas_id = atlas->id;
    s_actions.edit_atlas_revision = expected_revision;
    memcpy(s_edit_buf, atlas->name, strlen(atlas->name) + 1U);
    set_status("Rename atlas: type, Enter to commit, Esc to cancel.");
}
void start_anim_edit(int i) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
    const tp_snapshot_animation *animation = a ? tp_session_snapshot_animation_at(snapshot, a->id, i) : NULL;
    if (!animation) {
        return;
    }
    const gui_animation_ref ref = {
        a->id, animation->id, tp_session_snapshot_revision(snapshot)};
    start_anim_edit_ref(&ref);
}
void start_anim_edit_ref(const gui_animation_ref *ref) {
    if (!ref) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation = snapshot
        ? tp_session_snapshot_animation_by_id(snapshot, ref->atlas_id,
                                              ref->animation_id)
        : NULL;
    if (!animation) {
        return;
    }
    if (!edit_text_fits(animation->name, sizeof s_edit_buf, "Animation")) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ANIM;
    s_edit_anim = -1;
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(snapshot, ref->atlas_id);
    for (int i = 0; atlas && i < atlas->animation_count; ++i) {
        const tp_snapshot_animation *candidate =
            tp_session_snapshot_animation_at(snapshot, atlas->id, i);
        if (candidate && tp_id128_eq(candidate->id, ref->animation_id)) {
            s_edit_anim = i;
            break;
        }
    }
    s_actions.edit_anim_atlas_id = ref->atlas_id;
    s_actions.edit_anim_id = animation->id;
    s_actions.edit_anim_revision = ref->expected_revision;
    memcpy(s_edit_buf, animation->name, strlen(animation->name) + 1U);
    set_status("Rename animation: type, Enter to commit, Esc to cancel.");
}
void start_sprite_edit_ref(const gui_sprite_ref *sprite,
                           const char *display_name) {
    if (!sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0' || !display_name ||
        display_name[0] == '\0') {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
        ? tp_session_snapshot_atlas_by_id(snapshot, sprite->atlas_id)
        : NULL;
    if (!atlas) {
        return;
    }
    const tp_snapshot_sprite *ov = tp_session_snapshot_sprite_by_key(
        snapshot, sprite->atlas_id, sprite->source_id, sprite->source_key);
    const char *edit_value = (ov && ov->rename) ? ov->rename : display_name;
    if (!edit_text_fits(sprite->source_key,
                        sizeof s_actions.edit_sprite_source_key, "Region key") ||
        !edit_text_fits(display_name, sizeof s_edit_sprite, "Region") ||
        !edit_text_fits(edit_value, sizeof s_edit_buf, "Region")) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_SPRITE;
    s_actions.edit_sprite_atlas_id = sprite->atlas_id;
    s_actions.edit_sprite_source_id = sprite->source_id;
    s_actions.edit_sprite_revision = sprite->expected_revision;
    memcpy(s_actions.edit_sprite_source_key, sprite->source_key,
           strlen(sprite->source_key) + 1U);
    memcpy(s_edit_sprite, display_name, strlen(display_name) + 1U);
    memcpy(s_edit_buf, edit_value, strlen(edit_value) + 1U);
    set_status("Rename region: type, Enter to commit, Esc clears/cancels.");
}
void start_sprite_edit(const sprite_row *row) {
    if (!row || row->is_folder || row->missing || !row->sprite_name ||
        row->sprite_name[0] == '\0' || tp_id128_is_nil(row->source_id) ||
        !row->source_key || row->source_key[0] == '\0') {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                         : NULL;
    if (!atlas) {
        return;
    }
    const gui_sprite_ref sprite = {
        atlas->id, row->source_id, row->source_key,
        tp_session_snapshot_revision(snapshot)};
    start_sprite_edit_ref(&sprite, row->sprite_name);
}

bool gui_sprite_edit_matches(const sprite_row *row) {
    return row && s_edit_kind == EDIT_SPRITE &&
           row->source_key &&
           tp_id128_eq(s_actions.edit_sprite_source_id, row->source_id) &&
           strcmp(s_actions.edit_sprite_source_key, row->source_key) == 0;
}

bool gui_atlas_edit_matches(tp_id128 atlas_id) {
    return s_edit_kind == EDIT_ATLAS &&
           tp_id128_eq(s_actions.edit_atlas_id, atlas_id);
}

bool gui_animation_edit_matches(tp_id128 atlas_id, tp_id128 animation_id) {
    return s_edit_kind == EDIT_ANIM &&
           tp_id128_eq(s_actions.edit_anim_atlas_id, atlas_id) &&
           tp_id128_eq(s_actions.edit_anim_id, animation_id);
}

void clamp_selection(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    if (atlas_count == 0) {
        s_sel_atlas = 0;
        reset_selection();
        return;
    }
    if (s_sel_atlas >= atlas_count) {
        s_sel_atlas = atlas_count - 1;
    }
    if (s_sel_atlas < 0) {
        s_sel_atlas = 0;
    }
}
// #endregion

// #region animation + preview actions (ux.md §3.7b)
const tp_snapshot_animation *preview_animation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return s_preview_active && snapshot
               ? tp_session_snapshot_animation_by_id(
                     snapshot, s_actions.preview_animation_ref.atlas_id,
                     s_actions.preview_animation_ref.animation_id)
               : NULL;
}

static int snapshot_atlas_index_by_id(const tp_session_snapshot *snapshot,
                                      tp_id128 atlas_id) {
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; ++i) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, atlas_id)) {
            return i;
        }
    }
    return -1;
}

/* Copies the multi-selection into the shared buffers, natural-sorted; returns the count (0 on OOM,
 * with a status set -- the caller then does nothing, never truncates). */
static int build_sorted_selection(void) {
    const int n = s_multi_sel_count;
    if (!sel_sort_reserve(n)) { /* grow the sort scratch WITH the selection */
        set_status_ex(STATUS_ERROR, "Out of memory: could not sort the selection.");
        return 0;
    }
    for (int i = 0; i < n; i++) {
        s_sel_sort_buf[i] = s_multi_sel[i];
    }
    qsort(s_sel_sort_buf, (size_t)n, sizeof s_sel_sort_buf[0], nat_cmp_qsort);
    for (int i = 0; i < n; i++) {
        s_sel_sort_ptr[i] = s_sel_sort_buf[i].source_key;
        s_sel_sort_refs[i].source_id = s_sel_sort_buf[i].source_id;
        s_sel_sort_refs[i].src_key = s_sel_sort_buf[i].source_key;
    }
    return n;
}

static void pending_create_animation_dispose(pending_create_animation *request) {
    if (!request) {
        return;
    }
    free(request->name);
    gui_actions__frame_refs_dispose(request->frames, request->frame_count);
    memset(request, 0, sizeof *request);
}

void gui_request_create_animation_from_selection(void) {
    if (s_multi_sel_count <= 0) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        s_sel_atlas)
                                         : NULL;
    if (!atlas) {
        return;
    }
    const int frame_count = build_sorted_selection();
    if (frame_count <= 0) {
        return;
    }

    pending_create_animation request = {0};
    request.frames = gui_actions__frame_refs_copy(s_sel_sort_refs, frame_count);
    if (!request.frames) {
        set_status_ex(STATUS_ERROR,
                      "Out of memory: animation creation could not be queued.");
        return;
    }
    request.frame_count = frame_count;

    char base[192];
    tp_names_common_prefix(s_sel_sort_ptr, frame_count, base, sizeof base);
    request.name = gui_actions__strdup(base);
    if (!request.name) {
        pending_create_animation_dispose(&request);
        set_status_ex(STATUS_ERROR,
                      "Out of memory: animation creation could not be queued.");
        return;
    }
    request.active = true;
    request.atlas_id = atlas->id;
    request.expected_revision = tp_session_snapshot_revision(snapshot);

    pending_create_animation_dispose(&s_actions.pending_create_anim);
    s_actions.pending_create_anim = request;
}

void gui_request_open_preview(const gui_animation_ref *animation) {
    if (!animation || tp_id128_is_nil(animation->atlas_id) ||
        tp_id128_is_nil(animation->animation_id)) {
        return;
    }
    s_actions.pending_open_preview = true;
    s_actions.pending_open_preview_ref = *animation;
}

static bool resolve_animation_ref(const gui_animation_ref *animation,
                                  int *atlas_index, int *animation_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int ai = 0; ai < atlas_count; ai++) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, ai);
        if (!atlas || !tp_id128_eq(atlas->id, animation->atlas_id)) {
            continue;
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            const tp_snapshot_animation *candidate =
                tp_session_snapshot_animation_at(snapshot, atlas->id, i);
            if (candidate && tp_id128_eq(candidate->id,
                                         animation->animation_id)) {
                *atlas_index = ai;
                *animation_index = i;
                return true;
            }
        }
        return false;
    }
    return false;
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
    tp_names_common_prefix(s_sel_sort_ptr, n, base, sizeof base);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        s_sel_atlas)
                                         : NULL;
    const int idx = atlas ? gui_project_create_animation(
                                atlas->id, tp_session_snapshot_revision(snapshot),
                                base[0] ? base : NULL, s_sel_sort_refs, n)
                          : -1;
    if (idx >= 0) {
        s_sel_anim = idx;
        s_sel_anim_frame = -1;
        const tp_session_snapshot *after = gui_project_snapshot();
        const tp_snapshot_atlas *a = after ? tp_session_snapshot_atlas_at(after, s_sel_atlas) : NULL;
        const tp_snapshot_animation *created = a ? tp_session_snapshot_animation_at(after, a->id, idx) : NULL;
        set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                    created ? created->name : "?", n);
    }
    return idx;
}

/* Appends the current multi-selection (natural-sorted) as frames of animation `anim_index`.
 * DEFERRED: this is called from declare_animation_editor, which holds live
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
    gui_animation_ref animation;
    if (!gui_project_animation_ref_at(s_sel_atlas, anim_index, &animation)) {
        return;
    }
    gui_edit_anim_add_frames(&animation, s_sel_sort_refs, n);
    set_statusf("Adding %d frame(s) to the animation (Ctrl+Z to undo).", n); /* lands on the next drain */
}

/* Opens the preview player on animation `anim_index` (plays from the packed regions; if the atlas is
 * not packed yet, the canvas shows a "Pack to preview" hint). */
void open_preview(int anim_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
    const tp_snapshot_animation *animation = a ? tp_session_snapshot_animation_at(snapshot, a->id, anim_index) : NULL;
    if (!animation) {
        return;
    }
    cancel_edit();
    preview_target_reset(); /* the anim player owns the canvas -> never leave an export preview bound under it */
    s_sel_anim = anim_index;
    s_actions.preview_animation_ref = (gui_animation_ref){
        a->id, animation->id, tp_session_snapshot_revision(snapshot)};
    s_preview_active = true;
    s_preview_playing = true;
    s_preview_finished = false;
    s_preview_time = 0.0;
    if (!gui_pack_result(s_sel_atlas)) {
        set_status("Pack (Ctrl+P) to preview the animation on packed regions.");
    } else {
        set_statusf("Previewing '%s' \xE2\x80\x94 Space play/pause.", animation->name);
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

static bool preview_frames_reserve(int needed) {
    if (needed <= s_actions.preview_frames.capacity) {
        return true;
    }
    int capacity = s_actions.preview_frames.capacity
                       ? s_actions.preview_frames.capacity
                       : PREVIEW_IDXS_INIT_CAP;
    while (capacity < needed) {
        if (capacity > INT_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
#ifdef NTPACKER_GUI_SELFTEST
    s_actions.preview_frame_work.realloc_calls++;
#endif
    int *grown = realloc(s_actions.preview_frames.indices,
                         (size_t)capacity * sizeof *grown);
    if (!grown) {
        set_status_ex(STATUS_ERROR, "Out of memory: preview frames truncated.");
        return false;
    }
    s_actions.preview_frames.indices = grown;
    s_actions.preview_frames.capacity = capacity;
    return true;
}

static void preview_frames_rebuild(const tp_session_snapshot *snapshot,
                                   int atlas_index,
                                   const tp_snapshot_animation *animation,
                                   const tp_result *result,
                                   uint64_t result_version) {
#ifdef NTPACKER_GUI_SELFTEST
    s_actions.preview_frame_work.rebuilds++;
    s_actions.preview_frame_work.frame_span_lookups++;
#endif
    int frame_count = 0;
    const tp_snapshot_frame *frames = tp_session_snapshot_animation_frames(
        snapshot, s_actions.preview_animation_ref.atlas_id,
        s_actions.preview_animation_ref.animation_id, &frame_count);
    const bool complete = frame_count == animation->frame_count &&
                          preview_frames_reserve(frame_count);
    int count = 0;
    int ref_w = 1;
    int ref_h = 1;
    for (int i = 0; i < frame_count && count < s_actions.preview_frames.capacity; ++i) {
#ifdef NTPACKER_GUI_SELFTEST
        s_actions.preview_frame_work.frame_iterations++;
#endif
        const tp_snapshot_frame *frame = &frames[i];
        const int sprite_index = gui_pack_find_sprite_ref(
            atlas_index, frame->source_id, frame->source_key);
        if (sprite_index < 0 || sprite_index >= result->sprite_count) {
            continue;
        }
        s_actions.preview_frames.indices[count++] = sprite_index;
        if (result->sprites[sprite_index].sourceSize.w > ref_w) {
            ref_w = result->sprites[sprite_index].sourceSize.w;
        }
        if (result->sprites[sprite_index].sourceSize.h > ref_h) {
            ref_h = result->sprites[sprite_index].sourceSize.h;
        }
    }
    s_actions.preview_frames.atlas_id = s_actions.preview_animation_ref.atlas_id;
    s_actions.preview_frames.animation_id = s_actions.preview_animation_ref.animation_id;
    s_actions.preview_frames.model_generation =
        tp_session_snapshot_model_generation(snapshot);
    s_actions.preview_frames.pack_result_version = result_version;
    s_actions.preview_frames.count = count;
    s_actions.preview_frames.ref_w = ref_w;
    s_actions.preview_frames.ref_h = ref_h;
    s_actions.preview_frames.valid = complete;
}

/* Nudges the preview timeline by `delta` frame-ticks (pauses first). */
void preview_step(int delta) {
    if (!s_preview_active) {
        return;
    }
    const tp_snapshot_animation *an = preview_animation();
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
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_animation *an = snapshot
                                          ? tp_session_snapshot_animation_by_id(
                                                snapshot,
                                                s_actions.preview_animation_ref.atlas_id,
                                                s_actions.preview_animation_ref.animation_id)
                                          : NULL;
    const int atlas_index = snapshot_atlas_index_by_id(
        snapshot, s_actions.preview_animation_ref.atlas_id);
    const tp_result *pr = gui_pack_result(atlas_index);
    const uint64_t result_version = gui_pack_result_version(atlas_index);
    s_canvas.anim_sprite = -1;
    s_preview_frame_count = 0;
    if (!an || !pr || result_version == 0U) {
        return; /* declare_canvas draws the "Pack to preview" hint */
    }
    const uint64_t model_generation =
        tp_session_snapshot_model_generation(snapshot);
    if (!s_actions.preview_frames.valid ||
        !tp_id128_eq(s_actions.preview_frames.atlas_id,
                     s_actions.preview_animation_ref.atlas_id) ||
        !tp_id128_eq(s_actions.preview_frames.animation_id,
                     s_actions.preview_animation_ref.animation_id) ||
        s_actions.preview_frames.model_generation != model_generation ||
        s_actions.preview_frames.pack_result_version != result_version) {
        preview_frames_rebuild(snapshot, atlas_index, an, pr, result_version);
    }
    const int n = s_actions.preview_frames.count;
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
    s_canvas.anim_sprite = s_actions.preview_frames.indices[cur];
    s_canvas.anim_ref_w = s_actions.preview_frames.ref_w;
    s_canvas.anim_ref_h = s_actions.preview_frames.ref_h;
    s_canvas.anim_flip_h = an->flip_h;
    s_canvas.anim_flip_v = an->flip_v;
}
// #endregion

// #region file dialogs (tinyfiledialogs)
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
    char full[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_project_file(path, full, sizeof full)) {
        set_status_ex(STATUS_ERROR,
                      "Save path is invalid or exceeds the supported path limit.");
        return;
    }
    char err[256];
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
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
        char notice[256];
        if (gui_project_take_save_notice(notice, sizeof notice)) {
            set_statusf_ex(STATUS_WARNING, "%s", notice);
        } else {
            set_statusf("Saved %s", gui_project_display_name());
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

void gui_request_remove_animation(int animation_index) {
    gui_animation_ref animation;
    if (gui_project_animation_ref_at(s_sel_atlas, animation_index, &animation)) {
        gui_request_remove_animation_ref(&animation);
    }
}

void gui_request_remove_animation_ref(const gui_animation_ref *animation) {
    if (animation && !tp_id128_is_nil(animation->atlas_id) &&
        !tp_id128_is_nil(animation->animation_id)) {
        s_actions.pending_remove_anim = true;
        s_actions.pending_remove_anim_ref = *animation;
    }
}

void gui_request_remove_target(int target_index) {
    gui_target_ref target;
    if (gui_project_target_ref_at(s_sel_atlas, target_index, &target)) {
        gui_request_remove_target_ref(&target);
    }
}


void gui_request_remove_target_ref(const gui_target_ref *target) {
    if (target && !tp_id128_is_nil(target->atlas_id) &&
        !tp_id128_is_nil(target->target_id)) {
        s_actions.pending_remove_target = true;
        s_actions.pending_remove_target_ref = *target;
    }
}

static bool selected_atlas_intent(tp_id128 *atlas_id, int64_t *revision) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                         : NULL;
    if (!atlas) {
        return false;
    }
    *atlas_id = atlas->id;
    *revision = tp_session_snapshot_revision(snapshot);
    return true;
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
    int too_long = 0; /* a single path longer than the 1024-byte cap */
    int overflow = 0; /* more paths than the 32-path cap were selected */
    /* Collect the selected paths, then commit them as ONE transaction (H/P2-13): the old per-path
     * gui_project_add_source_kind loop made an N-file multi-select N undo steps + non-atomic (a mid-batch
     * failure left a partial add). tinyfd caps the list at 32 paths of <= 1024 bytes each. */
    char paths[32][1024];
    const char *ptrs[32];
    int n = 0;
    const char *start = res;
    for (;;) {
        const char *bar = strchr(start, '|');
        const size_t seg = bar ? (size_t)(bar - start) : strlen(start);
        if (seg > 0) {
            if (n >= (int)(sizeof paths / sizeof paths[0])) {
                overflow++; /* > 32 selected -> the tail is dropped (distinct from a too-long path) */
            } else if (seg >= sizeof paths[0]) {
                too_long++; /* one pathological over-long path */
            } else {
                memcpy(paths[n], start, seg);
                paths[n][seg] = '\0';
                normalize_slashes(paths[n]);
                ptrs[n] = paths[n];
                n++;
            }
        }
        if (!bar) {
            break;
        }
        start = bar + 1;
    }
    /* These come from the file-picker dialog: record the true kind. One atomic transaction, one undo step. */
    tp_id128 atlas_id;
    int64_t revision = 0;
    const bool ok = selected_atlas_intent(&atlas_id, &revision) &&
                    gui_project_add_sources(atlas_id, revision, ptrs, n,
                                            TP_SOURCE_KIND_FILE, &added, &dup);
    if (!ok) {
        /* the whole atomic batch was rejected (journal/disk-full, OOM, core) -- the reason is already on
         * the op-error channel; make THIS line an ERROR too, not a benign info-toned "Added 0". */
        set_status_ex(STATUS_ERROR, "Could not add the selected files.");
    } else if (too_long > 0 || overflow > 0) {
        set_statusf_ex(STATUS_WARNING,
                       "Added %d file source(s); %d skipped (%d too long, %d over the 32-file limit), %d already added",
                       added, too_long + overflow, too_long, overflow, dup);
    } else if (dup > 0) {
        set_statusf("Added %d file source(s); %d already added", added, dup);
    } else {
        set_statusf("Added %d file source(s)", added);
    }
}

static bool flush_failed(void); /* defined below; a discrete browse is a flush-first entry point */

/* Save dialog for a target's output path, relativized to the project like sources. Atlas-explicit so
 * the Export dialog (which spans all atlases) can browse any target, not just the selected atlas's. */
static void do_browse_target(const gui_target_ref *queued) {
    /* H/G3: commit any BUFFERED out-path gesture FIRST so the Save dialog seeds from the just-typed path,
     * not a stale committed one (clicking the "..." button is in-panel, so no blur gesture-commit fired).
     * Route through flush_failed() like every other flush-first entry: a journal-failed flush surfaces the
     * error and ABORTS -- never pop a Save dialog over a rejected-commit model. Re-fetch a/t AFTER the flush
     * (a committed flush clone-swaps the project). */
    if (flush_failed()) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot && queued
                                     ? tp_session_snapshot_atlas_by_id(snapshot,
                                                                        queued->atlas_id)
                                     : NULL;
    const tp_snapshot_target *t = a
                                      ? tp_session_snapshot_target_by_id(
                                            snapshot, a->id, queued->target_id)
                                      : NULL;
    if (!t) {
        return;
    }
    const gui_target_ref target = {
        .atlas_id = a->id,
        .target_id = t->id,
        .expected_revision = tp_session_snapshot_revision(snapshot),
    };
    const char *path = tinyfd_saveFileDialog("Export output path", t->out_path, 0, NULL, NULL);
    if (!path) {
        return;
    }
    char rel[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_relativize_to_project(path, gui_project_path(), rel,
                                         sizeof rel)) {
        set_status_ex(STATUS_ERROR,
                      "Output path is invalid or exceeds the supported path limit.");
        return;
    }
    /* Browse changes one field only. Commit the masked out-path operation as
     * this dialog's discrete gesture; never round-trip exporter/enabled through
     * frontend buffers where a long registered format id could be truncated. */
    if (gui_project_set_target_out_path(&target, rel) &&
        gui_project_flush_pending()) {
        set_statusf("Output path: %s", rel);
    }
}

void gui_request_browse_target(int atlas_index, int target_index) {
    gui_target_ref target;
    if (gui_project_target_ref_at(atlas_index, target_index, &target)) {
        s_actions.pending_browse_target = true;
        s_actions.pending_browse_target_ref = target;
    }
}

void gui_request_add_target(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (atlas) {
        s_actions.pending_add_target = true;
        s_actions.pending_add_target_atlas_id = atlas->id;
        s_actions.pending_add_target_revision = tp_session_snapshot_revision(snapshot);
    }
}

void gui_request_add_animation(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (atlas) {
        s_actions.pending_add_anim = true;
        s_actions.pending_add_anim_atlas_id = atlas->id;
        s_actions.pending_add_anim_revision = tp_session_snapshot_revision(snapshot);
    }
}

static void do_add_folder(void) {
    const char *dir = tinyfd_selectFolderDialog("Add Folder", "");
    if (!dir) {
        return;
    }
    char norm[TP_IDENTITY_PATH_MAX];
    if (!gui_paths_copy_normalized(dir, norm, sizeof norm)) {
        set_status_ex(STATUS_ERROR,
                      "Folder path is invalid or exceeds the supported path limit.");
        return;
    }
    tp_id128 atlas_id;
    int64_t revision = 0;
    const gui_add_status r = selected_atlas_intent(&atlas_id, &revision)
                                 ? gui_project_add_source(atlas_id, revision, norm)
                                 : GUI_ADD_FAILED;
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
        gui_project_discard_recovery_on_shutdown();
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
    char *abs;
    long long size;
    long long mtime;
} fp_entry;

static void fp_free(fp_entry *entries, int count) {
    for (int i = 0; i < count; ++i) {
        free(entries[i].abs);
    }
    free(entries);
}

static tp_status fp_push(fp_entry **arr, int *count, int *cap,
                         const char *abs, long long size, long long mtime,
                         tp_error *error) {
    if (*count == *cap) {
        if (*cap > INT_MAX / 2) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint has too many entries");
        }
        int nc = *cap ? *cap * 2 : 64;
        if ((size_t)nc > SIZE_MAX / sizeof **arr) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint table overflows size_t");
        }
        fp_entry *ne =
            (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
        if (!ne) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "refresh fingerprint allocation failed");
        }
        *arr = ne;
        *cap = nc;
    }
    char *path = gui_actions__strdup(abs);
    if (!path) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "refresh fingerprint path allocation failed");
    }
    (*arr)[*count] = (fp_entry){path, size, mtime};
    (*count)++;
    return TP_STATUS_OK;
}

static tp_status fp_collect(fp_entry **arr, int *count, int *cap,
                            tp_error *error) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int ai = 0; ai < atlas_count; ai++) {
        const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snapshot, ai);
        for (int si = 0; si < a->source_count; si++) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(snapshot, a->id, si);
            char abs[TP_IDENTITY_PATH_MAX];
            if (!source || tp_session_snapshot_resolve_path(snapshot, a->id, source->id,
                                                            abs, sizeof abs, error) != TP_STATUS_OK) {
                continue;
            }
            if (gui_scan_is_dir(abs)) {
                const gui_scan_result *sc = NULL;
                tp_status scan_status = gui_scan_get(abs, &sc, error);
                if (scan_status != TP_STATUS_OK) {
                    return scan_status;
                }
                for (int ci = 0; ci < sc->count; ci++) {
                    tp_status push_status = fp_push(
                        arr, count, cap, sc->entries[ci].abs,
                        sc->entries[ci].size, sc->entries[ci].mtime, error);
                    if (push_status != TP_STATUS_OK) {
                        return push_status;
                    }
                }
            } else {
                long long sz = -1;
                long long mt = -1;
                (void)gui_scan_stat(abs, &sz, &mt);
                tp_status push_status = fp_push(arr, count, cap, abs, sz, mt,
                                                error);
                if (push_status != TP_STATUS_OK) {
                    return push_status;
                }
            }
        }
    }
    return TP_STATUS_OK;
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
    tp_error error = {0};
    tp_status status = fp_collect(&before, &bn, &bc, &error);
    if (status != TP_STATUS_OK) {
        fp_free(before, bn);
        set_statusf_ex(STATUS_ERROR, "Refresh failed: %s", error.msg);
        return;
    }

    gui_project_invalidate_sources(); /* publish the external runtime refresh */

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    status = fp_collect(&after, &an, &ac, &error);
    if (status != TP_STATUS_OK) {
        fp_free(before, bn);
        fp_free(after, an);
        set_statusf_ex(STATUS_ERROR, "Refresh failed: %s", error.msg);
        return;
    }

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
    fp_free(before, bn);
    fp_free(after, an);

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed, changed);
}

/* Ctrl+P / Pack: start the selected atlas's typed session Pack job. On success clear the
 * preview-stale bit and upload the packed pages to the canvas (atlas-page view); on failure the
 * previous result + the "outdated" tag stay (ux.md §3.3b). */
/* Blocking pack of the selected atlas (deterministic path for selftest + --shot). Interactive
 * Pack starts the same session job and polls its typed result at frame boundaries. */
void do_pack_blocking(void) {
    if (flush_failed()) {
        return; /* fix2 [1]: a journal-failed flush dropped the edit -> abort (never pack a stale model + report success) */
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
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
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
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
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
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
        set_statusf_ex(STATUS_INFO, "Preview: %s\xE2\x80\xA6", e->display_name ? e->display_name : e->id);
    } else {
        preview_target_reset();
        set_statusf_ex(STATUS_ERROR, "Preview failed: %s", err);
    }
}

/* Per-frame reconciliation: a model edit since the preview packed makes it stale -> drop to Native (never
 * show a silently-wrong preview). Atlas switch / undo / redo / open / new drop it via reset_selection. */
static void preview_target_sync(void) {
    if (s_preview_target != 0 && !gui_pack_async_busy() &&
        !gui_pack_preview_result(s_sel_atlas)) {
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
            /* The core-captured immutable input token covers both committed
             * model state and source-runtime refreshes. */
            if (!info.input_changed) {
                gui_project_mark_packed();
            }
            s_last_pack_ms = info.ms;
            s_last_pack_atlas = info.atlas_index;
            const tp_result *r = gui_pack_result(info.atlas_index);
            const char *stale = info.input_changed ? " -- inputs changed, stale" : "";
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
            if (info.input_changed) {
                preview_target_reset();
                set_status_ex(STATUS_WARNING,
                              "Preview inputs changed -- run Preview again.");
            } else if (s_preview_target == 0) {
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
    /* Surface a pending transaction REJECT (core rejected a mutator's op -- out-of-range value
     * or bad reference): the model was left byte-unchanged, so report it as a soft error.
     * In practice the widgets clamp valid ranges, so this rarely fires. */
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
    const gui_sprite_ref sprite = {s_actions.edit_sprite_atlas_id, s_actions.edit_sprite_source_id,
                                   s_actions.edit_sprite_source_key, s_actions.edit_sprite_revision};
    if (gui_project_set_sprite_rename(&sprite, s_edit_buf)) {
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
 * no-op, so its return is DOMAIN-ONLY. The anim branch also routes through an op
 * (set_anim_id builds TP_OP_ANIMATION_RENAME -> commit_txn_now), so a false there is a core reject whose
 * structured message rides the op-error channel -- surfaced directly, no anim_id_exists heuristic (fix3's
 * heuristic was wrong: it matched the anim's own unchanged name). set_atlas_name / set_sprite_rename /
 * set_anim_id journal their rename op; on that op's OWN append failure they return false -> no success
 * message + the op-error surfaces (not a false success, not a wrong "already exists"). */
static void commit_active_edit(bool force) {
    if (s_edit_kind == EDIT_NONE) {
        return; /* nothing being edited */
    }
    const tp_session_snapshot *before_flush = gui_project_snapshot();
    const int64_t revision_before_flush = before_flush
        ? tp_session_snapshot_revision(before_flush)
        : 0;
    const bool rebase_atlas = s_edit_kind == EDIT_ATLAS &&
                              s_actions.edit_atlas_revision == revision_before_flush;
    const bool rebase_sprite = s_edit_kind == EDIT_SPRITE &&
                               s_actions.edit_sprite_revision == revision_before_flush;
    const bool rebase_anim = s_edit_kind == EDIT_ANIM &&
                             s_actions.edit_anim_revision == revision_before_flush;
    if (flush_failed()) {
        /* a buffered gesture could not be journaled -> abort the rename commit (error already surfaced) */
        if (force) {
            cancel_edit();
        }
        return;
    }
    const tp_session_snapshot *after_flush = gui_project_snapshot();
    const int64_t revision_after_flush = after_flush
        ? tp_session_snapshot_revision(after_flush)
        : revision_before_flush;
    if (revision_after_flush != revision_before_flush) {
        if (rebase_atlas) {
            s_actions.edit_atlas_revision = revision_after_flush;
        } else if (rebase_sprite) {
            s_actions.edit_sprite_revision = revision_after_flush;
        } else if (rebase_anim) {
            s_actions.edit_anim_revision = revision_after_flush;
        }
    }
    if (s_edit_kind == EDIT_ATLAS) {
        const tp_session_snapshot *snapshot = gui_project_snapshot();
        if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, s_actions.edit_atlas_id)) {
            set_status_ex(STATUS_WARNING, "The atlas changed before the rename could be applied.");
            cancel_edit();
            return;
        }
        if (!gui_project_set_atlas_name(s_actions.edit_atlas_id, s_actions.edit_atlas_revision,
                                        s_edit_buf)) {
            char atlas_err[256];
            if (gui_project_take_op_error(atlas_err, sizeof atlas_err)) {
                set_status_ex(STATUS_WARNING, atlas_err);
            } else {
                set_status_ex(STATUS_ERROR, "Could not rename the atlas.");
            }
            if (force) {
                cancel_edit();
            }
            return;
        }
        char committed_name[TP_SRCKEY_MAX];
        tp_error read_error = {0};
        if (gui_project_copy_atlas_name(s_actions.edit_atlas_id, committed_name,
                                        sizeof committed_name, &read_error) == TP_STATUS_OK) {
            set_statusf("Renamed atlas to '%s'", committed_name);
        } else {
            set_status_ex(STATUS_ERROR,
                          read_error.msg[0] ? read_error.msg : "Renamed atlas could not be read back.");
        }
        cancel_edit();
    } else if (s_edit_kind == EDIT_SPRITE) {
        commit_sprite_rename();
    } else if (s_edit_kind == EDIT_ANIM) {
        const gui_animation_ref animation = {
            s_actions.edit_anim_atlas_id, s_actions.edit_anim_id, s_actions.edit_anim_revision};
        if (!gui_project_set_anim_id(&animation, s_edit_buf)) {
            /* Animation rename is a first-class op now (undoable + journaled), so a false is a
             * core REJECT -- a name collision (validate enforces uniqueness) or a rare OOM/stale-index
             * failure. The structured reject rides commit_txn_now's op-error channel, so surface it
             * DIRECTLY instead of re-deriving the reason with the old anim_id_exists heuristic (which
             * could not tell the anim's own unchanged name from a real clash). The entry flush_failed()
             * already handled the journal-fail case, so the op-error here is the domain reject. */
            char anim_err[256];
            if (gui_project_take_op_error(anim_err, sizeof anim_err)) {
                set_status_ex(STATUS_WARNING, anim_err);
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

// #region R6b startup crash-recovery modal glue
void gui_actions_open_recovery(const gui_recovery_list *list) {
    if (!list || list->count == 0U) {
        s_recovery_open = false;
        return;
    }
    s_actions.recovery_list = *list; /* value copy of the fixed-size struct */
    s_actions.recovery_pending_row = -1;
    s_recovery_open = true;
}
int gui_actions_recovery_count(void) { return (int)s_actions.recovery_list.count; }
bool gui_actions_recovery_has_more(void) { return s_actions.recovery_list.has_more; }
const gui_recovery_entry *gui_actions_recovery_at(int i) {
    if (i < 0 || (size_t)i >= s_actions.recovery_list.count) {
        return NULL;
    }
    return &s_actions.recovery_list.items[i];
}
void gui_actions_recovery_request(int row, int action) {
    s_actions.recovery_pending_row = row;
    s_actions.recovery_pending_action = action;
}
/* Drop a resolved row (shift-down); close the modal when the list empties. */
static void recovery_remove_row(int row) {
    if (row < 0 || (size_t)row >= s_actions.recovery_list.count) {
        return;
    }
    for (size_t i = (size_t)row; i + 1U < s_actions.recovery_list.count; ++i) {
        s_actions.recovery_list.items[i] = s_actions.recovery_list.items[i + 1];
    }
    s_actions.recovery_list.count--;
    if (s_actions.recovery_list.count == 0U && !s_actions.recovery_list.has_more) {
        s_recovery_open = false;
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
     * (UAF fix). */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit_enter) {
        commit_active_edit(false);
    }
    s_pending_commit_edit_enter = false;

    /* Drain the deferred model-edit queue (settings / overrides / anim knobs / target edits the
     * declare fns enqueued last frame). Runs here, at frame top, with no live declare-fn pointer
     * held -- so the per-edit clone-swap can never dangle a panel's cached atlas/sprite/anim/target
     * pointer (decision 0015). */
    gui_actions__drain_edits();

    /* A gesture ended last frame (slider release / field Enter+blur / discrete pick): commit the
     * buffered transaction NOW that gui_actions__drain_edits has folded in this frame's final value, so one
     * interaction == one undo step (decision 0015). */
    if (s_actions.gesture_commit) {
        /* fix2: the bool is intentionally IGNORED -- this is the gesture-BOUNDARY commit (one interaction
         * = one undo step). A journal-failed flush here drops the gesture WITH the op-error surfaced
         * (poll_async shows it); there is no persist/discard "proceed as clean" decision after it to
         * abort (unlike save/new/pack). Audited fix2 [0]/[1]. */
        (void)gui_project_flush_pending();
        s_actions.gesture_commit = false;
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

    /* R6b: harvest a per-row recovery decision requested last frame. Runs here (same spot the confirm
     * harvest lands do_save()->tinyfd_*) so the Save-As dialog + disk-mutating resolve run outside
     * nt_ui_begin/end. NON-DESTRUCTIVE ON FAILURE: a failed save leaves the journal + the row for a retry. */
    if (s_actions.recovery_pending_row >= 0) {
        const int row = s_actions.recovery_pending_row;
        const int action = s_actions.recovery_pending_action;
        s_actions.recovery_pending_row = -1;
        const gui_recovery_entry *e = gui_actions_recovery_at(row);
        if (e != NULL) {
            /* Copy the typed row before the list may compact after resolution. */
            gui_recovery_entry entry = *e;
            char nm[256];
            (void)snprintf(nm, sizeof nm, "%s", e->name);
            const char *target = "";
            bool proceed = true;
            if (action == GUI_RECOVERY_DISCARD) {
                char prompt[GUI_RECOVERY_PATH_CAP + 320];
                (void)snprintf(prompt, sizeof prompt,
                               "Permanently discard recovered unsaved work for '%s'?\n\n%s\n\n"
                               "This cannot be undone.",
                               entry.name, entry.original_path[0] ? entry.original_path : "Untitled project");
                proceed = tinyfd_messageBox("Discard recovered work?", prompt, "yesno", "warning", 0) == 1;
            } else if (action == GUI_RECOVERY_SAVE_AS) {
                static const char *filt[] = {"*.ntpacker_project"};
                char recovered_default[GUI_RECOVERY_PATH_CAP + 32];
                const char *def = "recovered.ntpacker_project";
                if (entry.original_path[0] != '\0') {
                    static const char suffix[] = ".ntpacker_project";
                    const size_t path_len = strlen(entry.original_path);
                    const size_t suffix_len = sizeof suffix - 1u;
                    if (path_len >= suffix_len && strcmp(entry.original_path + path_len - suffix_len, suffix) == 0) {
                        (void)snprintf(recovered_default, sizeof recovered_default, "%.*s.recovered%s",
                                       (int)(path_len - suffix_len), entry.original_path, suffix);
                    } else {
                        (void)snprintf(recovered_default, sizeof recovered_default, "%s.recovered%s",
                                       entry.original_path, suffix);
                    }
                    def = recovered_default;
                }
                const char *picked = tinyfd_saveFileDialog("Save Recovered Project As", def, 1, filt, "ntpacker project");
                if (picked == NULL) {
                    proceed = false; /* cancelled -> keep the row, journal stays on disk */
                } else {
                    target = picked;
                }
            }
            if (proceed) {
                char err[256];
                tp_status st = gui_recovery_resolve_entry(&entry, (gui_recovery_action)action, target, err, sizeof err);
                if (st == TP_STATUS_OK) {
                    recovery_remove_row(row);
                    if (action == GUI_RECOVERY_DISCARD) {
                        set_statusf("Discarded recovered '%s'.", nm);
                    } else {
                        set_statusf("Recovered '%s' saved.", nm);
                    }
                } else {
                    /* NON-DESTRUCTIVE: the journal is still on disk; keep the row so the user can retry. */
                    set_statusf_ex(STATUS_ERROR, "Recover '%s' failed: %s", nm, err);
                }
            }
        }
    }

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
            const tp_snapshot_atlas *added = tp_session_snapshot_atlas_at(gui_project_snapshot(), idx);
            set_statusf("Added atlas '%s'", added ? added->name : "?");
        }
    }
    if (s_pending_remove_source) {
        /* fix3 [0]: side-effects + "Removed" run ONLY on a real removal -- a journal-failed flush
         * aborts the wrapper (returns false), so no false "Removed" / bad Ctrl+Z (op-error surfaced). */
        if (gui_project_remove_source(s_pending_remove_source_atlas_id,
                                      s_pending_remove_source_id,
                                      s_pending_remove_source_revision)) {
            reset_selection();
            set_status("Removed source (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_atlas) {
        if (gui_project_remove_atlas(s_pending_remove_atlas_id,
                                     s_pending_remove_atlas_revision)) {
            clamp_selection();
            reset_selection();
            set_status("Removed atlas (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_add_target) {
        const int ti = gui_project_add_target(s_actions.pending_add_target_atlas_id,
                                              s_actions.pending_add_target_revision);
        if (ti >= 0) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_remove_target) {
        if (gui_project_remove_target(&s_actions.pending_remove_target_ref)) {
            set_status("Removed export target (Ctrl+Z to undo).");
        }
    }
    if (s_actions.pending_browse_target) {
        do_browse_target(&s_actions.pending_browse_target_ref);
    }
    if (s_actions.pending_add_anim) {
        const int idx = gui_project_create_animation(
            s_actions.pending_add_anim_atlas_id, s_actions.pending_add_anim_revision,
            NULL, NULL, 0);
        if (idx >= 0) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_add_anim_atlas_id)
                : NULL;
            const tp_snapshot_animation *animation = after_atlas
                                                         ? tp_session_snapshot_animation_at(after_snapshot, after_atlas->id, idx)
                                                         : NULL;
            const tp_snapshot_atlas *selected = after_snapshot
                ? tp_session_snapshot_atlas_at(after_snapshot, s_sel_atlas)
                : NULL;
            if (selected && tp_id128_eq(selected->id, s_actions.pending_add_anim_atlas_id)) {
                s_sel_anim = idx;
                s_sel_anim_frame = -1;
            }
            set_statusf("Added animation '%s' (Ctrl+Z to undo).", animation ? animation->name : "?");
        }
    }
    if (s_actions.pending_create_anim.active) {
        const int idx = gui_project_create_animation(
            s_actions.pending_create_anim.atlas_id,
            s_actions.pending_create_anim.expected_revision,
            s_actions.pending_create_anim.name[0] ? s_actions.pending_create_anim.name : NULL,
            s_actions.pending_create_anim.frames,
            s_actions.pending_create_anim.frame_count);
        if (idx >= 0) {
            const tp_session_snapshot *after_snapshot = gui_project_snapshot();
            const tp_snapshot_atlas *after_atlas = after_snapshot
                ? tp_session_snapshot_atlas_by_id(
                      after_snapshot, s_actions.pending_create_anim.atlas_id)
                : NULL;
            const tp_snapshot_animation *animation = after_atlas
                ? tp_session_snapshot_animation_at(after_snapshot,
                                                   after_atlas->id, idx)
                : NULL;
            const tp_snapshot_atlas *selected = after_snapshot
                ? tp_session_snapshot_atlas_at(after_snapshot, s_sel_atlas)
                : NULL;
            if (selected &&
                tp_id128_eq(selected->id, s_actions.pending_create_anim.atlas_id)) {
                s_sel_anim = idx;
                s_sel_anim_frame = -1;
            }
            set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                        animation ? animation->name : "?",
                        s_actions.pending_create_anim.frame_count);
        }
    }
    pending_create_animation_dispose(&s_actions.pending_create_anim);
    if (s_actions.pending_remove_anim) {
            /* fix3 [0]: preview_stop + the selection reset + "Removed" run ONLY on a real removal. A
             * journal-failed flush aborts the wrapper (returns false) -> the animation is still there,
             * so we must NOT stop its preview or clear the selection. (preview_stop only resets flags,
             * so running it AFTER the removal is safe -- no project deref.) */
            const bool was_previewing =
                s_preview_active &&
                tp_id128_eq(s_actions.preview_animation_ref.atlas_id,
                            s_actions.pending_remove_anim_ref.atlas_id) &&
                tp_id128_eq(s_actions.preview_animation_ref.animation_id,
                            s_actions.pending_remove_anim_ref.animation_id);
            if (gui_project_remove_animation(&s_actions.pending_remove_anim_ref)) {
                if (was_previewing) {
                    preview_stop();
                }
                s_sel_anim = -1;
                s_sel_anim_frame = -1;
                set_status("Removed animation (Ctrl+Z to undo).");
            }
    }
    if (s_actions.pending_open_preview) {
        int atlas_index = -1;
        int animation_index = -1;
        if (resolve_animation_ref(&s_actions.pending_open_preview_ref, &atlas_index,
                                  &animation_index)) {
            s_sel_atlas = atlas_index;
            open_preview(animation_index);
        }
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
    s_actions.pending_add_target = false;
    s_actions.pending_add_target_atlas_id = tp_id128_nil();
    s_actions.pending_add_target_revision = 0;
    s_actions.pending_add_anim = false;
    s_actions.pending_open_preview = false;
    memset(&s_actions.pending_open_preview_ref, 0,
           sizeof s_actions.pending_open_preview_ref);
    s_actions.pending_add_anim_atlas_id = tp_id128_nil();
    s_actions.pending_add_anim_revision = 0;
    s_pending_remove_source = false;
    s_pending_remove_source_atlas_id = tp_id128_nil();
    s_pending_remove_source_id = tp_id128_nil();
    s_pending_remove_source_revision = 0;
    s_pending_remove_atlas = false;
    s_pending_remove_atlas_id = tp_id128_nil();
    s_pending_remove_atlas_revision = 0;
    s_actions.pending_remove_target = false;
    s_actions.pending_remove_anim = false;
    s_actions.pending_browse_target = false;
    memset(&s_actions.pending_browse_target_ref, 0,
           sizeof s_actions.pending_browse_target_ref);
    s_pending_preview_target = -1;
}
// #endregion
