#ifndef TP_CORE_TP_ARENA_H
#define TP_CORE_TP_ARENA_H

/* Minimal bump allocator that owns every tp_result allocation (SUMMARY.md
 * §5g, plan §1.3). The engine exposes no public arena, so tp_core ships its
 * own: destroying/resetting the arena frees/reuses everything allocated from
 * it in one shot -- this is the "owned tp_result" ownership model. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_arena tp_arena;

/* initial_block_size == 0 picks a small built-in default. Returns NULL on OOM. */
tp_arena *tp_arena_create(size_t initial_block_size);

/* 8-byte aligned. Grows by adding a new block when the current one is full.
 * Returns NULL on OOM; never asserts on caller-supplied size. */
void *tp_arena_alloc(tp_arena *arena, size_t size);

/* Copies `s` (incl. NUL terminator) into arena memory. NULL on OOM or NULL args. */
char *tp_arena_strdup(tp_arena *arena, const char *s);

/* Rewinds every block to empty without freeing them, so the next round of
 * allocations reuses the same memory. */
void tp_arena_reset(tp_arena *arena);

/* Frees every block and the arena itself. */
void tp_arena_destroy(tp_arena *arena);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_ARENA_H */
