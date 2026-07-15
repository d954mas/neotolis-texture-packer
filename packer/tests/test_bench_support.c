#include "unity.h"

#include "tp_bench_support.h"

#include <math.h>

void setUp(void) {}
void tearDown(void) {}

void test_percentiles_use_exact_nearest_rank(void) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);

    for (int i = 20; i >= 1; i--) {
        TEST_ASSERT_TRUE(tp_bench_samples_accept(&samples, (double)i));
    }

    TEST_ASSERT_EQUAL_INT(20, (int)samples.count);
    TEST_ASSERT_EQUAL_INT(10, (int)tp_bench_samples_percentile(&samples, 50));
    TEST_ASSERT_EQUAL_INT(19, (int)tp_bench_samples_percentile(&samples, 95));
}

void test_failed_sample_is_excluded_and_makes_run_fail(void) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);

    TEST_ASSERT_TRUE(tp_bench_samples_record(&samples, true, 1.25));
    TEST_ASSERT_FALSE(tp_bench_samples_record(&samples, false, 9999.0));
    TEST_ASSERT_TRUE(tp_bench_samples_record(&samples, true, 2.50));

    TEST_ASSERT_EQUAL_INT(2, (int)samples.count);
    TEST_ASSERT_EQUAL_INT(1, (int)samples.failed);
    TEST_ASSERT_EQUAL_INT(125, (int)(samples.values[0] * 100.0));
    TEST_ASSERT_EQUAL_INT(250, (int)(samples.values[1] * 100.0));
    TEST_ASSERT_FALSE(tp_bench_samples_valid(&samples));
}

void test_non_finite_samples_fail_closed(void) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);

    TEST_ASSERT_FALSE(tp_bench_samples_accept(&samples, (double)NAN));
    TEST_ASSERT_FALSE(tp_bench_samples_accept(&samples, (double)INFINITY));
    TEST_ASSERT_EQUAL_INT(0, (int)samples.count);
    TEST_ASSERT_EQUAL_INT(2, (int)samples.failed);
    TEST_ASSERT_FALSE(tp_bench_samples_valid(&samples));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_percentiles_use_exact_nearest_rank);
    RUN_TEST(test_failed_sample_is_excluded_and_makes_run_fail);
    RUN_TEST(test_non_finite_samples_fail_closed);
    return UNITY_END();
}
