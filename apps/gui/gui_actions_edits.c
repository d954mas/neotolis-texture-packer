#include "gui_actions_internal.h"

#include <stdlib.h>
#include <string.h>

#include "gui_state.h"
// #region deferred model-edit queue (decision 0015)
/* A commit clone-swaps the model + frees the old project, so declare_* render fns must not
 * commit while holding a live atlas/sprite/anim/target pointer. They ENQUEUE the edit here;
 * gui_actions__drain_edits() (run at frame top from apply_pending, no live pointer held) replays each via
 * the self-contained gui_project_* setters. The queue grows (never a fixed slot) so no edit is
 * ever dropped if two land in one frame; typically it holds 0 or 1.
 *
 * String args that can be LONG carry a HEAP copy, not a fixed slot: out_path is up to
 * TP_PATH_MAX (4096) so a 256-byte slot silently truncated + persisted a corrupted export
 * path on a mere target toggle (F2); add-frames carries a variable-length list of COPIED
 * sprite keys so "Add frames" can defer instead of committing synchronously mid-declare (F1).
 * each typed intent queue frees its heap payload after drain (or queue OOM). */

void gui_queue_atlas_setting(tp_id128 atlas_id, int64_t expected_revision,
                             gui_atlas_field field, int ivalue, float fvalue) {
    if (s_actions.atlas_setting_intent_count == s_actions.atlas_setting_intent_cap) {
        const int capacity = s_actions.atlas_setting_intent_cap ? s_actions.atlas_setting_intent_cap * 2 : 8;
        atlas_setting_intent *intents = (atlas_setting_intent *)realloc(
            s_actions.atlas_setting_intents, (size_t)capacity * sizeof *intents);
        if (!intents) {
            set_status_ex(STATUS_ERROR,
                          "Out of memory: this atlas edit could not be queued (change not applied).");
            return;
        }
        s_actions.atlas_setting_intents = intents;
        s_actions.atlas_setting_intent_cap = capacity;
    }
    s_actions.atlas_setting_intents[s_actions.atlas_setting_intent_count++] =
        (atlas_setting_intent){atlas_id, expected_revision, field, ivalue, fvalue};
}

/* Local heap strdup (POSIX strdup is not ISO C17). NULL treated as ""; NULL on OOM. */
char *gui_actions__strdup(const char *s) {
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

void gui_actions__frame_refs_dispose(tp_op_sprite_ref *frames, int count) {
    if (!frames) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(frames[i].src_key);
    }
    free(frames);
}

tp_op_sprite_ref *gui_actions__frame_refs_copy(const tp_op_sprite_ref *frames,
                                         int count) {
    if (!frames || count <= 0) {
        return NULL;
    }
    tp_op_sprite_ref *copy = calloc((size_t)count, sizeof *copy);
    if (!copy) {
        return NULL;
    }
    for (int i = 0; i < count; ++i) {
        if (!frames[i].src_key) {
            gui_actions__frame_refs_dispose(copy, count);
            return NULL;
        }
        copy[i].source_id = frames[i].source_id;
        copy[i].src_key = gui_actions__strdup(frames[i].src_key);
        if (!copy[i].src_key) {
            gui_actions__frame_refs_dispose(copy, count);
            return NULL;
        }
    }
    return copy;
}

/* Frees an edit's heap payload. Safe on a zeroed/partially-built edit. */
static void target_intent_dispose(target_edit_intent *e) {
    free(e->out_path);
    e->out_path = NULL;
}

/* Appends `e` to the queue (shallow copy -> the queue TAKES OWNERSHIP of e's heap out_path/keys;
 * the caller must not free them afterward, on success OR failure). On queue-realloc OOM the edit
 * is DROPPED: its heap payload is freed (no leak) and a status-bar error is raised so the drop is
 * visible -- the widget already returned "committed", so without this the value silently reverts
 * next frame with no explanation (F5). Returns true iff queued. */
static bool target_intent_push(target_edit_intent *e) {
    if (s_actions.target_intent_count == s_actions.target_intent_cap) {
        int nc = s_actions.target_intent_cap ? s_actions.target_intent_cap * 2 : 8;
        target_edit_intent *ne = (target_edit_intent *)realloc(
            s_actions.target_intents, (size_t)nc * sizeof *ne);
        if (!ne) {
            target_intent_dispose(e);
            set_status_ex(STATUS_ERROR, "Out of memory: this edit could not be queued (change not applied).");
            return false;
        }
        s_actions.target_intents = ne;
        s_actions.target_intent_cap = nc;
    }
    s_actions.target_intents[s_actions.target_intent_count++] = *e;
    return true;
}

static void queue_sprite_intent(sprite_intent_kind kind,
                                const gui_sprite_ref *sprite, int field,
                                int ivalue, float fvalue) {
    if (!sprite || tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) || !sprite->source_key ||
        sprite->source_key[0] == '\0') {
        return;
    }
    char *source_key = gui_actions__strdup(sprite->source_key);
    if (!source_key) {
        set_status_ex(STATUS_ERROR,
                      "Out of memory: this sprite edit could not be queued (change not applied).");
        return;
    }
    if (s_actions.sprite_intent_count == s_actions.sprite_intent_cap) {
        const int capacity = s_actions.sprite_intent_cap ? s_actions.sprite_intent_cap * 2 : 8;
        sprite_edit_intent *intents = (sprite_edit_intent *)realloc(
            s_actions.sprite_intents, (size_t)capacity * sizeof *intents);
        if (!intents) {
            free(source_key);
            set_status_ex(STATUS_ERROR,
                          "Out of memory: this sprite edit could not be queued (change not applied).");
            return;
        }
        s_actions.sprite_intents = intents;
        s_actions.sprite_intent_cap = capacity;
    }
    s_actions.sprite_intents[s_actions.sprite_intent_count++] = (sprite_edit_intent){
        kind, sprite->atlas_id, sprite->source_id, sprite->expected_revision,
        source_key, field, ivalue, fvalue};
}

void gui_queue_sprite_origin(const gui_sprite_ref *sprite, int axis, float value) {
    queue_sprite_intent(SPRITE_INTENT_ORIGIN, sprite, axis, 0, value);
}
void gui_queue_sprite_slice9(const gui_sprite_ref *sprite, int lrtb_index, int value) {
    queue_sprite_intent(SPRITE_INTENT_SLICE9, sprite, lrtb_index, value, 0.0F);
}
void gui_queue_sprite_override(const gui_sprite_ref *sprite, gui_sprite_ov which, int value) {
    queue_sprite_intent(SPRITE_INTENT_OVERRIDE, sprite, (int)which, value, 0.0F);
}
static bool animation_intent_push(animation_edit_intent *intent) {
    if (!intent || tp_id128_is_nil(intent->animation.atlas_id) ||
        tp_id128_is_nil(intent->animation.animation_id)) {
        return false;
    }
    if (s_actions.animation_intent_count == s_actions.animation_intent_cap) {
        const int capacity = s_actions.animation_intent_cap ? s_actions.animation_intent_cap * 2 : 8;
        animation_edit_intent *intents = (animation_edit_intent *)realloc(
            s_actions.animation_intents, (size_t)capacity * sizeof *intents);
        if (!intents) {
            gui_actions__frame_refs_dispose(intent->frames, intent->frame_count);
            set_status_ex(STATUS_ERROR,
                          "Out of memory: this animation edit could not be queued (change not applied).");
            return false;
        }
        s_actions.animation_intents = intents;
        s_actions.animation_intent_cap = capacity;
    }
    s_actions.animation_intents[s_actions.animation_intent_count++] = *intent;
    return true;
}

void gui_edit_anim_fps(const gui_animation_ref *animation, float fps) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FPS;
    if (animation) intent.animation = *animation;
    intent.value = fps;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_playback(const gui_animation_ref *animation, int playback) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_PLAYBACK;
    if (animation) intent.animation = *animation;
    intent.first = playback;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_flip(const gui_animation_ref *animation, bool flip_h, bool flip_v) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FLIP;
    if (animation) intent.animation = *animation;
    intent.flip_h = flip_h;
    intent.flip_v = flip_v;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_frame_remove(const gui_animation_ref *animation, int frame_index) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FRAME_REMOVE;
    if (animation) intent.animation = *animation;
    intent.first = frame_index;
    (void)animation_intent_push(&intent);
}
void gui_edit_anim_frame_move(const gui_animation_ref *animation, int frame_index, int delta) {
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_FRAME_MOVE;
    if (animation) intent.animation = *animation;
    intent.first = frame_index;
    intent.second = delta;
    (void)animation_intent_push(&intent);
}
static void edit_capture_target(target_edit_intent *edit,
                                 const gui_target_ref *target) {
    if (target) {
        edit->atlas_id = target->atlas_id;
        edit->target_id = target->target_id;
        edit->expected_revision = target->expected_revision;
    }
}

static bool edit_copy_exporter_id(char out[TP_EXPORTER_ID_MAX],
                                  const char *exporter_id) {
    tp_error error = {0};
    const tp_status status = tp_exporter_id_validate(exporter_id, &error);
    if (status != TP_STATUS_OK) {
        set_statusf_ex(STATUS_ERROR, "Export target edit rejected: %s",
                       error.msg);
        return false;
    }
    memcpy(out, exporter_id, strlen(exporter_id) + 1U);
    return true;
}

void gui_edit_target(const gui_target_ref *target, const char *exporter_id,
                     const char *out_path, bool enabled) {
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_FULL;
    edit_capture_target(&e, target);
    e.b0 = enabled;
    if (!edit_copy_exporter_id(e.s0, exporter_id)) {
        return;
    }
    e.out_path = gui_actions__strdup(out_path); /* HEAP full path -- a >255 out_path must not truncate (F2) */
    if (!e.out_path) {
        set_status_ex(STATUS_ERROR, "Out of memory: export target edit not applied.");
        return;
    }
    (void)target_intent_push(&e);
}
/* H/G3: the out-path text field's per-keystroke enqueue. Same heap out_path as gui_edit_target (up to
 * TP_PATH_MAX, no 255 slot), but the drain routes to the COALESCABLE gui_project_set_target_out_path so
 * the field's Enter/blur gesture-commit collapses the whole edit into ONE undo step. */
void gui_edit_target_out_path(const gui_target_ref *target, const char *out_path) {
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_OUT_PATH;
    edit_capture_target(&e, target);
    e.out_path = gui_actions__strdup(out_path); /* HEAP full path -- a >255 out_path must not truncate (F2) */
    if (!e.out_path) {
        set_status_ex(STATUS_ERROR, "Out of memory: export target edit not applied.");
        return;
    }
    (void)target_intent_push(&e);
}
/* H/G3: discrete enabled toggle. Carries ONLY the new enabled + the target index; the drain setter reads
 * exporter_id + out_path from the committed record AFTER flushing any buffered out-path gesture, so a
 * still-uncommitted typed out-path is preserved (not reverted by a stale re-send). */
void gui_edit_target_enabled(const gui_target_ref *target, bool enabled) {
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_ENABLED;
    edit_capture_target(&e, target);
    e.b0 = enabled;
    (void)target_intent_push(&e);
}
/* H/G3: discrete exporter change. Carries ONLY the new exporter_id + the target index; the drain setter
 * reads out_path + enabled from the committed record post-flush (same anti-stale-revert reason). */
void gui_edit_target_exporter(const gui_target_ref *target,
                              const char *exporter_id) {
    target_edit_intent e = {0};
    e.kind = TARGET_INTENT_EXPORTER;
    edit_capture_target(&e, target);
    if (!edit_copy_exporter_id(e.s0, exporter_id)) {
        return;
    }
    (void)target_intent_push(&e);
}

/* Enqueue an "add frames" edit carrying a COPY of canonical selection refs (F1). "Add frames" used to
 * commit synchronously from inside declare_animation_editor, which clone-swaps + frees the project
 * under the live `an`/`a` the same declare invocation keeps dereferencing (frame_count, frames[].
 * name) -> a use-after-free on an ordinary click. Deferring it (drain replays via
 * gui_project_anim_add_frames at frame top, no live pointer held) closes that last synchronous
 * commit; refs are copied NOW so a selection change before the drain cannot alter what lands. */
void gui_edit_anim_add_frames(const gui_animation_ref *animation,
                              const tp_op_sprite_ref *frames, int count) {
    if (count <= 0) {
        return;
    }
    animation_edit_intent intent = {0};
    intent.kind = ANIMATION_INTENT_ADD_FRAMES;
    if (animation) intent.animation = *animation;
    intent.frames = gui_actions__frame_refs_copy(frames, count);
    if (!intent.frames) {
        set_status_ex(STATUS_ERROR, "Out of memory: add-frames not applied.");
        return;
    }
    intent.frame_count = count;
    (void)animation_intent_push(&intent);
}

/* Replays every queued edit through the committing setters, then clears the queue. Runs at
 * frame top (apply_pending) with NO live declare-fn pointer held, so the per-edit clone-swap
 * is safe. Each setter re-fetches by index/name internally. */
void gui_actions__drain_edits(void) {
    for (int i = 0; i < s_actions.atlas_setting_intent_count; i++) {
        const atlas_setting_intent *intent = &s_actions.atlas_setting_intents[i];
        (void)gui_project_set_atlas_setting(intent->atlas_id,
                                            intent->expected_revision,
                                            intent->field, intent->ivalue,
                                            intent->fvalue);
    }
    s_actions.atlas_setting_intent_count = 0;
    for (int i = 0; i < s_actions.sprite_intent_count; i++) {
        sprite_edit_intent *intent = &s_actions.sprite_intents[i];
        const gui_sprite_ref sprite = {
            intent->atlas_id, intent->source_id, intent->source_key,
            intent->expected_revision};
        switch (intent->kind) {
            case SPRITE_INTENT_ORIGIN:
                (void)gui_project_set_sprite_origin(&sprite, intent->field,
                                                     intent->fvalue);
                break;
            case SPRITE_INTENT_SLICE9:
                (void)gui_project_set_sprite_slice9(&sprite, intent->field,
                                                     intent->ivalue);
                break;
            case SPRITE_INTENT_OVERRIDE:
                (void)gui_project_set_sprite_override(
                    &sprite, (gui_sprite_ov)intent->field, intent->ivalue);
                break;
        }
        free(intent->source_key);
        intent->source_key = NULL;
    }
    s_actions.sprite_intent_count = 0;
    for (int i = 0; i < s_actions.animation_intent_count; i++) {
        animation_edit_intent *intent = &s_actions.animation_intents[i];
        switch (intent->kind) {
            case ANIMATION_INTENT_FPS:
                (void)gui_project_set_anim_fps(&intent->animation, intent->value);
                break;
            case ANIMATION_INTENT_PLAYBACK:
                (void)gui_project_set_anim_playback(&intent->animation,
                                                    intent->first);
                break;
            case ANIMATION_INTENT_FLIP:
                (void)gui_project_set_anim_flip(&intent->animation,
                                                intent->flip_h,
                                                intent->flip_v);
                break;
            case ANIMATION_INTENT_FRAME_REMOVE:
                (void)gui_project_anim_remove_frame(&intent->animation,
                                                    intent->first);
                break;
            case ANIMATION_INTENT_FRAME_MOVE:
                (void)gui_project_anim_move_frame(&intent->animation,
                                                  intent->first,
                                                  intent->second);
                break;
            case ANIMATION_INTENT_ADD_FRAMES:
                (void)gui_project_anim_add_frames(
                    &intent->animation, intent->frames,
                    intent->frame_count);
                break;
        }
        gui_actions__frame_refs_dispose(intent->frames, intent->frame_count);
        intent->frames = NULL;
    }
    s_actions.animation_intent_count = 0;
    for (int i = 0; i < s_actions.target_intent_count; i++) {
        target_edit_intent *e = &s_actions.target_intents[i];
        const gui_target_ref target = {e->atlas_id, e->target_id,
                                       e->expected_revision};
        switch (e->kind) {
            case TARGET_INTENT_FULL:
                (void)gui_project_set_target(&target, e->s0,
                                             e->out_path ? e->out_path : "",
                                             e->b0);
                break;
            case TARGET_INTENT_OUT_PATH:
                (void)gui_project_set_target_out_path(
                    &target, e->out_path ? e->out_path : "");
                break;
            case TARGET_INTENT_ENABLED:
                (void)gui_project_set_target_enabled(&target, e->b0);
                break;
            case TARGET_INTENT_EXPORTER:
                (void)gui_project_set_target_exporter(&target, e->s0);
                break;
        }
        target_intent_dispose(e);
    }
    s_actions.target_intent_count = 0;
}

/* Set by a view widget the frame its edit GESTURE ENDS (slider release / field Enter+blur / a
 * discrete dropdown/checkbox pick). apply_pending flushes gui_project's pending transaction AFTER
 * gui_actions__drain_edits buffers this frame's value, so the whole gesture commits as ONE undo step
 * (decision 0015). One shared flag suffices: pending_route already flushes a prior
 * gesture when a different-key edit arrives, so the flag always targets the latest buffered edit. */
void gui_request_gesture_commit(void) { s_actions.gesture_commit = true; }
// #endregion

void gui_actions__apply_structural_edits(void) {
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
        gui_actions__browse_target(&s_actions.pending_browse_target_ref);
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
    gui_actions__pending_create_animation_dispose(
        &s_actions.pending_create_anim);
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
        if (gui_actions__resolve_animation_ref(
                &s_actions.pending_open_preview_ref, &atlas_index,
                &animation_index)) {
            s_sel_atlas = atlas_index;
            open_preview(animation_index);
        }
    }
}

void gui_actions__clear_pending(void) {
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
