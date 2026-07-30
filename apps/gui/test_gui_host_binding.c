#include "gui_host_binding.h"

#include <stdlib.h>
#include <string.h>

#include "time/nt_time.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_operation.h"
#include "unity.h"

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

static tp_operation make_rename_operation(
    const gui_host_binding *binding,
    const char *name) {
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(
            &binding->client);
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    const size_t size = strlen(name) + 1U;
    operation.u.atlas_rename.name =
        malloc(size);
    TEST_ASSERT_NOT_NULL(
        operation.u.atlas_rename.name);
    memcpy(
        operation.u.atlas_rename.name,
        name, size);
    return operation;
}

void setUp(void) {}
void tearDown(void) {}

static void set_worker_block_environment(void) {
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(
        0, _putenv_s(
               "TP_TEST_JOB_WORKER_BLOCK_MS",
               "10000"));
#else
    TEST_ASSERT_EQUAL_INT(
        0, setenv(
               "TP_TEST_JOB_WORKER_BLOCK_MS",
               "10000", 1));
#endif
}

static void clear_worker_block_environment(void) {
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(
        0, _putenv_s(
               "TP_TEST_JOB_WORKER_BLOCK_MS",
               ""));
#else
    TEST_ASSERT_EQUAL_INT(
        0, unsetenv(
               "TP_TEST_JOB_WORKER_BLOCK_MS"));
#endif
}

void test_prepare_failure_preserves_complete_old_binding(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *old_session = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, old_session, &error));
    const tp_session_snapshot *old_snapshot =
        gui_session_client_snapshot(
            &binding.client);
    const uint64_t old_generation =
        gui_session_client_instance_generation(&binding.client);

    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_host_binding_begin_replace(
            &binding, make_session(), true,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(
            &binding.client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_session_client_instance_generation(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_binding_lifecycle(&binding));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_SHUTDOWN,
        completed);
}

void test_active_session_alias_is_rejected_without_state_change(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *active = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, active, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(
            &binding.client);
    const uint64_t generation =
        gui_session_client_instance_generation(&binding.client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_binding_begin_replace(
            &binding, active, true,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        active,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_PTR(
        snapshot,
        gui_session_client_snapshot(
            &binding.client));
    TEST_ASSERT_EQUAL_UINT64(
        generation,
        gui_session_client_instance_generation(&binding.client));
    TEST_ASSERT_TRUE(
        binding.client.admission_open);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_binding_lifecycle(
            &binding));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
}

void test_prepared_replace_is_invisible_until_pump_cutover(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *old_session = make_session();
    tp_session *candidate = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, old_session, &error));
    const tp_session_snapshot *old_snapshot =
        gui_session_client_snapshot(
            &binding.client);
    const uint64_t old_generation =
        gui_session_client_instance_generation(&binding.client);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_replace(
            &binding, candidate, true,
            &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(
            &binding.client));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_EQUAL_PTR(
        candidate,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation + 1U,
        gui_session_client_instance_generation(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_REPLACE,
        completed);

    completed = GUI_HOST_TRANSITION_NONE;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
}

void test_shutdown_uses_same_drain_and_terminal_cut(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, make_session(), &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_READY_TO_CUTOVER,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_NOT_NULL(
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_NULL(
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_SHUTDOWN,
        completed);
}

void test_failed_prepare_preserves_receipt_and_cutover_clears_it(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *old_session = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, old_session, &error));
    tp_operation operation =
        make_rename_operation(
            &binding, "retained-old");
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .retained_transaction_id =
            "12121212121212121212121212121212",
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &binding.client,
            &request, &result, &error));
    gui_session_submit_terminal terminal = {0};
    TEST_ASSERT_TRUE(
        gui_session_client_pending_submit_query(
            &binding.client,
            request.retained_transaction_id,
            request.identity, &terminal));

    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_host_binding_begin_replace(
            &binding, make_session(), true,
            &error));
    TEST_ASSERT_TRUE(
        gui_session_client_pending_submit_query(
            &binding.client,
            request.retained_transaction_id,
            request.identity, &terminal));
    TEST_ASSERT_TRUE(
        binding.client.admission_open);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_replace(
            &binding, make_session(), true,
            &error));
    TEST_ASSERT_FALSE(
        binding.client.admission_open);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_FALSE(
        gui_session_client_pending_submit_query(
            &binding.client,
            request.retained_transaction_id,
            request.identity, &terminal));
    TEST_ASSERT_TRUE(
        binding.client.admission_open);

    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_REPLACE,
        completed);
    gui_session_submit_result_destroy(&result);
    tp_operation_free(&operation);
    completed = GUI_HOST_TRANSITION_NONE;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
}

/* USA-32 partial: a delayed worker keeps replacement in DRAINING while the
 * frame pump stays responsive. Limit: the ORDER half -- old-session destroy
 * strictly after terminal, and a non-blocking join -- is not asserted here. */
void test_active_job_completion_is_consumed_before_session_cutover(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *old_session = make_session();
    tp_session *candidate = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, old_session, &error));
    const tp_session_snapshot *old_snapshot =
        gui_session_client_snapshot(
            &binding.client);
    const uint64_t old_generation =
        gui_session_client_instance_generation(&binding.client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_queue_enqueue_export(
            &binding.queue,
            tp_id128_nil(),
            TP_GUI_HOST_BINDING_TEST_DIR,
            &error));
    /* The child inherits this test-only delay when it is spawned below. The
     * host itself clears the variable immediately and only uses normal polls. */
    set_worker_block_environment();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    clear_worker_block_environment();
    TEST_ASSERT_TRUE(
        gui_host_queue__test_active(
            &binding.queue));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_replace(
            &binding, candidate, true,
            &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_host_queue_enqueue_export(
            &binding.queue,
            tp_id128_nil(),
            TP_GUI_HOST_BINDING_TEST_DIR,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(
            &binding.client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_session_client_instance_generation(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_frame_begin(
            &binding.client,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(
            &binding.client));
    gui_session_client_frame_end(
        &binding.client);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_DRAINING,
        gui_host_binding_lifecycle(
            &binding));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(
            &binding.client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_session_client_instance_generation(&binding.client));
    TEST_ASSERT_FALSE(
        binding.client.admission_open);

    int draining_pumps = 0;
    bool saw_completion = false;
    for (int attempt = 0;
         attempt < 10000 &&
         gui_host_binding_lifecycle(
             &binding) !=
             GUI_HOST_OPEN;
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_host_binding_pump(
                &binding, &completed, &error));
        draining_pumps++;
        if (gui_host_binding_lifecycle(
                &binding) ==
            GUI_HOST_DRAINING) {
            TEST_ASSERT_EQUAL_INT(
                TP_STATUS_OK,
                gui_session_client_frame_begin(
                    &binding.client,
                    &error));
            gui_session_client_frame_end(
                &binding.client);
            if (gui_host_queue__test_has_staged(
                    &binding.queue)) {
                TEST_ASSERT_EQUAL_INT(
                    GUI_HOST_DRAINING,
                    gui_host_binding_lifecycle(
                        &binding));
                gui_host_completion completion =
                    {0};
                TEST_ASSERT_TRUE(
                    gui_host_queue_take_completion(
                        &binding.queue,
                        &completion));
                TEST_ASSERT_EQUAL_INT(
                    TP_SESSION_JOB_EXPORT,
                    completion.envelope.kind);
                TEST_ASSERT_EQUAL_INT(
                    TP_SESSION_JOB_CANCELLED,
                    completion.state);
                TEST_ASSERT_EQUAL_INT(
                    TP_STATUS_CANCELLED,
                    completion.status);
                gui_host_completion_destroy(
                    &completion);
                saw_completion = true;
            }
            TEST_ASSERT_EQUAL_PTR(
                old_session,
                gui_session_client_attached_session(&binding.client));
            TEST_ASSERT_EQUAL_UINT64(
                old_generation,
                gui_session_client_instance_generation(&binding.client));
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(draining_pumps > 0);
    TEST_ASSERT_TRUE(saw_completion);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_EQUAL_PTR(
        candidate,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_REPLACE,
        completed);

    completed = GUI_HOST_TRANSITION_NONE;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    for (int attempt = 0;
         attempt < 10000 &&
         gui_host_binding_lifecycle(
             &binding) !=
             GUI_HOST_CLOSED;
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_host_binding_pump(
                &binding, &completed, &error));
    }
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_binding_lifecycle(
            &binding));
}

/* Force-close during a replacement that never cuts over. The binding's prepared
 * bundle holds the ONLY pointer to the candidate session, so force_close must
 * release it (pre-fix this leaked the candidate under ASan) and must leave the
 * CLOSED state from which a fresh initial attach works. */
void test_force_close_mid_replace_releases_candidate_and_allows_fresh_attach(void) {
    gui_host_binding binding;
    gui_host_transition_kind completed =
        GUI_HOST_TRANSITION_NONE;
    tp_error error = {{0}};
    gui_host_binding_init(&binding);
    tp_session *old_session = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, old_session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_replace(
            &binding, make_session(), true,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(&binding.client));

    gui_host_binding_force_close(&binding);
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_CLOSED,
        gui_host_binding_lifecycle(&binding));
    TEST_ASSERT_NULL(
        gui_session_client_attached_session(&binding.client));

    tp_session *fresh = make_session();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_attach_initial(
            &binding, fresh, &error));
    TEST_ASSERT_EQUAL_PTR(
        fresh,
        gui_session_client_attached_session(&binding.client));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_OPEN,
        gui_host_binding_lifecycle(&binding));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_begin_shutdown(
            &binding, true, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_host_binding_pump(
            &binding, &completed, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_HOST_TRANSITION_SHUTDOWN,
        completed);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(
            argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(
        test_prepare_failure_preserves_complete_old_binding);
    RUN_TEST(
        test_active_session_alias_is_rejected_without_state_change);
    RUN_TEST(
        test_prepared_replace_is_invisible_until_pump_cutover);
    RUN_TEST(
        test_shutdown_uses_same_drain_and_terminal_cut);
    RUN_TEST(
        test_failed_prepare_preserves_receipt_and_cutover_clears_it);
    RUN_TEST(
        test_active_job_completion_is_consumed_before_session_cutover);
    RUN_TEST(
        test_force_close_mid_replace_releases_candidate_and_allows_fresh_attach);
    return UNITY_END();
}
