#include "gui_project_internal.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"
#include "gui_session_adapter.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_source_plan.h"
#include "tp_core/tp_srckey.h"

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

// #region mutation wrappers (each builds typed op(s) + commits through the model)
static gui_project_create_result create_failed(void) {
    return (gui_project_create_result){
        .visible_index = -1,
    };
}

static gui_project_create_result create_committed(
    tp_id128 created_id, int visible_index,
    const gui_session_submit_terminal *terminal) {
    NT_ASSERT(terminal != NULL);
    NT_ASSERT(terminal->committed);
    return (gui_project_create_result){
        .committed = true,
        .observation_pending =
            terminal->echo_state ==
                GUI_SESSION_SUBMIT_ECHO_PENDING,
        .created_id = created_id,
        .visible_index = visible_index,
    };
}

gui_project_create_result gui_project_add_atlas(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot) {
        return create_failed();
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
        return create_failed();
    }
    tp_id128 new_id;
    if (!gen_id(&new_id)) {
        return create_failed();
    }
    tp_id128 target_id;
    if (!gen_id(&target_id)) {
        return create_failed();
    }
    err = (tp_error){0};
    gui_session_submit_terminal terminal = {0};
    const tp_status status = gui_session_create_atlas(&s_project.binding.client, new_id, target_id, tp_session_snapshot_revision(snapshot), name,
        exporter_id, out_path, target_enabled, &terminal, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return create_failed();
    }
    snapshot = gui_project_snapshot();
    const int count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < count; i++) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, new_id)) {
            return create_committed(new_id, i, &terminal);
        }
    }
    return create_committed(new_id, -1, &terminal);
}

/* Returns true only after the identified removal commits. */
bool gui_project_remove_atlas(tp_id128 atlas_id, int64_t expected_revision) {
    if (tp_id128_is_nil(atlas_id) || !gui_session_client_is_attached(
            &s_project.binding.client)) {
        return false;
    }
    tp_error err = {0};
    const tp_status status = gui_session_remove_atlas(&s_project.binding.client, atlas_id, expected_revision, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
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

/* Batch-add multiple sources as ONE atomic transaction (H/P2-13) -- the "Add Files" multi-select path,
 * which previously committed one txn PER file (N undo steps + a mid-batch failure left a partial add).
 * The shared planner rejects invalid path elements and skips paths already in the atlas or queued in this
 * batch (reported through *out_dup), so the committed txn holds only distinct new sources.
 * Commits nothing when nothing is new. Returns true iff the txn committed (or was a clean no-op); false
 * on OOM or a core reject (the model is then byte-unchanged). Both out-counts are always set
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
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot || !tp_session_snapshot_atlas_by_id(snapshot, atlas_id) ||
        n_paths <= 0 || !paths) {
        return false;
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
        tp_error err = {0};
        const tp_status status = gui_session_add_sources(&s_project.binding.client, atlas_id, ids, distinct, m,
            (tp_snapshot_source_kind)kind,
            expected_revision, &err);
        ok = status == TP_STATUS_OK;
        if (!ok) {
            gui_project__note_session_reject(status, &err);
        } else {
            gui_project_invalidate_sources();
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
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || tp_id128_is_nil(atlas_id) || tp_id128_is_nil(source_id)) {
        return false;
    }
    tp_error err = {0};
    const tp_status status = gui_session_remove_source(&s_project.binding.client, atlas_id, source_id, expected_revision, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    gui_project_invalidate_sources();
    return true;
}

/* The one identified TEXT submit. `kind` is the typed operation the draft owner
 * resolved from its descriptor; this owns the target/session validation and the
 * session adapter routing, so the caller never sees the client. */
tp_status gui_project_submit_text(
    tp_op_kind kind, const gui_text_ref *ref,
    const char *value, gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err) {
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || !ref ||
        tp_id128_is_nil(ref->atlas_id) || !value ||
        !transaction_id) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "text draft submit requires an active session, target, value, and transaction");
    }
    switch (kind) {
        case TP_OP_ATLAS_RENAME:
            return gui_session_submit_atlas_name(
                &s_project.binding.client, ref->atlas_id,
                ref->expected_revision, value, identity,
                transaction_id, terminal, err);
        case TP_OP_ANIMATION_RENAME:
            if (tp_id128_is_nil(ref->entity_id)) {
                break;
            }
            return gui_session_submit_animation_name(
                &s_project.binding.client, ref->atlas_id,
                ref->entity_id, ref->expected_revision,
                value, identity, transaction_id,
                terminal, err);
        case TP_OP_SPRITE_NAME_SET:
            if (tp_id128_is_nil(ref->source_id) ||
                !ref->source_key ||
                ref->source_key[0] == '\0') {
                break;
            }
            return gui_session_submit_sprite_name(
                &s_project.binding.client, ref->atlas_id,
                ref->source_id, ref->source_key,
                ref->expected_revision, value, identity,
                transaction_id, terminal, err);
        case TP_OP_TARGET_SET:
            if (tp_id128_is_nil(ref->entity_id)) {
                break;
            }
            return gui_session_submit_target_out_path(
                &s_project.binding.client, ref->atlas_id,
                ref->entity_id, ref->expected_revision,
                value, identity, transaction_id,
                terminal, err);
        default:
            break;
    }
    return tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT,
        "text draft submit requires a stable target for its operation kind");
}

tp_status gui_project_submit_atlas_settings(
    tp_id128 atlas_id, int64_t expected_revision,
    const tp_op_atlas_settings *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err) {
    if (!gui_session_client_is_attached(
            &s_project.binding.client) ||
        tp_id128_is_nil(atlas_id) || !settings ||
        !transaction_id) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "atlas draft submit requires an active session, target, component, and transaction");
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (!snapshot ||
        !tp_session_snapshot_atlas_by_id(snapshot, atlas_id)) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "the edited atlas no longer exists");
    }
    return gui_session_set_atlas_settings(
        &s_project.binding.client, atlas_id,
        expected_revision, settings, identity,
        transaction_id, out_terminal, err);
}

tp_status gui_project_submit_sprite_settings(
    const gui_sprite_ref *sprite,
    const tp_op_sprite_set *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal,
    tp_error *err) {
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || !sprite ||
        tp_id128_is_nil(sprite->atlas_id) ||
        tp_id128_is_nil(sprite->source_id) ||
        !sprite->source_key || sprite->source_key[0] == '\0' ||
        !settings || !transaction_id) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "sprite draft submit requires an active session, stable target, component, and transaction");
    }
    return gui_session_set_sprite_override(
        &s_project.binding.client, sprite->atlas_id,
        sprite->source_id, sprite->source_key,
        sprite->expected_revision, settings,
        identity, transaction_id, terminal, err);
}

tp_status gui_project_submit_sprite_origin(
    const gui_sprite_ref *sprite, int axis, float value,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err) {
    if (!sprite || axis < 0 || axis > 1) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "sprite origin draft requires axis 0 or 1");
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *current =
        snapshot ? tp_session_snapshot_sprite_by_key(
                       snapshot, sprite->atlas_id,
                       sprite->source_id, sprite->source_key)
                 : NULL;
    tp_op_sprite_set settings = {
        .mask = TP_SPF_ORIGIN,
        .origin_x = axis == 0
                        ? value
                        : (current ? current->origin_x
                                   : TP_PROJECT_ORIGIN_DEFAULT),
        .origin_y = axis == 1
                        ? value
                        : (current ? current->origin_y
                                   : TP_PROJECT_ORIGIN_DEFAULT),
    };
    return gui_project_submit_sprite_settings(
        sprite, &settings, identity, transaction_id,
        terminal, err);
}

tp_status gui_project_submit_sprite_slice9(
    const gui_sprite_ref *sprite, int component, int value,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err) {
    if (!sprite || component < 0 || component >= 4) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "sprite slice9 draft requires component 0 through 3");
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *current =
        snapshot ? tp_session_snapshot_sprite_by_key(
                       snapshot, sprite->atlas_id,
                       sprite->source_id, sprite->source_key)
                 : NULL;
    tp_op_sprite_set settings = {.mask = TP_SPF_SLICE9};
    for (int index = 0; index < 4; ++index) {
        settings.slice9[index] =
            current ? current->slice9_lrtb[index] : 0;
    }
    settings.slice9[component] = value;
    return gui_project_submit_sprite_settings(
        sprite, &settings, identity, transaction_id,
        terminal, err);
}

gui_project_create_result gui_project_add_target(
    tp_id128 atlas_id, int64_t expected_revision) {
    /* target.create op for the default json-neotolis target (mirrors seed_default_target's exporter +
     * "out/<name>" path). An OP (not the lifecycle seed) so the added target is captured in the diff
     * history and Undo removes exactly this target -- a direct seed leaves no undo step, so Ctrl+Z would
     * revert the WRONG (prior) edit. */
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
                                         : NULL;
    if (!atlas) {
        return create_failed();
    }
    tp_id128 target_id;
    if (!gen_id(&target_id)) {
        return create_failed();
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
        return create_failed();
    }
    gui_session_submit_terminal terminal = {0};
    const tp_status status = gui_session_create_target(&s_project.binding.client, atlas_id, target_id, expected_revision,
        exporter_id, out_path, enabled, &terminal, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return create_failed();
    }
    snapshot = gui_project_snapshot();
    atlas = snapshot
                ? tp_session_snapshot_atlas_by_id(
                      snapshot, atlas_id)
                : NULL;
    for (int index = 0;
         atlas && index < atlas->target_count;
         ++index) {
        const tp_snapshot_target *target =
            tp_session_snapshot_target_at(
                snapshot, atlas_id, index);
        if (target &&
            tp_id128_eq(target->id, target_id)) {
            return create_committed(
                target_id, index, &terminal);
        }
    }
    return create_committed(target_id, -1, &terminal);
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). */
bool gui_project_remove_target(const gui_target_ref *target) {
    if (!target) return false;
    tp_error err = {0};
    const tp_status status = gui_session_remove_target(
        &s_project.binding.client, target->atlas_id,
        target->target_id, target->expected_revision,
        &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}

bool gui_project_set_target_enabled(
    const gui_target_ref *target, bool enabled) {
    tp_error err = {{0}};
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || !target ||
        tp_id128_is_nil(target->atlas_id) ||
        tp_id128_is_nil(target->target_id)) {
        return false;
    }
    const tp_op_target_set settings = {
        .mask = TP_TF_ENABLED,
        .enabled = enabled,
    };
    const tp_status status = gui_session_set_target(
        &s_project.binding.client, target->atlas_id,
        target->target_id, target->expected_revision,
        &settings, (gui_session_submit_identity){0},
        NULL, NULL, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}

bool gui_project_set_target_exporter(
    const gui_target_ref *target, const char *exporter_id) {
    tp_error err = {{0}};
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || !target ||
        tp_id128_is_nil(target->atlas_id) ||
        tp_id128_is_nil(target->target_id) ||
        !exporter_id) {
        return false;
    }
    const tp_op_target_set settings = {
        .mask = TP_TF_EXPORTER,
        .exporter_id = (char *)exporter_id,
    };
    const tp_status status = gui_session_set_target(
        &s_project.binding.client, target->atlas_id,
        target->target_id, target->expected_revision,
        &settings, (gui_session_submit_identity){0},
        NULL, NULL, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}
// #endregion
// #region animations
gui_project_create_result gui_project_create_animation(
    tp_id128 atlas_id, int64_t expected_revision,
    const char *base, const tp_op_sprite_ref *frames,
    int frame_count) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id)
                                         : NULL;
    if (!atlas) {
        return create_failed();
    }
    char id[128];
    tp_error naming_error = {0};
    const tp_status naming_status = tp_session_snapshot_next_animation_name(
        snapshot, atlas_id, base, id, sizeof id, &naming_error);
    if (naming_status != TP_STATUS_OK) {
        gui_project__note_session_reject(naming_status, &naming_error);
        return create_failed();
    }
    tp_id128 anim_id;
    if (!gen_id(&anim_id)) {
        return create_failed();
    }
    tp_error err = {0};
    gui_session_submit_terminal terminal = {0};
    const tp_status status = gui_session_create_animation(&s_project.binding.client, atlas_id, anim_id, expected_revision, id, frames,
        frame_count, &terminal, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return create_failed();
    }
    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    for (int i = 0; atlas && i < atlas->animation_count; i++) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(snapshot, atlas_id, i);
        if (animation && tp_id128_eq(animation->id, anim_id)) {
            return create_committed(anim_id, i, &terminal);
        }
    }
    return create_committed(anim_id, -1, &terminal);
}

/* fix3 [0]: bool -- true iff the removal committed (see gui_project_remove_atlas). The deferred
 * handler guards preview_stop + stable animation-selection reset + "Removed"
 * message on this. */
bool gui_project_remove_animation(const gui_animation_ref *animation) {
    if (!animation) {
        return false;
    }
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation(&s_project.binding.client, animation->atlas_id, animation->animation_id,
        animation->expected_revision, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}

tp_status gui_project_submit_animation_settings(
    const gui_animation_ref *animation,
    const tp_op_anim_settings *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal,
    tp_error *err) {
    if (!gui_session_client_is_attached(
            &s_project.binding.client) || !animation ||
        tp_id128_is_nil(animation->atlas_id) ||
        tp_id128_is_nil(animation->animation_id) ||
        !settings || !transaction_id) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "animation draft submit requires an active session, stable target, component, and transaction");
    }
    return gui_session_set_animation_settings(
        &s_project.binding.client, animation->atlas_id,
        animation->animation_id,
        animation->expected_revision, settings,
        identity, transaction_id, terminal, err);
}

bool gui_project_anim_add_frames(const gui_animation_ref *animation,
                                 const tp_op_sprite_ref *frames, int count) {
    if (!animation) {
        return false;
    }
    if (!frames || count <= 0) {
        return false;
    }
    tp_error err = {0};
    const tp_status status = gui_session_add_animation_frames(&s_project.binding.client, animation->atlas_id, animation->animation_id,
        animation->expected_revision, frames, count, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}

bool gui_project_anim_remove_frame(const gui_animation_ref *animation,
                                   int frame_index) {
    if (!animation) {
        return false;
    }
    tp_error err = {0};
    const tp_status status = gui_session_remove_animation_frame(&s_project.binding.client, animation->atlas_id, animation->animation_id,
        animation->expected_revision, frame_index, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}

bool gui_project_anim_move_frame(const gui_animation_ref *animation,
                                 int frame_index, int delta) {
    if (!animation) {
        return false;
    }
    const int to = frame_index + delta;
    if (to == frame_index) {
        return true; /* no-op move (edge button): skip commit, as before */
    }
    tp_error err = {0};
    const tp_status status = gui_session_move_animation_frame(&s_project.binding.client, animation->atlas_id, animation->animation_id,
        animation->expected_revision, frame_index, to, &err);
    if (status != TP_STATUS_OK) {
        gui_project__note_session_reject(status, &err);
        return false;
    }
    return true;
}
// #endregion
