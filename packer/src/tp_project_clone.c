/*
 * F2-02 task 2: an OOM-safe deep-clone of the tp_project model -- the atomicity
 * primitive the transaction engine copies the live model with before applying a
 * batch. On FULL success the clone is swapped in and the old model freed; on ANY
 * op/allocator failure the clone is discarded and the live model is byte-unchanged
 * (§7, §416). Provably atomic because the commit point is an allocation-free
 * pointer swap.
 *
 * Correctness rule for OOM safety: every owned pointer in the growing clone is
 * ALWAYS either a valid malloc or NULL, and collection counts only ever cover
 * slots whose scalar fields are copied and whose owned pointers are set (or NULL).
 * So a single tp_project_destroy on the partial clone frees exactly what was built
 * -- no leak, no double free, no use of an uninitialized pointer. free(NULL) is a
 * no-op, so a half-filled element with NULL tails is safe to destroy.
 *
 * A cloned array is allocated at its EXACT element count (calloc, zeroed) and the
 * per-collection count is grown one element at a time as each element's own
 * allocations succeed. tp_free_atlas / tp_free_anim (via tp_project_destroy) then
 * iterate only the filled prefix; the zeroed tail holds only NULL pointers.
 *
 * !! FORK-SYNC WARNING (review [5]) !! packer/src/tp_diff_entity.c keeps a DELIBERATE
 * second copy of this deep-copy (fill_atlas/anim/source/target/frame/copy_sprite_fields)
 * on its own fault seam so the diff never perturbs this file's clone-alloc-count goldens
 * (decision 0012 §6). A persistent field added HERE MUST be mirrored THERE, or Undo/Redo
 * silently restores a non-byte-identical project; the test_completeness_oracle_* net in
 * test_diff.c catches a missed field. Keep the two copies in sync.
 */

#include "tp_core/tp_project.h"

#include <stdlib.h>
#include <string.h>

#include "tp_txn_internal.h"

/* ---- allocation fault seam (test-only; default disabled) ------------------ */
/* The seam is per-thread so independent sessions may clone concurrently without
 * racing on process-global test instrumentation. Tests and benchmarks arm/read
 * it on the same thread that performs the clone. */
static _Thread_local int s_clone_fail = -1; /* countdown; -1 disabled. Fires once. */
static _Thread_local int s_clone_allocs = 0;
static _Thread_local size_t s_clone_allocation_bytes = 0U;

void tp_project__test_set_clone_alloc_fail(int nth) {
    s_clone_fail = nth;
    s_clone_allocs = 0;
    s_clone_allocation_bytes = 0U;
}
int tp_project__test_clone_alloc_count(void) { return s_clone_allocs; }
size_t tp_project__test_clone_allocation_bytes(void) {
    return s_clone_allocation_bytes;
}

typedef struct clone_ctx {
    int fail;
    int allocs;
    size_t allocation_bytes;
} clone_ctx;

static void *cl_alloc(clone_ctx *ctx, size_t n) {
    ctx->allocs++;
    if (ctx->fail == 0) {
        ctx->fail = -1;
        return NULL;
    }
    if (ctx->fail > 0) {
        ctx->fail--;
    }
    void *allocation = calloc(1, n);
    if (allocation) {
        ctx->allocation_bytes += n;
    }
    return allocation;
}

/* Duplicate an optionally-NULL owned string. Returns true on success (including
 * the intended-NULL case: src NULL -> *dst NULL, true); false ONLY on a real
 * allocation failure (src non-NULL but the dup could not be allocated). */
static bool cl_dup(clone_ctx *ctx, const char *src, char **dst) {
    if (!src) {
        *dst = NULL;
        return true;
    }
    size_t n = strlen(src) + 1U;
    char *p = (char *)cl_alloc(ctx, n);
    if (!p) {
        *dst = NULL;
        return false;
    }
    memcpy(p, src, n);
    *dst = p;
    return true;
}

/* Allocate a zeroed element array of `count` elements of `elem` bytes, or NULL
 * for count==0 (a valid empty collection). Returns false ONLY on allocation
 * failure for a non-empty array. */
static bool cl_array(clone_ctx *ctx, int count, size_t elem, void **out) {
    if (count <= 0) {
        *out = NULL;
        return true;
    }
    void *p = cl_alloc(ctx, (size_t)count * elem);
    *out = p;
    return p != NULL;
}

static bool clone_frames(clone_ctx *ctx, const tp_project_anim *src,
                         tp_project_anim *dst) {
    if (!cl_array(ctx, src->frame_count, sizeof(tp_project_frame),
                  (void **)&dst->frames)) {
        return false;
    }
    dst->frame_cap = src->frame_count; /* exact-fit; frame_count grows as we fill */
    for (int i = 0; i < src->frame_count; i++) {
        tp_project_frame *df = &dst->frames[i];
        df->source_ref = src->frames[i].source_ref; /* scalar: id value */
        if (!cl_dup(ctx, src->frames[i].name, &df->name)) {
            return false;
        }
        if (!cl_dup(ctx, src->frames[i].src_key, &df->src_key)) {
            return false;
        }
        dst->frame_count++; /* frame i fully built */
    }
    return true;
}

static bool clone_atlas(clone_ctx *ctx, const tp_project_atlas *src,
                        tp_project_atlas *dst) {
    *dst = *src;                    /* copy every scalar (knobs, id, id_synthetic) */
    dst->name = NULL;               /* re-own the pointer fields; drop aliases */
    dst->sources = NULL;
    dst->source_count = dst->source_cap = 0;
    dst->sprites = NULL;
    dst->sprite_count = dst->sprite_cap = 0;
    dst->animations = NULL;
    dst->animation_count = dst->animation_cap = 0;
    dst->targets = NULL;
    dst->target_count = dst->target_cap = 0;

    if (!cl_dup(ctx, src->name, &dst->name)) {
        return false;
    }

    if (!cl_array(ctx, src->source_count, sizeof(tp_project_source),
                  (void **)&dst->sources)) {
        return false;
    }
    dst->source_cap = src->source_count;
    for (int i = 0; i < src->source_count; i++) {
        dst->sources[i].id = src->sources[i].id;
        dst->sources[i].id_synthetic = src->sources[i].id_synthetic;
        dst->sources[i].kind = src->sources[i].kind;
        if (!cl_dup(ctx, src->sources[i].path, &dst->sources[i].path)) {
            return false;
        }
        dst->source_count++;
    }

    if (!cl_array(ctx, src->sprite_count, sizeof(tp_project_sprite),
                  (void **)&dst->sprites)) {
        return false;
    }
    dst->sprite_cap = src->sprite_count;
    for (int i = 0; i < src->sprite_count; i++) {
        tp_project_sprite *ds = &dst->sprites[i];
        *ds = src->sprites[i]; /* scalars: source_ref, origins, slice9, ov_* */
        ds->name = ds->src_key = ds->rename = NULL;
        if (!cl_dup(ctx, src->sprites[i].name, &ds->name)) {
            return false;
        }
        if (!cl_dup(ctx, src->sprites[i].src_key, &ds->src_key)) {
            return false;
        }
        if (!cl_dup(ctx, src->sprites[i].rename, &ds->rename)) {
            return false;
        }
        dst->sprite_count++;
    }

    if (!cl_array(ctx, src->animation_count, sizeof(tp_project_anim),
                  (void **)&dst->animations)) {
        return false;
    }
    dst->animation_cap = src->animation_count;
    for (int i = 0; i < src->animation_count; i++) {
        tp_project_anim *da = &dst->animations[i];
        *da = src->animations[i]; /* scalars: id, id_synthetic, fps, playback, flips */
        da->name = NULL;
        da->frames = NULL;
        da->frame_count = da->frame_cap = 0;
        dst->animation_count++; /* link the anim BEFORE filling frames so a frame
                                 * failure is freed by tp_free_anim on the prefix */
        if (!cl_dup(ctx, src->animations[i].name, &da->name)) {
            return false;
        }
        if (!clone_frames(ctx, &src->animations[i], da)) {
            return false;
        }
    }

    if (!cl_array(ctx, src->target_count, sizeof(tp_project_target),
                  (void **)&dst->targets)) {
        return false;
    }
    dst->target_cap = src->target_count;
    for (int i = 0; i < src->target_count; i++) {
        tp_project_target *dt = &dst->targets[i];
        dt->id = src->targets[i].id;
        dt->id_synthetic = src->targets[i].id_synthetic;
        dt->enabled = src->targets[i].enabled;
        dt->exporter_id = dt->out_path = NULL;
        if (!cl_dup(ctx, src->targets[i].exporter_id, &dt->exporter_id)) {
            return false;
        }
        if (!cl_dup(ctx, src->targets[i].out_path, &dt->out_path)) {
            return false;
        }
        dst->target_count++;
    }
    return true;
}

tp_project *tp_project_clone(const tp_project *src) {
    if (!src) {
        return NULL;
    }
    clone_ctx ctx = {s_clone_fail, 0, 0U};
    tp_project *dst = (tp_project *)cl_alloc(&ctx, sizeof(tp_project));
    if (!dst) {
        s_clone_fail = ctx.fail;
        s_clone_allocs = ctx.allocs;
        s_clone_allocation_bytes = ctx.allocation_bytes;
        return NULL;
    }
    dst->schema_version = src->schema_version;
    dst->project_dir = NULL;
    dst->source_base_dir = NULL;
    dst->atlases = NULL;
    dst->atlas_count = dst->atlas_cap = 0;

    if (!cl_dup(&ctx, src->project_dir, &dst->project_dir)) {
        tp_project_destroy(dst);
        dst = NULL;
        goto done;
    }
    if (!cl_dup(&ctx, src->source_base_dir, &dst->source_base_dir)) {
        tp_project_destroy(dst);
        dst = NULL;
        goto done;
    }
    if (!cl_array(&ctx, src->atlas_count, sizeof(tp_project_atlas),
                  (void **)&dst->atlases)) {
        tp_project_destroy(dst);
        dst = NULL;
        goto done;
    }
    dst->atlas_cap = src->atlas_count;
    for (int i = 0; i < src->atlas_count; i++) {
        /* Zero the slot and link it (count++) BEFORE deep-copy so a mid-atlas
         * failure leaves a fully-freeable atlas in the destroyed prefix. */
        memset(&dst->atlases[i], 0, sizeof(tp_project_atlas));
        dst->atlas_count++;
        if (!clone_atlas(&ctx, &src->atlases[i], &dst->atlases[i])) {
            tp_project_destroy(dst);
            dst = NULL;
            goto done;
        }
    }
done:
    s_clone_fail = ctx.fail;
    s_clone_allocs = ctx.allocs;
    s_clone_allocation_bytes = ctx.allocation_bytes;
    return dst;
}
