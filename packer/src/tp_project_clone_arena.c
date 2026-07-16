/*
 * P-01 (plan §17): arena-backed tp_project deep-clone. See tp_project_clone_arena.h
 * for the contract. Mirrors tp_project_clone.c field-for-field so the two stay
 * byte-identical; the difference is only the allocator (a single tp_arena vs
 * ~250k malloc/strdup calls) and the OOM model (structural: destroy the arena).
 */

#include "tp_project_clone_arena.h"

#include <string.h>

/* Arena alignment mirror (tp_arena.c TP_ARENA_ALIGN). Every arena allocation's
 * size is rounded up to this, so a block sized to the summed footprint holds the
 * whole clone with zero waste. */
#define TP_ACL_ALIGN 8U

static size_t acl_align_up(size_t v) {
    const size_t rem = v % TP_ACL_ALIGN;
    return (rem == 0U) ? v : v + (TP_ACL_ALIGN - rem);
}

/* Footprint contribution of an optionally-NULL owned string. */
static size_t acl_str_bytes(const char *s) { return s ? acl_align_up(strlen(s) + 1U) : 0U; }

/* ---- footprint (sizing pass) --------------------------------------------- */

static size_t acl_atlas_footprint(const tp_project_atlas *a) {
    size_t n = 0U;
    n += acl_str_bytes(a->name);

    if (a->source_count > 0) {
        n += acl_align_up((size_t)a->source_count * sizeof(tp_project_source));
    }
    for (int i = 0; i < a->source_count; i++) {
        n += acl_str_bytes(a->sources[i].path);
    }

    if (a->sprite_count > 0) {
        n += acl_align_up((size_t)a->sprite_count * sizeof(tp_project_sprite));
    }
    for (int i = 0; i < a->sprite_count; i++) {
        n += acl_str_bytes(a->sprites[i].name);
        n += acl_str_bytes(a->sprites[i].src_key);
        n += acl_str_bytes(a->sprites[i].rename);
    }

    if (a->animation_count > 0) {
        n += acl_align_up((size_t)a->animation_count * sizeof(tp_project_anim));
    }
    for (int i = 0; i < a->animation_count; i++) {
        const tp_project_anim *an = &a->animations[i];
        n += acl_str_bytes(an->name);
        if (an->frame_count > 0) {
            n += acl_align_up((size_t)an->frame_count * sizeof(tp_project_frame));
        }
        for (int f = 0; f < an->frame_count; f++) {
            n += acl_str_bytes(an->frames[f].name);
            n += acl_str_bytes(an->frames[f].src_key);
        }
    }

    if (a->target_count > 0) {
        n += acl_align_up((size_t)a->target_count * sizeof(tp_project_target));
    }
    for (int i = 0; i < a->target_count; i++) {
        n += acl_str_bytes(a->targets[i].exporter_id);
        n += acl_str_bytes(a->targets[i].out_path);
    }
    return n;
}

size_t tp_project_clone_arena_footprint(const tp_project *src) {
    if (!src) {
        return 0U;
    }
    size_t n = acl_align_up(sizeof(tp_project));
    n += acl_str_bytes(src->project_dir);
    n += acl_str_bytes(src->source_base_dir);
    if (src->atlas_count > 0) {
        n += acl_align_up((size_t)src->atlas_count * sizeof(tp_project_atlas));
    }
    for (int i = 0; i < src->atlas_count; i++) {
        n += acl_atlas_footprint(&src->atlases[i]);
    }
    return n;
}

/* ---- copy (fill pass) ----------------------------------------------------- */

/* Duplicate an optionally-NULL owned string into the arena. Returns true on
 * success (including the intended-NULL case: src NULL -> *dst NULL, true); false
 * ONLY on a true arena OOM (src non-NULL but strdup returned NULL). */
static bool acl_dup(tp_arena *ar, const char *src, char **dst) {
    if (!src) {
        *dst = NULL;
        return true;
    }
    char *p = tp_arena_strdup(ar, src);
    *dst = p;
    return p != NULL;
}

/* Allocate a clone array of `count` elements of `elem` bytes from the arena, or
 * NULL for count==0 (a valid empty collection). Returns false ONLY on OOM for a
 * non-empty array. Arena memory is uninitialized, so every element MUST be fully
 * written by the caller (each copy path below assigns every field). */
static bool acl_array(tp_arena *ar, int count, size_t elem, void **out) {
    if (count <= 0) {
        *out = NULL;
        return true;
    }
    void *p = tp_arena_alloc(ar, (size_t)count * elem);
    *out = p;
    return p != NULL;
}

static bool acl_clone_frames(tp_arena *ar, const tp_project_anim *src, tp_project_anim *dst) {
    if (!acl_array(ar, src->frame_count, sizeof(tp_project_frame), (void **)&dst->frames)) {
        return false;
    }
    dst->frame_count = src->frame_count;
    dst->frame_cap = src->frame_count; /* exact-fit, mirrors the malloc clone */
    for (int i = 0; i < src->frame_count; i++) {
        tp_project_frame *df = &dst->frames[i];
        df->source_ref = src->frames[i].source_ref; /* scalar id value */
        if (!acl_dup(ar, src->frames[i].name, &df->name)) {
            return false;
        }
        if (!acl_dup(ar, src->frames[i].src_key, &df->src_key)) {
            return false;
        }
    }
    return true;
}

static bool acl_clone_atlas(tp_arena *ar, const tp_project_atlas *src, tp_project_atlas *dst) {
    *dst = *src; /* copy every scalar (knobs, id, id_synthetic); re-own pointers below */
    dst->name = NULL;
    dst->sources = NULL;
    dst->source_count = dst->source_cap = 0;
    dst->sprites = NULL;
    dst->sprite_count = dst->sprite_cap = 0;
    dst->animations = NULL;
    dst->animation_count = dst->animation_cap = 0;
    dst->targets = NULL;
    dst->target_count = dst->target_cap = 0;

    if (!acl_dup(ar, src->name, &dst->name)) {
        return false;
    }

    if (!acl_array(ar, src->source_count, sizeof(tp_project_source), (void **)&dst->sources)) {
        return false;
    }
    dst->source_count = src->source_count;
    dst->source_cap = src->source_count;
    for (int i = 0; i < src->source_count; i++) {
        dst->sources[i].id = src->sources[i].id;
        dst->sources[i].id_synthetic = src->sources[i].id_synthetic;
        dst->sources[i].kind = src->sources[i].kind;
        if (!acl_dup(ar, src->sources[i].path, &dst->sources[i].path)) {
            return false;
        }
    }

    if (!acl_array(ar, src->sprite_count, sizeof(tp_project_sprite), (void **)&dst->sprites)) {
        return false;
    }
    dst->sprite_count = src->sprite_count;
    dst->sprite_cap = src->sprite_count;
    for (int i = 0; i < src->sprite_count; i++) {
        tp_project_sprite *ds = &dst->sprites[i];
        *ds = src->sprites[i]; /* scalars: source_ref, origins, slice9, ov_* */
        ds->name = ds->src_key = ds->rename = NULL;
        if (!acl_dup(ar, src->sprites[i].name, &ds->name)) {
            return false;
        }
        if (!acl_dup(ar, src->sprites[i].src_key, &ds->src_key)) {
            return false;
        }
        if (!acl_dup(ar, src->sprites[i].rename, &ds->rename)) {
            return false;
        }
    }

    if (!acl_array(ar, src->animation_count, sizeof(tp_project_anim), (void **)&dst->animations)) {
        return false;
    }
    dst->animation_count = src->animation_count;
    dst->animation_cap = src->animation_count;
    for (int i = 0; i < src->animation_count; i++) {
        tp_project_anim *da = &dst->animations[i];
        *da = src->animations[i]; /* scalars: id, id_synthetic, fps, playback, flips */
        da->name = NULL;
        da->frames = NULL;
        da->frame_count = da->frame_cap = 0;
        if (!acl_dup(ar, src->animations[i].name, &da->name)) {
            return false;
        }
        if (!acl_clone_frames(ar, &src->animations[i], da)) {
            return false;
        }
    }

    if (!acl_array(ar, src->target_count, sizeof(tp_project_target), (void **)&dst->targets)) {
        return false;
    }
    dst->target_count = src->target_count;
    dst->target_cap = src->target_count;
    for (int i = 0; i < src->target_count; i++) {
        tp_project_target *dt = &dst->targets[i];
        dt->id = src->targets[i].id;
        dt->id_synthetic = src->targets[i].id_synthetic;
        dt->enabled = src->targets[i].enabled;
        dt->exporter_id = dt->out_path = NULL;
        if (!acl_dup(ar, src->targets[i].exporter_id, &dt->exporter_id)) {
            return false;
        }
        if (!acl_dup(ar, src->targets[i].out_path, &dt->out_path)) {
            return false;
        }
    }
    return true;
}

tp_project *tp_project_clone_into_arena(const tp_project *src, tp_arena *arena) {
    if (!src || !arena) {
        return NULL;
    }
    tp_project *dst = (tp_project *)tp_arena_alloc(arena, sizeof(tp_project));
    if (!dst) {
        return NULL;
    }
    dst->schema_version = src->schema_version;
    dst->project_dir = NULL;
    dst->source_base_dir = NULL;
    dst->atlases = NULL;
    dst->atlas_count = 0;
    dst->atlas_cap = 0;

    if (!acl_dup(arena, src->project_dir, &dst->project_dir)) {
        return NULL;
    }
    if (!acl_dup(arena, src->source_base_dir, &dst->source_base_dir)) {
        return NULL;
    }
    if (!acl_array(arena, src->atlas_count, sizeof(tp_project_atlas), (void **)&dst->atlases)) {
        return NULL;
    }
    dst->atlas_count = src->atlas_count;
    dst->atlas_cap = src->atlas_count;
    for (int i = 0; i < src->atlas_count; i++) {
        if (!acl_clone_atlas(arena, &src->atlases[i], &dst->atlases[i])) {
            return NULL;
        }
    }
    return dst;
}
