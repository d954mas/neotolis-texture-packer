#include "tp_core/tp_project.h"

#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#define tp_getpid _getpid
#else
#include <sys/stat.h>
#include <unistd.h>
#define tp_getpid getpid
#endif

#include "cJSON.h"

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS (default-target seeding) */
#include "tp_core/tp_id.h"     /* structural shape-ID parse/format */
#include "tp_core/tp_identity.h" /* exact-buffer load/save fingerprints + file-size bound */
#include "tp_core/tp_names.h"  /* canonical display-name derivation */
#include "tp_core/tp_pack.h"
#include "tp_core/tp_srckey.h" /* canonical source-key normalization */
#include "tp_core/tp_utf8.h"
#include "tp_json_internal.h"
#include "tp_pack_constraints_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_fs_internal.h"
#include "tp_project_internal.h"         /* deterministic save fault seam for tests */
#include "tp_project_path_internal.h"
#include "tp_project_parse_internal.h"
#include "tp_project_write_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_source_path_text_internal.h"
#include "tp_strutil.h"                 /* shared tp_strdup (one core definition) */

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
/* small dynamic-array primitives (owned-string helpers live in tp_strutil.h) */
/* ======================================================================== */

/* Grows *arr to hold at least `needed` elements of `elem` bytes (doubling).
 * New tail slots are uninitialised -- the caller inits the appended element. */
static bool tp_grow(void **arr, int *cap, int needed, size_t elem) {
    if (needed <= *cap) {
        return true;
    }
    int new_cap = (*cap == 0) ? 4 : *cap * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    void *n = realloc(*arr, (size_t)new_cap * elem);
    if (!n) {
        return false;
    }
    *arr = n;
    *cap = new_cap;
    return true;
}

/* ======================================================================== */
/* model lifecycle + mutation                                               */
/* ======================================================================== */

static tp_project *tp_project_alloc_empty(void) {
    return (tp_project *)calloc(1, sizeof(tp_project));
}

static void tp_project_atlas_set_defaults(tp_project_atlas *a) {
    if (!a) {
        return;
    }
    tp_pack_settings s;
    tp_pack_settings_defaults(&s);
    a->max_size = s.max_size;
    a->padding = s.padding;
    a->margin = s.margin;
    a->extrude = s.extrude;
    a->alpha_threshold = s.alpha_threshold;
    a->max_vertices = s.max_vertices;
    a->shape = s.shape;
    a->allow_transform = s.allow_transform;
    a->power_of_two = s.power_of_two;
    a->pixels_per_unit = s.pixels_per_unit;
}

tp_status tp_project_add_atlas(tp_project *p, const char *name, int *out_index) {
    if (!p || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&p->atlases, &p->atlas_cap, p->atlas_count + 1, sizeof(tp_project_atlas))) {
        return TP_STATUS_OOM;
    }
    tp_project_atlas *a = &p->atlases[p->atlas_count];
    memset(a, 0, sizeof *a);
    a->name = tp_strdup(name);
    if (!a->name) {
        return TP_STATUS_OOM;
    }
    tp_project_atlas_set_defaults(a);
    if (out_index) {
        *out_index = p->atlas_count;
    }
    p->atlas_count++;
    return TP_STATUS_OK;
}

tp_status tp_project_set_atlas_name(tp_project_atlas *a, const char *name) {
    if (!a || !name || name[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char *copy = tp_strdup(name);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    free(a->name);
    a->name = copy;
    return TP_STATUS_OK;
}

static void tp_free_anim(tp_project_anim *an) {
    for (int i = 0; i < an->frame_count; i++) {
        free(an->frames[i].name);
        free(an->frames[i].src_key);
    }
    free(an->frames);
    free(an->name); /* `id` is a value (tp_id128), only `name` is owned */
}

static void tp_free_atlas(tp_project_atlas *a) {
    for (int i = 0; i < a->source_count; i++) {
        free(a->sources[i].path);
    }
    free(a->sources);
    for (int i = 0; i < a->sprite_count; i++) {
        free(a->sprites[i].name);
        free(a->sprites[i].src_key);
        free(a->sprites[i].rename);
    }
    free(a->sprites);
    for (int i = 0; i < a->animation_count; i++) {
        tp_free_anim(&a->animations[i]);
    }
    free(a->animations);
    for (int i = 0; i < a->target_count; i++) {
        free(a->targets[i].exporter_id);
        free(a->targets[i].out_path);
    }
    free(a->targets);
    free(a->name);
}

tp_status tp_project_remove_atlas(tp_project *p, int index) {
    if (!p) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= p->atlas_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_free_atlas(&p->atlases[index]);
    for (int i = index; i < p->atlas_count - 1; i++) {
        p->atlases[i] = p->atlases[i + 1];
    }
    p->atlas_count--;
    return TP_STATUS_OK;
}

int tp_project_find_atlas_by_id(const tp_project *p, tp_id128 id) {
    if (!p || tp_id128_is_nil(id)) {
        return -1;
    }
    for (int i = 0; i < p->atlas_count; i++) {
        if (tp_id128_eq(p->atlases[i].id, id)) {
            return i;
        }
    }
    return -1;
}

tp_project_atlas *tp_project_get_atlas(tp_project *p, int index) {
    if (!p || index < 0 || index >= p->atlas_count) {
        return NULL;
    }
    return &p->atlases[index];
}

tp_project *tp_project_create(void) {
    tp_project *p = tp_project_alloc_empty();
    if (!p) {
        return NULL;
    }
    if (tp_project_add_atlas(p, "atlas1", NULL) != TP_STATUS_OK) {
        tp_project_destroy(p);
        return NULL;
    }
    return p;
}

void tp_project_destroy(tp_project *p) {
    if (!p) {
        return;
    }
    for (int i = 0; i < p->atlas_count; i++) {
        tp_free_atlas(&p->atlases[i]);
    }
    free(p->atlases);
    free(p->project_dir);
    free(p->source_base_dir);
    free(p);
}

/* Raw append of a fully-formed source record (NO dedupe): materializes a source
 * with its persisted id/kind attached. `path` is duped. */
static tp_status atlas_push_source(tp_project_atlas *a, const char *path, tp_source_kind kind, tp_id128 id) {
    if (!tp_grow((void **)&a->sources, &a->source_cap, a->source_count + 1, sizeof(tp_project_source))) {
        return TP_STATUS_OOM;
    }
    char *copy = tp_strdup(path);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    tp_project_source *s = &a->sources[a->source_count];
    s->id = id;
    s->kind = kind;
    s->path = copy;
    a->source_count++;
    return TP_STATUS_OK;
}

/* True when the atlas already holds a source whose '/'-normalized path equals
 * `path`'s. Paths outside the bounded source-path contract compare unequal so
 * canonical validation can report each anomalous record. */
static bool atlas_has_source_path(const tp_project_atlas *a, const char *path) {
    for (int i = 0; i < a->source_count; i++) {
        if (tp_source_path_text_equal(a->sources[i].path, path)) {
            return true;
        }
    }
    return false;
}

tp_status tp_project_atlas_add_source_kind(tp_project_atlas *a, const char *path, tp_source_kind kind) {
    if (!a || !path) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    const tp_status admission = tp_source_path_text_admit(path);
    if (admission != TP_STATUS_OK) {
        return admission;
    }
    /* Dedupe: an exact ('/'-normalized) duplicate is an OK no-op (keeps its kind). */
    if (atlas_has_source_path(a, path)) {
        return TP_STATUS_OK;
    }
    return atlas_push_source(a, path, kind, tp_id128_nil());
}

tp_status tp_project_atlas_add_source(tp_project_atlas *a, const char *path) {
    return tp_project_atlas_add_source_kind(a, path, TP_SOURCE_KIND_FOLDER);
}

bool tp_project_atlas_has_source_path(const tp_project_atlas *a, const char *path) {
    return a && path && atlas_has_source_path(a, path);
}

tp_status tp_project_atlas_remove_source(tp_project_atlas *a, int index) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->source_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(a->sources[index].path);
    for (int i = index; i < a->source_count - 1; i++) {
        a->sources[i] = a->sources[i + 1];
    }
    a->source_count--;
    return TP_STATUS_OK;
}

tp_project_source *tp_project_atlas_find_source_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a || tp_id128_is_nil(id)) {
        return NULL;
    }
    for (int i = 0; i < a->source_count; i++) {
        if (tp_id128_eq(a->sources[i].id, id)) {
            return &a->sources[i];
        }
    }
    return NULL;
}

tp_status tp_project_atlas_remove_source_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a || tp_id128_is_nil(id)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    for (int i = 0; i < a->source_count; i++) {
        if (tp_id128_eq(a->sources[i].id, id)) {
            return tp_project_atlas_remove_source(a, i);
        }
    }
    return TP_STATUS_OUT_OF_BOUNDS;
}

tp_project_sprite *tp_project_atlas_find_sprite_by_source_key(
    tp_project_atlas *a, tp_id128 source_ref, const char *src_key) {
    if (!a || tp_id128_is_nil(source_ref) || !src_key) {
        return NULL;
    }
    for (int i = 0; i < a->sprite_count; i++) {
        if (a->sprites[i].src_key &&
            tp_id128_eq(a->sprites[i].source_ref, source_ref) &&
            strcmp(a->sprites[i].src_key, src_key) == 0) {
            return &a->sprites[i];
        }
    }
    return NULL;
}

static void sprite_init_defaults(tp_project_sprite *sprite) {
    memset(sprite, 0, sizeof *sprite);
    sprite->origin_x = TP_PROJECT_ORIGIN_DEFAULT;
    sprite->origin_y = TP_PROJECT_ORIGIN_DEFAULT;
    sprite->ov_shape = TP_PROJECT_OV_INHERIT;
    sprite->ov_allow_rotate = TP_PROJECT_OV_INHERIT;
    sprite->ov_max_vertices = TP_PROJECT_OV_INHERIT;
    sprite->ov_margin = TP_PROJECT_OV_INHERIT;
    sprite->ov_extrude = TP_PROJECT_OV_INHERIT;
}

static tp_project_sprite *sprite_push_default(tp_project_atlas *a) {
    if (!tp_grow((void **)&a->sprites, &a->sprite_cap,
                 a->sprite_count + 1, sizeof(tp_project_sprite))) {
        return NULL;
    }
    tp_project_sprite *sprite = &a->sprites[a->sprite_count++];
    sprite_init_defaults(sprite);
    return sprite;
}

tp_status tp_project_atlas_add_sprite_by_source_key(
    tp_project_atlas *a, tp_id128 source_ref, const char *src_key,
    tp_project_sprite **out) {
    if (!a || tp_id128_is_nil(source_ref) || !src_key || src_key[0] == '\0' ||
        !tp_project_atlas_find_source_by_id(a, source_ref)) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char normalized[TP_SRCKEY_MAX];
    if (tp_srckey_normalize(src_key, normalized, sizeof normalized, NULL) !=
            TP_STATUS_OK ||
        strcmp(normalized, src_key) != 0) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_project_sprite *existing =
        tp_project_atlas_find_sprite_by_source_key(a, source_ref, src_key);
    if (existing) {
        if (out) {
            *out = existing;
        }
        return TP_STATUS_OK;
    }
    char bridge[TP_SRCKEY_MAX];
    tp_sprite_export_key(src_key, bridge, sizeof bridge);
    char *name_copy = tp_strdup(bridge);
    char *key_copy = tp_strdup(src_key);
    if (!name_copy || !key_copy) {
        free(name_copy);
        free(key_copy);
        return TP_STATUS_OOM;
    }
    tp_project_sprite *sprite = sprite_push_default(a);
    if (!sprite) {
        free(name_copy);
        free(key_copy);
        return TP_STATUS_OOM;
    }
    sprite->name = name_copy;
    sprite->source_ref = source_ref;
    sprite->src_key = key_copy;
    if (out) {
        *out = sprite;
    }
    return TP_STATUS_OK;
}

static tp_status remove_sprite_at(tp_project_atlas *a, int index) {
    if (!a || index < 0 || index >= a->sprite_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(a->sprites[index].name);
    free(a->sprites[index].src_key);
    free(a->sprites[index].rename);
    for (int j = index; j < a->sprite_count - 1; j++) {
        a->sprites[j] = a->sprites[j + 1];
    }
    a->sprite_count--;
    memset(&a->sprites[a->sprite_count], 0, sizeof *a->sprites);
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_remove_sprite_by_source_key(
    tp_project_atlas *a, tp_id128 source_ref, const char *src_key) {
    tp_project_sprite *sprite =
        tp_project_atlas_find_sprite_by_source_key(a, source_ref, src_key);
    return sprite ? remove_sprite_at(a, (int)(sprite - a->sprites))
                  : TP_STATUS_OUT_OF_BOUNDS;
}

/* An override entry that would serialize to just its name (safe to drop). */
static bool tp_sprite_is_default(const tp_project_sprite *s) {
    return s->origin_x == TP_PROJECT_ORIGIN_DEFAULT && s->origin_y == TP_PROJECT_ORIGIN_DEFAULT &&
           s->slice9_lrtb[0] == 0 && s->slice9_lrtb[1] == 0 && s->slice9_lrtb[2] == 0 && s->slice9_lrtb[3] == 0 &&
           s->rename == NULL && s->ov_shape == TP_PROJECT_OV_INHERIT && s->ov_allow_rotate == TP_PROJECT_OV_INHERIT &&
           s->ov_max_vertices == TP_PROJECT_OV_INHERIT && s->ov_margin == TP_PROJECT_OV_INHERIT &&
           s->ov_extrude == TP_PROJECT_OV_INHERIT;
}

tp_status tp_project_atlas_set_sprite_rename_by_source_key(
    tp_project_atlas *a, tp_id128 source_ref, const char *src_key,
    const char *rename) {
    if (!a || tp_id128_is_nil(source_ref) || !src_key) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_project_sprite *sprite =
        tp_project_atlas_find_sprite_by_source_key(a, source_ref, src_key);
    if (!rename || rename[0] == '\0') {
        if (!sprite) {
            return TP_STATUS_OK;
        }
        free(sprite->rename);
        sprite->rename = NULL;
        return tp_sprite_is_default(sprite)
                   ? tp_project_atlas_remove_sprite_by_source_key(
                         a, source_ref, src_key)
                   : TP_STATUS_OK;
    }
    tp_status status = tp_project_atlas_add_sprite_by_source_key(
        a, source_ref, src_key, &sprite);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char *copy = tp_strdup(rename);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    free(sprite->rename);
    sprite->rename = copy;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_prune_sprite_by_source_key(
    tp_project_atlas *a, tp_id128 source_ref, const char *src_key) {
    if (!a || tp_id128_is_nil(source_ref) || !src_key) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    tp_project_sprite *sprite =
        tp_project_atlas_find_sprite_by_source_key(a, source_ref, src_key);
    if (sprite && tp_sprite_is_default(sprite)) {
        return tp_project_atlas_remove_sprite_by_source_key(a, source_ref,
                                                            src_key);
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_add_animation(tp_project_atlas *a, const char *name, tp_project_anim **out) {
    if (!a || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (!tp_grow((void **)&a->animations, &a->animation_cap, a->animation_count + 1, sizeof(tp_project_anim))) {
        return TP_STATUS_OOM;
    }
    tp_project_anim *an = &a->animations[a->animation_count];
    memset(an, 0, sizeof *an); /* session adoption assigns the fresh ID */
    an->name = tp_strdup(name);
    if (!an->name) {
        return TP_STATUS_OOM;
    }
    an->fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    an->playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    a->animation_count++;
    if (out) {
        *out = an;
    }
    return TP_STATUS_OK;
}

tp_project_anim *tp_project_atlas_find_animation_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a || tp_id128_is_nil(id)) {
        return NULL;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (tp_id128_eq(a->animations[i].id, id)) {
            return &a->animations[i];
        }
    }
    return NULL;
}

tp_status tp_project_atlas_remove_animation_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_id128_is_nil(id)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (tp_id128_eq(a->animations[i].id, id)) {
            tp_free_anim(&a->animations[i]);
            for (int j = i; j < a->animation_count - 1; j++) {
                a->animations[j] = a->animations[j + 1];
            }
            a->animation_count--;
            return TP_STATUS_OK;
        }
    }
    return TP_STATUS_OUT_OF_BOUNDS;
}

tp_status tp_project_anim_add_frame(tp_project_anim *anim,
                                    tp_id128 source_ref,
                                    const char *src_key) {
    if (!anim || tp_id128_is_nil(source_ref) || !src_key ||
        src_key[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char normalized[TP_SRCKEY_MAX];
    if (tp_srckey_normalize(src_key, normalized, sizeof normalized, NULL) !=
            TP_STATUS_OK ||
        strcmp(normalized, src_key) != 0) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    char bridge[TP_SRCKEY_MAX];
    tp_sprite_export_key(src_key, bridge, sizeof bridge);
    char *name_copy = tp_strdup(bridge);
    char *key_copy = tp_strdup(src_key);
    if (!name_copy || !key_copy) {
        free(name_copy);
        free(key_copy);
        return TP_STATUS_OOM;
    }
    if (!tp_grow((void **)&anim->frames, &anim->frame_cap,
                 anim->frame_count + 1, sizeof(tp_project_frame))) {
        free(name_copy);
        free(key_copy);
        return TP_STATUS_OOM;
    }
    tp_project_frame *fr = &anim->frames[anim->frame_count];
    fr->name = name_copy;
    fr->source_ref = source_ref;
    fr->src_key = key_copy;
    anim->frame_count++;
    return TP_STATUS_OK;
}

tp_status tp_project_anim_remove_frame(tp_project_anim *anim, int index) {
    if (!anim) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= anim->frame_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(anim->frames[index].name);
    free(anim->frames[index].src_key);
    for (int j = index; j < anim->frame_count - 1; j++) {
        anim->frames[j] = anim->frames[j + 1];
    }
    anim->frame_count--;
    return TP_STATUS_OK;
}

tp_status tp_project_anim_move_frame(tp_project_anim *anim, int index, int delta) {
    if (!anim) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= anim->frame_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    int dst = index + delta;
    if (dst < 0) {
        dst = 0;
    }
    if (dst > anim->frame_count - 1) {
        dst = anim->frame_count - 1;
    }
    if (dst == index) {
        return TP_STATUS_OK; /* no-op */
    }
    tp_project_frame moved = anim->frames[index];
    if (dst > index) {
        for (int j = index; j < dst; j++) {
            anim->frames[j] = anim->frames[j + 1];
        }
    } else {
        for (int j = index; j > dst; j--) {
            anim->frames[j] = anim->frames[j - 1];
        }
    }
    anim->frames[dst] = moved;
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_add_target(tp_project_atlas *a, const char *exporter_id, const char *out_path,
                                      tp_project_target **out) {
    if (!a || !exporter_id || !out_path) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    const tp_status id_status = tp_exporter_id_validate(exporter_id, NULL);
    if (id_status != TP_STATUS_OK) {
        return id_status;
    }
    if (!tp_grow((void **)&a->targets, &a->target_cap, a->target_count + 1, sizeof(tp_project_target))) {
        return TP_STATUS_OOM;
    }
    tp_project_target *t = &a->targets[a->target_count];
    memset(t, 0, sizeof *t);
    t->exporter_id = tp_strdup(exporter_id);
    t->out_path = tp_strdup(out_path);
    if (!t->exporter_id || !t->out_path) {
        free(t->exporter_id);
        free(t->out_path);
        return TP_STATUS_OOM;
    }
    t->enabled = true;
    a->target_count++;
    if (out) {
        *out = t;
    }
    return TP_STATUS_OK;
}

tp_status tp_project_atlas_seed_default_target(tp_project *p, int atlas_index) {
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    char path[TP_PATH_MAX];
    const int written = snprintf(path, sizeof path, "out/%s", a->name);
    if (written < 0 || (size_t)written >= sizeof path) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    return tp_project_atlas_add_target(a, TP_EXPORTER_ID_JSON_NEOTOLIS, path, NULL);
}

tp_status tp_project_atlas_remove_target(tp_project_atlas *a, int index) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->target_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free(a->targets[index].exporter_id);
    free(a->targets[index].out_path);
    for (int i = index; i < a->target_count - 1; i++) {
        a->targets[i] = a->targets[i + 1];
    }
    a->target_count--;
    return TP_STATUS_OK;
}

tp_project_target *tp_project_atlas_find_target_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a || tp_id128_is_nil(id)) {
        return NULL;
    }
    for (int i = 0; i < a->target_count; i++) {
        if (tp_id128_eq(a->targets[i].id, id)) {
            return &a->targets[i];
        }
    }
    return NULL;
}

tp_status tp_project_atlas_remove_target_by_id(tp_project_atlas *a, tp_id128 id) {
    if (!a) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (tp_id128_is_nil(id)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    for (int i = 0; i < a->target_count; i++) {
        if (tp_id128_eq(a->targets[i].id, id)) {
            return tp_project_atlas_remove_target(a, i);
        }
    }
    return TP_STATUS_OUT_OF_BOUNDS;
}

tp_status tp_project_atlas_set_target(tp_project_atlas *a, int index, const char *exporter_id, const char *out_path,
                                      bool enabled) {
    if (!a || !exporter_id || exporter_id[0] == '\0' || !out_path || out_path[0] == '\0') {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    if (index < 0 || index >= a->target_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    const tp_status id_status = tp_exporter_id_validate(exporter_id, NULL);
    if (id_status != TP_STATUS_OK) {
        return id_status;
    }
    char *eid = tp_strdup(exporter_id);
    char *op = tp_strdup(out_path);
    if (!eid || !op) {
        free(eid);
        free(op);
        return TP_STATUS_OOM;
    }
    tp_project_target *t = &a->targets[index];
    free(t->exporter_id);
    free(t->out_path);
    t->exporter_id = eid;
    t->out_path = op;
    t->enabled = enabled;
    return TP_STATUS_OK;
}

bool tp_project_out_path_shared(const tp_project *p, const char *out_path, const tp_project_target *self) {
    if (!p || !out_path || out_path[0] == '\0') {
        return false;
    }
    /* Compare SLASH-NORMALIZED (matching the exporter's tp_normalize_slashes), so a separator-only
     * difference ("out\x" vs "out/x") that resolves to ONE file is caught. */
    char want[TP_PATH_MAX];
    if ((size_t)snprintf(want, sizeof want, "%s", out_path) >= sizeof want) {
        return false; /* pathological length -> not a collision we can assert */
    }
    tp_normalize_slashes(want);
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        for (int t = 0; t < a->target_count; t++) {
            const tp_project_target *tg = &a->targets[t];
            if (tg == self) {
                continue; /* exclude the caller's own target by IDENTITY (robust to nil ids) */
            }
            if (!tg->enabled) {
                continue; /* the exporter skips disabled targets -> they never overwrite */
            }
            if (!tg->out_path || tg->out_path[0] == '\0') {
                continue;
            }
            char have[TP_PATH_MAX];
            if ((size_t)snprintf(have, sizeof have, "%s", tg->out_path) >= sizeof have) {
                continue;
            }
            tp_normalize_slashes(have);
            if (strcmp(want, have) == 0) {
                return true;
            }
        }
    }
    return false;
}

tp_status tp_project_next_atlas_defaults(const tp_project *p, char *name,
                                         size_t name_cap, char *out_path,
                                         size_t out_path_cap,
                                         const char **exporter_id,
                                         bool *target_enabled, tp_error *err) {
    if (!p || !name || name_cap == 0U || !out_path || out_path_cap == 0U ||
        !exporter_id || !target_enabled) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "atlas defaults need project and output fields");
    }
    for (int n = 1; n < INT_MAX; ++n) {
        const int nw = snprintf(name, name_cap, "atlas%d", n);
        const int ow = snprintf(out_path, out_path_cap, "out/%s", name);
        if (nw < 0 || ow < 0 || (size_t)nw >= name_cap ||
            (size_t)ow >= out_path_cap) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "atlas default name exceeds output capacity");
        }
        bool name_used = false;
        for (int i = 0; i < p->atlas_count; ++i) {
            if (p->atlases[i].name && strcmp(p->atlases[i].name, name) == 0) {
                name_used = true;
                break;
            }
        }
        if (!name_used && !tp_project_out_path_shared(p, out_path, NULL)) {
            *exporter_id = TP_EXPORTER_ID_JSON_NEOTOLIS;
            *target_enabled = true;
            return TP_STATUS_OK;
        }
    }
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "atlas default namespace is exhausted");
}

tp_status tp_project_next_animation_name(const tp_project *p, tp_id128 atlas_id,
                                         const char *base, char *name,
                                         size_t name_cap, tp_error *err) {
    if (!p || !name || name_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "animation defaults need project and output buffer");
    }
    const int atlas_index = tp_project_find_atlas_by_id(p, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "animation defaults atlas was not found");
    }
    const tp_project_atlas *atlas = &p->atlases[atlas_index];
    const bool automatic = !base || base[0] == '\0';
    for (int n = automatic ? 1 : 0; n < INT_MAX; ++n) {
        const int written = automatic
                                ? snprintf(name, name_cap, "anim%d", n)
                                : (n == 0 ? snprintf(name, name_cap, "%s", base)
                                          : snprintf(name, name_cap, "%s%d", base, n + 1));
        if (written < 0 || (size_t)written >= name_cap) {
            name[0] = '\0';
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "animation default name exceeds output capacity");
        }
        bool used = false;
        for (int i = 0; i < atlas->animation_count; ++i) {
            if (atlas->animations[i].name &&
                strcmp(atlas->animations[i].name, name) == 0) {
                used = true;
                break;
            }
        }
        if (!used) {
            return TP_STATUS_OK;
        }
    }
    name[0] = '\0';
    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                        "animation default namespace is exhausted");
}

tp_status tp_project_target_defaults(const tp_project *p, tp_id128 atlas_id,
                                     const char **exporter_id, char *out_path,
                                     size_t out_path_cap, bool *enabled,
                                     tp_error *err) {
    if (!p || !exporter_id || !out_path || out_path_cap == 0U || !enabled) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "target defaults need project and output fields");
    }
    const int atlas_index = tp_project_find_atlas_by_id(p, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "target defaults atlas was not found");
    }
    const char *atlas_name = p->atlases[atlas_index].name
                                 ? p->atlases[atlas_index].name
                                 : "atlas";
    const int written = snprintf(out_path, out_path_cap, "out/%s", atlas_name);
    if (written < 0 || (size_t)written >= out_path_cap) {
        out_path[0] = '\0';
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "target default path exceeds output capacity");
    }
    *exporter_id = TP_EXPORTER_ID_JSON_NEOTOLIS;
    *enabled = true;
    return TP_STATUS_OK;
}


typedef enum tp_temp_open_result {
    TP_TEMP_OPEN_OK = 0,
    TP_TEMP_OPEN_PATH_TOO_LONG,
    TP_TEMP_OPEN_FAILED,
} tp_temp_open_result;

static _Thread_local bool s_test_fail_next_temp_create;
static _Thread_local bool s_test_fail_next_file_sync;
static _Thread_local bool s_test_fail_next_parent_sync;
static _Thread_local tp_file_io_phase s_test_fail_next_save_io;
static bool s_test_save_max_bytes_armed;
static size_t s_test_save_max_bytes;

void tp_project__test_fail_next_temp_create(void) { s_test_fail_next_temp_create = true; }
void tp_project__test_fail_next_file_sync(void) { s_test_fail_next_file_sync = true; }
void tp_project__test_fail_next_parent_sync(void) {
    s_test_fail_next_parent_sync = true;
}
void tp_project__test_fail_next_save_io(tp_file_io_phase phase) {
    s_test_fail_next_save_io = phase;
}
void tp_project__test_set_save_max_bytes(size_t max_bytes) {
    s_test_save_max_bytes = max_bytes;
    s_test_save_max_bytes_armed = true;
}

/* Create a unique sibling temp atomically and keep that exact file open. There
 * is deliberately no remove-before-open: an existing name belongs to another
 * writer (or an interrupted save), so it is skipped rather than followed or
 * deleted. The returned FILE owns the underlying descriptor/handle. */
static tp_temp_open_result tp_open_save_temp(const char *path, char *tmp,
                                             size_t tmp_cap, FILE **out,
                                             int *out_native_code) {
    *out = NULL;
    *out_native_code = 0;
    if (s_test_fail_next_temp_create ||
        s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_OPEN) {
        s_test_fail_next_temp_create = false;
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        *out_native_code = EACCES;
        return TP_TEMP_OPEN_FAILED;
    }
    static _Atomic uint64_t counter;
    const unsigned long pid = (unsigned long)tp_getpid();
    for (unsigned int attempt = 0; attempt < 128U; attempt++) {
        const uint64_t serial =
            atomic_fetch_add_explicit(&counter, UINT64_C(1),
                                      memory_order_relaxed) +
            UINT64_C(1);
        int nt = snprintf(tmp, tmp_cap, "%s.savetmp.%08lx.%016llx", path,
                          pid, (unsigned long long)serial);
        if (nt <= 0 || (size_t)nt >= tmp_cap) {
            return TP_TEMP_OPEN_PATH_TOO_LONG;
        }
        FILE *f = tp_fs_create_exclusive(tmp, false);
        if (!f) {
            if (errno == EEXIST) {
                continue;
            }
            *out_native_code = errno != 0 ? errno : EIO;
            return TP_TEMP_OPEN_FAILED;
        }
#ifndef _WIN32
        /* Preserve collaborative permissions only for an existing regular
         * destination. A symlink is replaced, never followed; a new project
         * keeps the exclusive creator's private mode. */
        struct stat destination;
        if (lstat(path, &destination) == 0 &&
            S_ISREG(destination.st_mode) &&
            fchmod(fileno(f), destination.st_mode & 0777) != 0) {
            *out_native_code = errno != 0 ? errno : EIO;
            (void)tp_fs_close(f);
            (void)tp_fs_remove_file(tmp);
            return TP_TEMP_OPEN_FAILED;
        }
#endif
        *out = f;
        return TP_TEMP_OPEN_OK;
    }
    *out_native_code = EEXIST;
    return TP_TEMP_OPEN_FAILED;
}

typedef struct tp_save_path_restore_entry {
    char **slot;
    char *original;
} tp_save_path_restore_entry;

typedef struct tp_save_path_restore {
    tp_save_path_restore_entry *entries;
    size_t count;
    size_t capacity;
} tp_save_path_restore;

static tp_status tp_project_save_stage(tp_project *p, const char *path,
                                       tp_id128 *out_fingerprint,
                                       const tp_id128 *expected_fingerprint,
                                       bool create_only,
                                       tp_save_path_restore *restore,
                                       tp_error *err) {
    if (out_fingerprint) {
        memset(out_fingerprint, 0, sizeof *out_fingerprint);
    }
    if (!p || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save: NULL project or path");
    }
    if (!tp_utf8_is_valid_c_string(path)) {
        return tp_error_set(err, TP_STATUS_INVALID_UTF8,
                            "tp_project_save: path is not valid UTF-8");
    }

    char new_dir[TP_PATH_MAX];
    tp_status st = tp_abs_dir_of(path, new_dir, sizeof new_dir);
    if (st != TP_STATUS_OK) {
        return tp_error_set(err, st, "tp_project_save: path too long: %s", path);
    }

    /* Relativize absolute sources against the (new) project dir; normalize the
     * rest to '/'. Absolute paths not relatable to new_dir are kept as-is. */
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char norm[TP_PATH_MAX];
            if ((size_t)snprintf(norm, sizeof norm, "%s", a->sources[si].path) >= sizeof norm) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: source path too long");
            }
            tp_normalize_slashes(norm);
            char rel[TP_PATH_MAX];
            if (tp_path_is_absolute(norm)) {
                st = tp_relativize(norm, new_dir, rel, sizeof rel);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err, st, "tp_project_save: cannot relativize %s", norm);
                }
            } else if (p->source_base_dir || p->project_dir) {
                char resolved[TP_PATH_MAX];
                st = tp_project_resolve_source_path(p, norm, resolved,
                                                    sizeof resolved);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err, st,
                                        "tp_project_save: cannot resolve %s", norm);
                }
                st = tp_relativize(resolved, new_dir, rel, sizeof rel);
                if (st != TP_STATUS_OK) {
                    return tp_error_set(err, st,
                                        "tp_project_save: cannot relativize %s",
                                        resolved);
                }
            } else if ((size_t)snprintf(rel, sizeof rel, "%s", norm) >= sizeof rel) {
                return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: source path too long");
            }
            char *copy = tp_strdup(rel);
            if (!copy) {
                return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
            }
            if (restore) {
                if (restore->count >= restore->capacity) {
                    free(copy);
                    return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                        "tp_project_save: source restore capacity overflow");
                }
                tp_save_path_restore_entry *entry =
                    &restore->entries[restore->count++];
                entry->slot = &a->sources[si].path;
                entry->original = a->sources[si].path;
                a->sources[si].path = copy;
            } else {
                free(a->sources[si].path);
                a->sources[si].path = copy;
            }
        }
    }

    if (!p->source_base_dir) {
        p->source_base_dir = tp_strdup(new_dir);
        if (!p->source_base_dir) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_project_save: out of memory");
        }
    }

    /* Update the staged project dir (Save / Save-As). */
    char *dir_copy = tp_strdup(new_dir);
    if (!dir_copy) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: out of memory");
    }
    free(p->project_dir);
    p->project_dir = dir_copy;

    /* file-save = relativize (above) + canonical buffer + durable publish. */
    char *buf = NULL;
    size_t len = 0;
    tp_status bst = tp_project_save_buffer(p, &buf, &len, err);
    if (bst != TP_STATUS_OK) {
        return bst;
    }
    size_t save_max_bytes = (size_t)TP_IDENTITY_FILE_MAX_BYTES;
    if (s_test_save_max_bytes_armed) {
        save_max_bytes = s_test_save_max_bytes;
        s_test_save_max_bytes_armed = false;
    }
    if (len > save_max_bytes) {
        free(buf);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_project_save: serialized project exceeds the %zu-byte limit", save_max_bytes);
    }
    tp_id128 written_fingerprint = {{0}};
    if (out_fingerprint) {
        bst = tp_identity_bytes_fingerprint(buf, len, &written_fingerprint, err);
        if (bst != TP_STATUS_OK) {
            free(buf);
            return bst;
        }
    }

    /* Atomic durable write: serialize to a sibling temp, check a durable file
     * sync, close, atomically publish, then sync the containing directory. A
     * short write / file-sync / close failure leaves `path` untouched. A
     * directory-sync failure happens after publication and therefore returns
     * FILE_DURABILITY_UNCERTAIN: the saved bytes are authoritative and clients
     * must surface a warning rather than lie that Save had no side effect.
     *
     * ATOMIC-SAVE TRADEOFFS (accepted -- these are inherent to any atomic-rename save, as done by editors and
     * git): replacing the destination via rename/MoveFileEx swaps the inode. POSIX preserves an existing
     * regular destination's permission bits, while a new file keeps private mode 0600; owner and ACLs may
     * still change. On Windows the new file inherits the parent ACL. A `path`
     * that is a SYMLINK is replaced by a regular file rather than written through; a save into a READ-ONLY
     * containing directory fails (the sibling temp cannot be created) where an in-place truncate once succeeded.
     * For a `.ntpacker_project` JSON file these are immaterial, and every one fails CLOSED -- an error, never a
     * corrupt/partial file. */
    char tmp[TP_PATH_MAX];
    FILE *f = NULL;
    int native_code = 0;
    const tp_temp_open_result temp_rc =
        tp_open_save_temp(path, tmp, sizeof tmp, &f, &native_code);
    if (temp_rc == TP_TEMP_OPEN_PATH_TOO_LONG) {
        free(buf);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_save: path too long: %s", path);
    }
    if (temp_rc != TP_TEMP_OPEN_OK) {
        free(buf);
        return tp_error_set_file_io(
            err, TP_FILE_IO_PHASE_TEMP_OPEN, path, native_code,
            "tp_project_save: cannot create temporary file for %s", path);
    }
    tp_file_io_phase failed_phase = TP_FILE_IO_PHASE_NONE;
    bool wrote = false;
    if (s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_WRITE) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = ENOSPC;
        failed_phase = TP_FILE_IO_PHASE_TEMP_WRITE;
    } else {
        wrote = tp_fs_write_all(f, buf, len);
        if (!wrote) {
            native_code = errno != 0 ? errno : EIO;
            failed_phase = TP_FILE_IO_PHASE_TEMP_WRITE;
        }
    }
    bool synced = false;
    if (wrote) {
        if (s_test_fail_next_file_sync ||
            s_test_fail_next_save_io == TP_FILE_IO_PHASE_FILE_SYNC) {
            s_test_fail_next_file_sync = false;
            s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
            native_code = EIO;
            failed_phase = TP_FILE_IO_PHASE_FILE_SYNC;
        } else {
            synced = tp_fs_sync(f);
            if (!synced) {
                native_code = errno != 0 ? errno : EIO;
                failed_phase = TP_FILE_IO_PHASE_FILE_SYNC;
            }
        }
    }
    const bool close_result = tp_fs_close(f);
    bool closed = close_result;
    if (s_test_fail_next_save_io == TP_FILE_IO_PHASE_TEMP_CLOSE) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = EIO;
        failed_phase = TP_FILE_IO_PHASE_TEMP_CLOSE;
        closed = false;
    } else if (!close_result && failed_phase == TP_FILE_IO_PHASE_NONE) {
        native_code = errno != 0 ? errno : EIO;
        failed_phase = TP_FILE_IO_PHASE_TEMP_CLOSE;
    }
    free(buf);
    if (!wrote || !synced || !closed) {
        (void)tp_fs_remove_file(tmp);
        return tp_error_set_file_io(
            err, failed_phase, path, native_code,
            "tp_project_save: could not durably write temporary file for %s",
            path);
    }
    /* Optimistic concurrency is checked as late as portability allows: after the complete replacement
     * is durable in its sibling temp, immediately before the atomic promotion. This closes the large
     * serialize/write window left by GUI-only preflight checks. A non-cooperating writer can still race
     * the final fingerprint->rename instructions; no portable filesystem CAS exists for replacement. */
    if (expected_fingerprint) {
        tp_id128 current = {{0}};
        tp_status fps = tp_identity_file_fingerprint(path, &current, err);
        if (fps != TP_STATUS_OK || !tp_id128_eq(current, *expected_fingerprint)) {
            (void)tp_fs_remove_file(tmp);
            return tp_error_set(err, TP_STATUS_FILE_CHANGED_EXTERNALLY,
                                "tp_project_save: destination changed before publish: %s", path);
        }
    }
    /* Atomically publish the fully-written temp. Create-only publication must
     * fail if another writer won the destination after our earlier checks. */
    bool moved = false;
    bool destination_exists = false;
    const tp_file_io_phase publish_phase =
        create_only ? TP_FILE_IO_PHASE_ATOMIC_CREATE
                    : TP_FILE_IO_PHASE_ATOMIC_REPLACE;
    const bool inject_publish_failure =
        s_test_fail_next_save_io == publish_phase;
    if (inject_publish_failure) {
        s_test_fail_next_save_io = TP_FILE_IO_PHASE_NONE;
        native_code = EACCES;
    }
    if (!inject_publish_failure && create_only) {
        const tp_fs_move_result move_result =
            tp_fs_move_no_replace(tmp, path);
        moved = move_result == TP_FS_MOVE_OK;
        destination_exists =
            move_result == TP_FS_MOVE_DESTINATION_EXISTS;
        if (!moved && !destination_exists) {
            native_code = errno != 0 ? errno : EIO;
        }
    } else if (!inject_publish_failure) {
        moved = tp_fs_replace(tmp, path);
        if (!moved) {
            native_code = errno != 0 ? errno : EIO;
        }
    }
    if (!moved) {
        (void)tp_fs_remove_file(tmp);
        if (destination_exists) {
            return tp_error_set(err, TP_STATUS_FILE_EXISTS,
                                "tp_project_save: destination already exists: %s",
                                path);
        }
        return tp_error_set_file_io(
            err, publish_phase, path, native_code,
            "tp_project_save: could not finalize save to %s", path);
    }
    if (out_fingerprint) {
        *out_fingerprint = written_fingerprint;
    }
    bool parent_synced = false;
    if (s_test_fail_next_parent_sync) {
        s_test_fail_next_parent_sync = false;
    } else {
        parent_synced = tp_fs_sync_parent(path);
    }
    if (!parent_synced) {
        return tp_error_set(
            err, TP_STATUS_FILE_DURABILITY_UNCERTAIN,
            "tp_project_save: %s was published, but directory durability could not be confirmed",
            path);
    }
    return TP_STATUS_OK;
}

/* The staged clone owns serialization-only path normalization. Publish the new
 * project-file directory while preserving an established live source base. */
static void tp_project_adopt_saved_dir(tp_project *dst, tp_project *stage) {
    free(dst->project_dir);
    dst->project_dir = stage->project_dir;
    stage->project_dir = NULL;
    if (!dst->source_base_dir) {
        dst->source_base_dir = stage->source_base_dir;
        stage->source_base_dir = NULL;
    }
}

static tp_status tp_project_save_staged(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                        const tp_id128 *expected_fingerprint,
                                        bool create_only, tp_error *err) {
    if (out_fingerprint) {
        memset(out_fingerprint, 0, sizeof *out_fingerprint);
    }
    if (!p || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save: NULL project or path");
    }
    tp_project *stage = tp_project_clone(p);
    if (!stage) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_save: could not stage project paths");
    }
    tp_status st = tp_project_save_stage(stage, path, out_fingerprint,
                                         expected_fingerprint, create_only,
                                         NULL, err);
    if (st == TP_STATUS_OK ||
        st == TP_STATUS_FILE_DURABILITY_UNCERTAIN) {
        tp_project_adopt_saved_dir(p, stage);
    }
    tp_project_destroy(stage);
    return st;
}

tp_status tp_project_save_candidate_with_fingerprint(
    tp_project *candidate, const char *path,
    const tp_id128 *expected_fingerprint, bool create_only,
    tp_id128 *out_fingerprint, tp_error *err) {
    if (!candidate || !path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_save: NULL candidate or path");
    }
    size_t source_count = 0U;
    for (int ai = 0; ai < candidate->atlas_count; ++ai) {
        const int count = candidate->atlases[ai].source_count;
        if (count < 0 || (size_t)count > SIZE_MAX - source_count) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_project_save: source restore count overflow");
        }
        source_count += (size_t)count;
    }
    tp_save_path_restore restore = {0};
    if (source_count > 0U) {
        restore.entries = calloc(source_count, sizeof *restore.entries);
        if (!restore.entries) {
            return tp_error_set(err, TP_STATUS_OOM,
                                "tp_project_save: source restore allocation failed");
        }
        restore.capacity = source_count;
    }
    const tp_status status = tp_project_save_stage(
        candidate, path, out_fingerprint, expected_fingerprint, create_only,
        &restore, err);
    for (size_t i = 0U; i < restore.count; ++i) {
        tp_save_path_restore_entry *entry = &restore.entries[i];
        char *normalized = *entry->slot;
        *entry->slot = entry->original;
        free(normalized);
    }
    free(restore.entries);
    return status;
}

tp_status tp_project_save_with_fingerprint(tp_project *p, const char *path, tp_id128 *out_fingerprint,
                                           tp_error *err) {
    return tp_project_save_staged(p, path, out_fingerprint, NULL, false, err);
}

tp_status tp_project_save_new_with_fingerprint(
    tp_project *p, const char *path, tp_id128 *out_fingerprint, tp_error *err) {
    return tp_project_save_staged(p, path, out_fingerprint, NULL, true, err);
}

tp_status tp_project_save_if_unchanged(tp_project *p, const char *path,
                                       const tp_id128 *expected_fingerprint,
                                       tp_id128 *out_fingerprint, tp_error *err) {
    if (!expected_fingerprint) {
        if (out_fingerprint) {
            memset(out_fingerprint, 0, sizeof *out_fingerprint);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_save_if_unchanged: NULL expected fingerprint");
    }
    return tp_project_save_staged(p, path, out_fingerprint,
                                  expected_fingerprint, false, err);
}

tp_status tp_project_save(tp_project *p, const char *path, tp_error *err) {
    return tp_project_save_with_fingerprint(p, path, NULL, err);
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
