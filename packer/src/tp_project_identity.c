#include "tp_project_identity_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_srckey.h"
#include "tp_pack_constraints_internal.h"
#include "tp_project_internal.h"
#include "tp_source_path_text_internal.h"
#include "tp_utf8_internal.h"

tp_status tp_project_validate_sprite_pack_overrides(
    const tp_project_sprite *sprite, tp_error *error) {
    if (!sprite) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "sprite override record is NULL");
    }
    const tp_pack_sprite_constraint_input input = {
        .has_shape = sprite->ov_shape != TP_PROJECT_OV_INHERIT,
        .shape = sprite->ov_shape,
        .has_allow_rotate =
            sprite->ov_allow_rotate != TP_PROJECT_OV_INHERIT,
        .allow_rotate = sprite->ov_allow_rotate,
        .has_max_vertices =
            sprite->ov_max_vertices != TP_PROJECT_OV_INHERIT,
        .max_vertices = sprite->ov_max_vertices,
        .has_margin = sprite->ov_margin != TP_PROJECT_OV_INHERIT,
        .margin = sprite->ov_margin,
        .has_extrude = sprite->ov_extrude != TP_PROJECT_OV_INHERIT,
        .extrude = sprite->ov_extrude,
    };
    const tp_pack_sprite_constraint_facts facts =
        tp_pack_sprite_constraint_facts_of(&input);
    if (facts.shape_not_wire_representable) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "sprite ov_shape %d is not representable",
                            sprite->ov_shape);
    }
    if (facts.allow_rotate_not_wire_representable) {
        return tp_error_set(
            error, TP_STATUS_BAD_PROJECT,
            "sprite ov_allow_rotate %d must be 0 or inherit",
            sprite->ov_allow_rotate);
    }
    if (facts.max_vertices_not_wire_representable) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "sprite ov_max_vertices %d is not representable",
                            sprite->ov_max_vertices);
    }
    if (facts.margin_not_wire_representable) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "sprite ov_margin %d is not representable",
                            sprite->ov_margin);
    }
    if (facts.extrude_not_wire_representable) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "sprite ov_extrude %d is not representable",
                            sprite->ov_extrude);
    }
    return TP_STATUS_OK;
}

static _Thread_local bool s_measure_id_validation;
static _Thread_local size_t s_id_validation_probes;

void tp_project__test_id_validation_work_reset(void) {
    s_id_validation_probes = 0U;
    s_measure_id_validation = true;
}

size_t tp_project__test_id_validation_work_take(void) {
    s_measure_id_validation = false;
    return s_id_validation_probes;
}

typedef struct tp_identity_slot {
    tp_id128 *model;
    tp_id128 value;
} tp_identity_slot;

static bool size_add(size_t *value, size_t added) {
    if (*value > SIZE_MAX - added) {
        return false;
    }
    *value += added;
    return true;
}

static tp_status structural_id_count(const tp_project *project, size_t *out,
                                     tp_error *error) {
    *out = 0U;
    if (project->atlas_count < 0 || project->atlas_cap < 0 ||
        project->atlas_count > project->atlas_cap ||
        (project->atlas_count > 0 && !project->atlases)) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "project atlas storage is invalid");
    }
    size_t count = 0U;
    for (int ai = 0; ai < project->atlas_count; ai++) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        if (atlas->source_count < 0 || atlas->source_cap < 0 ||
            atlas->sprite_count < 0 || atlas->sprite_cap < 0 ||
            atlas->animation_count < 0 || atlas->animation_cap < 0 ||
            atlas->target_count < 0 || atlas->target_cap < 0 ||
            atlas->source_count > atlas->source_cap ||
            atlas->sprite_count > atlas->sprite_cap ||
            atlas->animation_count > atlas->animation_cap ||
            atlas->target_count > atlas->target_cap ||
            (atlas->source_count > 0 && !atlas->sources) ||
            (atlas->sprite_count > 0 && !atlas->sprites) ||
            (atlas->animation_count > 0 && !atlas->animations) ||
            (atlas->target_count > 0 && !atlas->targets)) {
            return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                "atlas collection storage is invalid");
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            const tp_project_anim *animation = &atlas->animations[i];
            if (animation->frame_count < 0 || animation->frame_cap < 0 ||
                animation->frame_count > animation->frame_cap ||
                (animation->frame_count > 0 && !animation->frames)) {
                return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                    "animation frame storage is invalid");
            }
        }
        if (!size_add(&count, 1U) ||
            !size_add(&count, (size_t)atlas->source_count) ||
            !size_add(&count, (size_t)atlas->animation_count) ||
            !size_add(&count, (size_t)atlas->target_count)) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "project has too many structural ids");
        }
    }
    *out = count;
    return TP_STATUS_OK;
}

static tp_status ids_are_unique(const tp_identity_slot *ids, size_t count,
                                tp_error *error) {
    if (count == 0U) {
        return TP_STATUS_OK;
    }
    if (count > SIZE_MAX / 2U) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "too many structural ids");
    }
    size_t capacity = 16U;
    while (capacity < count * 2U) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "too many structural ids");
        }
        capacity *= 2U;
    }
    size_t *table = (size_t *)calloc(capacity, sizeof *table);
    if (!table) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "assigning project ids: out of memory");
    }
    tp_project__test_note_id_resources(count * sizeof *ids,
                                       capacity * sizeof *table);
    tp_status status = TP_STATUS_OK;
    for (size_t i = 0U; i < count && status == TP_STATUS_OK; i++) {
        if (tp_id128_is_nil(ids[i].value)) {
            status = tp_error_set(error, TP_STATUS_ID_MALFORMED,
                                  "structural id assignment produced nil");
            break;
        }
        size_t bucket = (size_t)tp_id128_bucket(ids[i].value) & (capacity - 1U);
        bool inserted = false;
        for (size_t probe = 0U; probe < capacity; probe++) {
            if (s_measure_id_validation) {
                s_id_validation_probes++;
            }
            if (table[bucket] == 0U) {
                table[bucket] = i + 1U;
                inserted = true;
                break;
            }
            const size_t existing = table[bucket] - 1U;
            if (tp_id128_eq(ids[existing].value, ids[i].value)) {
                status = tp_error_set(error, TP_STATUS_DUPLICATE_ID,
                                      "structural ids must be unique");
                inserted = true;
                break;
            }
            bucket = (bucket + 1U) & (capacity - 1U);
        }
        if (!inserted) {
            status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                  "structural id index is full");
        }
    }
    free(table);
    return status;
}

static tp_status validate_structural_ids(const tp_project *project,
                                         size_t count,
                                         tp_error *error) {
    if (count == 0U) {
        return TP_STATUS_OK;
    }
    tp_identity_slot *ids = (tp_identity_slot *)calloc(count, sizeof *ids);
    if (!ids) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "validating structural ids: out of memory");
    }
    size_t n = 0U;
    for (int ai = 0; ai < project->atlas_count; ai++) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        ids[n++].value = atlas->id;
        for (int i = 0; i < atlas->source_count; i++) {
            ids[n++].value = atlas->sources[i].id;
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            ids[n++].value = atlas->animations[i].id;
        }
        for (int i = 0; i < atlas->target_count; i++) {
            ids[n++].value = atlas->targets[i].id;
        }
    }
    const tp_status status = ids_are_unique(ids, count, error);
    free(ids);
    return status;
}

bool tp_project_has_structural_id(const tp_project *project, tp_id128 id) {
    if (!project || tp_id128_is_nil(id)) {
        return false;
    }
    for (int ai = 0; ai < project->atlas_count; ai++) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        if (tp_id128_eq(atlas->id, id)) {
            return true;
        }
        for (int i = 0; i < atlas->source_count; i++) {
            if (tp_id128_eq(atlas->sources[i].id, id)) {
                return true;
            }
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            if (tp_id128_eq(atlas->animations[i].id, id)) {
                return true;
            }
        }
        for (int i = 0; i < atlas->target_count; i++) {
            if (tp_id128_eq(atlas->targets[i].id, id)) {
                return true;
            }
        }
    }
    return false;
}

tp_status tp_project_assign_missing_ids(tp_project *project, const tp_rng *rng,
                                        tp_error *error) {
    if (!project || !rng) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_assign_missing_ids: NULL argument");
    }
    size_t count = 0U;
    tp_status status = structural_id_count(project, &count, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (count == 0U) {
        return TP_STATUS_OK;
    }
    tp_identity_slot *ids = (tp_identity_slot *)calloc(count, sizeof *ids);
    if (!ids) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "assigning project ids: out of memory");
    }

    size_t n = 0U;
    for (int ai = 0; ai < project->atlas_count; ai++) {
        tp_project_atlas *atlas = &project->atlases[ai];
        ids[n++] = (tp_identity_slot){&atlas->id, atlas->id};
        for (int i = 0; i < atlas->source_count; i++) {
            ids[n++] = (tp_identity_slot){&atlas->sources[i].id,
                                          atlas->sources[i].id};
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            ids[n++] = (tp_identity_slot){&atlas->animations[i].id,
                                          atlas->animations[i].id};
        }
        for (int i = 0; i < atlas->target_count; i++) {
            ids[n++] = (tp_identity_slot){&atlas->targets[i].id,
                                          atlas->targets[i].id};
        }
    }

    status = TP_STATUS_OK;
    for (size_t i = 0U; i < count; i++) {
        if (tp_id128_is_nil(ids[i].value)) {
            status = tp_id128_generate(rng, &ids[i].value, error);
            if (status != TP_STATUS_OK) {
                break;
            }
        }
    }
    if (status == TP_STATUS_OK) {
        status = ids_are_unique(ids, count, error);
    }
    if (status == TP_STATUS_OK) {
        for (size_t i = 0U; i < count; i++) {
            *ids[i].model = ids[i].value;
        }
    }
    free(ids);
    return status;
}

static tp_status validate_reference(tp_id128 source_ref, const char *src_key,
                                    const char *name, const char *kind,
                                    tp_error *error) {
    if (tp_id128_is_nil(source_ref) || !src_key || !name) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "%s identity requires source, key, and derived name",
                            kind);
    }
    tp_error key_error = {0};
    const tp_status key_status = tp_srckey_validate_canonical(
        src_key, &key_error);
    if (key_status != TP_STATUS_OK) {
        if (key_status == TP_STATUS_OOM || key_status == TP_STATUS_OUT_OF_BOUNDS) {
            return tp_error_set(error, key_status, "%s key: %s", kind,
                                key_error.msg);
        }
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "%s key is not canonical: %s", kind, key_error.msg);
    }
    char derived[TP_SRCKEY_MAX];
    tp_sprite_export_key(src_key, derived, sizeof derived);
    if (strcmp(derived, name) != 0) {
        return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                            "%s derived name does not match its key", kind);
    }
    return TP_STATUS_OK;
}

typedef struct tp_source_id_index {
    const tp_project_source *sources;
    size_t *slots;
    size_t capacity;
} tp_source_id_index;

static tp_status validate_sources(const tp_project_atlas *atlas,
                                  tp_error *error) {
    if (atlas->source_count == 0) {
        return TP_STATUS_OK;
    }
    if ((size_t)atlas->source_count > SIZE_MAX / 2U) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "too many sources");
    }
    size_t capacity = 16U;
    const size_t needed = (size_t)atlas->source_count * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "too many sources");
        }
        capacity *= 2U;
    }
    size_t *slots = (size_t *)calloc(capacity, sizeof *slots);
    if (!slots) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "validating sources: out of memory");
    }

    tp_status status = TP_STATUS_OK;
    for (int i = 0; i < atlas->source_count && status == TP_STATUS_OK; i++) {
        const tp_project_source *source = &atlas->sources[i];
        if (!source->path || source->path[0] == '\0') {
            status = tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                  "source path must be non-empty");
            break;
        }
        const tp_status text_status =
            tp_source_path_text_admit(source->path);
        if (text_status == TP_STATUS_OUT_OF_BOUNDS) {
            status = tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                  "source path exceeds the supported limit");
            break;
        }
        if (source->kind != TP_SOURCE_KIND_FOLDER &&
            source->kind != TP_SOURCE_KIND_FILE) {
            status = tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                  "source kind is invalid");
            break;
        }
        tp_error utf8_error = {0};
        if (text_status == TP_STATUS_INVALID_UTF8) {
            (void)tp_utf8_validate_c_string(
                source->path, TP_STATUS_BAD_PROJECT, "source path",
                &utf8_error);
            status = tp_error_set(error, TP_STATUS_BAD_PROJECT, "%s",
                                  utf8_error.msg);
            break;
        }

        size_t bucket = (size_t)tp_source_path_text_hash(source->path) &
                        (capacity - 1U);
        for (size_t probe = 0U; probe < capacity; probe++) {
            if (slots[bucket] == 0U) {
                slots[bucket] = (size_t)i + 1U;
                break;
            }
            const tp_project_source *existing =
                &atlas->sources[slots[bucket] - 1U];
            if (tp_source_path_text_equal(existing->path, source->path)) {
                status = tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                      "duplicate source path '%s'",
                                      source->path);
                break;
            }
            bucket = (bucket + 1U) & (capacity - 1U);
        }
    }
    free(slots);
    return status;
}

static tp_status source_index_init(const tp_project_atlas *atlas,
                                   tp_source_id_index *index,
                                   tp_error *error) {
    memset(index, 0, sizeof *index);
    index->sources = atlas->sources;
    if (atlas->source_count == 0) {
        return TP_STATUS_OK;
    }
    if ((size_t)atlas->source_count > SIZE_MAX / 2U) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "too many source identities");
    }
    size_t capacity = 16U;
    const size_t needed = (size_t)atlas->source_count * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "too many source identities");
        }
        capacity *= 2U;
    }
    index->slots = (size_t *)calloc(capacity, sizeof *index->slots);
    if (!index->slots) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "validating source identities: out of memory");
    }
    index->capacity = capacity;
    for (int i = 0; i < atlas->source_count; i++) {
        const tp_id128 id = atlas->sources[i].id;
        size_t bucket = (size_t)tp_id128_bucket(id) & (capacity - 1U);
        while (index->slots[bucket] != 0U) {
            bucket = (bucket + 1U) & (capacity - 1U);
        }
        index->slots[bucket] = (size_t)i + 1U;
    }
    return TP_STATUS_OK;
}

static bool source_index_contains(const tp_source_id_index *index,
                                  tp_id128 id) {
    if (!index->slots || tp_id128_is_nil(id)) {
        return false;
    }
    size_t bucket = (size_t)tp_id128_bucket(id) & (index->capacity - 1U);
    for (size_t probe = 0U; probe < index->capacity; probe++) {
        const size_t stored = index->slots[bucket];
        if (stored == 0U) {
            return false;
        }
        if (tp_id128_eq(index->sources[stored - 1U].id, id)) {
            return true;
        }
        bucket = (bucket + 1U) & (index->capacity - 1U);
    }
    return false;
}

static uint64_t reference_hash(tp_id128 source_ref, const char *src_key) {
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0U; i < sizeof source_ref.bytes; i++) {
        hash ^= (uint64_t)source_ref.bytes[i];
        hash *= 1099511628211ULL;
    }
    for (const unsigned char *p = (const unsigned char *)src_key; *p; p++) {
        hash ^= (uint64_t)*p;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static tp_status validate_unique_sprites(const tp_project_atlas *atlas,
                                         tp_error *error) {
    if (atlas->sprite_count == 0) {
        return TP_STATUS_OK;
    }
    size_t capacity = 16U;
    while (capacity < (size_t)atlas->sprite_count * 2U) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "too many sprite overrides");
        }
        capacity *= 2U;
    }
    size_t *slots = (size_t *)calloc(capacity, sizeof *slots);
    if (!slots) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "validating sprite identities: out of memory");
    }
    tp_status status = TP_STATUS_OK;
    for (int i = 0; i < atlas->sprite_count && status == TP_STATUS_OK; i++) {
        const tp_project_sprite *sprite = &atlas->sprites[i];
        size_t bucket = (size_t)reference_hash(sprite->source_ref,
                                               sprite->src_key) &
                        (capacity - 1U);
        for (size_t probe = 0U; probe < capacity; probe++) {
            if (slots[bucket] == 0U) {
                slots[bucket] = (size_t)i + 1U;
                break;
            }
            const tp_project_sprite *existing =
                &atlas->sprites[slots[bucket] - 1U];
            if (tp_id128_eq(existing->source_ref, sprite->source_ref) &&
                strcmp(existing->src_key, sprite->src_key) == 0) {
                status = tp_error_set(
                    error, TP_STATUS_BAD_PROJECT,
                    "duplicate sprite override identity '%s'", sprite->src_key);
                break;
            }
            bucket = (bucket + 1U) & (capacity - 1U);
        }
    }
    free(slots);
    return status;
}

static tp_status validate_project(const tp_project *project,
                                  bool require_known_sources,
                                  tp_error *error) {
    if (!project) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_validate_canonical: NULL project");
    }
    size_t structural_count = 0U;
    tp_status status = structural_id_count(project, &structural_count, error);
    (void)structural_count;
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = validate_structural_ids(project, structural_count, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    for (int ai = 0; ai < project->atlas_count; ai++) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        if (!atlas->name || atlas->name[0] == '\0') {
            return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                "atlas name must be non-empty");
        }
        status = tp_utf8_validate_c_string(
            atlas->name, TP_STATUS_BAD_PROJECT, "atlas name", error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        status = validate_sources(atlas, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        tp_source_id_index sources;
        status = source_index_init(atlas, &sources, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        for (int i = 0; i < atlas->sprite_count; i++) {
            const tp_project_sprite *sprite = &atlas->sprites[i];
            status = tp_project_validate_sprite_pack_overrides(sprite, error);
            if (status != TP_STATUS_OK) {
                free(sources.slots);
                return status;
            }
            if (sprite->rename) {
                status = tp_utf8_validate_c_string(
                    sprite->rename, TP_STATUS_BAD_PROJECT, "sprite rename",
                    error);
                if (status != TP_STATUS_OK) {
                    free(sources.slots);
                    return status;
                }
            }
            status = validate_reference(sprite->source_ref, sprite->src_key,
                                        sprite->name, "sprite", error);
            if (status != TP_STATUS_OK) {
                free(sources.slots);
                return status;
            }
            if (require_known_sources &&
                !source_index_contains(&sources, sprite->source_ref)) {
                free(sources.slots);
                return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                    "sprite references an unknown source id");
            }
        }
        status = validate_unique_sprites(atlas, error);
        if (status != TP_STATUS_OK) {
            free(sources.slots);
            return status;
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            const tp_project_anim *animation = &atlas->animations[i];
            if (!animation->name || animation->name[0] == '\0') {
                free(sources.slots);
                return tp_error_set(error, TP_STATUS_BAD_PROJECT,
                                    "animation name must be non-empty");
            }
            status = tp_utf8_validate_c_string(
                animation->name, TP_STATUS_BAD_PROJECT, "animation name",
                error);
            if (status != TP_STATUS_OK) {
                free(sources.slots);
                return status;
            }
            for (int f = 0; f < animation->frame_count; f++) {
                const tp_project_frame *frame = &animation->frames[f];
                status = validate_reference(frame->source_ref, frame->src_key,
                                            frame->name, "animation frame", error);
                if (status != TP_STATUS_OK) {
                    free(sources.slots);
                    return status;
                }
                if (require_known_sources &&
                    !source_index_contains(&sources, frame->source_ref)) {
                    free(sources.slots);
                    return tp_error_set(
                        error, TP_STATUS_BAD_PROJECT,
                        "animation frame references an unknown source id");
                }
            }
        }
        for (int i = 0; i < atlas->target_count; i++) {
            const tp_project_target *target = &atlas->targets[i];
            if (!target->exporter_id || target->exporter_id[0] == '\0' ||
                !target->out_path || target->out_path[0] == '\0') {
                free(sources.slots);
                return tp_error_set(
                    error, TP_STATUS_BAD_PROJECT,
                    "target exporter_id and out_path must be non-empty");
            }
            status = tp_utf8_validate_c_string(
                target->exporter_id, TP_STATUS_BAD_PROJECT,
                "target exporter_id", error);
            if (status == TP_STATUS_OK &&
                tp_exporter_id_validate(target->exporter_id, NULL) !=
                    TP_STATUS_OK) {
                status = tp_error_set(
                    error, TP_STATUS_BAD_PROJECT,
                    "target exporter_id exceeds the canonical format-id limit");
            }
            if (status == TP_STATUS_OK) {
                status = tp_utf8_validate_c_string(
                    target->out_path, TP_STATUS_BAD_PROJECT,
                    "target out_path", error);
            }
            if (status != TP_STATUS_OK) {
                free(sources.slots);
                return status;
            }
        }
        free(sources.slots);
    }
    return TP_STATUS_OK;
}

tp_status tp_project_validate_schema_shape(const tp_project *project,
                                           tp_error *error) {
    return validate_project(project, false, error);
}

tp_status tp_project_validate_canonical(const tp_project *project,
                                        tp_error *error) {
    return validate_project(project, true, error);
}
