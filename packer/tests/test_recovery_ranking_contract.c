#define _CRT_SECURE_NO_WARNINGS

#include "tp_recovery_internal.h"
#include "unity.h"

#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static tp_recovery_candidate candidate(const char *path, int64_t timestamp,
                                       bool adoptable) {
    tp_recovery_candidate value;
    memset(&value, 0, sizeof value);
    (void)snprintf(value.journal_path, sizeof value.journal_path, "%s", path);
    value.timestamp = timestamp;
    value.adoptable = adoptable;
    return value;
}

static void insert(tp_recovery_candidates *out, const char *path,
                   int64_t timestamp, bool adoptable) {
    const tp_recovery_candidate value =
        candidate(path, timestamp, adoptable);
    tp_recovery__test_candidate_insert(out, &value);
}

void test_ranking_is_adoptable_then_timestamp_then_path(void) {
    tp_recovery_candidates ranked;
    memset(&ranked, 0, sizeof ranked);
    insert(&ranked, "z-nonadoptable.ntpjournal", 999, false);
    insert(&ranked, "z-equal.ntpjournal", 20, true);
    insert(&ranked, "a-equal.ntpjournal", 20, true);
    insert(&ranked, "newest.ntpjournal", 30, true);
    insert(&ranked, "a-nonadoptable.ntpjournal", 1, false);

    TEST_ASSERT_EQUAL_size_t(5U, ranked.count);
    TEST_ASSERT_EQUAL_STRING("newest.ntpjournal",
                             ranked.items[0].journal_path);
    TEST_ASSERT_EQUAL_STRING("a-equal.ntpjournal",
                             ranked.items[1].journal_path);
    TEST_ASSERT_EQUAL_STRING("z-equal.ntpjournal",
                             ranked.items[2].journal_path);
    TEST_ASSERT_EQUAL_STRING("z-nonadoptable.ntpjournal",
                             ranked.items[3].journal_path);
    TEST_ASSERT_EQUAL_STRING("a-nonadoptable.ntpjournal",
                             ranked.items[4].journal_path);
}

static void fill_equal_timestamp(tp_recovery_candidates *out, bool reverse) {
    memset(out, 0, sizeof *out);
    for (size_t offset = 0; offset < 20U; offset++) {
        const size_t index = reverse ? 19U - offset : offset;
        char path[64];
        (void)snprintf(path, sizeof path,
                       "candidate-%02zu.ntpjournal", index);
        insert(out, path, 100, true);
    }
}

void test_equal_timestamp_cap_is_permutation_invariant(void) {
    tp_recovery_candidates ascending;
    tp_recovery_candidates descending;
    fill_equal_timestamp(&ascending, false);
    fill_equal_timestamp(&descending, true);

    TEST_ASSERT_EQUAL_size_t(TP_RECOVERY_MAX_CANDIDATES, ascending.count);
    TEST_ASSERT_EQUAL_size_t(TP_RECOVERY_MAX_CANDIDATES, descending.count);
    TEST_ASSERT_TRUE(ascending.has_more);
    TEST_ASSERT_TRUE(descending.has_more);
    for (size_t index = 0U; index < TP_RECOVERY_MAX_CANDIDATES; index++) {
        char expected[64];
        (void)snprintf(expected, sizeof expected,
                       "candidate-%02zu.ntpjournal", index);
        TEST_ASSERT_EQUAL_STRING(expected,
                                 ascending.items[index].journal_path);
        TEST_ASSERT_EQUAL_STRING(expected,
                                 descending.items[index].journal_path);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_ranking_is_adoptable_then_timestamp_then_path);
    RUN_TEST(test_equal_timestamp_cap_is_permutation_invariant);
    return UNITY_END();
}
