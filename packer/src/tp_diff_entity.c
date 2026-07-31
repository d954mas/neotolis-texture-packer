/*
 * Element-level deep-copy / free + positional collection primitives for the
 * semantic diff. The diff STATE-CAPTURES touched entities
 * (docs/architecture/model-operations-and-session.md), so
 * it needs to copy one source/sprite/animation/target/frame/atlas-subtree out of the
 * live model and, on inverse/redo, splice a deep copy back in at an exact index.
 *
 * Persistent field ownership is defined once in tp_project_owned.c. This file
 * supplies the independent diff allocator/fault domain and positional splice
 * mechanics; clone supplies a different allocator to the same owned-copy code.
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
#include "tp_project_owned_internal.h"
#include "tp_project_mutation_internal.h"

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

static void *diff_owned_allocate(void *context, size_t size) {
    (void)context;
    return tp_diff__alloc(size);
}

static tp_project_owned_allocator diff_owned_allocator(void) {
    return (tp_project_owned_allocator){diff_owned_allocate, NULL};
}

void tp_diff__free_frames(tp_project_frame *frames, int count) {
    tp_project_owned_free_frames(frames, count);
}

void tp_diff__free_sprite_fields(tp_project_sprite *sprite) {
    tp_project_owned_free_sprite(sprite);
}

tp_status tp_diff__copy_frames(const tp_project_frame *src, int count,
                               tp_project_frame **out) {
    return tp_project_owned_copy_frames(
        out, src, count, diff_owned_allocator());
}

tp_status tp_diff__copy_sprite_fields(const tp_project_sprite *src,
                                      tp_project_sprite *dst) {
    return tp_project_owned_copy_sprite(
        dst, src, diff_owned_allocator());
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
            if (tp_project_owned_copy_source(
                    d, (const tp_project_source *)src,
                    diff_owned_allocator()) != TP_STATUS_OK) {
                tp_project_owned_free_source(d);
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
            if (tp_project_owned_copy_target(
                    d, (const tp_project_target *)src,
                    diff_owned_allocator()) != TP_STATUS_OK) {
                tp_project_owned_free_target(d);
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
            if (tp_project_owned_copy_frame(
                    d, (const tp_project_frame *)src,
                    diff_owned_allocator()) != TP_STATUS_OK) {
                tp_project_owned_free_frame(d);
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
            if (tp_project_owned_copy_anim(
                    d, (const tp_project_anim *)src,
                    diff_owned_allocator()) != TP_STATUS_OK) {
                tp_project_owned_free_anim(d);
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
            if (tp_project_owned_copy_atlas(
                    d, (const tp_project_atlas *)src,
                    diff_owned_allocator()) != TP_STATUS_OK) {
                tp_project_owned_free_atlas(d);
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
        case TP_DIFF_COLL_SOURCE:
            tp_project_owned_free_source((tp_project_source *)elem);
            break;
        case TP_DIFF_COLL_TARGET:
            tp_project_owned_free_target((tp_project_target *)elem);
            break;
        case TP_DIFF_COLL_FRAME: {
            tp_project_frame *f = (tp_project_frame *)elem;
            tp_project_owned_free_frame(f);
            break;
        }
        case TP_DIFF_COLL_ANIM:
            tp_project_owned_free_anim((tp_project_anim *)elem);
            break;
        case TP_DIFF_COLL_ATLAS:
            tp_project_owned_free_atlas((tp_project_atlas *)elem);
            break;
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
    return tp_project_owned_copy_atlas(
        (tp_project_atlas *)slot, src, diff_owned_allocator());
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
    return tp_project_owned_copy_source(
        (tp_project_source *)slot, src, diff_owned_allocator());
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
    return tp_project_owned_copy_anim(
        (tp_project_anim *)slot, src, diff_owned_allocator());
}
tp_status tp_diff__remove_anim(tp_project_atlas *a, int index) {
    if (index < 0 || index >= a->animation_count) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    tp_project_owned_free_anim(&a->animations[index]);
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
    return tp_project_owned_copy_target(
        (tp_project_target *)slot, src, diff_owned_allocator());
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
    return tp_project_owned_copy_frame(
        (tp_project_frame *)slot, src, diff_owned_allocator());
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
