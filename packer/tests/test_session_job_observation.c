#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_job.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_job_owner_internal.h"
#include "tp_session_internal.h"
#include "tp_session_job_observation_internal.h"
#include "unity.h"

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *seed = context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = (uint8_t)(*seed + (uint8_t)i);
    }
    *seed = (uint8_t)(*seed + 17U);
    return (int)length;
}

static tp_session *make_session(void) {
    uint8_t seed = 1U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_session_create(&rng, &session, &err), err.msg);
    return session;
}

static tp_id128 first_atlas_id(tp_session *session) {
    tp_session_snapshot *snapshot = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &err), err.msg);
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static tp_session_observation_token initial_token(tp_session *session) {
    tp_session_observation *observation = NULL;
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err), err.msg);
    TEST_ASSERT_NOT_NULL(observation);
    const tp_session_observation_token token =
        tp_session_observation_token_query(observation);
    tp_session_observation_destroy(observation);
    return token;
}

typedef struct fake_job {
    tp_session_owned_job owner;
    tp_session_job_sample sample;
    tp_session_job_result terminal_result;
    tp_session_job_target targets[2];
    int pump_count;
    int destroy_count;
} fake_job;

static void fake_cancel(tp_session_owned_job *owner) {
    fake_job *job = (fake_job *)owner;
    job->sample.cancellation_requested = true;
}

static void fake_destroy(tp_session_owned_job *owner) {
    fake_job *job = (fake_job *)owner;
    job->destroy_count++;
}

static void fake_pump(tp_session_owned_job *owner) {
    fake_job *job = (fake_job *)owner;
    job->pump_count++;
}

static bool fake_observe(tp_session_owned_job *owner,
                         tp_session_job_sample *out) {
    fake_job *job = (fake_job *)owner;
    *out = job->sample;
    return true;
}

static void fake_job_init(fake_job *job, uint64_t instance_generation,
                          uint64_t request_id, tp_id128 target,
                          tp_session_job_kind kind) {
    memset(job, 0, sizeof *job);
    tp_session_owned_job_init(&job->owner, fake_cancel, fake_destroy);
    job->owner.pump = fake_pump;
    job->targets[0].kind = TP_SESSION_JOB_TARGET_ATLAS;
    job->targets[0].atlas_id = target;
    job->sample.state = TP_SESSION_JOB_RUNNING;
    job->sample.total = 100;
    job->terminal_result.kind = kind;
    job->terminal_result.state = TP_SESSION_JOB_SUCCEEDED;
    job->terminal_result.status = TP_STATUS_OK;
    const tp_session_job_descriptor descriptor = {
        .session_instance_generation = instance_generation,
        .request_id = request_id,
        .kind = kind,
        .targets = job->targets,
        .target_count = tp_id128_is_nil(target) ? 0U : 1U,
    };
    tp_session_owned_job_configure_observation(
        &job->owner, &descriptor, fake_observe);
}

static void fake_job_complete(fake_job *job,
                              tp_session_job_state state) {
    job->sample.state = state;
    job->sample.current = job->sample.total;
    job->sample.terminal_status =
        state == TP_SESSION_JOB_CANCELLED ? TP_STATUS_CANCELLED
                                         : TP_STATUS_OK;
    job->terminal_result.state = state;
    job->terminal_result.status = job->sample.terminal_status;
    job->sample.terminal_result = &job->terminal_result;
}

static void remove_atlas(tp_session *session, tp_id128 atlas_id) {
    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = atlas_id;
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "42000000000000000000000000000001", 33U);
    request.expected_revision = tp_session_revision(session);
    request.label = "Delete active job target";
    request.author = "human";
    request.ops = &op;
    request.op_count = 1U;
    tp_txn_result result = {0};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &err), err.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
}

void setUp(void) {
    tp_session__test_reset_snapshot_allocations();
}

void tearDown(void) {
    tp_session__test_reset_snapshot_allocations();
}

void test_reverse_completion_accepts_only_latest_request(void) {
    tp_session *session = make_session();
    const tp_id128 atlas_id = first_atlas_id(session);
    fake_job a;
    fake_job b;
    fake_job_init(&a, 7U, 11U, atlas_id, TP_SESSION_JOB_PACK);
    fake_job_init(&b, 7U, 12U, atlas_id, TP_SESSION_JOB_PACK);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &a.owner, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &b.owner, &err));

    fake_job_complete(&b, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_TERMINAL,
        tp_session_job_observation_admit_internal(session, &b.owner));
    fake_job_complete(&a, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_SUPERSEDED,
        tp_session_job_observation_admit_internal(session, &a.owner));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err));
    const tp_session_job_observed_state state =
        tp_session_observation_job_state(observation);
    TEST_ASSERT_TRUE(state.present);
    TEST_ASSERT_TRUE(state.terminal);
    TEST_ASSERT_TRUE(state.result_accepted);
    TEST_ASSERT_EQUAL_UINT64(12U, state.request_id);
    const tp_session_job_observed_result *accepted =
        tp_session_observation_job_result(observation);
    TEST_ASSERT_NOT_NULL(accepted);
    TEST_ASSERT_EQUAL_UINT64(12U, accepted->request_id);
    TEST_ASSERT_EQUAL_PTR(&b.terminal_result, accepted->result);

    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
    tp_session_job_release_internal(&a.owner);
    tp_session_job_release_internal(&b.owner);
    TEST_ASSERT_EQUAL_INT(1, a.destroy_count);
    TEST_ASSERT_EQUAL_INT(1, b.destroy_count);
}

void test_deleted_target_rejects_terminal_result(void) {
    tp_session *session = make_session();
    const tp_id128 atlas_id = first_atlas_id(session);
    fake_job job;
    fake_job_init(&job, 8U, 21U, atlas_id, TP_SESSION_JOB_PACK);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &job.owner, &err));
    remove_atlas(session, atlas_id);
    fake_job_complete(&job, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_TARGET_DELETED,
        tp_session_job_observation_admit_internal(session, &job.owner));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err));
    const tp_session_job_observed_state state =
        tp_session_observation_job_state(observation);
    TEST_ASSERT_TRUE(state.terminal);
    TEST_ASSERT_FALSE(state.result_accepted);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_TARGET_DELETED, state.rejection);
    TEST_ASSERT_NULL(tp_session_observation_job_result(observation));

    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
    tp_session_job_release_internal(&job.owner);
}

void test_cancel_racing_success_rejects_result(void) {
    tp_session *session = make_session();
    fake_job job;
    fake_job_init(&job, 9U, 31U, first_atlas_id(session),
                  TP_SESSION_JOB_PACK);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &job.owner, &err));
    fake_job_complete(&job, TP_SESSION_JOB_SUCCEEDED);
    job.sample.cancellation_requested = true;
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_CANCELLED,
        tp_session_job_observation_admit_internal(session, &job.owner));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err));
    const tp_session_job_observed_state state =
        tp_session_observation_job_state(observation);
    TEST_ASSERT_TRUE(state.terminal);
    TEST_ASSERT_FALSE(state.result_accepted);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_CANCELLED, state.rejection);
    TEST_ASSERT_NULL(tp_session_observation_job_result(observation));

    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
    tp_session_job_release_internal(&job.owner);
}

void test_duplicate_completion_does_not_advance_result_generation(void) {
    tp_session *session = make_session();
    fake_job a;
    fake_job b;
    fake_job_init(&a, 10U, 41U, first_atlas_id(session),
                  TP_SESSION_JOB_PACK);
    fake_job_init(&b, 10U, 42U, a.targets[0].atlas_id,
                  TP_SESSION_JOB_PACK);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &a.owner, &err));
    fake_job_complete(&a, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_TERMINAL,
        tp_session_job_observation_admit_internal(session, &a.owner));
    tp_session_observation *pinned = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &pinned, &err));
    const tp_session_observation_token terminal_token =
        tp_session_observation_token_query(pinned);

    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_DUPLICATE,
        tp_session_job_observation_admit_internal(session, &a.owner));
    tp_session_observation *unchanged =
        (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, &terminal_token, &unchanged, &err));
    TEST_ASSERT_NULL(unchanged);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &b.owner, &err));
    fake_job_complete(&b, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_TERMINAL,
        tp_session_job_observation_admit_internal(session, &b.owner));
    tp_session_job_release_internal(&a.owner);
    TEST_ASSERT_EQUAL_INT(0, a.destroy_count);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observation_job_result(pinned)->result->status);
    tp_session_observation_destroy(pinned);
    TEST_ASSERT_EQUAL_INT(1, a.destroy_count);

    tp_session_destroy(session);
    tp_session_job_release_internal(&b.owner);
}

void test_new_job_keeps_previous_result_envelope_identity(void) {
    tp_session *session = make_session();
    const tp_id128 atlas_id = first_atlas_id(session);
    fake_job a;
    fake_job b;
    fake_job_init(&a, 70U, 71U, atlas_id, TP_SESSION_JOB_PACK);
    fake_job_init(&b, 70U, 72U, atlas_id, TP_SESSION_JOB_EXPORT);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &a.owner, &err));
    fake_job_complete(&a, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_TERMINAL,
        tp_session_job_observation_admit_internal(session, &a.owner));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &b.owner, &err));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(session, NULL, &observation, &err));
    const tp_session_job_observed_state current =
        tp_session_observation_job_state(observation);
    TEST_ASSERT_EQUAL_UINT64(72U, current.request_id);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, current.kind);
    const tp_session_job_observed_result *accepted =
        tp_session_observation_job_result(observation);
    TEST_ASSERT_NOT_NULL(accepted);
    TEST_ASSERT_EQUAL_UINT64(70U,
                             accepted->session_instance_generation);
    TEST_ASSERT_EQUAL_UINT64(71U, accepted->request_id);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, accepted->kind);
    TEST_ASSERT_EQUAL_PTR(&a.terminal_result, accepted->result);
    TEST_ASSERT_EQUAL_size_t(1U, accepted->target_count);
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id,
                                accepted->targets[0].atlas_id));

    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
    tp_session_job_release_internal(&a.owner);
    tp_session_job_release_internal(&b.owner);
}

void test_old_instance_and_post_close_completions_are_rejected(void) {
    tp_session *session = make_session();
    const tp_id128 atlas_id = first_atlas_id(session);
    fake_job old_job;
    fake_job current_job;
    fake_job_init(&old_job, 50U, 51U, atlas_id, TP_SESSION_JOB_PACK);
    fake_job_init(&current_job, 51U, 51U, atlas_id,
                  TP_SESSION_JOB_PACK);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(
            session, &old_job.owner, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(
            session, &current_job.owner, &err));
    fake_job_complete(&old_job, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_OLD_INSTANCE,
        tp_session_job_observation_admit_internal(
            session, &old_job.owner));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_discard(session, &err));
    fake_job_complete(&current_job, TP_SESSION_JOB_SUCCEEDED);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_SESSION_CLOSED,
        tp_session_job_observation_admit_internal(
            session, &current_job.owner));

    tp_session_destroy(session);
    tp_session_job_release_internal(&old_job.owner);
    tp_session_job_release_internal(&current_job.owner);
}

void test_progress_burst_is_coalesced_without_event_or_project_snapshot(void) {
    tp_session *session = make_session();
    const tp_session_observation_token before = initial_token(session);
    fake_job job;
    fake_job_init(&job, 60U, 61U, first_atlas_id(session),
                  TP_SESSION_JOB_EXPORT);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(session, &job.owner, &err));
    for (int current = 1; current <= 100; ++current) {
        job.sample.current = current;
        TEST_ASSERT_EQUAL_INT(
            TP_SESSION_JOB_ADMISSION_PROGRESS,
            tp_session_job_observation_admit_internal(
                session, &job.owner));
    }
    tp_session__test_reset_snapshot_allocations();

    tp_session_observation *delta = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &before, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    const tp_session_observation_token after =
        tp_session_observation_token_query(delta);
    TEST_ASSERT_EQUAL_UINT64(before.event_sequence, after.event_sequence);
    TEST_ASSERT_GREATER_THAN_UINT64(
        before.job_state_generation, after.job_state_generation);
    TEST_ASSERT_EQUAL_UINT64(
        before.result_generation, after.result_generation);
    TEST_ASSERT_EQUAL_INT(
        100, tp_session_observation_job_state(delta).current);
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session_observation_event_count(delta));
    TEST_ASSERT_NULL(tp_session_observation_snapshot(delta));
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session__test_snapshot_allocation_count());
    TEST_ASSERT_EQUAL_UINT64(0U, tp_session_event_sequence(session));

    tp_session_observation_destroy(delta);
    tp_session_destroy(session);
    tp_session_job_release_internal(&job.owner);
}

void test_observe_does_not_admit_job_progress(void) {
    tp_session *session = make_session();
    fake_job job;
    fake_job_init(&job, 80U, 81U, first_atlas_id(session),
                  TP_SESSION_JOB_EXPORT);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_job_observation_begin_internal(
            session, &job.owner, &err));

    const tp_session_observation_token before = initial_token(session);
    job.sample.current = 50;

    tp_session_observation *unchanged =
        (tp_session_observation *)(uintptr_t)1U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &before, &unchanged, &err), err.msg);
    TEST_ASSERT_NULL(unchanged);
    TEST_ASSERT_EQUAL_INT(0, job.pump_count);

    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_ADMISSION_PROGRESS,
        tp_session_job_observation_admit_internal(
            session, &job.owner));

    tp_session_observation *delta = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_observe(session, &before, &delta, &err), err.msg);
    TEST_ASSERT_NOT_NULL(delta);
    TEST_ASSERT_EQUAL_INT(
        50, tp_session_observation_job_state(delta).current);

    tp_session_observation_destroy(delta);
    tp_session_destroy(session);
    tp_session_job_release_internal(&job.owner);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_reverse_completion_accepts_only_latest_request);
    RUN_TEST(test_deleted_target_rejects_terminal_result);
    RUN_TEST(test_cancel_racing_success_rejects_result);
    RUN_TEST(
        test_duplicate_completion_does_not_advance_result_generation);
    RUN_TEST(test_new_job_keeps_previous_result_envelope_identity);
    RUN_TEST(
        test_old_instance_and_post_close_completions_are_rejected);
    RUN_TEST(
        test_progress_burst_is_coalesced_without_event_or_project_snapshot);
    RUN_TEST(test_observe_does_not_admit_job_progress);
    return UNITY_END();
}
