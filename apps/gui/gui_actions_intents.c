/* The ONE deferred-intent queue of the GUI action layer.
 *
 * Everything a view or an action owner defers to the next between-frame
 * boundary is one `gui_intent` in one growable queue: file dialogs, structural
 * create/remove commands, refresh, pack/export, and the two value-edit families
 * that used to be their own arrays. This TU owns the queue's push, its single
 * drain switch, its payload destruction, and the revision rebase.
 *
 * The drain ORDER is the enum order documented on `gui_intent_kind` and it is
 * the transcription of the four mechanisms this queue replaced. Nothing here
 * decides an order of its own. */

#include "gui_actions_internal.h"
#include "gui_project.h"

#include <stdlib.h>
#include <string.h>

#include "gui_state.h"

/* --- ordering ------------------------------------------------------------ */

/* Drain step of a kind. Identity except for the two families that were
 * growable arrays: their kinds share one step, because inside those arrays the
 * legacy drain ran in ENQUEUE order across kinds. */
static int intent_step(gui_intent_kind kind) {
    if (kind <= GUI_INTENT_ANIM_ADD_FRAMES) {
        return 0;
    }
    if (kind <= GUI_INTENT_TARGET_EXPORTER) {
        return 1;
    }
    return (int)kind - 3;
}

static const struct {
    gui_intent_kind first;
    gui_intent_kind last;
} k_intent_phase_range[] = {
    [GUI_INTENT_PHASE_EDIT] = {GUI_INTENT_ANIM_FRAME_REMOVE,
                               GUI_INTENT_TARGET_EXPORTER},
    [GUI_INTENT_PHASE_DIALOG] = {GUI_INTENT_SAVE, GUI_INTENT_ADD_FOLDER},
    [GUI_INTENT_PHASE_STRUCTURAL] = {GUI_INTENT_ADD_ATLAS,
                                     GUI_INTENT_OPEN_PREVIEW},
    [GUI_INTENT_PHASE_REFRESH] = {GUI_INTENT_REFRESH, GUI_INTENT_REFRESH},
    [GUI_INTENT_PHASE_PACK] = {GUI_INTENT_CANCEL,
                               GUI_INTENT_PREVIEW_TARGET},
};

/* The two edit families append (a click each); every other kind was one
 * boolean/struct slot, so a repeat request replaces the queued one. */
static bool intent_is_single_slot(gui_intent_kind kind) {
    return kind > GUI_INTENT_TARGET_EXPORTER;
}

/* --- payload ownership --------------------------------------------------- */

/* Default-less on purpose: a kind that grows an owned payload must appear here
 * or the build fails, which is the whole point of one queue. */
static void intent_payload_dispose(gui_intent *intent) {
    switch (intent->kind) {
        case GUI_INTENT_ANIM_FRAME_REMOVE:
        case GUI_INTENT_ANIM_FRAME_MOVE:
        case GUI_INTENT_ANIM_ADD_FRAMES:
            gui_actions__frame_refs_dispose(
                intent->payload.animation_edit.frames,
                intent->payload.animation_edit.frame_count);
            intent->payload.animation_edit.frames = NULL;
            intent->payload.animation_edit.frame_count = 0;
            break;
        case GUI_INTENT_CREATE_ANIMATION:
            free(intent->payload.create_animation.name);
            intent->payload.create_animation.name = NULL;
            gui_actions__frame_refs_dispose(
                intent->payload.create_animation.frames,
                intent->payload.create_animation.frame_count);
            intent->payload.create_animation.frames = NULL;
            intent->payload.create_animation.frame_count = 0;
            break;
        case GUI_INTENT_TARGET_ENABLED:
        case GUI_INTENT_TARGET_EXPORTER:
        case GUI_INTENT_SAVE:
        case GUI_INTENT_SAVE_AS:
        case GUI_INTENT_ADD_FILES:
        case GUI_INTENT_ADD_FOLDER:
        case GUI_INTENT_ADD_ATLAS:
        case GUI_INTENT_REMOVE_SOURCE:
        case GUI_INTENT_REMOVE_ATLAS:
        case GUI_INTENT_ADD_TARGET:
        case GUI_INTENT_REMOVE_TARGET:
        case GUI_INTENT_BROWSE_TARGET:
        case GUI_INTENT_ADD_ANIMATION:
        case GUI_INTENT_REMOVE_ANIMATION:
        case GUI_INTENT_OPEN_PREVIEW:
        case GUI_INTENT_REFRESH:
        case GUI_INTENT_CANCEL:
        case GUI_INTENT_PACK:
        case GUI_INTENT_EXPORT:
        case GUI_INTENT_PREVIEW_TARGET:
            break; /* no owned payload */
    }
}

/* --- queue --------------------------------------------------------------- */

bool gui_actions__intent_queued(gui_intent_kind kind) {
    for (int i = 0; i < s_actions.intent_count; ++i) {
        if (s_actions.intents[i].kind == kind) {
            return true;
        }
    }
    return false;
}

bool gui_actions__intent_push(const gui_intent *intent) {
    if (!intent) {
        return false;
    }
    if (intent_is_single_slot(intent->kind)) {
        for (int i = 0; i < s_actions.intent_count; ++i) {
            if (s_actions.intents[i].kind != intent->kind) {
                continue;
            }
            gui_intent replaced = s_actions.intents[i];
            s_actions.intents[i] = *intent;
            intent_payload_dispose(&replaced);
            return true;
        }
    }
    if (s_actions.intent_count == s_actions.intent_cap) {
        const int capacity =
            s_actions.intent_cap ? s_actions.intent_cap * 2 : 8;
        gui_intent *grown = (gui_intent *)realloc(
            s_actions.intents, (size_t)capacity * sizeof *grown);
        if (!grown) {
            gui_intent rejected = *intent;
            intent_payload_dispose(&rejected);
            set_status_ex(
                STATUS_ERROR,
                "Out of memory: this edit could not be queued (change not applied).");
            return false;
        }
        s_actions.intents = grown;
        s_actions.intent_cap = capacity;
    }
    s_actions.intents[s_actions.intent_count++] = *intent;
    return true;
}

/* Teardown of the queue itself. Whatever is still queued at exit will never be
 * drained, so its owned payload (the copied frame refs and animation name an
 * intent carries) is disposed exactly as a drain would have, and the array the
 * pushes grew is released. */
void gui_actions__intent_shutdown(void) {
    for (int i = 0; i < s_actions.intent_count; ++i) {
        intent_payload_dispose(&s_actions.intents[i]);
    }
    free(s_actions.intents);
    s_actions.intents = NULL;
    s_actions.intent_count = 0;
    s_actions.intent_cap = 0;
    s_actions.pending_frame_selection.present = false;
    s_actions.pending_added_atlas_status_id =
        tp_id128_nil();
    s_actions.pending_created_animation.present = false;
    s_actions.pending_history_reconcile.present = false;
    s_actions.pending_view_reconcile = false;
}

/* Drops every queued intent whose kind is in [first,last], destroying payloads. */
static void intent_clear_range(gui_intent_kind first, gui_intent_kind last) {
    int kept = 0;
    for (int i = 0; i < s_actions.intent_count; ++i) {
        if (s_actions.intents[i].kind >= first &&
            s_actions.intents[i].kind <= last) {
            intent_payload_dispose(&s_actions.intents[i]);
            continue;
        }
        s_actions.intents[kept++] = s_actions.intents[i];
    }
    s_actions.intent_count = kept;
}

void gui_actions__reconcile_observation(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    if (s_actions.pending_history_reconcile.present) {
        gui_view_reconcile_observation(snapshot);
        if (tp_id128_is_nil(
                s_actions.pending_history_reconcile
                    .animation_id) ||
            gui_view_animation_index(snapshot) < 0) {
            gui_view_select_animation(tp_id128_nil());
        }
        s_actions.pending_history_reconcile.present =
            false;
    } else if (s_actions.pending_view_reconcile) {
        gui_view_reconcile_observation(snapshot);
    }
    s_actions.pending_view_reconcile = false;

    if (s_actions.pending_frame_selection.present) {
        const tp_id128 atlas_id =
            s_actions.pending_frame_selection.atlas_id;
        const tp_id128 animation_id =
            s_actions.pending_frame_selection.animation_id;
        const int frame_index =
            s_actions.pending_frame_selection.frame_index;
        const tp_snapshot_animation *animation =
            snapshot
                ? tp_session_snapshot_animation_by_id(
                      snapshot, atlas_id, animation_id)
                : NULL;
        if (tp_id128_eq(
                gui_view_atlas_id(), atlas_id) &&
            tp_id128_eq(
                gui_view_animation_id(),
                animation_id)) {
            gui_view_select_animation_frame(
                snapshot,
                animation &&
                        frame_index >= 0 &&
                        frame_index <
                            animation->frame_count
                    ? frame_index
                    : -1);
        }
        s_actions.pending_frame_selection.present =
            false;
    }

    if (!tp_id128_is_nil(
            s_actions.pending_added_atlas_status_id)) {
        const tp_snapshot_atlas *added =
            snapshot
                ? tp_session_snapshot_atlas_by_id(
                      snapshot,
                      s_actions
                          .pending_added_atlas_status_id)
                : NULL;
        if (added) {
            set_statusf(
                "Added atlas '%s'", added->name);
        } else {
            set_status_ex(
                STATUS_ERROR,
                "The added atlas was not present after publication.");
        }
        s_actions.pending_added_atlas_status_id =
            tp_id128_nil();
    }

    if (s_actions.pending_created_animation.present) {
        const tp_snapshot_animation *animation =
            snapshot
                ? tp_session_snapshot_animation_by_id(
                      snapshot,
                      s_actions.pending_created_animation
                          .atlas_id,
                      s_actions.pending_created_animation
                          .animation_id)
                : NULL;
        if (!animation) {
            set_status_ex(
                STATUS_ERROR,
                "The created animation was not present after publication.");
        } else if (
            s_actions.pending_created_animation
                .with_frames) {
            set_statusf(
                "Created animation '%s' with %d frame(s) (Ctrl+Z to undo).",
                animation->name,
                s_actions.pending_created_animation
                    .frame_count);
        } else {
            set_statusf(
                "Added animation '%s' (Ctrl+Z to undo).",
                animation->name);
        }
        s_actions.pending_created_animation.present =
            false;
    }

}

void gui_actions__record_created_animation(
    tp_id128 atlas_id, tp_id128 animation_id,
    int frame_count, bool with_frames) {
    if (tp_id128_eq(gui_view_atlas_id(), atlas_id)) {
        gui_view_select_animation(animation_id);
    }
    s_actions.pending_created_animation.atlas_id =
        atlas_id;
    s_actions.pending_created_animation.animation_id =
        animation_id;
    s_actions.pending_created_animation.frame_count =
        frame_count;
    s_actions.pending_created_animation.with_frames =
        with_frames;
    s_actions.pending_created_animation.present = true;
}

/* --- executors ----------------------------------------------------------- */

static void intent_add_atlas(void) {
    const gui_project_create_result created = gui_project_add_atlas();
    if (!created.committed) {
        return;
    }
    gui_view_select_atlas(created.created_id);
    /* The selection just moved to the new atlas, and the animation player resolves
     * its frames against the VIEWED atlas' packed result -- so it stops here, the
     * same way every other atlas switch stops it (gui_view_lists' reset_selection).
     * Without this the player stayed armed on the previous atlas and kept reading
     * a result the canvas is no longer bound to. */
    preview_stop();
    s_actions.pending_added_atlas_status_id =
        created.created_id;
}

/* One created animation reports itself the same way whether it came from the
 * "add animation" command or from a multi-selection. */
static void intent_remove_animation(const gui_animation_ref *ref) {
    /* preview_stop + selection reset + success text run only after a real
     * removal. On any operation rejection the animation remains, so we must not
     * clear its UI state. (preview_stop only resets flags, so running it AFTER
     * the removal is safe -- no project deref.) */
    const bool was_previewing =
        s_preview_active &&
        tp_id128_eq(s_actions.preview_animation_ref.atlas_id, ref->atlas_id) &&
        tp_id128_eq(s_actions.preview_animation_ref.animation_id,
                    ref->animation_id);
    if (!gui_project_remove_animation(ref)) {
        return;
    }
    if (was_previewing) {
        preview_stop();
    }
    if (tp_id128_eq(gui_view_animation_id(), ref->animation_id)) {
        gui_view_select_animation(tp_id128_nil());
    }
    set_status("Removed animation (Ctrl+Z to undo).");
}

/* A target edit carries its own identity + captured revision; the mutators take
 * the canonical ref. */
static gui_target_ref intent_edited_target(const gui_intent *intent) {
    const gui_target_ref target = {intent->payload.target_edit.atlas_id,
                                   intent->payload.target_edit.target_id,
                                   intent->payload.target_edit.expected_revision};
    return target;
}

static void intent_anim_frame_remove(const gui_intent *intent) {
    const gui_animation_ref *ref = &intent->payload.animation_edit.animation;
    const int frame_index = intent->payload.animation_edit.first;
    const bool removes_selection =
        gui_view_animation_frame(gui_project_snapshot()) == frame_index &&
        tp_id128_eq(gui_view_atlas_id(), ref->atlas_id) &&
        tp_id128_eq(gui_view_animation_id(), ref->animation_id);
    if (gui_project_anim_remove_frame(ref, frame_index) && removes_selection) {
        s_actions.pending_frame_selection.atlas_id =
            ref->atlas_id;
        s_actions.pending_frame_selection.animation_id =
            ref->animation_id;
        s_actions.pending_frame_selection.frame_index =
            -1;
        s_actions.pending_frame_selection.present = true;
    }
}

/* The one drain switch. Default-less: every deferred intent the GUI has must
 * name its executor here, and a new kind cannot be added without one. */
static void intent_execute(const gui_intent *intent) {
    switch (intent->kind) {
        case GUI_INTENT_ANIM_FRAME_REMOVE:
            intent_anim_frame_remove(intent);
            break;
        case GUI_INTENT_ANIM_FRAME_MOVE:
            if (gui_project_anim_move_frame(
                    &intent->payload.animation_edit.animation,
                    intent->payload.animation_edit.first,
                    intent->payload.animation_edit.second) &&
                intent->payload.animation_edit.follow_selection) {
                s_actions.pending_frame_selection.atlas_id =
                    intent->payload.animation_edit.animation.atlas_id;
                s_actions.pending_frame_selection.animation_id =
                    intent->payload.animation_edit.animation.animation_id;
                s_actions.pending_frame_selection.frame_index =
                    intent->payload.animation_edit.first +
                    intent->payload.animation_edit.second;
                s_actions.pending_frame_selection.present = true;
            }
            break;
        case GUI_INTENT_ANIM_ADD_FRAMES:
            (void)gui_project_anim_add_frames(
                &intent->payload.animation_edit.animation,
                intent->payload.animation_edit.frames,
                intent->payload.animation_edit.frame_count);
            break;
        case GUI_INTENT_TARGET_ENABLED: {
            const gui_target_ref target = intent_edited_target(intent);
            (void)gui_project_set_target_enabled(
                &target, intent->payload.target_edit.enabled);
            break;
        }
        case GUI_INTENT_TARGET_EXPORTER: {
            const gui_target_ref target = intent_edited_target(intent);
            (void)gui_project_set_target_exporter(
                &target, intent->payload.target_edit.exporter_id);
            break;
        }
        case GUI_INTENT_SAVE:
            (void)gui_actions__save();
            break;
        case GUI_INTENT_SAVE_AS:
            (void)gui_actions__save_as(
                intent->payload.save_as.prepared
                    ? &intent->payload.save_as.plan
                    : NULL);
            break;
        case GUI_INTENT_ADD_FILES:
            gui_actions__add_files();
            break;
        case GUI_INTENT_ADD_FOLDER:
            gui_actions__add_folder();
            break;
        case GUI_INTENT_ADD_ATLAS:
            intent_add_atlas();
            break;
        case GUI_INTENT_REMOVE_SOURCE:
            /* Side effects and success text run only after a real removal. */
            if (gui_project_remove_source(
                    intent->payload.source.atlas_id,
                    intent->payload.source.source_id,
                    intent->payload.source.expected_revision)) {
                reset_selection();
                set_status("Removed source (Ctrl+Z to undo).");
            }
            break;
        case GUI_INTENT_REMOVE_ATLAS:
            if (gui_project_remove_atlas(
                    intent->payload.scope.atlas_id,
                    intent->payload.scope.expected_revision)) {
                s_actions.pending_view_reconcile = true;
                set_status("Removed atlas (Ctrl+Z to undo).");
            }
            break;
        case GUI_INTENT_ADD_TARGET:
            if (gui_project_add_target(intent->payload.scope.atlas_id,
                                       intent->payload.scope.expected_revision)
                    .committed) {
                set_status("Added export target (Ctrl+Z to undo).");
            }
            break;
        case GUI_INTENT_REMOVE_TARGET:
            if (gui_project_remove_target(&intent->payload.target)) {
                set_status("Removed export target (Ctrl+Z to undo).");
            }
            break;
        case GUI_INTENT_BROWSE_TARGET:
            gui_actions__browse_target(&intent->payload.target);
            break;
        case GUI_INTENT_ADD_ANIMATION: {
            const gui_project_create_result created = gui_project_create_animation(
                intent->payload.scope.atlas_id,
                intent->payload.scope.expected_revision, NULL, NULL, 0);
            if (created.committed) {
                gui_actions__record_created_animation(
                    intent->payload.scope.atlas_id,
                    created.created_id, 0, false);
            }
            break;
        }
        case GUI_INTENT_CREATE_ANIMATION: {
            const gui_project_create_result created = gui_project_create_animation(
                intent->payload.create_animation.atlas_id,
                intent->payload.create_animation.expected_revision,
                intent->payload.create_animation.name[0]
                    ? intent->payload.create_animation.name
                    : NULL,
                intent->payload.create_animation.frames,
                intent->payload.create_animation.frame_count);
            if (created.committed) {
                gui_actions__record_created_animation(
                    intent->payload.create_animation.atlas_id,
                    created.created_id,
                    intent->payload.create_animation.frame_count, true);
            }
            break;
        }
        case GUI_INTENT_REMOVE_ANIMATION:
            intent_remove_animation(&intent->payload.animation);
            break;
        case GUI_INTENT_OPEN_PREVIEW:
            open_preview_ref(&intent->payload.animation);
            break;
        case GUI_INTENT_REFRESH:
            gui_actions__refresh();
            break;
        case GUI_INTENT_CANCEL:
            gui_actions__cancel();
            break;
        case GUI_INTENT_PACK:
            do_pack();
            break;
        case GUI_INTENT_EXPORT:
            gui_actions__export();
            break;
        case GUI_INTENT_PREVIEW_TARGET:
            gui_actions__preview_target_start(
                &intent->payload.preview_target);
            break;
    }
}

void gui_actions__intent_drain(gui_intent_phase phase) {
    if (!gui_project_observation_is_valid()) {
        return;
    }
    const int first = intent_step(k_intent_phase_range[phase].first);
    const int last = intent_step(k_intent_phase_range[phase].last);
    for (int step = first; step <= last; ++step) {
        for (int i = 0; i < s_actions.intent_count; ++i) {
            if (intent_step(s_actions.intents[i].kind) != step) {
                continue;
            }
            /* Take the entry by VALUE and vacate its slot first: an executor
             * may enqueue or clear, and the array can move under us. */
            gui_intent entry = s_actions.intents[i];
            memmove(&s_actions.intents[i], &s_actions.intents[i + 1],
                    (size_t)(s_actions.intent_count - i - 1) *
                        sizeof *s_actions.intents);
            s_actions.intent_count--;
            i--;
            const int64_t revision_before =
                gui_project_committed_revision();
            intent_execute(&entry);
            const int64_t revision_after =
                gui_project_committed_revision();
            if (revision_before >= 0 &&
                revision_after >= 0) {
                /* A view may enqueue several edits from one published cut.
                 * Once the first edit commits, the controller—not its caller—
                 * advances remaining edits to the exact next revision. */
                gui_actions__rebase_deferred_edits(
                    revision_before, revision_after);
            }
            intent_payload_dispose(&entry);
            if (!gui_project_observation_is_valid()) {
                return;
            }
        }
    }
}

/* Discrete commands captured in the same frame as a successful draft submit
 * advance only from that exact revision. Already-stale commands stay stale. */
void gui_actions__rebase_deferred_edits(int64_t revision_before,
                                        int64_t revision_after) {
    if (revision_after == revision_before) {
        return;
    }
    /* A submit commits exactly one transaction, but the observation refresh it
     * performs can also fold in FOREIGN commits (another client on the same live
     * session). More than one revision therefore means these intents really are
     * stale -- leave them stale so the session rejects them on expected_revision.
     * Not an invariant: a concurrent writer must never abort the GUI. */
    if (revision_after != revision_before + 1) {
        return;
    }
    for (int i = 0; i < s_actions.intent_count; ++i) {
        gui_intent *intent = &s_actions.intents[i];
        switch (intent->kind) {
            case GUI_INTENT_ANIM_FRAME_REMOVE:
            case GUI_INTENT_ANIM_FRAME_MOVE:
            case GUI_INTENT_ANIM_ADD_FRAMES:
                if (intent->payload.animation_edit.animation
                        .expected_revision == revision_before) {
                    intent->payload.animation_edit.animation
                        .expected_revision = revision_after;
                }
                break;
            case GUI_INTENT_TARGET_ENABLED:
            case GUI_INTENT_TARGET_EXPORTER:
                if (intent->payload.target_edit.expected_revision ==
                    revision_before) {
                    intent->payload.target_edit.expected_revision =
                        revision_after;
                }
                break;
            default:
                /* Only the two value-edit families rebase; a structural command
                 * captured its revision as its own gesture boundary. */
                break;
        }
    }
}

void gui_actions__discard_deferred_edits(void) {
    intent_clear_range(GUI_INTENT_ANIM_FRAME_REMOVE, GUI_INTENT_TARGET_EXPORTER);
}

/* Resets the one-shot request slots. The two value-edit families are NOT
 * cleared here: they are consumed by their own drain or dropped by
 * gui_actions__discard_deferred_edits, exactly as the legacy arrays were. */
void gui_actions__clear_pending(void) {
    gui_actions__clear_history_request();
    s_actions.pending_lifecycle_request = GUI_LIFECYCLE_REQUEST_NONE;
    intent_clear_range(GUI_INTENT_SAVE, GUI_INTENT_PREVIEW_TARGET);
}

/* --- payload-free view ingress ------------------------------------------- */

static void intent_request(gui_intent_kind kind) {
    const gui_intent intent = {.kind = kind};
    (void)gui_actions__intent_push(&intent);
}

void gui_request_save(void) { intent_request(GUI_INTENT_SAVE); }
void gui_request_save_as(void) { intent_request(GUI_INTENT_SAVE_AS); }
void gui_request_add_files(void) { intent_request(GUI_INTENT_ADD_FILES); }
void gui_request_add_folder(void) { intent_request(GUI_INTENT_ADD_FOLDER); }
void gui_request_add_atlas(void) { intent_request(GUI_INTENT_ADD_ATLAS); }
void gui_request_refresh(void) { intent_request(GUI_INTENT_REFRESH); }
void gui_request_reload_formats(void) {
    if (!gui_project_format_reload_active()) {
        s_actions.format_reload_requested = true;
    }
}
bool gui_actions_format_reload_active(void) {
    return s_actions.format_reload_requested ||
           gui_project_format_reload_active();
}
void gui_request_cancel(void) { intent_request(GUI_INTENT_CANCEL); }
void gui_request_pack(void) { intent_request(GUI_INTENT_PACK); }
void gui_request_export(void) { intent_request(GUI_INTENT_EXPORT); }

void gui_request_remove_atlas(tp_id128 atlas_id, int64_t expected_revision) {
    const gui_intent intent = {
        .kind = GUI_INTENT_REMOVE_ATLAS,
        .payload.scope = {atlas_id, expected_revision}};
    (void)gui_actions__intent_push(&intent);
}

void gui_request_remove_source(tp_id128 atlas_id, tp_id128 source_id,
                               int64_t expected_revision) {
    const gui_intent intent = {
        .kind = GUI_INTENT_REMOVE_SOURCE,
        .payload.source = {atlas_id, source_id, expected_revision}};
    (void)gui_actions__intent_push(&intent);
}

void gui_request_preview_target_native(void) {
    const gui_intent intent = {
        .kind = GUI_INTENT_PREVIEW_TARGET,
        .payload.preview_target = {
            .kind = GUI_PREVIEW_TARGET_NATIVE,
        },
    };
    (void)gui_actions__intent_push(&intent);
}

void gui_request_preview_target_format(const char *format_id) {
    const tp_format_descriptor *format =
        gui_project_format_find(format_id);
    gui_preview_target target = {0};
    if (!format ||
        !gui_preview_target_make_format(
            format->id, &target)) {
        return;
    }
    const gui_intent intent = {
        .kind = GUI_INTENT_PREVIEW_TARGET,
        .payload.preview_target = target,
    };
    (void)gui_actions__intent_push(&intent);
}
