/* Immutable snapshot materialization and the DTO/selector query surface, split
 * from tp_session.c as the session family's read-only second responsibility zone.
 * It reads committed state through tp_session_layout.h under the shared writer
 * gate and never mutates the live model or its project. */
#include "tp_core/tp_session.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"
#include "tp_model_seam.h"
#include "tp_project_generation_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_session_layout.h"
#include "tp_session_snapshot_internal.h"

// #region allocation accounting
static _Thread_local size_t s_snapshot_allocations;
static _Thread_local size_t s_snapshot_allocation_bytes;
static _Thread_local size_t s_snapshot_fail_after = SIZE_MAX;

void tp_session__test_reset_snapshot_allocations(void) {
    s_snapshot_allocations = 0U;
    s_snapshot_allocation_bytes = 0U;
    s_snapshot_fail_after = SIZE_MAX;
}

size_t tp_session__test_snapshot_allocation_count(void) {
    return s_snapshot_allocations;
}

size_t tp_session__test_snapshot_allocation_bytes(void) {
    return s_snapshot_allocation_bytes;
}

void tp_session__test_fail_snapshot_allocation_after(size_t successful) {
    s_snapshot_fail_after = successful;
}

void tp_session__test_fail_next_generation_owner_allocation(void) {
    tp_project_generation__test_fail_next_allocation();
}

static void *snapshot_calloc(size_t count, size_t size) {
    if (s_snapshot_fail_after != SIZE_MAX) {
        if (s_snapshot_fail_after == 0U) {
            s_snapshot_fail_after = SIZE_MAX;
            return NULL;
        }
        s_snapshot_fail_after--;
    }
    void *allocation = calloc(count, size);
    if (allocation) {
        s_snapshot_allocations++;
        s_snapshot_allocation_bytes += count * size;
    }
    return allocation;
}

_Static_assert((int)TP_SNAPSHOT_SOURCE_FOLDER == (int)TP_SOURCE_KIND_FOLDER,
               "snapshot folder kind must match project vocabulary");
_Static_assert((int)TP_SNAPSHOT_SOURCE_FILE == (int)TP_SOURCE_KIND_FILE,
               "snapshot file kind must match project vocabulary");
// #endregion

// #region materialization
static tp_status snapshot_materialize(tp_session_snapshot *snapshot,
                                      tp_error *err);

tp_status tp_session_snapshot_create(const tp_session *session,
                                     tp_session_snapshot **out, tp_error *err) {
    if (!session || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot requires session and output");
    }
    *out = NULL;
    tp_session_snapshot *snapshot = (tp_session_snapshot *)snapshot_calloc(1U, sizeof *snapshot);
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_OOM, "snapshot allocation failed");
    }

    /* Only generation retention and scalar metadata capture require a
     * consistent admission point. The retained generation is immutable, so DTO
     * materialization must not keep the single-writer gate held. */
    gate_lock(session);
    tp_project_generation *generation = NULL;
    tp_status retain_status = tp_model__retain_project_generation(
        session->model, &generation, err);
    if (retain_status != TP_STATUS_OK) {
        gate_unlock(session);
        free(snapshot);
        return retain_status;
    }
    snapshot->generation = generation;
    snapshot->project = tp_project_generation_project(generation);
    snapshot->atlas_count = snapshot->project->atlas_count;
    snapshot->revision = tp_model_revision(session->model);
    snapshot->admission_sequence = session->admission_sequence;
    snapshot->model_generation = session->model_generation;
    snapshot->source_generation = session->source_generation;
    snapshot->event_sequence = session->event_sequence;
    snapshot->dirty = tp_model_dirty(session->model);
    snapshot->recovery_health =
        tp_session__recovery_health_locked(session);
    snapshot->identity = session->identity;
    snapshot->saved_file_fingerprint = session->saved_file_fingerprint;
    snapshot->has_saved_file_fingerprint = session->has_saved_file_fingerprint;
    gate_unlock(session);

    tp_status status = snapshot_materialize(snapshot, err);
    if (status == TP_STATUS_OK) {
        *out = snapshot;
    }
    return status;
}

static tp_status snapshot_materialize(tp_session_snapshot *snapshot,
                                      tp_error *err) {
    const tp_project *project = snapshot->project;
    if (project->atlas_count > 0) {
        snapshot->atlases = (tp_snapshot_atlas_storage *)snapshot_calloc(
            (size_t)project->atlas_count, sizeof *snapshot->atlases);
        if (!snapshot->atlases) {
            tp_session_snapshot_destroy(snapshot);
            return tp_error_set(err, TP_STATUS_OOM, "snapshot atlas allocation failed");
        }
    }
    for (int i = 0; i < project->atlas_count; ++i) {
        const tp_project_atlas *source = &project->atlases[i];
        tp_snapshot_atlas_storage *storage = &snapshot->atlases[i];
        tp_snapshot_atlas *atlas = &storage->dto;
        atlas->id = source->id;
        atlas->name = source->name;
        atlas->max_size = source->max_size;
        atlas->padding = source->padding;
        atlas->margin = source->margin;
        atlas->extrude = source->extrude;
        atlas->alpha_threshold = source->alpha_threshold;
        atlas->max_vertices = source->max_vertices;
        atlas->shape = source->shape;
        atlas->allow_transform = source->allow_transform;
        atlas->power_of_two = source->power_of_two;
        atlas->pixels_per_unit = source->pixels_per_unit;
        atlas->source_count = source->source_count;
        atlas->sprite_count = source->sprite_count;
        atlas->animation_count = source->animation_count;
        atlas->target_count = source->target_count;

        if (source->source_count > 0) {
            storage->sources = (tp_snapshot_source *)snapshot_calloc(
                (size_t)source->source_count, sizeof *storage->sources);
        }
        if (source->sprite_count > 0) {
            storage->sprites = (tp_snapshot_sprite *)snapshot_calloc(
                (size_t)source->sprite_count, sizeof *storage->sprites);
        }
        if (source->animation_count > 0) {
            storage->animations = (tp_snapshot_animation *)snapshot_calloc(
                (size_t)source->animation_count, sizeof *storage->animations);
            storage->frame_offsets = (int *)snapshot_calloc(
                (size_t)source->animation_count + 1U, sizeof *storage->frame_offsets);
        }
        if (source->target_count > 0) {
            storage->targets = (tp_snapshot_target *)snapshot_calloc(
                (size_t)source->target_count, sizeof *storage->targets);
        }
        if ((source->source_count > 0 && !storage->sources) ||
            (source->sprite_count > 0 && !storage->sprites) ||
            (source->animation_count > 0 && (!storage->animations || !storage->frame_offsets)) ||
            (source->target_count > 0 && !storage->targets)) {
            tp_session_snapshot_destroy(snapshot);
            return tp_error_set(err, TP_STATUS_OOM, "snapshot nested DTO allocation failed");
        }
        int frame_count = 0;
        for (int j = 0; j < source->animation_count; ++j) {
            storage->frame_offsets[j] = frame_count;
            frame_count += source->animations[j].frame_count;
        }
        if (source->animation_count > 0) {
            storage->frame_offsets[source->animation_count] = frame_count;
        }
        if (frame_count > 0) {
            storage->frames = (tp_snapshot_frame *)snapshot_calloc(
                (size_t)frame_count, sizeof *storage->frames);
            if (!storage->frames) {
                tp_session_snapshot_destroy(snapshot);
                return tp_error_set(err, TP_STATUS_OOM, "snapshot frame DTO allocation failed");
            }
        }
        for (int j = 0; j < source->source_count; ++j) {
            storage->sources[j].id = source->sources[j].id;
            storage->sources[j].kind = (tp_snapshot_source_kind)source->sources[j].kind;
            storage->sources[j].path = source->sources[j].path;
        }
        for (int j = 0; j < source->sprite_count; ++j) {
            const tp_project_sprite *input = &source->sprites[j];
            tp_snapshot_sprite *output = &storage->sprites[j];
            output->id = tp_sprite_id(input->source_ref, input->src_key);
            output->source_id = input->source_ref;
            output->source_key = input->src_key;
            output->name = input->name;
            output->origin_x = input->origin_x;
            output->origin_y = input->origin_y;
            memcpy(output->slice9_lrtb, input->slice9_lrtb, sizeof output->slice9_lrtb);
            output->rename = input->rename;
            output->override_shape = input->ov_shape;
            output->override_allow_rotate = input->ov_allow_rotate;
            output->override_max_vertices = input->ov_max_vertices;
            output->override_margin = input->ov_margin;
            output->override_extrude = input->ov_extrude;
        }
        for (int j = 0; j < source->animation_count; ++j) {
            const tp_project_anim *input = &source->animations[j];
            tp_snapshot_animation *output = &storage->animations[j];
            output->id = input->id;
            output->name = input->name;
            output->fps = input->fps;
            output->playback = input->playback;
            output->flip_h = input->flip_h;
            output->flip_v = input->flip_v;
            output->frame_count = input->frame_count;
            const int offset = storage->frame_offsets[j];
            for (int k = 0; k < input->frame_count; ++k) {
                storage->frames[offset + k].sprite_id = tp_sprite_id(
                    input->frames[k].source_ref, input->frames[k].src_key);
                storage->frames[offset + k].source_id = input->frames[k].source_ref;
                storage->frames[offset + k].source_key = input->frames[k].src_key;
                storage->frames[offset + k].name = input->frames[k].name;
            }
        }
        for (int j = 0; j < source->target_count; ++j) {
            storage->targets[j].id = source->targets[j].id;
            storage->targets[j].exporter_id = source->targets[j].exporter_id;
            storage->targets[j].out_path = source->targets[j].out_path;
            storage->targets[j].enabled = source->targets[j].enabled;
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_session_snapshot_load(const char *path,
                                   tp_session_snapshot **out, tp_error *err) {
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot load requires path and output");
    }
    *out = NULL;
    char canonical[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_project_path_canonical(
        path, canonical, sizeof canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    tp_id128 before;
    status = tp_identity_file_fingerprint(canonical, &before, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_project *project = NULL;
    tp_id128 loaded;
    status = tp_project_load_with_fingerprint(canonical, &project, &loaded, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (!tp_id128_eq(before, loaded)) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                            "project changed while it was read");
    }
    tp_session_snapshot *snapshot =
        (tp_session_snapshot *)snapshot_calloc(1U, sizeof *snapshot);
    if (!snapshot) {
        tp_project_destroy(project);
        return tp_error_set(err, TP_STATUS_OOM, "snapshot allocation failed");
    }
    snapshot->generation = tp_project_generation_create_owned(project);
    if (!snapshot->generation) {
        tp_project_destroy(project);
        free(snapshot);
        return tp_error_set(err, TP_STATUS_OOM,
                            "snapshot project generation allocation failed");
    }
    snapshot->project = tp_project_generation_project(snapshot->generation);
    snapshot->atlas_count = snapshot->project->atlas_count;
    snapshot->recovery_health.notice_id =
        TP_SESSION_NOTICE_RECOVERY_DEGRADED;
    snapshot->recovery_health.available = true;
    snapshot->recovery_health.first_cause = TP_STATUS_OK;
    snapshot->identity.kind = TP_IDENTITY_SAVED;
    (void)snprintf(snapshot->identity.canonical_path,
                   sizeof snapshot->identity.canonical_path, "%s", canonical);
    snapshot->saved_file_fingerprint = loaded;
    snapshot->has_saved_file_fingerprint = true;
    status = snapshot_materialize(snapshot, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    *out = snapshot;
    return TP_STATUS_OK;
}

tp_status tp_session_snapshot_apply_preview(
    const tp_session_snapshot *snapshot, const tp_txn_request *request,
    tp_txn_result *result, tp_error *err) {
    if (!snapshot || !request) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot preview requires snapshot and request");
    }
    return tp_model__apply_snapshot_preview(
        snapshot->project, snapshot->revision, request, result, err);
}

void tp_session_snapshot_destroy(tp_session_snapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    if (snapshot->atlases) {
        for (int i = 0; i < snapshot->atlas_count; ++i) {
            free(snapshot->atlases[i].sources);
            free(snapshot->atlases[i].sprites);
            free(snapshot->atlases[i].animations);
            free(snapshot->atlases[i].frames);
            free(snapshot->atlases[i].frame_offsets);
            free(snapshot->atlases[i].targets);
        }
    }
    free(snapshot->atlases);
    tp_project_generation_release(snapshot->generation);
    free(snapshot);
}
