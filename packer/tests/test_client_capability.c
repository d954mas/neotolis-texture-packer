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

typedef struct capability_case {
    tp_client_capability capability;
    tp_client_capability_availability expected[3];
} capability_case;

void test_capability_matrix_matches_the_client_contract(void) {
#define A TP_CLIENT_CAPABILITY_AVAILABLE
#define NA TP_CLIENT_CAPABILITY_NOT_APPLICABLE
#define NI TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED
    static const capability_case cases[] = {
        {TP_CLIENT_CAPABILITY_TRANSACTION, {NA, A, A}},
        {TP_CLIENT_CAPABILITY_PERSISTENCE, {A, A, A}},
        {TP_CLIENT_CAPABILITY_EVENTS, {NA, A, A}},
        {TP_CLIENT_CAPABILITY_HISTORY, {NA, A, A}},
        {TP_CLIENT_CAPABILITY_RECOVERY, {NA, A, A}},
        {TP_CLIENT_CAPABILITY_PACK_JOB, {NA, A, A}},
        {TP_CLIENT_CAPABILITY_EXPORT_COMMAND, {A, A, A}},
        {TP_CLIENT_CAPABILITY_INSPECT_ASYNC_JOB, {NA, NI, NI}},
        {TP_CLIENT_CAPABILITY_VALIDATE_ASYNC_JOB, {NA, NI, NI}},
        {TP_CLIENT_CAPABILITY_SOURCE_REFRESH_JOB, {NA, A, A}},
    };
#undef NI
#undef NA
#undef A

    TEST_ASSERT_EQUAL_INT(
        TP_CLIENT_CAPABILITY_COUNT -
            TP_CLIENT_CAPABILITY_TRANSACTION,
        (int)(sizeof cases / sizeof cases[0]));
    for (size_t row = 0;
         row < sizeof cases / sizeof cases[0];
         ++row) {
        for (int client_index = 0;
             client_index < 3; ++client_index) {
            const tp_client_kind client =
                (tp_client_kind)(
                    TP_CLIENT_FILE_CLI +
                    client_index);
            const tp_client_capability_availability
                expected =
                    cases[row]
                        .expected[client_index];
            assert_capability(
                client, cases[row].capability,
                expected,
                expected ==
                        TP_CLIENT_CAPABILITY_AVAILABLE
                    ? TP_STATUS_OK
                    : TP_STATUS_UNSUPPORTED_CAPABILITY);
        }
    }
}

void test_capability_query_rejects_invalid_boundaries(void) {
    tp_client_capability_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            TP_CLIENT_GUI,
            TP_CLIENT_CAPABILITY_TRANSACTION,
            NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            (tp_client_kind)0,
            TP_CLIENT_CAPABILITY_TRANSACTION,
            &result));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            (tp_client_kind)(
                TP_CLIENT_LIVE_HEADLESS + 1),
            TP_CLIENT_CAPABILITY_TRANSACTION,
            &result));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            TP_CLIENT_GUI,
            (tp_client_capability)0,
            &result));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_client_capability_query(
            TP_CLIENT_GUI, TP_CLIENT_CAPABILITY_COUNT,
            &result));
}

void test_capability_availability_ids_are_stable(void) {
    TEST_ASSERT_EQUAL_STRING(
        "available",
        tp_client_capability_availability_id(
            TP_CLIENT_CAPABILITY_AVAILABLE));
    TEST_ASSERT_EQUAL_STRING(
        "not_applicable",
        tp_client_capability_availability_id(
            TP_CLIENT_CAPABILITY_NOT_APPLICABLE));
    TEST_ASSERT_EQUAL_STRING(
        "not_implemented",
        tp_client_capability_availability_id(
            TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED));
    TEST_ASSERT_EQUAL_STRING(
        "unknown_availability",
        tp_client_capability_availability_id(
            (tp_client_capability_availability)0));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(
        test_capability_matrix_matches_the_client_contract);
    RUN_TEST(
        test_capability_query_rejects_invalid_boundaries);
    RUN_TEST(
        test_capability_availability_ids_are_stable);
    return UNITY_END();
}
