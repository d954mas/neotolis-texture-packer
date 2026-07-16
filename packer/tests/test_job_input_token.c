#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *seed = (uint8_t *)ctx;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)len;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &err));
    TEST_ASSERT_NOT_NULL(session);
    return session;
}

static tp_session_input_token snapshot_input_token(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_session_input_token token =
        tp_session_snapshot_input_token(snapshot);
    tp_session_snapshot_destroy(snapshot);
    return token;
}

static void rename_default_atlas(tp_session *session) {
    tp_error err = {{0}};
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    operation.u.atlas_rename.name = (char *)malloc(sizeof "renamed");
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    memcpy(operation.u.atlas_rename.name, "renamed", sizeof "renamed");

    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "abababababababababababababababab",
           sizeof request.id_hex);
    request.expected_revision = tp_session_snapshot_revision(snapshot);
    request.ops = &operation;
    request.op_count = 1U;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_apply(session, &request, &result, &err));
    TEST_ASSERT_TRUE(result.committed);

    tp_txn_result_free(&result);
    tp_operation_free(&operation);
    tp_session_snapshot_destroy(snapshot);
}

void test_zero_generations_are_a_valid_fresh_job_token(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);
    TEST_ASSERT_EQUAL_UINT64(0U, at_start.model_generation);
    TEST_ASSERT_EQUAL_UINT64(0U, at_start.source_generation);

    tp_session_pack_job_result job_result;
    memset(&job_result, 0, sizeof job_result);
    job_result.input_token_at_start = at_start;

    const tp_session_input_token current = snapshot_input_token(session);
    TEST_ASSERT_TRUE(tp_session_input_token_equal(
        job_result.input_token_at_start, current));
    tp_session_destroy(session);
}

void test_model_mutation_invalidates_the_captured_job_token(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);

    rename_default_atlas(session);
    const tp_session_input_token current = snapshot_input_token(session);

    TEST_ASSERT_EQUAL_UINT64(at_start.model_generation + 1U,
                             current.model_generation);
    TEST_ASSERT_EQUAL_UINT64(at_start.source_generation,
                             current.source_generation);
    TEST_ASSERT_FALSE(tp_session_input_token_equal(at_start, current));
    tp_session_destroy(session);
}

void test_source_runtime_change_invalidates_without_model_mutation(void) {
    tp_session *session = make_session();
    const tp_session_input_token at_start = snapshot_input_token(session);
    tp_error err = {{0}};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_invalidate_sources(session, &err));
    const tp_session_input_token current = snapshot_input_token(session);

    TEST_ASSERT_EQUAL_UINT64(at_start.model_generation,
                             current.model_generation);
    TEST_ASSERT_EQUAL_UINT64(at_start.source_generation + 1U,
                             current.source_generation);
    TEST_ASSERT_FALSE(tp_session_input_token_equal(at_start, current));
    tp_session_destroy(session);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_generations_are_a_valid_fresh_job_token);
    RUN_TEST(test_model_mutation_invalidates_the_captured_job_token);
    RUN_TEST(test_source_runtime_change_invalidates_without_model_mutation);
    return UNITY_END();
}
