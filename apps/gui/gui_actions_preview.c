#include "gui_actions_internal.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_canvas.h"
#include "gui_pack.h"

#include "tp_core/tp_names.h"

#include "app/nt_app.h"
/* Initial capacity of the grow-only active-preview frame map. */
#define PREVIEW_IDXS_INIT_CAP 512

#ifdef NTPACKER_GUI_SELFTEST
void gui_preview_frame_work_reset(void) {
    memset(&s_actions.preview_frame_work, 0, sizeof s_actions.preview_frame_work);
}
gui_preview_frame_work gui_preview_frame_work_get(void) {
    return s_actions.preview_frame_work;
}
#endif

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

void gui_actions__pending_create_animation_dispose(
    pending_create_animation *request) {
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
        gui_actions__pending_create_animation_dispose(&request);
        set_status_ex(STATUS_ERROR,
                      "Out of memory: animation creation could not be queued.");
        return;
    }
    request.active = true;
    request.atlas_id = atlas->id;
    request.expected_revision = tp_session_snapshot_revision(snapshot);

    gui_actions__pending_create_animation_dispose(&s_actions.pending_create_anim);
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

bool gui_actions__resolve_animation_ref(const gui_animation_ref *animation,
                                        int *atlas_index,
                                        int *animation_index) {
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
