#include "tp_core/tp_project.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_utf8.h"
#include "tp_fs_internal.h"
#include "tp_json_internal.h"
#include "tp_pack_constraints_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_internal.h"
#include "tp_project_model_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_parse_internal.h"
#include "tp_project_path_internal.h"
#include "tp_project_write_internal.h"
#include "tp_source_path_text_internal.h"
#include "tp_strutil.h"

const tp_project_json_limits TP_PROJECT_JSON_LIMITS = {
    (size_t)TP_IDENTITY_FILE_MAX_BYTES,
    (size_t)TP_PROJECT_JSON_MAX_NODES,
    (size_t)TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES,
    (size_t)TP_PROJECT_JSON_MAX_DEPTH,
};

static _Thread_local bool s_test_measure_load_lookups;
static _Thread_local tp_project_load_lookup_work s_test_load_lookup_work;
static _Thread_local bool s_test_measure_load_resources;
static _Thread_local tp_project_load_resources s_test_load_resources;

void tp_project__test_load_lookup_work_reset(void) {
    memset(&s_test_load_lookup_work, 0, sizeof s_test_load_lookup_work);
    s_test_measure_load_lookups = true;
}

tp_project_load_lookup_work tp_project__test_load_lookup_work_take(void) {
    s_test_measure_load_lookups = false;
    return s_test_load_lookup_work;
}

void tp_project__test_load_resources_reset(void) {
    memset(&s_test_load_resources, 0, sizeof s_test_load_resources);
    s_test_measure_load_resources = true;
}

tp_project_load_resources tp_project__test_load_resources_take(void) {
    s_test_measure_load_resources = false;
    return s_test_load_resources;
}

bool tp_project__test_load_resources_enabled(void) {
    return s_test_measure_load_resources;
}

void tp_project__test_note_id_resources(size_t refs_bytes,
                                        size_t index_bytes) {
    if (!s_test_measure_load_resources) {
        return;
    }
    if (refs_bytes > s_test_load_resources.id_refs_bytes) {
        s_test_load_resources.id_refs_bytes = refs_bytes;
    }
    if (index_bytes > s_test_load_resources.id_index_bytes) {
        s_test_load_resources.id_index_bytes = index_bytes;
    }
}

/* ======================================================================== */
/* load                                                                     */
/* ======================================================================== */

static tp_status tp_json_int_in_range(const cJSON *item, const char *label,
                                      int minimum, int maximum, int *out,
                                      tp_error *err) {
    if (!cJSON_IsNumber(item)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "field '%s' must be a number", label);
    }
    const double value = item->valuedouble;
    if (!isfinite(value) || value < (double)minimum ||
        value > (double)maximum) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "field '%s' must be an integer in [%d,%d]",
                            label, minimum, maximum);
    }
    const int converted = (int)value;
    if ((double)converted != value) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "field '%s' must be an integer", label);
    }
    *out = converted;
    return TP_STATUS_OK;
}

static tp_status tp_json_float(const cJSON *item, const char *label,
                               float *out, tp_error *err) {
    if (!cJSON_IsNumber(item)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "field '%s' must be a number", label);
    }
    const double value = item->valuedouble;
    if (!isfinite(value) || value < -(double)FLT_MAX ||
        value > (double)FLT_MAX) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "field '%s' must be a finite 32-bit number", label);
    }
    *out = (float)value;
    return TP_STATUS_OK;
}

static tp_status tp_opt_int(const cJSON *o, const char *k, int *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    return tp_json_int_in_range(it, k, INT_MIN, INT_MAX, dst, err);
}

static tp_status tp_opt_float(const cJSON *o, const char *k, float *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    return tp_json_float(it, k, dst, err);
}

static tp_status tp_opt_bool(const cJSON *o, const char *k, bool *dst, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsBool(it)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "field '%s' must be a boolean", k);
    }
    *dst = cJSON_IsTrue(it) ? true : false;
    return TP_STATUS_OK;
}

/* Parse a kind-checked structural shape-ID at `key`. An absent key remains nil
 * and canonical validation rejects it; malformed, wrong-kind, or explicit nil
 * values are ID_MALFORMED. */
static tp_status tp_load_id(const cJSON *o, const char *key, tp_id_kind expect_kind, tp_id128 *out, tp_error *err) {
    *out = tp_id128_nil();
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!it) {
        return TP_STATUS_OK; /* canonical validation reports the missing ID */
    }
    if (!cJSON_IsString(it) || !it->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "'%s' must be a shape-ID string", key);
    }
    tp_id_kind k = TP_ID_KIND_INVALID;
    tp_id128 id;
    tp_status st = tp_id_parse(it->valuestring, &k, &id, err);
    if (st != TP_STATUS_OK) {
        return st; /* TP_STATUS_ID_MALFORMED with a specific reason in err */
    }
    if (k != expect_kind) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "'%s' = '%s' has the wrong kind prefix", key,
                            it->valuestring);
    }
    if (tp_id128_is_nil(id)) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "'%s' is a nil structural id", key);
    }
    *out = id;
    return TP_STATUS_OK;
}

/* Parse-local borrowed-string index. It exists only while one JSON array is
 * materialized; the project model remains the sole owner of stored strings. */
typedef struct tp_load_lookup_slot {
    uint64_t hash;
    const char *key;
    int model_index;
} tp_load_lookup_slot;

typedef struct tp_load_lookup {
    tp_load_lookup_slot *slots;
    size_t capacity;
} tp_load_lookup;

#define TP_LOAD_LOOKUP_MAX_PROBES 64U

static bool tp_load_lookup_init(tp_load_lookup *lookup, int expected) {
    memset(lookup, 0, sizeof *lookup);
    if (expected <= 0) {
        return true;
    }
    if ((size_t)expected > SIZE_MAX / 2U) {
        return false;
    }
    size_t capacity = 16U;
    const size_t needed = (size_t)expected * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            return false;
        }
        capacity *= 2U;
    }
    lookup->slots =
        (tp_load_lookup_slot *)calloc(capacity, sizeof *lookup->slots);
    if (!lookup->slots) {
        return false;
    }
    lookup->capacity = capacity;
    if (s_test_measure_load_resources) {
        const size_t bytes = capacity * sizeof *lookup->slots;
        if (bytes > s_test_load_resources.source_index_peak_bytes) {
            s_test_load_resources.source_index_peak_bytes = bytes;
        }
    }
    return true;
}

static void tp_load_lookup_free(tp_load_lookup *lookup) {
    free(lookup->slots);
    memset(lookup, 0, sizeof *lookup);
}

static tp_load_lookup_slot *tp_load_lookup_find(tp_load_lookup *lookup,
                                                const char *key) {
    if (!lookup || lookup->capacity == 0U || !key) {
        return NULL;
    }
    const uint64_t hash = tp_source_path_text_hash(key);
    const size_t start = (size_t)hash & (lookup->capacity - 1U);
    for (size_t i = 0U;
         i < lookup->capacity && i < (size_t)TP_LOAD_LOOKUP_MAX_PROBES; i++) {
        tp_load_lookup_slot *slot =
            &lookup->slots[(start + i) & (lookup->capacity - 1U)];
        if (!slot->key) {
            slot->hash = hash;
            return slot;
        }
        if (s_test_measure_load_lookups) {
            s_test_load_lookup_work.source_path_comparisons++;
        }
        if (slot->hash == hash) {
            if (tp_source_path_text_equal(slot->key, key)) {
                return slot;
            }
        }
    }
    return NULL;
}

static tp_status tp_project_reject_unknown_object_keys(
    const cJSON *object, const char *object_name,
    const char *const *allowed_keys, size_t allowed_count, tp_error *err) {
    if (!cJSON_IsObject(object)) {
        return TP_STATUS_OK; /* the object-specific loader owns type errors */
    }
    for (const cJSON *child = object->child; child; child = child->next) {
        bool known = false;
        for (size_t i = 0U; i < allowed_count; i++) {
            if (child->string && strcmp(child->string, allowed_keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return tp_error_set(
                err, TP_STATUS_BAD_PROJECT, "unknown %s key '%s'",
                object_name, child->string ? child->string : "");
        }
    }
    return TP_STATUS_OK;
}

#define TP_PROJECT_KEY_COUNT(keys) (sizeof(keys) / sizeof((keys)[0]))

/* Canonical v5 is deliberately closed. A new field requires a schema bump;
 * otherwise a misspelled field could load successfully and disappear on save. */
static tp_status tp_project_reject_unknown_schema_keys(
    const cJSON *root, tp_error *err) {
    static const char *const root_keys[] = {"version", "atlases"};
    static const char *const atlas_keys[] = {
        "allow_transform", "alpha_threshold", "animations", "extrude",
        "id",              "margin",          "max_size",   "max_vertices",
        "name",            "padding",         "pixels_per_unit",
        "power_of_two",    "shape",           "sources",    "sprites",
        "targets",
    };
    static const char *const source_keys[] = {"id", "kind", "path"};
    static const char *const sprite_keys[] = {
        "allow_rotate", "extrude", "key",    "margin", "max_vertices",
        "origin",       "rename",  "shape",  "slice9", "source",
    };
    static const char *const animation_keys[] = {
        "flip_h", "flip_v", "fps", "frames", "id", "name", "playback",
    };
    static const char *const frame_keys[] = {"key", "source"};
    static const char *const target_keys[] = {
        "enabled", "exporter_id", "id", "out_path",
    };

    tp_status status = tp_project_reject_unknown_object_keys(
        root, "project", root_keys, TP_PROJECT_KEY_COUNT(root_keys), err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    const cJSON *atlases =
        cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (!cJSON_IsArray(atlases)) {
        return TP_STATUS_OK; /* required/type checks run in the loader */
    }
    const cJSON *atlas = NULL;
    cJSON_ArrayForEach(atlas, atlases) {
        status = tp_project_reject_unknown_object_keys(
            atlas, "atlas", atlas_keys, TP_PROJECT_KEY_COUNT(atlas_keys),
            err);
        if (status != TP_STATUS_OK || !cJSON_IsObject(atlas)) {
            return status;
        }

        const cJSON *sources =
            cJSON_GetObjectItemCaseSensitive(atlas, "sources");
        if (cJSON_IsArray(sources)) {
            const cJSON *source = NULL;
            cJSON_ArrayForEach(source, sources) {
                status = tp_project_reject_unknown_object_keys(
                    source, "source", source_keys,
                    TP_PROJECT_KEY_COUNT(source_keys), err);
                if (status != TP_STATUS_OK) {
                    return status;
                }
            }
        }

        const cJSON *sprites =
            cJSON_GetObjectItemCaseSensitive(atlas, "sprites");
        if (cJSON_IsArray(sprites)) {
            const cJSON *sprite = NULL;
            cJSON_ArrayForEach(sprite, sprites) {
                status = tp_project_reject_unknown_object_keys(
                    sprite, "sprite", sprite_keys,
                    TP_PROJECT_KEY_COUNT(sprite_keys), err);
                if (status != TP_STATUS_OK) {
                    return status;
                }
            }
        }

        const cJSON *animations =
            cJSON_GetObjectItemCaseSensitive(atlas, "animations");
        if (cJSON_IsArray(animations)) {
            const cJSON *animation = NULL;
            cJSON_ArrayForEach(animation, animations) {
                status = tp_project_reject_unknown_object_keys(
                    animation, "animation", animation_keys,
                    TP_PROJECT_KEY_COUNT(animation_keys), err);
                if (status != TP_STATUS_OK) {
                    return status;
                }
                if (!cJSON_IsObject(animation)) {
                    continue;
                }
                const cJSON *frames =
                    cJSON_GetObjectItemCaseSensitive(animation, "frames");
                if (cJSON_IsArray(frames)) {
                    const cJSON *frame = NULL;
                    cJSON_ArrayForEach(frame, frames) {
                        status = tp_project_reject_unknown_object_keys(
                            frame, "animation frame", frame_keys,
                            TP_PROJECT_KEY_COUNT(frame_keys), err);
                        if (status != TP_STATUS_OK) {
                            return status;
                        }
                    }
                }
            }
        }

        const cJSON *targets =
            cJSON_GetObjectItemCaseSensitive(atlas, "targets");
        if (cJSON_IsArray(targets)) {
            const cJSON *target = NULL;
            cJSON_ArrayForEach(target, targets) {
                status = tp_project_reject_unknown_object_keys(
                    target, "target", target_keys,
                    TP_PROJECT_KEY_COUNT(target_keys), err);
                if (status != TP_STATUS_OK) {
                    return status;
                }
            }
        }
    }
    return TP_STATUS_OK;
}

#undef TP_PROJECT_KEY_COUNT

/* Appends a fresh all-default sprite record and returns it, or NULL on OOM.
 * Identity is (source, key), so export-name bridges never deduplicate records. */
static tp_status tp_load_sprite(tp_project_atlas *a, const cJSON *js,
                                tp_error *err) {
    if (!cJSON_IsObject(js)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite override must be an object");
    }
    tp_id128 source_ref = tp_id128_nil();
    tp_status st = tp_load_id(js, "source", TP_ID_KIND_SOURCE, &source_ref, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    const cJSON *keyj = cJSON_GetObjectItemCaseSensitive(js, "key");
    if (keyj && (!cJSON_IsString(keyj) || !keyj->valuestring)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'key' must be a string");
    }
    if (tp_id128_is_nil(source_ref) || !keyj) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "sprite override requires 'source' and 'key'");
    }
    if (tp_project_atlas_find_sprite_by_source_key(
            a, source_ref, keyj->valuestring)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "duplicate sprite override identity");
    }
    char bridge[TP_SRCKEY_MAX];
    tp_sprite_export_key(keyj->valuestring, bridge, sizeof bridge);
    tp_project_sprite *s = sprite_push_default(a);
    if (!s) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "out of memory adding sprite override");
    }
    s->source_ref = source_ref;
    s->src_key = tp_strdup(keyj->valuestring);
    s->name = tp_strdup(bridge);
    if (!s->src_key || !s->name) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "out of memory adding sprite override");
    }
    const cJSON *origin = cJSON_GetObjectItemCaseSensitive(js, "origin");
    if (origin) {
        if (!cJSON_IsArray(origin) || cJSON_GetArraySize(origin) != 2) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'origin' must be a [x,y] array");
        }
        st = tp_json_float(cJSON_GetArrayItem(origin, 0), "origin[0]",
                           &s->origin_x, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
        st = tp_json_float(cJSON_GetArrayItem(origin, 1), "origin[1]",
                           &s->origin_y, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
    }
    const cJSON *rename = cJSON_GetObjectItemCaseSensitive(js, "rename");
    if (rename) {
        if (!cJSON_IsString(rename) || !rename->valuestring) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'rename' must be a string");
        }
        free(s->rename);
        s->rename = tp_strdup(rename->valuestring);
        if (!s->rename) {
            return tp_error_set(err, TP_STATUS_OOM, "out of memory reading sprite rename");
        }
    }
    const cJSON *slice9 = cJSON_GetObjectItemCaseSensitive(js, "slice9");
    if (slice9) {
        if (!cJSON_IsArray(slice9) || cJSON_GetArraySize(slice9) != 4) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "sprite 'slice9' must be a [l,r,t,b] array");
        }
        for (int k = 0; k < 4; k++) {
            int value = 0;
            st = tp_json_int_in_range(cJSON_GetArrayItem(slice9, k),
                                      "slice9 item", 0, UINT16_MAX, &value,
                                      err);
            if (st != TP_STATUS_OK) {
                return st;
            }
            s->slice9_lrtb[k] = (uint16_t)value;
        }
    }
    /* Per-sprite packing overrides (absent = inherit, already seeded to -1). Values
     * are read verbatim; tp_pack validates the ranges (kept lenient here). */
    static const struct {
        const char *key;
        size_t offset;
        int minimum;
        int maximum;
        bool (*representable)(int value);
    } ov_fields[] = {
        {"shape", offsetof(tp_project_sprite, ov_shape), TP_PACK_SHAPE_MIN,
         TP_PACK_SHAPE_MAX, tp_pack_sprite_shape_wire_representable},
        {"allow_rotate", offsetof(tp_project_sprite, ov_allow_rotate), 0, 0,
         tp_pack_sprite_rotate_wire_representable},
        {"max_vertices", offsetof(tp_project_sprite, ov_max_vertices), 1,
         TP_PACK_MAX_VERTICES,
         tp_pack_sprite_max_vertices_wire_representable},
        {"margin", offsetof(tp_project_sprite, ov_margin), 1, UINT8_MAX,
         tp_pack_sprite_spacing_wire_representable},
        {"extrude", offsetof(tp_project_sprite, ov_extrude), 1, UINT8_MAX,
         tp_pack_sprite_spacing_wire_representable},
    };
    for (size_t i = 0; i < sizeof ov_fields / sizeof ov_fields[0]; i++) {
        const cJSON *jv = cJSON_GetObjectItemCaseSensitive(js, ov_fields[i].key);
        if (jv) {
            int value = 0;
            st = tp_json_int_in_range(jv, ov_fields[i].key, INT_MIN,
                                      INT_MAX, &value, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
            if (value != TP_PROJECT_OV_INHERIT &&
                !ov_fields[i].representable(value)) {
                return tp_error_set(
                    err, TP_STATUS_BAD_PROJECT,
                    "sprite override '%s' must be inherit (-1) or in [%d,%d]",
                    ov_fields[i].key, ov_fields[i].minimum,
                    ov_fields[i].maximum);
            }
            *(int16_t *)((char *)s + ov_fields[i].offset) = (int16_t)value;
        }
    }
    return TP_STATUS_OK;
}
static tp_status tp_load_anim(tp_project_atlas *a, const cJSON *ja,
                              tp_error *err) {
    if (!cJSON_IsObject(ja)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation must be an object");
    }
    const char *name_key = "name";
    const cJSON *nm = cJSON_GetObjectItemCaseSensitive(ja, name_key);
    if (!cJSON_IsString(nm) || !nm->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation missing string '%s'", name_key);
    }
    tp_project_anim *an = NULL;
    tp_status st = tp_project_atlas_add_animation(a, nm->valuestring, &an);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding animation");
    }
    if ((st = tp_load_id(ja, "id", TP_ID_KIND_ANIM, &an->id, err)) != TP_STATUS_OK) {
        return st;
    }
    const cJSON *frames = cJSON_GetObjectItemCaseSensitive(ja, "frames");
    if (frames) {
        if (!cJSON_IsArray(frames)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation 'frames' must be an array");
        }
        const cJSON *fr = NULL;
        cJSON_ArrayForEach(fr, frames) {
            if (cJSON_IsObject(fr)) {
                tp_id128 sref = tp_id128_nil();
                st = tp_load_id(fr, "source", TP_ID_KIND_SOURCE, &sref, err);
                if (st != TP_STATUS_OK) {
                    return st;
                }
                const cJSON *kj = cJSON_GetObjectItemCaseSensitive(fr, "key");
                if (!cJSON_IsString(kj) || !kj->valuestring) {
                    return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation frame object needs a string 'key'");
                }
                if (tp_id128_is_nil(sref)) {
                    return tp_error_set(err, TP_STATUS_BAD_PROJECT, "animation frame object needs a 'source'");
                }
                st = tp_project_anim_add_frame(an, sref, kj->valuestring);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err,
                                        st == TP_STATUS_INVALID_ARGUMENT
                                            ? TP_STATUS_BAD_PROJECT
                                            : st,
                                        "invalid animation frame identity");
                }
            } else {
                return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                    "animation frame must be a {source, key} object");
            }
        }
    }
    if ((st = tp_opt_float(ja, "fps", &an->fps, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_int(ja, "playback", &an->playback, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_bool(ja, "flip_h", &an->flip_h, err)) != TP_STATUS_OK) {
        return st;
    }
    if ((st = tp_opt_bool(ja, "flip_v", &an->flip_v, err)) != TP_STATUS_OK) {
        return st;
    }
    return TP_STATUS_OK;
}

static tp_status tp_load_target(tp_project_atlas *a, const cJSON *jt,
                                tp_error *err) {
    if (!cJSON_IsObject(jt)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target must be an object");
    }
    const cJSON *exporter = cJSON_GetObjectItemCaseSensitive(jt, "exporter_id");
    const cJSON *out_path = cJSON_GetObjectItemCaseSensitive(jt, "out_path");
    if (!cJSON_IsString(exporter) || !exporter->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target missing string 'exporter_id'");
    }
    if (!cJSON_IsString(out_path) || !out_path->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "target missing string 'out_path'");
    }
    tp_project_target *t = NULL;
    tp_status st = tp_project_atlas_add_target(a, exporter->valuestring, out_path->valuestring, &t);
    if (st != TP_STATUS_OK) {
        if (st == TP_STATUS_OOM) {
            return tp_error_set(err, st, "out of memory adding target");
        }
        return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                            "target exporter_id violates the canonical format-id contract");
    }
    if ((st = tp_load_id(jt, "id", TP_ID_KIND_TARGET, &t->id, err)) != TP_STATUS_OK) {
        return st;
    }
    return tp_opt_bool(jt, "enabled", &t->enabled, err);
}

/* Parse a canonical source kind token. Absent means the sparse folder default;
 * a future kind requires a schema bump and is rejected by the version gate. */
static tp_status tp_load_source_kind(const cJSON *jsrc, tp_source_kind *out, tp_error *err) {
    *out = TP_SOURCE_KIND_FOLDER;
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(jsrc, "kind");
    if (!k) {
        return TP_STATUS_OK; /* absent -> folder default */
    }
    if (!cJSON_IsString(k) || !k->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "source 'kind' must be a string");
    }
    if (strcmp(k->valuestring, "folder") == 0) {
        *out = TP_SOURCE_KIND_FOLDER;
    } else if (strcmp(k->valuestring, "file") == 0) {
        *out = TP_SOURCE_KIND_FILE;
    } else {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "unknown source kind '%s'", k->valuestring);
    }
    return TP_STATUS_OK;
}

/* Load one canonical source object {id, kind?, path}. Duplicate normalized paths
 * violate graph integrity and are rejected; the loader never self-heals by
 * silently dropping a record. */
static tp_status tp_load_source_obj(tp_project_atlas *a, const cJSON *jsrc,
                                    tp_load_lookup *source_lookup,
                                    tp_error *err) {
    if (!cJSON_IsObject(jsrc)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "source must be an object");
    }
    const cJSON *jpath = cJSON_GetObjectItemCaseSensitive(jsrc, "path");
    if (!cJSON_IsString(jpath) || !jpath->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "source missing string 'path'");
    }
    tp_source_kind kind = TP_SOURCE_KIND_FOLDER;
    tp_status st = tp_load_source_kind(jsrc, &kind, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_id128 id = tp_id128_nil();
    if ((st = tp_load_id(jsrc, "id", TP_ID_KIND_SOURCE, &id, err)) != TP_STATUS_OK) {
        return st;
    }
    tp_load_lookup_slot *slot = NULL;
    if (tp_source_path_text_admit(jpath->valuestring) == TP_STATUS_OK) {
        slot = tp_load_lookup_find(source_lookup, jpath->valuestring);
        if (!slot) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "source lookup exceeds the work limit");
        }
        if (slot->key) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT,
                                "duplicate source path '%s'",
                                jpath->valuestring);
        }
    }
    /* Overlong tagged paths remain separate so validation can diagnose every
     * anomalous record; they never enter the dedupe index. */
    st = atlas_push_source(a, jpath->valuestring, kind, id);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding source");
    }
    if (slot) {
        slot->key = a->sources[a->source_count - 1].path;
        slot->model_index = a->source_count - 1;
    }
    return TP_STATUS_OK;
}

static tp_status tp_load_atlas(tp_project *p, const cJSON *jatlas,
                               tp_error *err) {
    if (!cJSON_IsObject(jatlas)) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas must be an object");
    }
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(jatlas, "name");
    if (!cJSON_IsString(name) || !name->valuestring) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas missing string 'name'");
    }
    int idx = 0;
    tp_status st = tp_project_add_atlas(p, name->valuestring, &idx);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "out of memory adding atlas");
    }
    tp_project_atlas *a = &p->atlases[idx];
    if ((st = tp_load_id(jatlas, "id", TP_ID_KIND_ATLAS, &a->id, err)) != TP_STATUS_OK) {
        return st;
    }

    if ((st = tp_opt_int(jatlas, "max_size", &a->max_size, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "padding", &a->padding, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "margin", &a->margin, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "extrude", &a->extrude, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "alpha_threshold", &a->alpha_threshold, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "max_vertices", &a->max_vertices, err)) != TP_STATUS_OK ||
        (st = tp_opt_int(jatlas, "shape", &a->shape, err)) != TP_STATUS_OK ||
        (st = tp_opt_bool(jatlas, "allow_transform", &a->allow_transform, err)) != TP_STATUS_OK ||
        (st = tp_opt_bool(jatlas, "power_of_two", &a->power_of_two, err)) != TP_STATUS_OK ||
        (st = tp_opt_float(jatlas, "pixels_per_unit", &a->pixels_per_unit, err)) != TP_STATUS_OK) {
        return st;
    }

    const cJSON *sources = cJSON_GetObjectItemCaseSensitive(jatlas, "sources");
    if (sources) {
        if (!cJSON_IsArray(sources)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'sources' must be an array");
        }
        tp_load_lookup source_lookup;
        if (!tp_load_lookup_init(&source_lookup,
                                 cJSON_GetArraySize(sources))) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "out of memory indexing sources");
        }
        const cJSON *src = NULL;
        cJSON_ArrayForEach(src, sources) {
            st = tp_load_source_obj(a, src, &source_lookup, err);
            if (st != TP_STATUS_OK) {
                tp_load_lookup_free(&source_lookup);
                return st;
            }
        }
        tp_load_lookup_free(&source_lookup);
    }

    const cJSON *sprites = cJSON_GetObjectItemCaseSensitive(jatlas, "sprites");
    if (sprites) {
        if (!cJSON_IsArray(sprites)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'sprites' must be an array");
        }
        const cJSON *js = NULL;
        cJSON_ArrayForEach(js, sprites) {
            st = tp_load_sprite(a, js, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
    }

    const cJSON *anims = cJSON_GetObjectItemCaseSensitive(jatlas, "animations");
    if (anims) {
        if (!cJSON_IsArray(anims)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'animations' must be an array");
        }
        const cJSON *ja = NULL;
        cJSON_ArrayForEach(ja, anims) {
            if ((st = tp_load_anim(a, ja, err)) != TP_STATUS_OK) {
                return st;
            }
        }
    }

    const cJSON *targets = cJSON_GetObjectItemCaseSensitive(jatlas, "targets");
    if (targets) {
        if (!cJSON_IsArray(targets)) {
            return tp_error_set(err, TP_STATUS_BAD_PROJECT, "atlas 'targets' must be an array");
        }
        const cJSON *jt = NULL;
        cJSON_ArrayForEach(jt, targets) {
            if ((st = tp_load_target(a, jt, err)) != TP_STATUS_OK) {
                return st;
            }
        }
    }
    return TP_STATUS_OK;
}

static tp_status tp_read_file(const char *path, char **out, size_t *out_len, tp_error *err) {
    *out = NULL;
    *out_len = 0U;
    FILE *f = tp_fs_fopen(path, "rb");
    if (!f) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: cannot open %s", path);
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        (void)tp_fs_close(f);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: cannot seek %s", path);
    }
    const long size = ftell(f);
    if (size < 0 || fseek(f, 0, SEEK_SET) != 0) {
        (void)tp_fs_close(f);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: cannot size %s", path);
    }
    if ((uint64_t)size > (uint64_t)TP_IDENTITY_FILE_MAX_BYTES) {
        (void)tp_fs_close(f);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_load: %s exceeds the %u-byte limit", path,
                            (unsigned int)TP_IDENTITY_FILE_MAX_BYTES);
    }
    char *buf = (char *)malloc((size_t)size + 1U);
    if (!buf) {
        (void)tp_fs_close(f);
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_load: out of memory reading %s", path);
    }
    const size_t got = fread(buf, 1U, (size_t)size, f);
    const int read_failed = ferror(f);
    const bool closed = tp_fs_close(f);
    if (read_failed || !closed || got != (size_t)size) {
        free(buf);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: short read from %s", path);
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return TP_STATUS_OK;
}

typedef struct tp_json_preflight_frame {
    char opener;
    size_t commas;
    bool nonempty;
} tp_json_preflight_frame;

/* Bounds cJSON allocation count and depth before tree materialization. */
tp_status tp_project_json_admit(
    const char *text, size_t len, const tp_project_json_limits *limits,
    tp_error *err) {
    if (!text || !limits || limits->bytes == 0U || limits->nodes == 0U ||
        limits->container_entries == 0U || limits->depth == 0U ||
        limits->depth > (size_t)TP_PROJECT_JSON_MAX_DEPTH) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid project JSON admission arguments");
    }
    if (len > limits->bytes) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "project JSON exceeds the supported byte limit");
    }
    tp_status string_status = tp_json_reject_c_string_ambiguity(
        text, len, TP_STATUS_BAD_PROJECT, "project JSON", err);
    if (string_status != TP_STATUS_OK) {
        return string_status;
    }
    tp_json_preflight_frame stack[TP_PROJECT_JSON_MAX_DEPTH];
    size_t depth = 0U;
    size_t nodes = 0U;

    for (size_t i = 0U; i < len;) {
        const unsigned char c = (unsigned char)text[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            i++;
            continue;
        }
        const bool closes_top = depth > 0U &&
            ((c == '}' && stack[depth - 1U].opener == '{') ||
             (c == ']' && stack[depth - 1U].opener == '['));
        if (depth > 0U && !closes_top) {
            stack[depth - 1U].nonempty = true;
        }

        if (c == '"') {
            if (nodes >= limits->nodes) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "project JSON exceeds the structural node limit");
            }
            nodes++;
            i++;
            while (i < len) {
                if (text[i] == '\\') {
                    i += (i + 1U < len) ? 2U : 1U;
                } else if (text[i] == '"') {
                    i++;
                    break;
                } else {
                    i++;
                }
            }
            continue;
        }
        if (c == '{' || c == '[') {
            if (nodes >= limits->nodes) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "project JSON exceeds the structural node limit");
            }
            nodes++;
            if (depth >= limits->depth) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "project JSON exceeds the nesting depth limit");
            }
            stack[depth++] = (tp_json_preflight_frame){(char)c, 0U, false};
            i++;
            continue;
        }
        if (c == '}' || c == ']') {
            if (!closes_top) {
                i++;
                continue; /* malformed syntax: cJSON owns the diagnostic */
            }
            const tp_json_preflight_frame frame = stack[depth - 1U];
            const size_t entries = frame.nonempty ? frame.commas + 1U : 0U;
            if (entries > limits->container_entries) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                    "project JSON container exceeds the entry limit");
            }
            depth--;
            i++;
            continue;
        }
        if (c == ',') {
            if (depth > 0U) {
                if (stack[depth - 1U].commas >=
                    limits->container_entries) {
                    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                        "project JSON container exceeds the entry limit");
                }
                stack[depth - 1U].commas++;
            }
            i++;
            continue;
        }
        if (c == ':') {
            i++;
            continue;
        }

        if (nodes >= limits->nodes) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "project JSON exceeds the structural node limit");
        }
        nodes++;
        i++;
        while (i < len) {
            const unsigned char next = (unsigned char)text[i];
            if (next == ' ' || next == '\t' || next == '\r' || next == '\n' ||
                next == ',' || next == ':' || next == '{' || next == '}' ||
                next == '[' || next == ']') {
                break;
            }
            i++;
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_project__test_json_admit(
    const char *text, size_t len, const tp_project_json_limits *limits,
    tp_error *err) {
    return tp_project_json_admit(text, len, limits, err);
}

/* Parse core shared by load (from file) + load_buffer (from memory). Borrows
 * `text` (does not free it); leaves project_dir NULL -- the file loader sets it. */
static tp_status tp_project_parse(const char *text, size_t len, tp_project **out, tp_error *err) {
    *out = NULL;

    tp_status status = tp_project_json_admit(
        text, len, &TP_PROJECT_JSON_LIMITS, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(text, len, &parse_end, 0);
    if (!root) {
        const char *ep = cJSON_GetErrorPtr();
        const long off = (ep && ep >= text) ? (long)(ep - text) : -1L;
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: malformed JSON near offset %ld", off);
    }

    const char *const text_end = text + len;
    const char *trailing = parse_end;
    while (trailing && trailing < text_end &&
           (*trailing == ' ' || *trailing == '\t' || *trailing == '\r' ||
            *trailing == '\n')) {
        trailing++;
    }
    if (!trailing || trailing != text_end) {
        const long off = trailing && trailing >= text
                             ? (long)(trailing - text)
                             : -1L;
        cJSON_Delete(root);
        return tp_error_set(
            err, TP_STATUS_BAD_PROJECT,
            "tp_project_load: trailing data near offset %ld", off);
    }

    status = tp_json_reject_duplicate_keys(
        root, TP_STATUS_BAD_PROJECT, "project JSON", err);
    if (status != TP_STATUS_OK) {
        cJSON_Delete(root);
        return status;
    }

    status = TP_STATUS_OK;
    tp_project *p = NULL;

    if (!cJSON_IsObject(root)) {
        status = tp_error_set(err, TP_STATUS_BAD_PROJECT, "tp_project_load: root must be an object");
        goto done;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    int file_version = 0;
    if (!version) {
        status = tp_error_set(err, TP_STATUS_BAD_PROJECT,
                              "tp_project_load: missing integer 'version'");
        goto done;
    }
    status = tp_json_int_in_range(version, "version", INT_MIN, INT_MAX,
                                  &file_version, err);
    if (status != TP_STATUS_OK) {
        goto done;
    }
    if (file_version != TP_PROJECT_SCHEMA_VERSION) {
        status = tp_error_set(err, TP_STATUS_BAD_VERSION,
                              "project schema version %d is not supported (this build requires %d)",
                              file_version, TP_PROJECT_SCHEMA_VERSION);
        goto done;
    }

    status = tp_project_reject_unknown_schema_keys(root, err);
    if (status != TP_STATUS_OK) {
        goto done;
    }

    const cJSON *atlases = cJSON_GetObjectItemCaseSensitive(root, "atlases");
    if (!atlases) {
        status = tp_error_set(
            err, TP_STATUS_BAD_PROJECT,
            "tp_project_load: missing required array 'atlases'");
        goto done;
    }
    if (!cJSON_IsArray(atlases)) {
        status = tp_error_set(
            err, TP_STATUS_BAD_PROJECT,
            "tp_project_load: 'atlases' must be an array");
        goto done;
    }

    p = tp_project_alloc_empty();
    if (!p) {
        status = tp_error_set(err, TP_STATUS_OOM, "tp_project_load: out of memory");
        goto done;
    }

    const cJSON *ja = NULL;
    cJSON_ArrayForEach(ja, atlases) {
        status = tp_load_atlas(p, ja, err);
        if (status != TP_STATUS_OK) {
            goto done;
        }
    }

    /* The parser accepts a canonical-shaped dangling source reference so
     * validate-file can report it. IDs, tagged sources, and reference fields are
     * otherwise strict; session adoption/save adds the known-source graph gate. */
    status = tp_project_validate_schema_shape(p, err);
    if (status != TP_STATUS_OK) {
        goto done;
    }

done:
    cJSON_Delete(root);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(p);
        return status;
    }
    *out = p;
    return TP_STATUS_OK;
}

tp_status tp_project_load_with_fingerprint(const char *path, tp_project **out, tp_id128 *out_fingerprint,
                                           tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (out_fingerprint) {
        memset(out_fingerprint, 0, sizeof *out_fingerprint);
    }
    if (!path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_load: NULL path or out");
    }
    if (!tp_utf8_is_valid_c_string(path)) {
        return tp_error_set(err, TP_STATUS_INVALID_UTF8,
                            "tp_project_load: path is not valid UTF-8");
    }

    size_t len = 0;
    char *text = NULL;
    tp_status status = tp_read_file(path, &text, &len, err);
    if (status != TP_STATUS_OK) {
        return status;
    }

    tp_id128 consumed_fingerprint = {{0}};
    if (out_fingerprint) {
        status = tp_identity_bytes_fingerprint(text, len, &consumed_fingerprint, err);
        if (status != TP_STATUS_OK) {
            free(text);
            return status;
        }
    }
    status = tp_project_parse(text, len, out, err);
    free(text);
    if (status != TP_STATUS_OK) {
        return status;
    }

    char dir[TP_PATH_MAX];
    status = tp_abs_dir_of(path, dir, sizeof dir);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(*out);
        *out = NULL;
        return tp_error_set(err, status, "tp_project_load: path too long: %s", path);
    }
    (*out)->project_dir = tp_strdup(dir);
    (*out)->source_base_dir = tp_strdup(dir);
    if (!(*out)->project_dir || !(*out)->source_base_dir) {
        tp_project_destroy(*out);
        *out = NULL;
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_load: out of memory");
    }
    if (out_fingerprint) {
        *out_fingerprint = consumed_fingerprint;
    }
    return TP_STATUS_OK;
}

tp_status tp_project_load(const char *path, tp_project **out, tp_error *err) {
    return tp_project_load_with_fingerprint(path, out, NULL, err);
}

tp_status tp_project_load_buffer(const char *buf, size_t len, tp_project **out, tp_error *err) {
    tp_project_write_note_load_buffer_call();
    if (out) {
        *out = NULL;
    }
    if (!buf || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_load_buffer: NULL buf or out");
    }
    return tp_project_parse(buf, len, out, err); /* project_dir stays NULL */
}
