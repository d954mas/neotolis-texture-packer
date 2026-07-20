/* Immutable snapshot query and selector surface. */
#include "tp_core/tp_session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"
#include "tp_session_snapshot_internal.h"

static tp_status resolve_snapshot_source_path(const tp_project *project,
                                              const char *source_path,
                                              char *out, size_t capacity,
                                              tp_error *err) {
    const tp_status status = tp_project_resolve_source_path(
        project, source_path, out, capacity);
    if (status == TP_STATUS_OK) {
        return TP_STATUS_OK;
    }
    if (status == TP_STATUS_PATH_NOT_ABSOLUTE) {
        return tp_error_set(err, status,
                            "snapshot source path has no absolute base");
    }
    if (status == TP_STATUS_OUT_OF_BOUNDS) {
        return tp_error_set(err, status,
                            "resolved snapshot source path exceeds output capacity");
    }
    return tp_error_set(err, status,
                        "snapshot source path resolution failed");
}

const tp_project *tp_session_snapshot_project_internal(
    const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->project : NULL;
}
// #endregion

// #region scalar & DTO queries
int64_t tp_session_snapshot_revision(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->revision : 0;
}

uint64_t tp_session_snapshot_model_generation(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->model_generation : 0U;
}

uint64_t tp_session_snapshot_admission_sequence(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->admission_sequence : 0U;
}

uint64_t tp_session_snapshot_source_generation(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->source_generation : 0U;
}

tp_session_input_token tp_session_snapshot_input_token(
    const tp_session_snapshot *snapshot) {
    const tp_session_input_token token = {
        .model_generation = snapshot ? snapshot->model_generation : 0U,
        .source_generation = snapshot ? snapshot->source_generation : 0U,
    };
    return token;
}

bool tp_session_input_token_equal(tp_session_input_token left,
                                  tp_session_input_token right) {
    return left.model_generation == right.model_generation &&
           left.source_generation == right.source_generation;
}

uint64_t tp_session_snapshot_event_sequence(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->event_sequence : 0U;
}

bool tp_session_snapshot_dirty(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->dirty;
}

bool tp_session_snapshot_recovery_available(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->recovery_health.available;
}

tp_session_recovery_health tp_session_snapshot_recovery_health_query(
    const tp_session_snapshot *snapshot) {
    if (snapshot) {
        return snapshot->recovery_health;
    }
    const tp_session_recovery_health health = {
        .notice_id = TP_SESSION_NOTICE_RECOVERY_DEGRADED,
        .first_cause = TP_STATUS_OK,
    };
    return health;
}

tp_session_identity tp_session_snapshot_identity(const tp_session_snapshot *snapshot) {
    tp_session_identity identity;
    memset(&identity, 0, sizeof identity);
    if (snapshot) {
        identity = snapshot->identity;
    }
    return identity;
}

int tp_session_snapshot_project_schema_version(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->project ? TP_PROJECT_SCHEMA_VERSION : 0;
}

const char *tp_session_snapshot_project_dir(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->project && snapshot->project->project_dir
               ? snapshot->project->project_dir
               : "";
}

const char *tp_session_snapshot_canonical_path(const tp_session_snapshot *snapshot) {
    return snapshot && snapshot->identity.kind == TP_IDENTITY_SAVED
               ? snapshot->identity.canonical_path
               : "";
}

bool tp_session_snapshot_saved_file_fingerprint(
    const tp_session_snapshot *snapshot, tp_id128 *out_fingerprint) {
    if (!snapshot || !out_fingerprint || !snapshot->has_saved_file_fingerprint) {
        return false;
    }
    *out_fingerprint = snapshot->saved_file_fingerprint;
    return true;
}

int tp_session_snapshot_atlas_count(const tp_session_snapshot *snapshot) {
    return snapshot ? snapshot->atlas_count : 0;
}

const tp_snapshot_atlas *tp_session_snapshot_atlas_at(const tp_session_snapshot *snapshot,
                                                      int index) {
    if (!snapshot || index < 0 || index >= snapshot->atlas_count) {
        return NULL;
    }
    return &snapshot->atlases[index].dto;
}

const tp_snapshot_atlas *tp_session_snapshot_atlas_by_id(const tp_session_snapshot *snapshot,
                                                         tp_id128 id) {
    if (!snapshot || tp_id128_is_nil(id)) {
        return NULL;
    }
    for (int i = 0; i < snapshot->atlas_count; ++i) {
        if (tp_id128_eq(snapshot->atlases[i].dto.id, id)) {
            return &snapshot->atlases[i].dto;
        }
    }
    return NULL;
}

static const tp_snapshot_atlas_storage *atlas_storage(const tp_session_snapshot *snapshot,
                                                      tp_id128 atlas_id) {
    if (!snapshot || tp_id128_is_nil(atlas_id)) {
        return NULL;
    }
    for (int i = 0; i < snapshot->atlas_count; ++i) {
        if (tp_id128_eq(snapshot->atlases[i].dto.id, atlas_id)) {
            return &snapshot->atlases[i];
        }
    }
    return NULL;
}

const tp_snapshot_source *tp_session_snapshot_source_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.source_count ? &atlas->sources[index] : NULL;
}

tp_status tp_session_snapshot_source_resolved_at(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    const tp_snapshot_source **out_source, char *out_path, size_t capacity,
    tp_error *err) {
    if (out_source) {
        *out_source = NULL;
    }
    if (out_path && capacity > 0U) {
        out_path[0] = '\0';
    }
    if (!snapshot || !out_source || !out_path || capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid direct snapshot source query");
    }
    if (atlas_index < 0 || atlas_index >= snapshot->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "snapshot atlas index is out of bounds");
    }
    const tp_snapshot_atlas_storage *atlas = &snapshot->atlases[atlas_index];
    if (source_index < 0 || source_index >= atlas->dto.source_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "snapshot source index is out of bounds");
    }
    const tp_snapshot_source *source = &atlas->sources[source_index];
    *out_source = source;
    return resolve_snapshot_source_path(snapshot->project, source->path,
                                        out_path, capacity, err);
}

const tp_snapshot_source *tp_session_snapshot_source_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 source_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(source_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.source_count; ++i) {
        if (tp_id128_eq(atlas->sources[i].id, source_id)) {
            return &atlas->sources[i];
        }
    }
    return NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.sprite_count ? &atlas->sprites[index] : NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_at_index(
    const tp_session_snapshot *snapshot, int atlas_index, int sprite_index) {
    if (!snapshot || atlas_index < 0 || atlas_index >= snapshot->atlas_count) {
        return NULL;
    }
    const tp_snapshot_atlas_storage *atlas = &snapshot->atlases[atlas_index];
    return sprite_index >= 0 && sprite_index < atlas->dto.sprite_count
               ? &atlas->sprites[sprite_index]
               : NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_by_key(const tp_session_snapshot *snapshot,
                                                            tp_id128 atlas_id, tp_id128 source_id,
                                                            const char *source_key) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(source_id) || !source_key) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.sprite_count; ++i) {
        if (tp_id128_eq(atlas->sprites[i].source_id, source_id) && atlas->sprites[i].source_key &&
            strcmp(atlas->sprites[i].source_key, source_key) == 0) {
            return &atlas->sprites[i];
        }
    }
    return NULL;
}

const tp_snapshot_sprite *tp_session_snapshot_sprite_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 sprite_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(sprite_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.sprite_count; ++i) {
        if (tp_id128_eq(atlas->sprites[i].id, sprite_id)) {
            return &atlas->sprites[i];
        }
    }
    return NULL;
}

const tp_snapshot_animation *tp_session_snapshot_animation_at(const tp_session_snapshot *snapshot,
                                                              tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.animation_count ? &atlas->animations[index] : NULL;
}

const tp_snapshot_animation *tp_session_snapshot_animation_by_id(const tp_session_snapshot *snapshot,
                                                                 tp_id128 atlas_id, tp_id128 animation_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(animation_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            return &atlas->animations[i];
        }
    }
    return NULL;
}

const tp_snapshot_frame *tp_session_snapshot_animation_frame_at(const tp_session_snapshot *snapshot,
                                                                tp_id128 atlas_id, tp_id128 animation_id,
                                                                int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || index < 0) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            return index < atlas->animations[i].frame_count
                       ? &atlas->frames[atlas->frame_offsets[i] + index]
                       : NULL;
        }
    }
    return NULL;
}

const tp_snapshot_frame *tp_session_snapshot_animation_frames(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, int *out_count) {
    if (out_count) {
        *out_count = 0;
    }
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(animation_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.animation_count; ++i) {
        if (tp_id128_eq(atlas->animations[i].id, animation_id)) {
            const int count = atlas->animations[i].frame_count;
            if (out_count) {
                *out_count = count;
            }
            return count > 0 ? &atlas->frames[atlas->frame_offsets[i]] : NULL;
        }
    }
    return NULL;
}

const tp_snapshot_target *tp_session_snapshot_target_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    return atlas && index >= 0 && index < atlas->dto.target_count ? &atlas->targets[index] : NULL;
}

const tp_snapshot_target *tp_session_snapshot_target_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 target_id) {
    const tp_snapshot_atlas_storage *atlas = atlas_storage(snapshot, atlas_id);
    if (!atlas || tp_id128_is_nil(target_id)) {
        return NULL;
    }
    for (int i = 0; i < atlas->dto.target_count; ++i) {
        if (tp_id128_eq(atlas->targets[i].id, target_id)) {
            return &atlas->targets[i];
        }
    }
    return NULL;
}
// #endregion

// #region selectors & resolution
tp_status tp_session_snapshot_resolve_path(const tp_session_snapshot *snapshot,
                                           tp_id128 atlas_id, tp_id128 source_id,
                                           char *out, size_t capacity, tp_error *err) {
    if (!snapshot || !out || capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid snapshot path query");
    }
    const tp_project_atlas *atlas =
        tp_project_atlas_by_id(snapshot->project, atlas_id);
    if (!atlas) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "snapshot atlas id was not found");
    }
    const tp_project_source *source =
        tp_project_atlas_source_by_id(atlas, source_id);
    if (!source) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "snapshot source id was not found");
    }
    return resolve_snapshot_source_path(snapshot->project, source->path, out,
                                        capacity, err);
}


tp_status tp_session_snapshot_resolve_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_scope,
    tp_selector_kind want, const char *selector, tp_selector_result *out,
    tp_selector_candidates *candidates, tp_error *err) {
    if (!snapshot || !snapshot->project || !selector || !out ||
        want <= TP_SEL_NONE || want > TP_SEL_TARGET) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot selector needs a snapshot, structural kind, selector, and output");
    }
    const tp_project *project = snapshot->project;
    tp_project scoped;
    int atlas_index = -1;
    if (!tp_id128_is_nil(atlas_scope)) {
        atlas_index = tp_project_find_atlas_by_id(project, atlas_scope);
        if (atlas_index < 0) {
            return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                "selector atlas scope does not exist");
        }
        scoped = *project;
        scoped.atlases = &project->atlases[atlas_index];
        scoped.atlas_count = 1;
        project = &scoped;
    }

    char qualified[TP_IDENTITY_PATH_MAX + 16U];
    const char *resolved_selector = selector;
    const char *kind_token = tp_selector_kind_token(want);
    const size_t kind_token_len = strlen(kind_token);
    const bool already_qualified =
        strncmp(selector, kind_token, kind_token_len) == 0 &&
        selector[kind_token_len] == ':';
    tp_id_kind id_kind = TP_ID_KIND_INVALID;
    tp_id128 parsed_id = tp_id128_nil();
    if (tp_id_parse(selector, &id_kind, &parsed_id, NULL) != TP_STATUS_OK &&
        !already_qualified) {
        const int written = snprintf(qualified, sizeof qualified, "%s:%s",
                                     kind_token, selector);
        if (written < 0 || (size_t)written >= sizeof qualified) {
            return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                "selector is too long");
        }
        resolved_selector = qualified;
    }
    tp_status status = tp_selector_resolve(project, resolved_selector, NULL, -1,
                                            out, candidates, err);
    if (status == TP_STATUS_OK && out->kind != want) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "selector resolved to %s, expected %s",
                            tp_selector_kind_token(out->kind),
                            tp_selector_kind_token(want));
    }
    if (atlas_index >= 0) {
        if (status == TP_STATUS_OK) {
            out->atlas_index = atlas_index;
        }
        if (candidates) {
            for (int i = 0; i < candidates->count; ++i) {
                candidates->v[i].atlas_index = atlas_index;
            }
        }
    }
    return status;
}

tp_status tp_session_snapshot_resolve_sprite_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, tp_selector_result *out, tp_id128 *out_source_id,
    char *out_source_key, size_t source_key_capacity,
    tp_selector_candidates *candidates, tp_error *err) {
    if (!snapshot || !snapshot->project || tp_id128_is_nil(atlas_id) ||
        !selector || !out || !out_source_id || !out_source_key ||
        source_key_capacity == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "sprite selector needs a snapshot, atlas, selector, and outputs");
    }
    out_source_key[0] = '\0';
    *out_source_id = tp_id128_nil();
    const int atlas_index = tp_project_find_atlas_by_id(snapshot->project,
                                                        atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "sprite selector atlas does not exist");
    }

    /* An explicit source-id compound is already a unique structural address and
     * must remain usable while its file is missing (orphan metadata is retained
     * until the source key returns). Normalize it without consulting the disk. */
    const char *compound_colon = strchr(selector, ':');
    if (compound_colon) {
        const size_t prefix_length = (size_t)(compound_colon - selector);
        if (prefix_length < TP_ID_TEXT_CAP) {
            char prefix[TP_ID_TEXT_CAP];
            memcpy(prefix, selector, prefix_length);
            prefix[prefix_length] = '\0';
            tp_id_kind kind = TP_ID_KIND_INVALID;
            tp_id128 source_id = tp_id128_nil();
            if (tp_id_parse(prefix, &kind, &source_id, NULL) == TP_STATUS_OK &&
                kind == TP_ID_KIND_SOURCE) {
                if (!tp_session_snapshot_source_by_id(snapshot, atlas_id,
                                                      source_id)) {
                    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                        "sprite selector source does not exist in the atlas");
                }
                tp_status normalize_status = tp_srckey_normalize(
                    compound_colon + 1, out_source_key, source_key_capacity,
                    err);
                if (normalize_status != TP_STATUS_OK) {
                    return normalize_status;
                }
                out->kind = TP_SEL_SPRITE;
                out->id = tp_sprite_id(source_id, out_source_key);
                out->atlas_index = atlas_index;
                *out_source_id = source_id;
                return TP_STATUS_OK;
            }
        }
    }

    /* A persisted orphan can also be addressed by its deterministic sprite id;
     * unlike a human key, this does not need a live index to be unambiguous. */
    tp_id128 parsed_sprite = tp_id128_nil();
    if (tp_sprite_id_parse(selector, &parsed_sprite, NULL) == TP_STATUS_OK) {
        const tp_snapshot_sprite *persisted =
            tp_session_snapshot_sprite_by_id(snapshot, atlas_id, parsed_sprite);
        if (persisted) {
            if (!tp_session_snapshot_source_by_id(snapshot, atlas_id,
                                                  persisted->source_id)) {
                return tp_error_set(err, TP_STATUS_NOT_FOUND,
                                    "persisted sprite source does not exist in the atlas");
            }
            const size_t key_length = strlen(persisted->source_key);
            if (key_length >= source_key_capacity) {
                return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                    "resolved sprite key exceeds output capacity");
            }
            out->kind = TP_SEL_SPRITE;
            out->id = persisted->id;
            out->atlas_index = atlas_index;
            *out_source_id = persisted->source_id;
            memcpy(out_source_key, persisted->source_key, key_length + 1U);
            return TP_STATUS_OK;
        }
    }

    tp_sprite_index index;
    tp_status status = tp_sprite_index_build(snapshot->project, atlas_index,
                                              &index, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    char qualified[TP_IDENTITY_PATH_MAX + 16U];
    const char *resolved_selector = selector;
    bool canonical_form = false;
    if (tp_sprite_id_parse(selector, &parsed_sprite, NULL) == TP_STATUS_OK ||
        strncmp(selector, "sprite:", sizeof("sprite:") - 1U) == 0) {
        canonical_form = true;
    } else {
        const char *colon = strchr(selector, ':');
        if (colon) {
            const size_t prefix_length = (size_t)(colon - selector);
            if (prefix_length < TP_ID_TEXT_CAP) {
                char prefix[TP_ID_TEXT_CAP];
                memcpy(prefix, selector, prefix_length);
                prefix[prefix_length] = '\0';
                tp_id_kind kind = TP_ID_KIND_INVALID;
                tp_id128 parsed_source = tp_id128_nil();
                canonical_form =
                    tp_id_parse(prefix, &kind, &parsed_source, NULL) ==
                        TP_STATUS_OK &&
                    kind == TP_ID_KIND_SOURCE;
            }
        }
    }
    if (!canonical_form) {
        const int written = snprintf(qualified, sizeof qualified, "sprite:%s",
                                     selector);
        if (written < 0 || (size_t)written >= sizeof qualified) {
            tp_sprite_index_free(&index);
            return tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                "sprite selector is too long");
        }
        resolved_selector = qualified;
    }

    status = tp_selector_resolve(snapshot->project, resolved_selector, &index,
                                 atlas_index, out, candidates, err);
    if (status == TP_STATUS_OK && out->kind != TP_SEL_SPRITE) {
        status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                              "selector resolved to %s, expected sprite",
                              tp_selector_kind_token(out->kind));
    }
    if (status == TP_STATUS_OK) {
        const tp_sprite_ref *ref = tp_sprite_index_by_id(&index, out->id);
        if (!ref) {
            status = tp_error_set(err, TP_STATUS_NOT_FOUND,
                                  "resolved sprite is no longer present");
        } else {
            const size_t key_length = strlen(ref->source_key);
            if (key_length >= source_key_capacity) {
                status = tp_error_set(err, TP_STATUS_OUT_OF_RANGE,
                                      "resolved sprite key exceeds output capacity");
            } else {
                *out_source_id = ref->source_id;
                memcpy(out_source_key, ref->source_key, key_length + 1U);
            }
        }
    }
    tp_sprite_index_free(&index);
    return status;
}

bool tp_session_snapshot_target_out_path_shared(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, const char *out_path) {
    if (!snapshot || !out_path) {
        return false;
    }
    const tp_project_atlas *atlas =
        tp_project_atlas_by_id(snapshot->project, atlas_id);
    if (!atlas) {
        return false;
    }
    const tp_project_target *target =
        tp_project_atlas_target_by_id(atlas, target_id);
    return target && tp_project_out_path_shared(snapshot->project, out_path,
                                                target);
}

tp_status tp_session_snapshot_next_atlas_defaults(
    const tp_session_snapshot *snapshot, char *name, size_t name_cap,
    char *out_path, size_t out_path_cap, const char **exporter_id,
    bool *target_enabled, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "atlas defaults need a snapshot");
    }
    return tp_project_next_atlas_defaults(snapshot->project, name, name_cap,
                                          out_path, out_path_cap, exporter_id,
                                          target_enabled, err);
}

tp_status tp_session_snapshot_next_animation_name(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, const char *base,
    char *name, size_t name_cap, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "animation defaults need a snapshot");
    }
    return tp_project_next_animation_name(snapshot->project, atlas_id, base,
                                          name, name_cap, err);
}

tp_status tp_session_snapshot_target_defaults(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char **exporter_id, char *out_path, size_t out_path_cap,
    bool *enabled, tp_error *err) {
    if (!snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target defaults need a snapshot");
    }
    return tp_project_target_defaults(snapshot->project, atlas_id, exporter_id,
                                      out_path, out_path_cap, enabled, err);
}

tp_status tp_session_snapshot_resolve_frame(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, const char *selector, int *out_index,
    tp_error *err) {
    if (!snapshot || !selector || !out_index) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "frame query needs snapshot, selector, and output");
    }
    char *end = NULL;
    const long parsed = strtol(selector, &end, 10);
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(snapshot, atlas_id, animation_id);
    if (!animation) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "animation was not found");
    }
    if (end != selector && *end == '\0') {
        if (parsed >= 0 && parsed < animation->frame_count) {
            *out_index = (int)parsed;
            return TP_STATUS_OK;
        }
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "animation has no frame at index %ld", parsed);
    }
    int match = -1;
    int count = 0;
    for (int i = 0; i < animation->frame_count; ++i) {
        const tp_snapshot_frame *frame =
            tp_session_snapshot_animation_frame_at(snapshot, atlas_id,
                                                    animation_id, i);
        if (frame && frame->name && strcmp(frame->name, selector) == 0) {
            match = i;
            count++;
        }
    }
    if (count > 1) {
        return tp_error_set(err, TP_STATUS_AMBIGUOUS_SELECTOR,
                            "frame selector '%s' is ambiguous (%d matches)",
                            selector, count);
    }
    if (count == 1) {
        *out_index = match;
        return TP_STATUS_OK;
    }
    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                        "animation has no frame '%s'", selector);
}

tp_status tp_session_snapshot_resolve_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, const tp_snapshot_target **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!snapshot || !selector || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target query needs snapshot, selector, and output");
    }
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(snapshot,
                                                                     atlas_id);
    if (!atlas) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "target atlas was not found");
    }
    char *end = NULL;
    const long parsed = strtol(selector, &end, 10);
    if (end != selector && *end == '\0') {
        const tp_snapshot_target *target =
            parsed >= 0 && parsed < atlas->target_count
                ? tp_session_snapshot_target_at(snapshot, atlas_id, (int)parsed)
                : NULL;
        if (target) {
            *out = target;
            return TP_STATUS_OK;
        }
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "atlas has no target at index %ld", parsed);
    }
    tp_selector_result resolved;
    tp_status status = tp_session_snapshot_resolve_selector(
        snapshot, atlas_id, TP_SEL_TARGET, selector, &resolved, NULL, err);
    if (status == TP_STATUS_OK) {
        *out = tp_session_snapshot_target_by_id(snapshot, atlas_id, resolved.id);
        return *out ? TP_STATUS_OK
                    : tp_error_set(err, TP_STATUS_NOT_FOUND,
                                   "resolved target was not present in the snapshot");
    }
    if (status != TP_STATUS_NOT_FOUND) {
        return status;
    }
    int match = -1;
    int count = 0;
    for (int i = 0; i < atlas->target_count; ++i) {
        const tp_snapshot_target *target = tp_session_snapshot_target_at(
            snapshot, atlas_id, i);
        if (target && target->exporter_id &&
            strcmp(target->exporter_id, selector) == 0) {
            match = i;
            count++;
        }
    }
    if (count > 1) {
        return tp_error_set(err, TP_STATUS_AMBIGUOUS_SELECTOR,
                            "target selector '%s' is ambiguous (%d matches)",
                            selector, count);
    }
    if (count == 1) {
        *out = tp_session_snapshot_target_at(snapshot, atlas_id, match);
        return TP_STATUS_OK;
    }
    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                        "atlas has no target '%s'", selector);
}
// #endregion

// #region serialization
tp_status tp_session_snapshot_serialize(const tp_session_snapshot *snapshot,
                                        char **out, size_t *out_len,
                                        tp_error *err) {
    if (!snapshot || !out || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "snapshot serialization requires snapshot and outputs");
    }
    return tp_project_save_buffer(snapshot->project, out, out_len, err);
}

tp_id128 tp_session_snapshot_semantic_identity(
    const tp_session_snapshot *snapshot) {
    return snapshot ? tp_semantic_identity(snapshot->project) : tp_id128_nil();
}
// #endregion
