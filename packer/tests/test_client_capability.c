#include "tp_core/tp_client_capability.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_capability(
    tp_client_kind client, tp_client_capability capability,
    tp_client_capability_availability expected_availability,
    tp_status expected_status) {
    tp_client_capability_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        expected_status,
        tp_client_capability_query(client, capability, &result));
    TEST_ASSERT_EQUAL_INT(client, result.client);
    TEST_ASSERT_EQUAL_INT(capability, result.capability);
    TEST_ASSERT_EQUAL_INT(
        expected_availability, result.availability);
}

void test_source_refresh_capability_is_typed_for_every_client(void) {
    assert_capability(
        TP_CLIENT_FILE_CLI,
        TP_CLIENT_CAPABILITY_SOURCE_REFRESH_JOB,
        TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
        TP_STATUS_UNSUPPORTED_CAPABILITY);
    assert_capability(
        TP_CLIENT_GUI,
        TP_CLIENT_CAPABILITY_SOURCE_REFRESH_JOB,
        TP_CLIENT_CAPABILITY_AVAILABLE,
        TP_STATUS_OK);
    assert_capability(
        TP_CLIENT_LIVE_HEADLESS,
        TP_CLIENT_CAPABILITY_SOURCE_REFRESH_JOB,
        TP_CLIENT_CAPABILITY_AVAILABLE,
        TP_STATUS_OK);
}

void test_capability_count_is_not_queryable(void) {
    tp_client_capability_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            TP_CLIENT_GUI, TP_CLIENT_CAPABILITY_COUNT,
            &result));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(
        test_source_refresh_capability_is_typed_for_every_client);
    RUN_TEST(test_capability_count_is_not_queryable);
    return UNITY_END();
}
