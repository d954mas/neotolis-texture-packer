#ifndef TP_CORE_SRC_TP_PROJECT_CLONE_ARENA_H
#define TP_CORE_SRC_TP_PROJECT_CLONE_ARENA_H

/*
 * P-01 (plan §17): an ARENA-backed deep-clone of the tp_project model, the
 * measured alternative to the malloc/strdup tp_project_clone (tp_project_clone.c).
 *
 * The malloc clone pays ~250k small malloc/free calls for a huge project (the
 * dominant cost, ~5.86 ms measured). This clone allocates every owned field from a
 * single tp_arena, so the whole copy is a walk of bump allocations over one (or a
 * few) contiguous block(s): memcpy-bound, and freed in one tp_arena_destroy.
 *
 * BYTE-IDENTITY (sacred): the produced project serializes (tp_project_save_buffer)
 * byte-for-byte identical to the source -- pinned by a maximal-project test in
 * test_transaction.c. Every field the malloc clone copies is copied here in the
 * same way, so this is a faithful drop-in structurally.
 *
 * NO-LEAK is STRUCTURAL: every allocation lives in `arena`. The caller ALWAYS
 * owns `arena` and destroys it (whether the clone succeeded or hit OOM mid-walk),
 * so a partial clone leaks nothing -- there is no per-field unwinding to get wrong.
 *
 * !! FORK-SYNC WARNING !! This mirrors tp_project_clone.c (and its diff-fork twin
 * tp_diff_entity.c). A persistent field added to the model MUST be added to the
 * copy here AND to the footprint sizing below, or the byte-identity test fails.
 *
 * NOTE: the returned project is arena-owned -- it MUST NOT be passed to
 * tp_project_destroy (that would free() arena pointers). Reclaim it with
 * tp_arena_destroy(arena). This is exactly why full integration into the live
 * model is deferred (see the P-01 report): the shipping model is edited in place
 * by the GUI/CLI and freed with tp_project_destroy.
 */

#include <stddef.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_project.h"

/* Exact number of arena bytes tp_project_clone_into_arena will consume for `src`
 * (each field's size rounded up to the arena's 8-byte alignment, summed). Sizing an
 * arena to this makes the whole clone land in ONE contiguous block -- best locality
 * and the cleanest memcpy-bound benchmark number. 0 for a NULL src. */
size_t tp_project_clone_arena_footprint(const tp_project *src);

/* Deep-clones `src` into `arena` (every owned field bump-allocated from it,
 * including the root tp_project). Returns the arena-allocated clone, or NULL on a
 * NULL src or a true arena OOM. On OOM nothing is freed here -- the caller destroys
 * `arena` wholesale. The clone is byte-identical to `src` under
 * tp_project_save_buffer. */
tp_project *tp_project_clone_into_arena(const tp_project *src, tp_arena *arena);

#endif /* TP_CORE_SRC_TP_PROJECT_CLONE_ARENA_H */
