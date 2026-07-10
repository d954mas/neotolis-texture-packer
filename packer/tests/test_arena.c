/* tp_arena: bump allocator smoke test -- alloc sizes incl. block growth
 * beyond the initial block, 8-byte alignment, strdup, reset+reuse, destroy. */

#include "tp_core/tp_arena.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

void test_arena_alloc_basic(void) {
    tp_arena *arena = tp_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);

    void *a = tp_arena_alloc(arena, 16);
    void *b = tp_arena_alloc(arena, 8);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(a != b);

    tp_arena_destroy(arena);
}

void test_arena_alignment(void) {
    tp_arena *arena = tp_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);

    /* Odd sizes must not break 8-byte alignment of subsequent allocations. */
    for (int i = 0; i < 8; i++) {
        void *p = tp_arena_alloc(arena, (size_t)(i + 1));
        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)((uintptr_t)p % 8U));
    }

    tp_arena_destroy(arena);
}

void test_arena_growth_beyond_initial_block(void) {
    tp_arena *arena = tp_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);

    /* Bigger than the initial block: must trigger a new block, not fail. */
    void *big = tp_arena_alloc(arena, 4096);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 0xAB, 4096);

    /* Still usable afterwards. */
    void *small = tp_arena_alloc(arena, 8);
    TEST_ASSERT_NOT_NULL(small);
    TEST_ASSERT_TRUE(small != big);

    tp_arena_destroy(arena);
}

void test_arena_strdup(void) {
    tp_arena *arena = tp_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);

    char *copy = tp_arena_strdup(arena, "hello arena");
    TEST_ASSERT_NOT_NULL(copy);
    TEST_ASSERT_EQUAL_STRING("hello arena", copy);

    tp_arena_destroy(arena);
}

void test_arena_reset_then_realloc(void) {
    tp_arena *arena = tp_arena_create(64);
    TEST_ASSERT_NOT_NULL(arena);

    void *first = tp_arena_alloc(arena, 32);
    TEST_ASSERT_NOT_NULL(first);

    tp_arena_reset(arena);

    void *second = tp_arena_alloc(arena, 32);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(first == second); /* reset rewinds the bump pointer */

    tp_arena_destroy(arena);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arena_alloc_basic);
    RUN_TEST(test_arena_alignment);
    RUN_TEST(test_arena_growth_beyond_initial_block);
    RUN_TEST(test_arena_strdup);
    RUN_TEST(test_arena_reset_then_realloc);
    return UNITY_END();
}
