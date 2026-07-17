/*
 * Element-level deep-copy / free + positional collection primitives for the
 * semantic diff. The diff STATE-CAPTURES touched entities (docs/decisions/0012), so
 * it needs to copy one source/sprite/animation/target/frame/atlas-subtree out of the
 * live model and, on inverse/redo, splice a deep copy back in at an exact index.
 *
 * These mirror tp_project_clone.c's element copies (fill_atlas/anim/source/target/
 * frame/copy_sprite_fields <-> clone_atlas/clone_frames) but are kept SEPARATE on
 * purpose: routing them through the diff fault-seam (tp_diff__alloc), not the clone
 * seam (cl_alloc), keeps tp_project_clone.c's clone alloc-count goldens byte-stable, and the diff
 * owns its captured data with its own single-free discipline (decision 0012 §6).
 *
 * !! FORK-SYNC WARNING !! A persistent field added to tp_project_clone.c
 * MUST be added here too, or Undo/Redo silently restores a non-byte-identical project.
 * The safety net is the completeness oracle test_completeness_oracle_* in test_diff.c:
 * it sets EVERY persistent field of EVERY entity kind non-default and asserts
 * remove->inverse is byte-identical, so a missed field fails there loudly. When you add
 * a field, update BOTH copies AND make_maximal() in test_diff.c.
 *
 * OOM-safety invariant (same as tp_project_clone): every owned pointer in a growing
 * copy is ALWAYS a valid malloc or NULL, and a collection count only ever covers
 * slots whose scalars are set and whose owned pointers are set-or-NULL. So a single
 * free of the partial copy (or tp_project_destroy of the clone it was spliced into)
 * frees exactly what was built -- no leak, no double free.
 */

#include "tp_core/tp_project.h"

#include <stdlib.h>
#include <string.h>

#include "tp_diff_internal.h"

/* ---- allocation fault seam (test-only; default disabled) ------------------ */
static _Thread_local int s_fail = -1; /* countdown; -1 disabled. Fires once. */
static _Thread_local int s_count = 0; /* allocations since the last reset */
static _Thread_local bool s_record_budget_active = false;
static _Thread_local bool s_record_budget_exceeded = false;
static _Thread_local size_t s_record_budget_limit = 0U;
static _Thread_local size_t s_record_budget_bytes = 0U;

void tp_diff__test_set_alloc_fail(int nth) { s_fail = nth; }
int tp_diff__test_alloc_count(void) { return s_count; }
void tp_diff__test_reset_alloc_count(void) { s_count = 0; }

void tp_diff__record_budget_begin(size_t byte_limit) {
    s_record_budget_active = true;
    s_record_budget_exceeded = false;
    s_record_budget_limit = byte_limit;
    s_record_budget_bytes = 0U;
}

bool tp_diff__record_budget_exceeded(void) { return s_record_budget_active && s_record_budget_exceeded; }

bool tp_diff__record_budget_end(size_t *bytes) {
    const bool ok = s_record_budget_active && !s_record_budget_exceeded;
    if (bytes) {
        *bytes = ok ? s_record_budget_bytes : 0U;
    }
    s_record_budget_active = false;
    s_record_budget_exceeded = false;
    s_record_budget_limit = 0U;
    s_record_budget_bytes = 0U;
    return ok;
}

void *tp_diff__alloc(size_t n) {
    s_count++;
    if (s_record_budget_active && n > s_record_budget_limit - s_record_budget_bytes) {
        s_record_budget_exceeded = true;
        return NULL;
    }
    if (s_fail == 0) {
        s_fail = -1;
        return NULL;
    }
    if (s_fail > 0) {
        s_fail--;
    }
    void *p = calloc(1, n);
    if (p && s_record_budget_active) {
        s_record_budget_bytes += n;
    }
    return p;
}

char *tp_diff__dup(const char *s, bool *ok) {
    if (!s) {
        if (ok) {
            *ok = true;
        }
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *p = (char *)tp_diff__alloc(n);
    if (!p) {
        if (ok) {
            *ok = false;
        }
        return NULL;
    }
    memcpy(p, s, n);
    if (ok) {
        *ok = true;
    }
    return p;
}

/* ---- free helpers -------------------------------------------------------- */

void tp_diff__free_frames(tp_project_frame *frames, int count) {
    if (!frames) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(frames[i].name);
        free(frames[i].src_key);
    }
    free(frames);
}

void tp_diff__free_sprite_fields(tp_project_sprite *s) {
    if (!s) {
        return;
    }
    free(s->name);
    free(s->src_key);
    free(s->rename);
    s->name = s->src_key = s->rename = NULL;
}

static void free_source(tp_project_source *s) { free(s->path); }
static void free_target(tp_project_target *t) {
    free(t->exporter_id);
    free(t->out_path);
}
static void free_anim(tp_project_anim *a) {
    free(a->name);
    tp_diff__free_frames(a->frames, a->frame_count);
}
static void free_atlas(tp_project_atlas *a) {
    free(a->name);
    for (int i = 0; i < a->source_count; i++) {
        free_source(&a->sources[i]);
    }
    free(a->sources);
    for (int i = 0; i < a->sprite_count; i++) {
        tp_diff__free_sprite_fields(&a->sprites[i]);
    }
    free(a->sprites);
    for (int i = 0; i < a->animation_count; i++) {
        free_anim(&a->animations[i]);
    }
    free(a->animations);
    for (int i = 0; i < a->target_count; i++) {
        free_target(&a->targets[i]);
    }
    free(a->targets);
}

/* ---- fill helpers (deep-copy src INTO a zeroed dst slot) ------------------ */

tp_status tp_diff__copy_frames(const tp_project_frame *src, int count, tp_project_frame **out) {
    *out = NULL;
    if (count <= 0) {
        return TP_STATUS_OK;
    }
    tp_project_frame *fr = (tp_project_frame *)tp_diff__alloc((size_t)count * sizeof *fr);
    if (!fr) {
        return TP_STATUS_OOM;
    }
    for (int i = 0; i < count; i++) {
        bool ok = true;
        fr[i].source_ref = src[i].source_ref;
        fr[i].name = tp_diff__dup(src[i].name, &ok);
        if (!ok) {
            tp_diff__free_frames(fr, i + 1);
            return TP_STATUS_OOM;
        }
        fr[i].src_key = tp_diff__dup(src[i].src_key, &ok);
        if (!ok) {
            tp_diff__free_frames(fr, i + 1);
            return TP_STATUS_OOM;
        }
    }
    *out = fr;
    return TP_STATUS_OK;
}

tp_status tp_diff__copy_sprite_fields(const tp_project_sprite *src, tp_project_sprite *dst) {
    *dst = *src; /* scalars: source_ref, origins, slice9, ov_* */
    dst->name = dst->src_key = dst->rename = NULL;
    bool ok = true;
    dst->name = tp_diff__dup(src->name, &ok);
    if (!ok) {
        tp_diff__free_sprite_fields(dst);
        return TP_STATUS_OOM;
    }
    dst->src_key = tp_diff__dup(src->src_key, &ok);
    if (!ok) {
        tp_diff__free_sprite_fields(dst);
        return TP_STATUS_OOM;
    }
    dst->rename = tp_diff__dup(src->rename, &ok);
    if (!ok) {
        tp_diff__free_sprite_fields(dst);
        return TP_STATUS_OOM;
    }
    return TP_STATUS_OK;
}

static tp_status fill_source(tp_project_source *dst, const tp_project_source *src) {
    dst->id = src->id;
    dst->id_synthetic = src->id_synthetic;
    dst->kind = src->kind;
    bool ok = true;
    dst->path = tp_diff__dup(src->path, &ok);
    return ok ? TP_STATUS_OK : TP_STATUS_OOM;
}

static tp_status fill_target(tp_project_target *dst, const tp_project_target *src) {
    dst->id = src->id;
    dst->id_synthetic = src->id_synthetic;
    dst->enabled = src->enabled;
    dst->exporter_id = dst->out_path = NULL;
    bool ok = true;
    dst->exporter_id = tp_diff__dup(src->exporter_id, &ok);
    if (!ok) {
        return TP_STATUS_OOM;
    }
    dst->out_path = tp_diff__dup(src->out_path, &ok);
    return ok ? TP_STATUS_OK : TP_STATUS_OOM;
}

static tp_status fill_frame(tp_project_frame *dst, const tp_project_frame *src) {
    dst->source_ref = src->source_ref;
    dst->name = dst->src_key = NULL;
    bool ok = true;
    dst->name = tp_diff__dup(src->name, &ok);
    if (!ok) {
        return TP_STATUS_OOM;
    }
    dst->src_key = tp_diff__dup(src->src_key, &ok);
    return ok ? TP_STATUS_OK : TP_STATUS_OOM;
}

static tp_status fill_anim(tp_project_anim *dst, const tp_project_anim *src) {
    *dst = *src; /* scalars: id, id_synthetic, fps, playback, flips */
    dst->name = NULL;
    dst->frames = NULL;
    dst->frame_count = dst->frame_cap = 0;
    bool ok = true;
    dst->name = tp_diff__dup(src->name, &ok);
    if (!ok) {
        return TP_STATUS_OOM;
    }
    tp_status st = tp_diff__copy_frames(src->frames, src->frame_count, &dst->frames);
    if (st != TP_STATUS_OK) {
        return st;
    }
    dst->frame_count = src->frame_count;
    dst->frame_cap = src->frame_count;
    return TP_STATUS_OK;
}

/* Deep-copy a full atlas subtree into a zeroed dst slot (name + sources + sparse
 * sprites + animations w/ frames + targets). Grows each sub-count as it fills so a
 * mid-way OOM leaves a destroy-safe partial atlas. */
static tp_status fill_atlas(tp_project_atlas *dst, const tp_project_atlas *src) {
    *dst = *src; /* every scalar knob + id + id_synthetic */
    dst->name = NULL;
    dst->sources = NULL;
    dst->source_count = dst->source_cap = 0;
    dst->sprites = NULL;
    dst->sprite_count = dst->sprite_cap = 0;
    dst->animations = NULL;
    dst->animation_count = dst->animation_cap = 0;
    dst->targets = NULL;
    dst->target_count = dst->target_cap = 0;

    bool ok = true;
    dst->name = tp_diff__dup(src->name, &ok);
    if (!ok) {
        return TP_STATUS_OOM;
    }

    if (src->source_count > 0) {
        dst->sources = (tp_project_source *)tp_diff__alloc((size_t)src->source_count * sizeof(tp_project_source));
        if (!dst->sources) {
            return TP_STATUS_OOM;
        }
        dst->source_cap = src->source_count;
        for (int i = 0; i < src->source_count; i++) {
            if (fill_source(&dst->sources[i], &src->sources[i]) != TP_STATUS_OK) {
                dst->source_count = i + 1; /* include the partial slot for destroy */
                return TP_STATUS_OOM;
            }
            dst->source_count++;
        }
    }
    if (src->sprite_count > 0) {
        dst->sprites = (tp_project_sprite *)tp_diff__alloc((size_t)src->sprite_count * sizeof(tp_project_sprite));
        if (!dst->sprites) {
            return TP_STATUS_OOM;
        }
        dst->sprite_cap = src->sprite_count;
        for (int i = 0; i < src->sprite_count; i++) {
            if (tp_diff__copy_sprite_fields(&src->sprites[i], &dst->sprites[i]) != TP_STATUS_OK) {
                dst->sprite_count = i + 1;
                return TP_STATUS_OOM;
            }
            dst->sprite_count++;
        }
    }
    if (src->animation_count > 0) {
        dst->animations = (tp_project_anim *)tp_diff__alloc((size_t)src->animation_count * sizeof(tp_project_anim));
        if (!dst->animations) {
            return TP_STATUS_OOM;
        }
        dst->animation_cap = src->animation_count;
        for (int i = 0; i < src->animation_count; i++) {
            if (fill_anim(&dst->animations[i], &src->animations[i]) != TP_STATUS_OK) {
                dst->animation_count = i + 1;
                return TP_STATUS_OOM;
            }
            dst->animation_count++;
        }
    }
    if (src->target_count > 0) {
        dst->targets = (tp_project_target *)tp_diff__alloc((size_t)src->target_count * sizeof(tp_project_target));
        if (!dst->targets) {
            return TP_STATUS_OOM;
        }
        dst->target_cap = src->target_count;
        for (int i = 0; i < src->target_count; i++) {
            if (fill_target(&dst->targets[i], &src->targets[i]) != TP_STATUS_OK) {
                dst->target_count = i + 1;
                return TP_STATUS_OOM;
            }
            dst->target_count++;
        }
    }
    return TP_STATUS_OK;
}

/* ---- standalone captured-element copy / free (COLL shape) ----------------- */

tp_status tp_diff__copy_elem(tp_diff_coll coll, const void *src, void **out) {
    *out = NULL;
    switch (coll) {
        case TP_DIFF_COLL_SOURCE: {
            tp_project_source *d = (tp_project_source *)tp_diff__alloc(sizeof *d);
            if (!d) {
                return TP_STATUS_OOM;
            }
            if (fill_source(d, (const tp_project_source *)src) != TP_STATUS_OK) {
                free_source(d);
                free(d);
                return TP_STATUS_OOM;
            }
            *out = d;
            return TP_STATUS_OK;
        }
        case TP_DIFF_COLL_TARGET: {
            tp_project_target *d = (tp_project_target *)tp_diff__alloc(sizeof *d);
            if (!d) {
                return TP_STATUS_OOM;
            }
            if (fill_target(d, (const tp_project_target *)src) != TP_STATUS_OK) {
                free_target(d);
                free(d);
                return TP_STATUS_OOM;
            }
            *out = d;
            return TP_STATUS_OK;
        }
        case TP_DIFF_COLL_FRAME: {
            tp_project_frame *d = (tp_project_frame *)tp_diff__alloc(sizeof *d);
            if (!d) {
                return TP_STATUS_OOM;
            }
            if (fill_frame(d, (const tp_project_frame *)src) != TP_STATUS_OK) {
                free(d->name);
                free(d->src_key);
                free(d);
                return TP_STATUS_OOM;
            }
            *out = d;
            return TP_STATUS_OK;
        }
        case TP_DIFF_COLL_ANIM: {
            tp_project_anim *d = (tp_project_anim *)tp_diff__alloc(sizeof *d);
            if (!d) {
                return TP_STATUS_OOM;
            }
            if (fill_anim(d, (const tp_project_anim *)src) != TP_STATUS_OK) {
                free_anim(d);
                free(d);
                return TP_STATUS_OOM;
            }
            *out = d;
            return TP_STATUS_OK;
        }
        case TP_DIFF_COLL_ATLAS: {
            tp_project_atlas *d = (tp_project_atlas *)tp_diff__alloc(sizeof *d);
            if (!d) {
                return TP_STATUS_OOM;
            }
            if (fill_atlas(d, (const tp_project_atlas *)src) != TP_STATUS_OK) {
                free_atlas(d);
                free(d);
                return TP_STATUS_OOM;
            }
            *out = d;
            return TP_STATUS_OK;
        }
    }
    return TP_STATUS_INVALID_ARGUMENT;
}

void tp_diff__free_elem(tp_diff_coll coll, void *elem) {
    if (!elem) {
        return;
    }
    switch (coll) {
        case TP_DIFF_COLL_SOURCE: free_source((tp_project_source *)elem); break;
        case TP_DIFF_COLL_TARGET: free_target((tp_project_target *)elem); break;
        case TP_DIFF_COLL_FRAME: {
            tp_project_frame *f = (tp_project_frame *)elem;
            free(f->name);
            free(f->src_key);
            break;
        }
        case TP_DIFF_COLL_ANIM: free_anim((tp_project_anim *)elem); break;
        case TP_DIFF_COLL_ATLAS: free_atlas((tp_project_atlas *)elem); break;
    }
    free(elem);
}

/* ---- positional collection primitives ------------------------------------ */

/* Open a zeroed hole at `index` in a dynamic array (grows through the diff seam).
 * Returns the slot, or NULL on OOM (array + count unchanged). index in [0,*count]. */
static void *arr_open(void **arr, int *count, int *cap, size_t esz, int index) {
    if (index < 0 || index > *count) {
        return NULL;
    }
    if (*count == *cap) {
        int ncap = (*cap == 0) ? 4 : (*cap * 2);
        void *n = tp_diff__alloc((size_t)ncap * esz);
        if (!n) {
            return NULL;
        }
        if (*arr && *count > 0) {
            memcpy(n, *arr, (size_t)(*count) * esz);
        }
        free(*arr);
        *arr = n;
        *cap = ncap;
    }
    char *base = (char *)*arr;
    memmove(base + (size_t)(index + 1) * esz, base + (size_t)index * esz, (size_t)(*count - index) * esz);
    void *slot = base + (size_t)index * esz;
    memset(slot, 0, esz);
    (*count)++;
    return slot;
}

/* Close the hole at `index` (caller already freed the element's owned pointers).
 * index in [0,*count). */
static void arr_remove(void *arr, int *count, size_t esz, int index) {
    char *base = (char *)arr;
    memmove(base + (size_t)index * esz, base + (size_t)(index + 1) * esz, (size_t)(*count - index - 1) * esz);
    (*count)--;
    memset(base + (size_t)(*count) * esz, 0, esz); /* clear the freed tail alias */
}

tp_status tp_diff__insert_atlas(tp_project *p, int index, const tp_project_atlas *src) {
    if (index < 0 || index > p->atlas_count) {
        return TP_STATUS_OUT_OF_BOUNDS; /* corrupted position -> distinct from OOM */
    }
    void *slot = arr_open((void **)&p->atlases, &p->atlas_count, &p->atlas_cap, sizeof(tp_project_atlas), index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return fill_atlas((tp_project_atlas *)slot, src);
}
/* The positional REMOVE direction delegates to the canonical public remover:
 * the bounds-check + element free-discipline + down-shift lives in ONE place, killing
 * the drift risk of a second free-list). Only re-zero the vacated tail slot afterward so
 * the diff's array invariant holds ([count,cap) carry only NULL owned pointers; the
 * public remover leaves a stale alias there). The positional INSERT direction is the
 * genuinely new capability the diff owns, so it stays local. */
tp_status tp_diff__remove_atlas(tp_project *p, int index) {
    tp_status st = tp_project_remove_atlas(p, index);
    if (st == TP_STATUS_OK) {
        memset(&p->atlases[p->atlas_count], 0, sizeof *p->atlases);
    }
    return st;
}

tp_status tp_diff__insert_source(tp_project_atlas *a, int index, const tp_project_source *src) {
    if (index < 0 || index > a->source_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    void *slot = arr_open((void **)&a->sources, &a->source_count, &a->source_cap, sizeof(tp_project_source), index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return fill_source((tp_project_source *)slot, src);
}
tp_status tp_diff__remove_source(tp_project_atlas *a, int index) {
    tp_status st = tp_project_atlas_remove_source(a, index); /* delegate free+shift */
    if (st == TP_STATUS_OK) {
        memset(&a->sources[a->source_count], 0, sizeof *a->sources);
    }
    return st;
}

tp_status tp_diff__insert_anim(tp_project_atlas *a, int index, const tp_project_anim *src) {
    if (index < 0 || index > a->animation_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    void *slot = arr_open((void **)&a->animations, &a->animation_count, &a->animation_cap, sizeof(tp_project_anim),
                          index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return fill_anim((tp_project_anim *)slot, src);
}
tp_status tp_diff__remove_anim(tp_project_atlas *a, int index) {
    if (index < 0 || index >= a->animation_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    free_anim(&a->animations[index]);
    arr_remove(a->animations, &a->animation_count, sizeof(tp_project_anim), index);
    return TP_STATUS_OK;
}

tp_status tp_diff__insert_target(tp_project_atlas *a, int index, const tp_project_target *src) {
    if (index < 0 || index > a->target_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    void *slot = arr_open((void **)&a->targets, &a->target_count, &a->target_cap, sizeof(tp_project_target), index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return fill_target((tp_project_target *)slot, src);
}
tp_status tp_diff__remove_target(tp_project_atlas *a, int index) {
    tp_status st = tp_project_atlas_remove_target(a, index); /* delegate free+shift */
    if (st == TP_STATUS_OK) {
        memset(&a->targets[a->target_count], 0, sizeof *a->targets);
    }
    return st;
}

tp_status tp_diff__insert_frame(tp_project_anim *an, int index, const tp_project_frame *src) {
    if (index < 0 || index > an->frame_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    void *slot = arr_open((void **)&an->frames, &an->frame_count, &an->frame_cap, sizeof(tp_project_frame), index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return fill_frame((tp_project_frame *)slot, src);
}
tp_status tp_diff__remove_frame_at(tp_project_anim *an, int index) {
    tp_status st = tp_project_anim_remove_frame(an, index); /* delegate free+shift */
    if (st == TP_STATUS_OK) {
        memset(&an->frames[an->frame_count], 0, sizeof *an->frames);
    }
    return st;
}

tp_status tp_diff__insert_sprite(tp_project_atlas *a, int index, const tp_project_sprite *src) {
    if (index < 0 || index > a->sprite_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    void *slot = arr_open((void **)&a->sprites, &a->sprite_count, &a->sprite_cap, sizeof(tp_project_sprite), index);
    if (!slot) {
        return TP_STATUS_OOM;
    }
    return tp_diff__copy_sprite_fields(src, (tp_project_sprite *)slot);
}
tp_status tp_diff__remove_sprite_at(tp_project_atlas *a, int index) {
    if (index < 0 || index >= a->sprite_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_diff__free_sprite_fields(&a->sprites[index]);
    arr_remove(a->sprites, &a->sprite_count, sizeof(tp_project_sprite), index);
    return TP_STATUS_OK;
}
tp_status tp_diff__replace_sprite_at(tp_project_atlas *a, int index, const tp_project_sprite *src) {
    if (index < 0 || index >= a->sprite_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_diff__free_sprite_fields(&a->sprites[index]); /* *dst=*src in copy would leak the old strings */
    return tp_diff__copy_sprite_fields(src, &a->sprites[index]);
}
