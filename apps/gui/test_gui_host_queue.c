#include "gui_host_queue.h"

#include <string.h>

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_project.h"
#include "time/nt_time.h"
#include "unity.h"

/* The published rejection vocabulary is exactly the classes that have a
 * producer: CANCELLED/TARGET_DELETED from session job admission and
 * OLD_INSTANCE/SESSION_CLOSED from host start admission. Adding a member without
 * a producer, or reordering them, breaks this pin. */
_Static_assert(
    TP_SESSION_JOB_REJECTION_NONE == 0 &&
        TP_SESSION_JOB_REJECTION_CANCELLED == 1 &&
        TP_SESSION_JOB_REJECTION_TARGET_DELETED == 2 &&
        TP_SESSION_JOB_REJECTION_OLD_INSTANCE == 3 &&
        TP_SESSION_JOB_REJECTION_SESSION_CLOSED == 4,
    "public job rejection vocabulary is producer-backed and closed");

static tp_session *make_session(void) {
    tp_rng rng = tp_rng_os();
    tp_session *session = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_default_project(
            &rng, &session, &error));
    return session;
}

static void drain_until_staged(
    gui_host_queue *queue, tp_session *session) {
    tp_error error = {{0}};
    for (int attempt = 0; attempt < 10000; ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_host_queue_drain(
                queue, session, &error));
        if (gui_host_queue__test_has_staged(queue)) {
            return;
        }
        nt_time_sleep(0.001);
    }
    TEST_FAIL_MESSAGE(
        "host queue did not stage export completion");
}

static void stage_export_completion(
    gui_host_queue *queue, tp_session *session) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR, &error));
    drain_until_staged(queue, session);
}

static void reduce_current_observation(
    gui_host_queue *queue, tp_session *session,
    uint64_t instance_generation) {
    tp_error error = {{0}};
    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation,
            &error));
    gui_host_queue_reduce_observation(
        queue, observation,
        instance_generation);
    tp_session_observation_destroy(observation);
}

void setUp(void) {}
/* tearDown owns every armed seam (as packer/tests/test_job_worker_process.c
 * does): Unity longjmps out of a failing assert, so a seam cleared only on the
 * last line of its own test would stay armed for every test after the first
 * failure and turn one red case into a cascade. */
void tearDown(void) { gui_host_queue__test_fail_cancels(0U); }

void test_open_queue_deep_copies_start_payload_and_assigns_identity(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 41U, &error));

    char work_dir[64] = "copied-work";
    char exporter[64] = "defold";
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_pack(
            &queue, tp_id128_nil(),
            work_dir, exporter, &error));
    memset(work_dir, 'x', sizeof work_dir);
    memset(exporter, 'y', sizeof exporter);

    gui_host_command command = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue__test_peek_start(
            &queue, &command));
    TEST_ASSERT_EQUAL_STRING(
        "copied-work", command.work_dir);
    TEST_ASSERT_EQUAL_STRING(
        "defold", command.preview_exporter_id);
    TEST_ASSERT_EQUAL_UINT64(
        41U,
        command.envelope.session_instance_generation);
    TEST_ASSERT_EQUAL_UINT64(
        1U, command.envelope.request_id);
}

void test_lifecycle_rejects_ingress_outside_open(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(), ".", &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 7U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(), ".", &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
    gui_host_queue_commit_cutover(
        &queue, 8U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    gui_host_queue_commit_close(&queue);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_queue_lifecycle(&queue));
}

void test_open_rejects_zero_instance_generation(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_open(&queue, 0U, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_queue_lifecycle(&queue));
}

void test_begin_drain_rejects_queued_start_without_admitting_it(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 11U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(), ".", &error));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_FALSE(completion.publish_result);
    /* The refusal is typed: the class is authoritative and the prose is detail. */
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_SESSION_CLOSED,
        completion.rejection);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_FAILED, completion.state);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        completion.status);
    TEST_ASSERT_NOT_EQUAL_INT(
        0, (int)strlen(completion.error.msg));
    TEST_ASSERT_EQUAL_UINT64(
        1U, completion.envelope.request_id);
    gui_host_completion_destroy(&completion);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
}

/* A queued start that no longer belongs to the host instance is refused at the
 * drain with the typed OLD_INSTANCE class instead of prose alone. The refusal is
 * host-side: it is READY the moment it is staged, so it never travels through
 * the observation reducer that projects accepted results. */
void test_stale_instance_start_is_rejected_as_old_instance(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 73U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR, &error));
    gui_host_queue__test_retag_pending_start_instance(
        &queue, 72U);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_active(&queue));

    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_FALSE(completion.publish_result);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_OLD_INSTANCE,
        completion.rejection);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_FAILED, completion.state);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        completion.status);
    TEST_ASSERT_NOT_EQUAL_INT(
        0, (int)strlen(completion.error.msg));
    TEST_ASSERT_EQUAL_UINT64(
        72U,
        completion.envelope
            .session_instance_generation);
    gui_host_completion_destroy(&completion);

    /* The host stays a usable OPEN owner behind the refusal. */
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue_busy(&queue));
    tp_session_destroy(session);
}

void test_duplicate_queued_cancel_is_rejected(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 13U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(), ".", &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_cancel(
            &queue, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_enqueue_cancel(
            &queue, &error));
}

void test_terminal_receipt_waits_for_authoritative_observation(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 19U, &error));
    stage_export_completion(&queue, session);

    gui_host_completion completion = {0};
    TEST_ASSERT_FALSE(
        gui_host_queue_take_completion(
            &queue, &completion));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation, &error));
    gui_host_queue_reduce_observation(
        &queue, observation, 19U);
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_TRUE(completion.publish_result);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_EXPORT,
        completion.envelope.kind);
    gui_host_completion_destroy(&completion);
    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
}

void test_wrong_instance_receipt_is_discarded(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 23U, &error));
    stage_export_completion(&queue, session);

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation, &error));
    gui_host_queue_reduce_observation(
        &queue, observation, 24U);
    gui_host_completion completion = {0};
    TEST_ASSERT_FALSE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_has_staged(&queue));
    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
}

void test_superseded_receipt_is_not_published(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 29U, &error));
    stage_export_completion(&queue, session);
    gui_host_queue__test_retag_staged_request(
        &queue, 999U);

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation, &error));
    gui_host_queue_reduce_observation(
        &queue, observation, 29U);
    gui_host_completion completion = {0};
    TEST_ASSERT_FALSE(
        gui_host_queue_take_completion(
            &queue, &completion));
    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
}

void test_terminal_observation_is_not_republished(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 31U, &error));
    stage_export_completion(&queue, session);

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation, &error));
    gui_host_queue_reduce_observation(
        &queue, observation, 31U);
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    gui_host_completion_destroy(&completion);
    gui_host_queue_reduce_observation(
        &queue, observation, 31U);
    TEST_ASSERT_FALSE(
        gui_host_queue_take_completion(
            &queue, &completion));
    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
}

void test_draining_waits_for_terminal_observation_classification(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 37U, &error));
    stage_export_completion(&queue, session);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));

    tp_session_observation *observation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_observe(
            session, NULL, &observation, &error));
    gui_host_queue_reduce_observation(
        &queue, observation, 37U);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    gui_host_completion_destroy(&completion);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
    tp_session_observation_destroy(observation);
    tp_session_destroy(session);
}

void test_begin_drain_does_not_repeat_an_admitted_cancel(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 41U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR,
            &error));
    gui_host_queue__test_fail_next_poll();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_cancel(
            &queue, &error));
    gui_host_queue__test_fail_next_poll();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(
            &queue, &error));
    for (int attempt = 0;
         attempt < 10000 &&
         !gui_host_queue__test_has_staged(
             &queue);
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_host_queue_drain(
                &queue, session, &error));
        if (!gui_host_queue__test_has_staged(
                &queue)) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(
        gui_host_queue__test_has_staged(
            &queue));
    reduce_current_observation(
        &queue, session, 41U);
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_CANCELLED,
        completion.rejection);
    gui_host_completion_destroy(&completion);
    tp_session_destroy(session);
}

void test_poll_failure_keeps_active_owner_and_draining_cancel_wins(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 43U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR,
            &error));
    gui_host_queue__test_fail_next_poll();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_TRUE(
        gui_host_queue__test_active(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_has_staged(
            &queue));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(
            &queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    for (int attempt = 0;
         attempt < 10000 &&
         !gui_host_queue__test_has_staged(
             &queue);
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_host_queue_drain(
                &queue, session, &error));
        if (!gui_host_queue__test_has_staged(
                &queue)) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(
        gui_host_queue__test_has_staged(
            &queue));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    reduce_current_observation(
        &queue, session, 43U);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));

    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_FALSE(
        completion.publish_result);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_CANCELLED,
        completion.state);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_CANCELLED,
        completion.rejection);
    gui_host_completion_destroy(&completion);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
    tp_session_destroy(session);
}

/* A cancel admission that keeps failing must not wedge the drain: the job poll
 * is the only thing that clears the active lease, so an early return on the
 * failed cancel left a DRAINING host that could never reach READY_TO_CUTOVER --
 * New, Open and Exit froze behind it. */
void test_failing_cancel_admission_does_not_wedge_drain(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 67U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_TRUE(
        gui_host_queue__test_active(&queue));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_cancel(
            &queue, &error));
    gui_host_queue__test_fail_cancels(100000U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));

    bool saw_cancel_failure = false;
    for (int attempt = 0;
         attempt < 10000 &&
         !gui_host_queue__test_has_staged(&queue);
         ++attempt) {
        const tp_status status =
            gui_host_queue_drain(
                &queue, session, &error);
        if (status != TP_STATUS_OK) {
            saw_cancel_failure = true;
            TEST_ASSERT_EQUAL_INT(
                TP_STATUS_OOM, status);
        }
        if (!gui_host_queue__test_has_staged(
                &queue)) {
            nt_time_sleep(0.001);
        }
    }
    /* The rejection was real, the request stayed queued, and the drain still
     * made progress through it. */
    TEST_ASSERT_TRUE(saw_cancel_failure);
    TEST_ASSERT_TRUE(queue.cancel_queued);
    TEST_ASSERT_TRUE(
        gui_host_queue__test_has_staged(&queue));

    /* With the job terminal the cancel becomes moot (NOT_FOUND) and resolves. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_drain(
            &queue, session, &error));
    TEST_ASSERT_FALSE(queue.cancel_queued);

    reduce_current_observation(
        &queue, session, 67U);
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    gui_host_completion_destroy(&completion);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));
    tp_session_destroy(session);
}

/* The forced close is the exit path's terminal answer: it must reach CLOSED
 * from a DRAINING owner that still holds an active lease and a staged
 * completion, because the session behind them is destroyed right after. */
void test_force_close_reaches_closed_from_a_staged_draining_owner(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 71U, &error));
    stage_export_completion(&queue, session);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(&queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_TRUE(
        gui_host_queue__test_has_staged(&queue));

    gui_host_queue_force_close(&queue);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_has_staged(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_active(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue_busy(&queue));
    TEST_ASSERT_EQUAL_UINT64(
        0U, queue.session_instance_generation);
    tp_session_destroy(session);
}

void test_take_failure_keeps_active_owner_until_retry_detaches(void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 47U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR,
            &error));
    gui_host_queue__test_fail_next_take();
    bool saw_failure = false;
    for (int attempt = 0;
         attempt < 10000 &&
         !gui_host_queue__test_has_staged(
             &queue);
         ++attempt) {
        const tp_status status =
            gui_host_queue_drain(
                &queue, session, &error);
        if (status == TP_STATUS_OOM) {
            saw_failure = true;
            TEST_ASSERT_TRUE(
                gui_host_queue__test_active(
                    &queue));
            TEST_ASSERT_FALSE(
                gui_host_queue__test_has_staged(
                    &queue));
        } else {
            TEST_ASSERT_EQUAL_INT(
                TP_STATUS_OK, status);
        }
        if (!gui_host_queue__test_has_staged(
                &queue)) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(saw_failure);
    TEST_ASSERT_TRUE(
        gui_host_queue__test_has_staged(
            &queue));
    reduce_current_observation(
        &queue, session, 47U);
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_TRUE(
        completion.publish_result);
    gui_host_completion_destroy(&completion);
    tp_session_destroy(session);
}

void test_nonfallible_cutover_reopens_with_only_new_generation(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(
            &queue, 51U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(
            &queue, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_queue_lifecycle(&queue));

    gui_host_queue_commit_cutover(
        &queue, 52U);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_EQUAL_UINT64(
        52U,
        queue.session_instance_generation);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(), ".",
            &error));
    gui_host_command command = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue__test_peek_start(
            &queue, &command));
    TEST_ASSERT_EQUAL_UINT64(
        52U,
        command.envelope
            .session_instance_generation);
}

void test_nonfallible_close_clears_ready_owner(void) {
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(
            &queue, 61U, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_begin_drain(
            &queue, &error));
    gui_host_queue_commit_close(&queue);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_EQUAL_UINT64(
        0U,
        queue.session_instance_generation);
}

/* Request-id exhaustion is a structured rejection, not a wedge: the admission
 * refuses the start, nothing is staged, and the queue stays a drainable OPEN host
 * that admits work again as soon as the counter has room. */
void test_exhausted_request_id_space_rejects_start_without_wedging_queue(
    void) {
    tp_session *session = make_session();
    gui_host_queue queue;
    tp_error error = {{0}};
    gui_host_queue_init(&queue);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_open(&queue, 71U, &error));
    gui_host_queue__test_set_next_request_id(
        &queue, UINT64_MAX);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR, &error));
    TEST_ASSERT_NOT_EQUAL_INT(
        0, (int)strlen(error.msg));
    /* Structured refusal only: no queued start, no active lease, no staged
     * completion, and the host is still OPEN. */
    gui_host_command rejected = {0};
    TEST_ASSERT_FALSE(
        gui_host_queue__test_peek_start(
            &queue, &rejected));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_active(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue__test_has_staged(&queue));
    TEST_ASSERT_FALSE(
        gui_host_queue_busy(&queue));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_queue_lifecycle(&queue));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_MAX, queue.next_request_id);

    /* Still drainable, and still able to admit work once ids are available. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_drain(
            &queue, session, &error));
    gui_host_queue__test_set_next_request_id(
        &queue, UINT64_MAX - 1U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &queue, tp_id128_nil(),
            TP_GUI_HOST_QUEUE_TEST_DIR, &error));
    drain_until_staged(&queue, session);
    reduce_current_observation(
        &queue, session, 71U);
    gui_host_completion completion = {0};
    TEST_ASSERT_TRUE(
        gui_host_queue_take_completion(
            &queue, &completion));
    TEST_ASSERT_EQUAL_UINT64(
        UINT64_MAX,
        completion.envelope.request_id);
    gui_host_completion_destroy(&completion);
    gui_host_queue_force_close(&queue);
    tp_session_destroy(session);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(
            argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(
        test_open_queue_deep_copies_start_payload_and_assigns_identity);
    RUN_TEST(test_lifecycle_rejects_ingress_outside_open);
    RUN_TEST(test_open_rejects_zero_instance_generation);
    RUN_TEST(
        test_begin_drain_rejects_queued_start_without_admitting_it);
    RUN_TEST(
        test_stale_instance_start_is_rejected_as_old_instance);
    RUN_TEST(test_duplicate_queued_cancel_is_rejected);
    RUN_TEST(
        test_terminal_receipt_waits_for_authoritative_observation);
    RUN_TEST(test_wrong_instance_receipt_is_discarded);
    RUN_TEST(test_superseded_receipt_is_not_published);
    RUN_TEST(test_terminal_observation_is_not_republished);
    RUN_TEST(
        test_draining_waits_for_terminal_observation_classification);
    RUN_TEST(
        test_begin_drain_does_not_repeat_an_admitted_cancel);
    RUN_TEST(
        test_poll_failure_keeps_active_owner_and_draining_cancel_wins);
    RUN_TEST(
        test_failing_cancel_admission_does_not_wedge_drain);
    RUN_TEST(
        test_force_close_reaches_closed_from_a_staged_draining_owner);
    RUN_TEST(
        test_take_failure_keeps_active_owner_until_retry_detaches);
    RUN_TEST(
        test_nonfallible_cutover_reopens_with_only_new_generation);
    RUN_TEST(
        test_nonfallible_close_clears_ready_owner);
    RUN_TEST(
        test_exhausted_request_id_space_rejects_start_without_wedging_queue);
    return UNITY_END();
}
