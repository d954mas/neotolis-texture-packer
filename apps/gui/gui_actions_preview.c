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
    s_actions.preview_had_result = false;
    s_preview_playing = false;
    s_preview_finished = false;
    s_preview_time = 0.0;
    s_canvas.anim_sprite = -1;
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ANIM) {
        s_canvas.mode = gui_canvas_has_atlas(&s_canvas) ? GUI_CANVAS_ATLAS : GUI_CANVAS_SOURCE;
    }
}

void reset_selection(void) {
    gui_rows_select_primary(NULL);
    gui_rows_set_focus_view_index(-1);
    gui_rows_set_anchor_view_index(-1);
    multi_sel_clear();
    gui_view_select_animation(tp_id128_nil());
    preview_stop();
    preview_target_reset(); /* export-target preview is bound to the selection/atlas -> drop it on any reset */
}

void cancel_edit(void) {
    gui_draft_discard();
}

void start_atlas_edit_ref(tp_id128 atlas_id, int64_t expected_revision) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
        ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
        : NULL;
    if (!atlas) {
        return;
    }
    if (gui_text_edit_begin_atlas_name(
            atlas->id, expected_revision, atlas->name)) {
        set_status("Rename atlas: type, Enter to commit, Esc to discard.");
    }
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
    if (gui_text_edit_begin_animation_name(
            ref, animation->name)) {
        set_status("Rename animation: type, Enter to commit, Esc to discard.");
    }
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
    if (gui_text_edit_begin_sprite_rename(
            sprite, edit_value)) {
        set_status("Rename region: type, Enter to commit, Esc to discard.");
    }
}
void start_sprite_edit(const sprite_row *row) {
    if (!row || row->is_folder || row->missing || !row->sprite_name ||
        row->sprite_name[0] == '\0' || tp_id128_is_nil(row->source_id) ||
        !row->source_key || row->source_key[0] == '\0') {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_index = gui_view_atlas_index(snapshot);
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, atlas_index)
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
    const gui_draft_owner *draft = &s_actions.draft;
    return row && draft->family == GUI_DRAFT_TEXT &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           draft->text.kind == GUI_TEXT_EDIT_SPRITE_RENAME &&
           row->source_key &&
           tp_id128_eq(draft->text.source_id, row->source_id) &&
           strcmp(draft->text.source_key, row->source_key) == 0;
}

bool gui_atlas_edit_matches(tp_id128 atlas_id) {
    const gui_draft_owner *draft = &s_actions.draft;
    return draft->family == GUI_DRAFT_TEXT &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           draft->text.kind == GUI_TEXT_EDIT_ATLAS_NAME &&
           tp_id128_eq(draft->lifecycle.target_id, atlas_id);
}

bool gui_animation_edit_matches(tp_id128 atlas_id, tp_id128 animation_id) {
    const gui_draft_owner *draft = &s_actions.draft;
    return draft->family == GUI_DRAFT_TEXT &&
           draft->lifecycle.phase != GUI_EDIT_IDLE &&
           draft->text.kind == GUI_TEXT_EDIT_ANIMATION_NAME &&
           tp_id128_eq(draft->text.atlas_id, atlas_id) &&
           tp_id128_eq(draft->lifecycle.target_id, animation_id);
}

// #endregion

// #region animation + preview actions
const tp_snapshot_animation *preview_animation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    return s_preview_active && snapshot
               ? tp_session_snapshot_animation_by_id(
                     snapshot, s_actions.preview_animation_ref.atlas_id,
                     s_actions.preview_animation_ref.animation_id)
               : NULL;
}

/* Index of the atlas carrying `atlas_id` in the snapshot, or -1. Re-resolves the
 * viewed atlas after an undo/redo shifts atlas ordering, and for the preview player. */
static int gui_actions__snapshot_atlas_index_by_id(const tp_session_snapshot *snapshot,
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

void gui_request_create_animation_from_selection(void) {
    if (s_multi_sel_count <= 0) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_index = gui_view_atlas_index(snapshot);
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    if (!atlas) {
        return;
    }
    const int frame_count = build_sorted_selection();
    if (frame_count <= 0) {
        return;
    }

    gui_intent intent = {.kind = GUI_INTENT_CREATE_ANIMATION};
    intent.payload.create_animation.frames =
        gui_actions__frame_refs_copy(s_sel_sort_refs, frame_count);
    if (!intent.payload.create_animation.frames) {
        set_status_ex(STATUS_ERROR,
                      "Out of memory: animation creation could not be queued.");
        return;
    }
    intent.payload.create_animation.frame_count = frame_count;

    char base[192];
    tp_names_common_prefix(s_sel_sort_ptr, frame_count, base, sizeof base);
    intent.payload.create_animation.name = gui_actions__strdup(base);
    if (!intent.payload.create_animation.name) {
        gui_actions__frame_refs_dispose(intent.payload.create_animation.frames,
                                        frame_count);
        set_status_ex(STATUS_ERROR,
                      "Out of memory: animation creation could not be queued.");
        return;
    }
    intent.payload.create_animation.atlas_id = atlas->id;
    intent.payload.create_animation.expected_revision =
        tp_session_snapshot_revision(snapshot);
    (void)gui_actions__intent_push(&intent);
}

void gui_request_open_preview(const gui_animation_ref *animation) {
    if (!animation || tp_id128_is_nil(animation->atlas_id) ||
        tp_id128_is_nil(animation->animation_id)) {
        return;
    }
    const gui_intent intent = {.kind = GUI_INTENT_OPEN_PREVIEW,
                               .payload.animation = *animation};
    (void)gui_actions__intent_push(&intent);
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
    const int atlas_index = gui_view_atlas_index(snapshot);
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    const gui_project_create_result created =
        atlas
            ? gui_project_create_animation(
                  atlas->id,
                  tp_session_snapshot_revision(snapshot),
                  base[0] ? base : NULL,
                  s_sel_sort_refs, n)
            : (gui_project_create_result){
                  .visible_index = -1,
              };
    if (created.committed) {
        gui_view_select_animation(created.created_id);
        const tp_session_snapshot *after = gui_project_snapshot();
        const tp_snapshot_atlas *a = after
            ? tp_session_snapshot_atlas_by_id(
                  after, gui_view_atlas_id())
            : NULL;
        const tp_snapshot_animation *created_animation =
            a
                ? tp_session_snapshot_animation_by_id(
                      after, a->id, created.created_id)
                : NULL;
        set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                    created_animation
                        ? created_animation->name
                        : "?",
                    n);
    }
    return created.visible_index;
}

/* Appends the current multi-selection (natural-sorted) as frames of one stable animation.
 * DEFERRED: this is called from declare_animation_editor, which holds live
 * `a`/`an` pointers it keeps dereferencing AFTER this returns. A synchronous commit here would
 * clone-swap + free the project under those pointers -> use-after-free on a plain "Add frames"
 * click. So it builds the sorted selection (read-only) and ENQUEUES an add-frames edit carrying
 * COPIED keys; apply_pending drains it next frame with no live pointer held (benign one-frame
 * lag, consistent with every other panel edit). */
void add_selection_frames_to_animation(
    const gui_animation_ref *animation) {
    if (!animation || tp_id128_is_nil(animation->atlas_id) ||
        tp_id128_is_nil(animation->animation_id) ||
        s_multi_sel_count <= 0) {
        return;
    }
    const int n = build_sorted_selection();
    if (n <= 0) {
        return; /* OOM in the sort scratch (status already set) -- do nothing rather than truncate */
    }
    gui_edit_anim_add_frames(animation, s_sel_sort_refs, n);
    set_statusf("Adding %d frame(s) to the animation (Ctrl+Z to undo).", n); /* lands on the next drain */
}

/* Opens the preview player on a stable animation ref (plays from the packed regions; if the atlas is
 * not packed yet, the canvas shows a "Pack to preview" hint). */
void open_preview_ref(const gui_animation_ref *ref) {
    if (!ref || tp_id128_is_nil(ref->atlas_id) ||
        tp_id128_is_nil(ref->animation_id)) {
        return;
    }
    /* Opening the preview is an outer action, so the active draft is SUBMITTED
     * first and the action continues only on a terminal success -- it must never
     * silently discard the user's edit (same ordering class as
     * gui_actions__browse_target). The snapshot is read after the submit. */
    if (!gui_actions__submit_draft()) {
        return;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_index =
        gui_actions__snapshot_atlas_index_by_id(
            snapshot, ref->atlas_id);
    const tp_snapshot_animation *animation =
        snapshot
            ? tp_session_snapshot_animation_by_id(
                  snapshot, ref->atlas_id,
                  ref->animation_id)
            : NULL;
    if (atlas_index < 0 || !animation) {
        return;
    }
    preview_target_reset(); /* the anim player owns the canvas -> never leave an export preview bound under it */
    gui_view_select_atlas(ref->atlas_id);
    gui_view_select_animation(ref->animation_id);
    s_actions.preview_animation_ref = (gui_animation_ref){
        ref->atlas_id, ref->animation_id,
        tp_session_snapshot_revision(snapshot)};
    s_preview_active = true;
    s_preview_playing = true;
    s_preview_finished = false;
    s_preview_time = 0.0;
    /* A fresh arming has never seen a result yet, so a not-yet-packed atlas keeps
     * showing the hint instead of being reported as a released result. */
    s_actions.preview_had_result = false;
    if (!gui_pack_result(ref->atlas_id)) {
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

void gui_actions__preview_shutdown(void) {
    /* Freeing the frame map while the player stays ARMED leaves preview state
     * (active flag, animation ref, had-result) pointing at a session that is
     * about to go away -- harmless at process exit, but it bleeds straight into
     * the next case whenever the shutdown is a teardown. preview_stop reads only
     * s_actions/s_canvas (and merely NULL-tests s_canvas.result), so it is safe
     * ahead of the pack/session shutdowns that follow this one, and it is a pure
     * assignment sequence, so calling it twice costs nothing. */
    preview_stop();
    free(s_actions.preview_frames.indices);
    memset(&s_actions.preview_frames, 0, sizeof s_actions.preview_frames);
}

static void preview_frames_rebuild(const tp_session_snapshot *snapshot,
                                   int atlas_index,
                                   const tp_snapshot_animation *animation,
                                   const tp_result *result,
                                   uint64_t result_version) {
#ifdef NTPACKER_GUI_SELFTEST
    s_actions.preview_frame_work.rebuilds++;
#endif
    /* The canonical index this rebuild resolves through belongs to the RESIDENT
     * atlas, and gui_pack builds it as part of becoming resident. When that build
     * fails on OOM the entry stays cached on purpose, so the index is RETRIED --
     * but every lookup until then returns -1. Resolving the frames anyway would
     * produce an EMPTY map and cache it as valid, which turns "retried" into
     * "never again until the version or model generation changes". So the missing
     * index ends the rebuild here, with the map left invalid: nothing is cached
     * and the next frame tries again.
     *
     * This is the one residency-taking read of the rebuild, and it costs nothing
     * extra: the per-frame lookups below each take the same read, and
     * update_preview only reaches a rebuild for the atlas the canvas is bound to,
     * so it is the resident fast path -- never the residency flip S21 removed
     * from the every-frame path. `result` (the caller's peek of THIS atlas) is
     * unaffected either way: making an atlas resident cannot evict itself. */
    const tp_id128 atlas_id =
        s_actions.preview_animation_ref.atlas_id;
    if (!gui_pack_result(atlas_id)) {
        s_actions.preview_frames.count = 0;
        s_actions.preview_frames.valid = false;
        return;
    }
#ifdef NTPACKER_GUI_SELFTEST
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
            atlas_id, frame->source_id, frame->source_key);
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
    /* Zero resolved frames is cacheable from here: the index was available, so
     * "no packed region for these frames" is a real model/pack answer, not a
     * missing lookup table, and re-deriving it every frame is exactly the work
     * this map exists to avoid. */
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
    const int atlas_index = gui_actions__snapshot_atlas_index_by_id(
        snapshot, s_actions.preview_animation_ref.atlas_id);
    /* PEEK, not a residency-taking read: the atlas the canvas shows owns the one
     * active pin, and the player only observes. Reading it the mutating way made
     * every frame of a preview whose atlas is not the viewed one flip residency
     * twice (two index rebuilds + two LRU passes per frame), and under budget
     * pressure could evict the very result the canvas had just bound. */
    const tp_result *pr = gui_pack_result_peek(
        s_actions.preview_animation_ref.atlas_id);
    const uint64_t result_version = gui_pack_result_version(
        s_actions.preview_animation_ref.atlas_id);
    s_canvas.anim_sprite = -1;
    s_preview_frame_count = 0;
    if (!an) {
        return; /* declare_canvas draws the "Pack to preview" hint */
    }
    if (!pr || result_version == 0U) {
        /* Never packed -> the hint is the honest presentation (open_preview_ref
         * says so). But a result that was PLAYING and then disappeared -- the
         * byte-budget store evicted it, or the atlas was cleared -- would leave
         * the player armed on a hint nobody asked for, so it stops for real.
         * This is a fact about the preview's OWN atlas, so it is decided before
         * the canvas-binding guard below. */
        if (s_actions.preview_had_result) {
            preview_stop();
            set_status_ex(STATUS_WARNING,
                          "Preview stopped: the packed result was released. "
                          "Pack (Ctrl+P) to preview again.");
        }
        return;
    }
    s_actions.preview_had_result = true;
    /* Everything below indexes the result the CANVAS is bound to, and the canvas
     * binds the VIEWED atlas' result (preview_target_result) -- including the
     * canonical frame lookup, which resolves against the resident atlas. Every
     * switch path stops the player, so this holds by construction; a divergence
     * would resolve one atlas' regions out of another atlas' result, so it is
     * checked rather than assumed. */
    if (!tp_id128_eq(s_actions.preview_animation_ref.atlas_id,
                     gui_view_atlas_id())) {
        return;
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
