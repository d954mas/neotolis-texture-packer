#include "tp_core/tp_arena.h"

#include <stdlib.h>
#include <string.h>

#define TP_ARENA_ALIGN 8U
#define TP_ARENA_DEFAULT_BLOCK_SIZE 4096U

typedef struct tp_arena_block {
    struct tp_arena_block *next;
    unsigned char *data;
    size_t capacity;
    size_t used;
} tp_arena_block;

struct tp_arena {
    tp_arena_block *head;
    tp_arena_block *current;
    size_t initial_block_size;
};

static size_t tp_align_up(size_t value, size_t align) {
    const size_t rem = value % align;
    return (rem == 0U) ? value : value + (align - rem);
}

static tp_arena_block *tp_arena_block_create(size_t capacity) {
    tp_arena_block *block = (tp_arena_block *)malloc(sizeof(tp_arena_block));
    if (!block) {
        return NULL;
    }
    block->data = (unsigned char *)malloc(capacity);
    if (!block->data) {
        free(block);
        return NULL;
    }
    block->next = NULL;
    block->capacity = capacity;
    block->used = 0U;
    return block;
}

tp_arena *tp_arena_create(size_t initial_block_size) {
    if (initial_block_size == 0U) {
        initial_block_size = TP_ARENA_DEFAULT_BLOCK_SIZE;
    }

    tp_arena *arena = (tp_arena *)malloc(sizeof(tp_arena));
    if (!arena) {
        return NULL;
    }

    tp_arena_block *block = tp_arena_block_create(initial_block_size);
    if (!block) {
        free(arena);
        return NULL;
    }

    arena->head = block;
    arena->current = block;
    arena->initial_block_size = initial_block_size;
    return arena;
}

void *tp_arena_alloc(tp_arena *arena, size_t size) {
    if (!arena) {
        return NULL;
    }

    tp_arena_block *block = arena->current;
    size_t used = tp_align_up(block->used, TP_ARENA_ALIGN);
    const size_t aligned_size = tp_align_up(size, TP_ARENA_ALIGN);

    if (used > block->capacity || aligned_size > block->capacity - used) {
        size_t new_capacity = arena->initial_block_size;
        if (aligned_size > new_capacity) {
            new_capacity = aligned_size;
        }
        tp_arena_block *new_block = tp_arena_block_create(new_capacity);
        if (!new_block) {
            return NULL;
        }
        block->next = new_block;
        arena->current = new_block;
        block = new_block;
        used = 0U;
    }

    unsigned char *ptr = block->data + used;
    block->used = used + aligned_size;
    return (void *)ptr;
}

char *tp_arena_strdup(tp_arena *arena, const char *s) {
    if (!arena || !s) {
        return NULL;
    }
    const size_t len = strlen(s) + 1U;
    char *copy = (char *)tp_arena_alloc(arena, len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len);
    return copy;
}

void tp_arena_reset(tp_arena *arena) {
    if (!arena) {
        return;
    }
    for (tp_arena_block *block = arena->head; block; block = block->next) {
        block->used = 0U;
    }
    arena->current = arena->head;
}

void tp_arena_destroy(tp_arena *arena) {
    if (!arena) {
        return;
    }
    tp_arena_block *block = arena->head;
    while (block) {
        tp_arena_block *next = block->next;
        free(block->data);
        free(block);
        block = next;
    }
    free(arena);
}
