#include "tp_core/tp_project.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS (default-target seeding) */
#include "tp_core/tp_id.h"     /* structural shape-ID parse/format */
#include "tp_core/tp_names.h"  /* canonical display-name derivation */
#include "tp_core/tp_pack.h"
#include "tp_core/tp_srckey.h" /* canonical source-key normalization */
#include "tp_project_internal.h"
#include "tp_project_model_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_path_internal.h"
#include "tp_source_path_text_internal.h"
#include "tp_strutil.h"                 /* shared tp_strdup (one core definition) */

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

tp_project *tp_project_alloc_empty(void) {
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
tp_status atlas_push_source(tp_project_atlas *a, const char *path, tp_source_kind kind, tp_id128 id) {
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

tp_project_sprite *sprite_push_default(tp_project_atlas *a) {
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
