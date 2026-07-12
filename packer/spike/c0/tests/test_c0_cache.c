/* C0-03 task 4: the memory Pack-result cache interface + reference impl. Pins
 * get/put/evict by pack_input_hash, the configurable byte-budget LRU, pinning of
 * the active result, and selection-by-hash independence from insertion order.
 * The executable form of C0-03-contract.md §4. No production budget is baked. */

#include "tp_c0/tp_c0_cache.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static tp_c0_id128 keyi(int i) {
    tp_c0_id128 id = tp_c0_id128_nil();
    id.bytes[0] = (uint8_t)(i + 1); /* +1 keeps it non-nil */
    id.bytes[15] = 0xCC;
    return id;
}

static tp_c0_detail put_bytes(tp_c0_cache *c, int i, size_t n) {
    /* A blob of `n` bytes filled with a key-derived pattern. */
    uint8_t buf[256];
    if (n > sizeof buf) {
        n = sizeof buf;
    }
    memset(buf, (int)(i + 1), n);
    return tp_c0_cache_put(c, keyi(i), buf, n, NULL);
}

/* ---- basic put/get/miss -------------------------------------------------- */

void test_put_get_roundtrip(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(1024);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_cache_put(c, keyi(1), "layout-blob", 11, NULL));
    size_t len = 0;
    const void *p = tp_c0_cache_get(c, keyi(1), &len);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(11, len);
    TEST_ASSERT_EQUAL_MEMORY("layout-blob", p, 11);
    /* miss */
    TEST_ASSERT_NULL(tp_c0_cache_get(c, keyi(999), &len));
    TEST_ASSERT_EQUAL_UINT(0, len);
    TEST_ASSERT_EQUAL_INT(1, tp_c0_cache_count(c));
    TEST_ASSERT_EQUAL_UINT(1024, tp_c0_cache_budget(c));
    tp_c0_cache_destroy(c);
}

void test_put_faults(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(64);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_NIL, tp_c0_cache_put(c, tp_c0_id128_nil(), "x", 1, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_EMPTY, tp_c0_cache_put(c, keyi(1), "x", 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_NULL_ARG, tp_c0_cache_put(c, keyi(1), NULL, 4, &err));
    TEST_ASSERT_EQUAL_INT(0, tp_c0_cache_count(c));
    tp_c0_cache_destroy(c);
}

void test_replace_same_key(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(1024);
    tp_c0_cache_put(c, keyi(1), "aaaa", 4, NULL);
    tp_c0_cache_put(c, keyi(1), "bbbbbb", 6, NULL);
    size_t len = 0;
    const void *p = tp_c0_cache_get(c, keyi(1), &len);
    TEST_ASSERT_EQUAL_UINT(6, len);
    TEST_ASSERT_EQUAL_MEMORY("bbbbbb", p, 6);
    TEST_ASSERT_EQUAL_INT(1, tp_c0_cache_count(c)); /* replaced, not duplicated */
    tp_c0_cache_destroy(c);
}

/* ---- byte-budget LRU ----------------------------------------------------- */

void test_lru_eviction_by_budget(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(20);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, put_bytes(c, 1, 8));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, put_bytes(c, 2, 8)); /* 16 <= 20 */
    TEST_ASSERT_EQUAL_INT(2, tp_c0_cache_count(c));
    /* Touch K1 so K2 becomes least-recently-used. get() pins the entry, so unpin
     * right after to keep it a pure recency touch that stays in the budget. */
    TEST_ASSERT_NOT_NULL(tp_c0_cache_get(c, keyi(1), NULL));
    TEST_ASSERT_TRUE(tp_c0_cache_unpin(c, keyi(1)));
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, put_bytes(c, 3, 8)); /* 24 > 20 -> evict LRU (K2) */
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(1)));
    TEST_ASSERT_FALSE(tp_c0_cache_contains(c, keyi(2)));
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(3)));
    TEST_ASSERT_EQUAL_UINT(16, tp_c0_cache_unpinned_bytes(c));
    tp_c0_cache_destroy(c);
}

void test_single_over_budget_item_retained(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(4);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, put_bytes(c, 1, 100)); /* alone, over budget */
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(1)));
    TEST_ASSERT_EQUAL_UINT(100, tp_c0_cache_unpinned_bytes(c)); /* soft cap keeps the sole entry */
    tp_c0_cache_destroy(c);
}

/* ---- pinning the active result ------------------------------------------- */

void test_pinned_active_never_evicted(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(10);
    put_bytes(c, 1, 8);
    TEST_ASSERT_TRUE(tp_c0_cache_pin(c, keyi(1))); /* active result pinned */
    TEST_ASSERT_TRUE(tp_c0_cache_is_pinned(c, keyi(1)));
    TEST_ASSERT_EQUAL_UINT(0, tp_c0_cache_unpinned_bytes(c)); /* pinned excluded from budget */
    put_bytes(c, 2, 8);
    put_bytes(c, 3, 8); /* unpinned pressure evicts K2, never the pinned K1 */
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(1)));
    TEST_ASSERT_FALSE(tp_c0_cache_contains(c, keyi(2)));
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(3)));
    /* Unpin re-subjects it to the budget. */
    TEST_ASSERT_TRUE(tp_c0_cache_unpin(c, keyi(1)));
    TEST_ASSERT_FALSE(tp_c0_cache_is_pinned(c, keyi(1)));
    tp_c0_cache_destroy(c);
}

void test_explicit_evict(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(1024);
    put_bytes(c, 1, 8);
    TEST_ASSERT_TRUE(tp_c0_cache_evict(c, keyi(1)));
    TEST_ASSERT_FALSE(tp_c0_cache_contains(c, keyi(1)));
    TEST_ASSERT_FALSE(tp_c0_cache_evict(c, keyi(1))); /* already gone */
    tp_c0_cache_destroy(c);
}

/* Selection is BY HASH, independent of insertion order (task-5 relies on this). */
void test_selection_by_hash_not_order(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(1024);
    tp_c0_cache_put(c, keyi(1), "older", 5, NULL);
    tp_c0_cache_put(c, keyi(2), "newer", 5, NULL);
    size_t len = 0;
    const void *older = tp_c0_cache_get(c, keyi(1), &len); /* pick the older hash explicitly */
    TEST_ASSERT_NOT_NULL(older);
    TEST_ASSERT_EQUAL_MEMORY("older", older, 5);
    tp_c0_cache_destroy(c);
}

/* F5: get() returns a pointer that stays valid across a later budget-busting put.
 * Auto-pinning makes the contract safe by construction -- reading the pointer
 * after the eviction pressure must not touch freed memory (UBSan/ASan clean). */
void test_get_pointer_survives_budget_busting_put(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(16);
    put_bytes(c, 1, 8); /* pattern byte = 2 */
    size_t len = 0;
    const void *p = tp_c0_cache_get(c, keyi(1), &len); /* auto-pins K1 */
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT(8, len);
    TEST_ASSERT_TRUE(tp_c0_cache_is_pinned(c, keyi(1))); /* get pinned it */
    /* Budget-busting puts would evict the LRU unpinned entry -- but K1 is pinned. */
    put_bytes(c, 2, 200);
    put_bytes(c, 3, 200);
    TEST_ASSERT_TRUE(tp_c0_cache_contains(c, keyi(1))); /* survived, not freed */
    const uint8_t *bytes = (const uint8_t *)p;
    for (int i = 0; i < 8; i++) {
        TEST_ASSERT_EQUAL_UINT8(2, bytes[i]); /* original blob intact, no UAF */
    }
    TEST_ASSERT_TRUE(tp_c0_cache_unpin(c, keyi(1))); /* caller releases when done */
    tp_c0_cache_destroy(c);
}

/* Entry table full and all pinned -> buffer_too_small, never an abort. */
void test_entry_table_full_all_pinned(void) {
    tp_c0_cache *c = tp_c0_cache_mem_create(1u << 20);
    for (int i = 0; i < TP_C0_CACHE_MAX_ENTRIES; i++) {
        TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_cache_put(c, keyi(i), "z", 1, NULL));
        TEST_ASSERT_TRUE(tp_c0_cache_pin(c, keyi(i)));
    }
    tp_error err;
    tp_c0_detail d = tp_c0_cache_put(c, keyi(TP_C0_CACHE_MAX_ENTRIES), "z", 1, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_BUFFER_TOO_SMALL, d);
    TEST_ASSERT_EQUAL_INT(TP_C0_CACHE_MAX_ENTRIES, tp_c0_cache_count(c));
    tp_c0_cache_destroy(c);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_put_get_roundtrip);
    RUN_TEST(test_put_faults);
    RUN_TEST(test_replace_same_key);
    RUN_TEST(test_lru_eviction_by_budget);
    RUN_TEST(test_single_over_budget_item_retained);
    RUN_TEST(test_pinned_active_never_evicted);
    RUN_TEST(test_explicit_evict);
    RUN_TEST(test_selection_by_hash_not_order);
    RUN_TEST(test_get_pointer_survives_budget_busting_put);
    RUN_TEST(test_entry_table_full_all_pinned);
    return UNITY_END();
}
