/* tp_name_map: hash(name)->name reverse map (plan §2.8). Also doubles as the
 * R5 check (plan §6): nt_hash64_str is expected to work with no nt_hash_init
 * call -- if this ever crashes/asserts, that assumption is wrong and setUp
 * must call nt_hash_init(NULL) first. */

#include "tp_core/tp_error.h"
#include "tp_core/tp_name_map.h"
#include "unity.h"

#include "hash/nt_hash.h"

#include <stddef.h>
#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

void test_name_map_insert_and_lookup(void) {
    tp_name_map *map = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(map);

    static const char *names[] = {"disc_red", "disc_green", "disc_blue", "walk_01", "walk_02"};
    const size_t count = sizeof(names) / sizeof(names[0]);

    for (size_t i = 0; i < count; i++) {
        tp_status status = tp_name_map_insert(map, names[i]);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, status);
    }

    for (size_t i = 0; i < count; i++) {
        uint64_t hash = nt_hash64_str(names[i]).value;
        const char *resolved = tp_name_map_lookup(map, hash);
        TEST_ASSERT_NOT_NULL(resolved);
        TEST_ASSERT_EQUAL_STRING(names[i], resolved);
    }

    tp_name_map_destroy(map);
}

void test_name_map_reinsert_same_name_is_ok(void) {
    tp_name_map *map = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(map);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert(map, "dup"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert(map, "dup"));

    tp_name_map_destroy(map);
}

void test_name_map_lookup_miss_returns_null(void) {
    tp_name_map *map = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(map);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert(map, "known"));
    TEST_ASSERT_NULL(tp_name_map_lookup(map, 0xDEADBEEFULL));

    tp_name_map_destroy(map);
}

void test_name_map_forced_collision(void) {
    tp_name_map *map = tp_name_map_create();
    TEST_ASSERT_NOT_NULL(map);

    /* Same precomputed hash, two distinct names -- simulates a real xxh64
     * collision without needing to find one. */
    const uint64_t shared_hash = 0x1234567890ABCDEFULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_name_map_insert_hashed(map, shared_hash, "name_a"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_HASH_COLLISION, tp_name_map_insert_hashed(map, shared_hash, "name_b"));

    tp_name_map_destroy(map);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_name_map_insert_and_lookup);
    RUN_TEST(test_name_map_reinsert_same_name_is_ok);
    RUN_TEST(test_name_map_lookup_miss_returns_null);
    RUN_TEST(test_name_map_forced_collision);
    return UNITY_END();
}
