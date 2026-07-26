#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gui_session_client.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_session_snapshot_query.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

typedef struct reducer_probe {
    int calls;
    bool resync;
    tp_session_event_kind last_event;
    uint64_t instance_generation;
    const tp_session_snapshot *snapshot;
    const gui_session_client *client;
    const tp_session_snapshot *published_during_reduce;
} reducer_probe;

static void probe_reduce(
    void *context, const tp_session_observation *observation,
    uint64_t instance_generation) {
    reducer_probe *probe = context;
    probe->calls++;
    probe->resync =
        tp_session_observation_resync_required(observation);
    const size_t count =
        tp_session_observation_event_count(observation);
    const tp_session_event *event =
        count > 0U
            ? tp_session_observation_event_at(
                  observation, count - 1U)
            : NULL;
    probe->last_event =
        event ? event->kind : (tp_session_event_kind)0;
    probe->instance_generation = instance_generation;
    probe->snapshot =
        tp_session_observation_snapshot(observation);
    probe->published_during_reduce =
        gui_session_client_snapshot(probe->client);
}

static tp_session *make_session(void) {
    tp_rng rng = tp_rng_os();
    tp_session *session = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_create_default_project(&rng, &session, &error),
        error.msg);
    return session;
}

static tp_id128 first_atlas_id(tp_session *session) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error));
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 id = atlas->id;
    tp_session_snapshot_destroy(snapshot);
    return id;
}

static void rename_first_atlas(
    tp_session *session, const char *name,
    const char *transaction_id) {
    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = first_atlas_id(session);
    const size_t name_size = strlen(name) + 1U;
    operation.u.atlas_rename.name = malloc(name_size);
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    memcpy(operation.u.atlas_rename.name, name, name_size);
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(
        request.id_hex, sizeof request.id_hex, "%s",
        transaction_id);
    request.expected_revision = tp_session_revision(session);
    request.label = "test rename";
    request.author = "human";
    request.ops = &operation;
    request.op_count = 1U;
    tp_txn_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_apply(session, &request, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    tp_operation_free(&operation);
}

void setUp(void) {}
void tearDown(void) {}

void test_attach_owns_initial_resync_and_fans_out(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    TEST_ASSERT_NOT_NULL(gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_INT(1, probe.calls);
    TEST_ASSERT_TRUE(probe.resync);
    TEST_ASSERT_EQUAL_UINT64(
        1U, gui_session_client_instance_generation(&client));
    TEST_ASSERT_EQUAL_UINT64(
        1U, probe.instance_generation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_external_commit_becomes_the_displayed_snapshot(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *before =
        gui_session_client_snapshot(&client);
    const int64_t revision_before =
        tp_session_snapshot_revision(before);
    rename_first_atlas(
        session, "external",
        "11111111111111111111111111111111");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    const tp_session_snapshot *after =
        gui_session_client_snapshot(&client);
    TEST_ASSERT_NOT_EQUAL(before, after);
    TEST_ASSERT_EQUAL_INT64(
        revision_before + 1,
        tp_session_snapshot_revision(after));
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_EVENT_MODEL_COMMITTED, probe.last_event);
    TEST_ASSERT_EQUAL_PTR(
        before, probe.published_during_reduce);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_explicit_resync_publishes_a_complete_snapshot(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *before =
        gui_session_client_snapshot(&client);
    const uint64_t lifetime =
        gui_session_client_snapshot_lifetime_generation(&client);
    rename_first_atlas(
        session, "resynced",
        "12121212121212121212121212121212");

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_resync(&client, &error));
    const tp_session_snapshot *after =
        gui_session_client_snapshot(&client);
    TEST_ASSERT_NOT_EQUAL(before, after);
    TEST_ASSERT_TRUE(
        gui_session_client_snapshot_lifetime_generation(&client) >
        lifetime);
    TEST_ASSERT_TRUE(tp_session_observation_resync_required(
        gui_session_client_observation(&client)));
    TEST_ASSERT_EQUAL_STRING(
        "resynced",
        tp_session_snapshot_atlas_at(after, 0)->name);

    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_unchanged_observe_does_not_swap_or_advance_lifetime(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    const uint64_t lifetime =
        gui_session_client_snapshot_lifetime_generation(&client);
    const int calls = probe.calls;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_EQUAL_PTR(
        snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        lifetime,
        gui_session_client_snapshot_lifetime_generation(&client));
    TEST_ASSERT_EQUAL_INT(calls, probe.calls);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_one_batch_fans_out_to_every_registered_reducer(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe first = {0};
    reducer_probe second = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    first.client = &client;
    second.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &first, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &second, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    rename_first_atlas(
        session, "fanout",
        "44444444444444444444444444444444");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_EQUAL_INT(2, first.calls);
    TEST_ASSERT_EQUAL_INT(2, second.calls);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_EVENT_MODEL_COMMITTED, first.last_event);
    TEST_ASSERT_EQUAL_INT(
        first.last_event, second.last_event);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_external_save_updates_identity_and_dirty_snapshot(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    rename_first_atlas(
        session, "save-visible",
        "55555555555555555555555555555555");
    char path[1024];
    (void)snprintf(
        path, sizeof path, "%s/gui_session_client.ntp",
        TP_GUI_SESSION_CLIENT_TEST_DIR);
    (void)remove(path);
    tp_session_save_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_session_save_as(
            session, path, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.saved);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    TEST_ASSERT_FALSE(tp_session_snapshot_dirty(snapshot));
    TEST_ASSERT_EQUAL_INT(
        TP_IDENTITY_SAVED,
        tp_session_snapshot_identity(snapshot).kind);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_EVENT_SAVED, probe.last_event);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
    (void)remove(path);
}

void test_event_gap_produces_complete_resync(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    for (int index = 0; index < 70; ++index) {
        char name[64];
        char transaction_id[33];
        (void)snprintf(name, sizeof name, "gap-%d", index);
        (void)snprintf(
            transaction_id, sizeof transaction_id,
            "%032x", index + 1);
        rename_first_atlas(session, name, transaction_id);
    }
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_TRUE(probe.resync);
    TEST_ASSERT_NOT_NULL(probe.snapshot);
    TEST_ASSERT_EQUAL_INT64(
        70, tp_session_snapshot_revision(probe.snapshot));
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_frame_pin_defers_swap_until_the_next_frame(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_frame_begin(&client, &error));
    const tp_session_snapshot *pinned =
        gui_session_client_snapshot(&client);
    rename_first_atlas(
        session, "during-frame",
        "22222222222222222222222222222222");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_request_observe(&client, &error));
    TEST_ASSERT_EQUAL_PTR(
        pinned, gui_session_client_snapshot(&client));
    gui_session_client_frame_end(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_frame_begin(&client, &error));
    TEST_ASSERT_NOT_EQUAL(
        pinned, gui_session_client_snapshot(&client));
    gui_session_client_frame_end(&client);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_observe_failure_preserves_snapshot_token_and_generation(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    rename_first_atlas(
        session, "retry",
        "33333333333333333333333333333333");
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    const tp_session_observation_token token =
        gui_session_client_observed_token(&client);
    const uint64_t lifetime =
        gui_session_client_snapshot_lifetime_generation(&client);
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_EQUAL_PTR(
        snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_TRUE(tp_session_observation_token_equal(
        token, gui_session_client_observed_token(&client)));
    TEST_ASSERT_EQUAL_UINT64(
        lifetime,
        gui_session_client_snapshot_lifetime_generation(&client));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_NOT_EQUAL(
        snapshot, gui_session_client_snapshot(&client));
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_runtime_only_delta_keeps_the_model_snapshot_owner(void) {
    tp_session *session = make_session();
    gui_session_client client;
    reducer_probe probe = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    probe.client = &client;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_register_reducer(
            &client, probe_reduce, &probe, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    const uint64_t lifetime =
        gui_session_client_snapshot_lifetime_generation(&client);
    const uint64_t source_generation =
        gui_session_client_source_runtime_generation(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_invalidate_sources(session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_EQUAL_PTR(
        snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        lifetime,
        gui_session_client_snapshot_lifetime_generation(&client));
    TEST_ASSERT_TRUE(
        gui_session_client_source_runtime_generation(&client) >
        source_generation);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED, probe.last_event);
    TEST_ASSERT_NULL(probe.snapshot);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_attach_replaces_binding_only_after_initial_observation(void) {
    tp_session *first = make_session();
    tp_session *second = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, first, &error));
    const tp_session_snapshot *first_snapshot =
        gui_session_client_snapshot(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, second, &error));
    TEST_ASSERT_NOT_EQUAL(
        first_snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        2U, gui_session_client_instance_generation(&client));
    tp_session_destroy(first);
    TEST_ASSERT_NOT_NULL(gui_session_client_snapshot(&client));
    gui_session_client_detach(&client);
    tp_session_destroy(second);
}

void test_failed_reattach_keeps_old_binding_and_generation(void) {
    tp_session *first = make_session();
    tp_session *second = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, first, &error));
    const tp_session_snapshot *first_snapshot =
        gui_session_client_snapshot(&client);
    const tp_session_observation_token first_token =
        gui_session_client_observed_token(&client);
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_session_client_attach(&client, second, &error));
    TEST_ASSERT_EQUAL_PTR(
        first_snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_TRUE(tp_session_observation_token_equal(
        first_token, gui_session_client_observed_token(&client)));
    TEST_ASSERT_EQUAL_UINT64(
        1U, gui_session_client_instance_generation(&client));
    rename_first_atlas(
        first, "still-bound",
        "66666666666666666666666666666666");
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_EQUAL_INT64(
        1,
        tp_session_snapshot_revision(
            gui_session_client_snapshot(&client)));
    gui_session_client_detach(&client);
    TEST_ASSERT_NULL(gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_observe(&client, &error));
    tp_session_destroy(first);
    tp_session_destroy(second);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_attach_owns_initial_resync_and_fans_out);
    RUN_TEST(test_external_commit_becomes_the_displayed_snapshot);
    RUN_TEST(test_explicit_resync_publishes_a_complete_snapshot);
    RUN_TEST(
        test_unchanged_observe_does_not_swap_or_advance_lifetime);
    RUN_TEST(
        test_one_batch_fans_out_to_every_registered_reducer);
    RUN_TEST(
        test_external_save_updates_identity_and_dirty_snapshot);
    RUN_TEST(test_event_gap_produces_complete_resync);
    RUN_TEST(test_frame_pin_defers_swap_until_the_next_frame);
    RUN_TEST(
        test_observe_failure_preserves_snapshot_token_and_generation);
    RUN_TEST(
        test_runtime_only_delta_keeps_the_model_snapshot_owner);
    RUN_TEST(
        test_attach_replaces_binding_only_after_initial_observation);
    RUN_TEST(
        test_failed_reattach_keeps_old_binding_and_generation);
    return UNITY_END();
}
