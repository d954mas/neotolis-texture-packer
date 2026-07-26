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

static tp_operation make_rename_operation(
    tp_session *session, const char *name) {
    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = first_atlas_id(session);
    const size_t size = strlen(name) + 1U;
    operation.u.atlas_rename.name = malloc(size);
    TEST_ASSERT_NOT_NULL(operation.u.atlas_rename.name);
    memcpy(operation.u.atlas_rename.name, name, size);
    return operation;
}

typedef struct test_rng_state {
    uint8_t next;
    int calls;
    bool fail;
} test_rng_state;

typedef struct collision_rng_state {
    int calls;
} collision_rng_state;

static int collision_rng_fill(
    void *context, uint8_t *out, size_t size) {
    collision_rng_state *state = context;
    const uint8_t first =
        state->calls < 2 ? 0x40U : 0x80U;
    state->calls++;
    for (size_t index = 0U; index < size; ++index) {
        out[index] = (uint8_t)(first + index);
    }
    return (int)size;
}

static int test_rng_fill(
    void *context, uint8_t *out, size_t size) {
    test_rng_state *state = context;
    state->calls++;
    if (state->fail) {
        return -1;
    }
    for (size_t index = 0U; index < size; ++index) {
        out[index] = state->next++;
    }
    return (int)size;
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
    gui_session_client_prepared prepared = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_prepare(
            &client, second, &prepared, &error));
    TEST_ASSERT_EQUAL_PTR(
        first_snapshot,
        gui_session_client_snapshot(&client));
    tp_session *retired =
        gui_session_client_commit_prepared(
            &client, &prepared);
    TEST_ASSERT_EQUAL_PTR(first, retired);
    TEST_ASSERT_NOT_EQUAL(
        first_snapshot, gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        2U, gui_session_client_instance_generation(&client));
    tp_session_destroy(retired);
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
    gui_session_client_prepared prepared = {0};
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_session_client_prepare(
            &client, second, &prepared, &error));
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

void test_submit_generates_full_id_and_publishes_common_echo(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    test_rng_state rng_state = {.next = 0x10U};
    const tp_rng rng = {test_rng_fill, &rng_state};
    gui_session_client__test_set_transaction_rng(&client, rng);
    tp_operation operation =
        make_rename_operation(session, "submitted");
    const gui_session_submit_identity identity = {
        .origin_view_id = {{1U}},
        .draft_instance_id = {{2U}},
    };
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = identity,
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.transaction.committed);
    TEST_ASSERT_FALSE(result.transaction.no_change);
    TEST_ASSERT_FALSE(result.observation_pending);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.observation_status);
    TEST_ASSERT_EQUAL_STRING(
        "101112131415161718191a1b1c1d1e1f",
        result.transaction.transaction_id);
    const tp_session_observation *observation =
        gui_session_client_observation(&client);
    TEST_ASSERT_EQUAL_size_t(
        1U, tp_session_observation_event_count(observation));
    const tp_session_event *event =
        tp_session_observation_event_at(observation, 0U);
    TEST_ASSERT_EQUAL_STRING("human", event->author);
    TEST_ASSERT_EQUAL_STRING("atlas.rename", event->label);
    TEST_ASSERT_EQUAL_STRING(
        result.transaction.transaction_id,
        event->transaction_id);
    gui_session_submit_terminal pending = {0};
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, result.transaction.transaction_id,
        identity, &pending));
    TEST_ASSERT_TRUE(pending.committed);
    TEST_ASSERT_EQUAL_INT64(1, pending.revision);
    TEST_ASSERT_EQUAL_INT(
        GUI_SESSION_SUBMIT_ECHO_OBSERVED,
        pending.echo_state);
    gui_session_submit_result_destroy(&result);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_submit_no_change_is_terminal_without_event(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    tp_operation operation = make_rename_operation(
        session, tp_session_snapshot_atlas_at(snapshot, 0)->name);
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error));
    TEST_ASSERT_FALSE(result.transaction.committed);
    TEST_ASSERT_TRUE(result.transaction.no_change);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));
    TEST_ASSERT_EQUAL_size_t(
        0U, tp_session_observation_event_count(
                gui_session_client_observation(&client)));
    gui_session_submit_result_destroy(&result);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_submit_rejects_pinned_frame_before_rng_or_admission(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    test_rng_state rng_state = {.next = 1U};
    gui_session_client__test_set_transaction_rng(
        &client, (tp_rng){test_rng_fill, &rng_state});
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_frame_begin(&client, &error));
    tp_operation operation =
        make_rename_operation(session, "forbidden");
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_submit(
            &client, &request, &result, &error));
    TEST_ASSERT_EQUAL_INT(0, rng_state.calls);
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));
    gui_session_client_frame_end(&client);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_submit_rng_failure_is_structured_and_non_mutating(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    test_rng_state rng_state = {.fail = true};
    gui_session_client__test_set_transaction_rng(
        &client, (tp_rng){test_rng_fill, &rng_state});
    tp_operation operation = make_rename_operation(session, "rng-fail");
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_RNG_FAILED,
        gui_session_client_submit(
            &client, &request, &result, &error));
    TEST_ASSERT_EQUAL_INT64(0, tp_session_revision(session));
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_retained_id_retry_commits_once_and_returns_duplicate_result(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    tp_operation operation = make_rename_operation(session, "once");
    gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = {
            .origin_view_id = {{7U}},
            .draft_instance_id = {{8U}},
        },
        .retained_transaction_id =
            "77777777777777777777777777777777",
    };
    gui_session_submit_result first = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &first, &error));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    gui_session_submit_result duplicate = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &duplicate, &error));
    TEST_ASSERT_TRUE(duplicate.transaction.committed);
    TEST_ASSERT_EQUAL_INT64(1, duplicate.transaction.revision);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    TEST_ASSERT_EQUAL_INT(1, tp_session_history_count(session));
    gui_session_submit_result_destroy(&first);
    gui_session_submit_result_destroy(&duplicate);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_retained_id_retry_recovers_after_local_result_copy_failure(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    tp_operation operation =
        make_rename_operation(session, "copy-retry");
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = {
            .origin_view_id = {{9U}},
            .draft_instance_id = {{10U}},
        },
        .retained_transaction_id =
            "78787878787878787878787878787878",
    };

    gui_session_client__test_fail_next_result_copy();
    gui_session_submit_result first = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &first, &error));
    TEST_ASSERT_TRUE(first.transaction.committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    gui_session_submit_result_destroy(&first);

    gui_session_submit_result retry = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_DUPLICATE_ID,
        gui_session_client_submit(
            &client, &request, &retry, &error));
    TEST_ASSERT_FALSE(retry.transaction.committed);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_DUPLICATE_ID,
        retry.terminal_status);
    TEST_ASSERT_EQUAL_INT64(1, retry.transaction.revision);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    TEST_ASSERT_EQUAL_INT(1, tp_session_history_count(session));

    tp_operation later_operation =
        make_rename_operation(session, "after-copy-retry");
    const gui_session_submit_request later_request = {
        .operations = &later_operation,
        .operation_count = 1,
        .expected_revision = 1,
        .semantic_label = "atlas.rename",
        .retained_transaction_id =
            "79797979797979797979797979797979",
    };
    gui_session_submit_result later = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &later_request, &later,
            &error));
    TEST_ASSERT_EQUAL_INT64(
        2, tp_session_revision(session));

    gui_session_submit_result retry_after_revision = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_DUPLICATE_ID,
        gui_session_client_submit(
            &client, &request,
            &retry_after_revision, &error));
    TEST_ASSERT_EQUAL_INT64(
        2, retry_after_revision.transaction.revision);
    TEST_ASSERT_EQUAL_INT64(
        2, tp_session_revision(session));

    gui_session_submit_result_destroy(
        &retry_after_revision);
    gui_session_submit_result_destroy(&later);
    tp_operation_free(&later_operation);
    gui_session_submit_result_destroy(&retry);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_retained_id_requires_exact_view_and_draft_identity(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    tp_operation operation =
        make_rename_operation(session, "identity-owner");
    const gui_session_submit_identity owner = {
        .origin_view_id = {{0x21U}},
        .draft_instance_id = {{0x31U}},
    };
    gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = owner,
        .retained_transaction_id =
            "89898989898989898989898989898989",
    };
    gui_session_submit_result accepted = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &accepted, &error));

    gui_session_submit_terminal terminal = {0};
    gui_session_submit_identity wrong_view = owner;
    wrong_view.origin_view_id.bytes[0]++;
    TEST_ASSERT_FALSE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        wrong_view, &terminal));
    gui_session_submit_result rejected = {0};
    request.identity = wrong_view;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_submit(
            &client, &request, &rejected, &error));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));

    gui_session_submit_identity wrong_draft = owner;
    wrong_draft.draft_instance_id.bytes[0]++;
    TEST_ASSERT_FALSE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        wrong_draft, &terminal));
    request.identity = wrong_draft;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_submit(
            &client, &request, &rejected, &error));
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        owner, &terminal));
    TEST_ASSERT_TRUE(terminal.committed);

    gui_session_submit_result_destroy(&accepted);
    gui_session_submit_result_destroy(&rejected);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_successful_reattach_clears_receipts_but_failed_reattach_preserves_them(void) {
    tp_session *first = make_session();
    tp_session *second = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, first, &error));
    tp_operation operation =
        make_rename_operation(first, "first-session");
    const gui_session_submit_identity identity = {
        .origin_view_id = {{0x61U}},
        .draft_instance_id = {{0x71U}},
    };
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = identity,
        .retained_transaction_id =
            "abababababababababababababababab",
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error));
    gui_session_submit_terminal terminal = {0};
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        identity, &terminal));

    gui_session_client_prepared prepared = {0};
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_session_client_prepare(
            &client, second, &prepared, &error));
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        identity, &terminal));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_prepare(
            &client, second, &prepared, &error));
    tp_session *retired =
        gui_session_client_commit_prepared(
            &client, &prepared);
    TEST_ASSERT_EQUAL_PTR(first, retired);
    TEST_ASSERT_FALSE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        identity, &terminal));
    TEST_ASSERT_FALSE(
        gui_session_client_last_submit(&client, &terminal));

    gui_session_submit_result_destroy(&result);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(retired);
    tp_session_destroy(second);
}

void test_pending_receipt_waits_for_exact_echo_and_resync_reconciles_it(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    tp_operation operation =
        make_rename_operation(session, "echo-pending");
    const gui_session_submit_identity identity = {
        .origin_view_id = {{0x81U}},
        .draft_instance_id = {{0x91U}},
    };
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .identity = identity,
        .retained_transaction_id =
            "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd",
    };
    gui_session_submit_result result = {0};
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error));
    gui_session_submit_terminal terminal = {0};
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        identity, &terminal));
    TEST_ASSERT_EQUAL_INT(
        GUI_SESSION_SUBMIT_ECHO_PENDING,
        terminal.echo_state);

    for (int index = 0; index < 70; ++index) {
        char name[64];
        char transaction_id[33];
        (void)snprintf(name, sizeof name, "resync-%d", index);
        (void)snprintf(
            transaction_id, sizeof transaction_id,
            "%032x", index + 0x100);
        rename_first_atlas(session, name, transaction_id);
    }
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_TRUE(tp_session_observation_resync_required(
        gui_session_client_observation(&client)));
    TEST_ASSERT_TRUE(gui_session_client_pending_submit_query(
        &client, request.retained_transaction_id,
        identity, &terminal));
    TEST_ASSERT_EQUAL_INT(
        GUI_SESSION_SUBMIT_ECHO_OBSERVED,
        terminal.echo_state);

    gui_session_submit_result_destroy(&result);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_unobserved_receipt_capacity_rejects_before_core_mutation(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    for (size_t index = 0U;
         index < GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS;
         ++index) {
        char name[64];
        char transaction_id[33];
        (void)snprintf(name, sizeof name, "pending-%zu", index);
        (void)snprintf(
            transaction_id, sizeof transaction_id,
            "%032zx", index + 1U);
        tp_operation operation =
            make_rename_operation(session, name);
        const gui_session_submit_request request = {
            .operations = &operation,
            .operation_count = 1,
            .expected_revision =
                tp_session_revision(session),
            .semantic_label = "atlas.rename",
            .retained_transaction_id = transaction_id,
        };
        gui_session_submit_result result = {0};
        gui_session_client__test_fail_next_observe();
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_session_client_submit(
                &client, &request, &result, &error));
        gui_session_submit_result_destroy(&result);
        tp_operation_free(&operation);
    }
    const int64_t revision_before =
        tp_session_revision(session);
    tp_operation overflow =
        make_rename_operation(session, "must-not-commit");
    const gui_session_submit_request overflow_request = {
        .operations = &overflow,
        .operation_count = 1,
        .expected_revision = revision_before,
        .semantic_label = "atlas.rename",
        .retained_transaction_id =
            "ffffffffffffffffffffffffffffffff",
    };
    gui_session_submit_result overflow_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        gui_session_client_submit(
            &client, &overflow_request,
            &overflow_result, &error));
    TEST_ASSERT_EQUAL_INT64(
        revision_before, tp_session_revision(session));
    gui_session_submit_result_destroy(&overflow_result);
    tp_operation_free(&overflow);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_generated_id_collision_regenerates_instead_of_replaying(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    collision_rng_state rng_state = {0};
    gui_session_client__test_set_transaction_rng(
        &client,
        (tp_rng){collision_rng_fill, &rng_state});
    tp_operation first =
        make_rename_operation(session, "collision-first");
    gui_session_submit_request request = {
        .operations = &first,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
    };
    gui_session_submit_result first_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &first_result, &error));
    tp_operation_free(&first);

    tp_operation second =
        make_rename_operation(session, "collision-second");
    request.operations = &second;
    request.expected_revision = 1;
    gui_session_submit_result second_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &second_result, &error));
    TEST_ASSERT_EQUAL_INT(3, rng_state.calls);
    TEST_ASSERT_EQUAL_INT64(2, tp_session_revision(session));
    TEST_ASSERT_NOT_EQUAL(
        0, strcmp(
               first_result.transaction.transaction_id,
               second_result.transaction.transaction_id));

    gui_session_submit_result_destroy(&first_result);
    gui_session_submit_result_destroy(&second_result);
    tp_operation_free(&second);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_retained_retry_replays_complete_committed_and_rejected_results(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    tp_operation rename =
        make_rename_operation(session, "complete-retry");
    gui_session_submit_request request = {
        .operations = &rename,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .retained_transaction_id =
            "dededededededededededededededede",
    };
    gui_session_submit_result committed = {0};
    gui_session_submit_result committed_retry = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &committed, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &committed_retry, &error));
    TEST_ASSERT_EQUAL_INT(
        committed.transaction.op_count,
        committed_retry.transaction.op_count);
    TEST_ASSERT_EQUAL_STRING(
        committed.transaction.ops[0].wire,
        committed_retry.transaction.ops[0].wire);
    TEST_ASSERT_EQUAL_INT(
        committed.transaction.ops[0].addr_count,
        committed_retry.transaction.ops[0].addr_count);

    tp_operation rejected = {0};
    rejected.kind = TP_OP_ATLAS_SETTINGS_SET;
    rejected.atlas_id = first_atlas_id(session);
    rejected.u.atlas_settings.mask = TP_AF_PADDING;
    rejected.u.atlas_settings.padding = -1;
    request.operations = &rejected;
    request.expected_revision = 1;
    request.retained_transaction_id =
        "efefefefefefefefefefefefefefefef";
    gui_session_submit_result rejection = {0};
    gui_session_submit_result rejection_retry = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_RANGE,
        gui_session_client_submit(
            &client, &request, &rejection, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_RANGE,
        gui_session_client_submit(
            &client, &request, &rejection_retry, &error));
    TEST_ASSERT_EQUAL_INT(
        rejection.transaction.error_count,
        rejection_retry.transaction.error_count);
    TEST_ASSERT_EQUAL_STRING(
        rejection.transaction.errors[0].field,
        rejection_retry.transaction.errors[0].field);
    TEST_ASSERT_EQUAL_STRING(
        rejection.transaction.errors[0].message,
        rejection_retry.transaction.errors[0].message);

    gui_session_submit_result_destroy(&committed);
    gui_session_submit_result_destroy(&committed_retry);
    gui_session_submit_result_destroy(&rejection);
    gui_session_submit_result_destroy(&rejection_retry);
    tp_operation_free(&rename);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_submit_100_operations_is_one_revision_event_and_undo_step(void) {
    enum { OPERATION_COUNT = 100 };
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_id128 atlas_id = first_atlas_id(session);
    tp_operation operations[OPERATION_COUNT] = {0};
    for (int index = 0; index < OPERATION_COUNT; ++index) {
        char name[64];
        (void)snprintf(
            name, sizeof name, "batch-name-%03d", index);
        operations[index] =
            make_rename_operation(session, name);
        operations[index].atlas_id = atlas_id;
    }
    const gui_session_submit_request request = {
        .operations = operations,
        .operation_count = OPERATION_COUNT,
        .expected_revision = 0,
        .semantic_label = "atlas.rename.batch",
        .identity = {
            .origin_view_id = {{0x41U}},
            .draft_instance_id = {{0x51U}},
        },
    };
    gui_session_submit_result result = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error),
        error.msg);
    TEST_ASSERT_TRUE(result.transaction.committed);
    TEST_ASSERT_EQUAL_INT(
        OPERATION_COUNT, result.transaction.op_count);
    TEST_ASSERT_EQUAL_INT64(1, tp_session_revision(session));
    TEST_ASSERT_EQUAL_UINT64(
        1U, tp_session_event_sequence(session));
    TEST_ASSERT_EQUAL_INT(1, tp_session_history_count(session));
    TEST_ASSERT_TRUE(tp_session_can_undo(session));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_session_undo(session, &error));
    TEST_ASSERT_FALSE(tp_session_can_undo(session));
    TEST_ASSERT_EQUAL_INT(1, tp_session_history_count(session));
    tp_session_snapshot *undone = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(
            session, &undone, &error));
    TEST_ASSERT_EQUAL_STRING(
        "atlas1",
        tp_session_snapshot_atlas_by_id(
            undone, atlas_id)->name);
    tp_session_snapshot_destroy(undone);

    gui_session_submit_result_destroy(&result);
    for (int index = 0; index < OPERATION_COUNT; ++index) {
        tp_operation_free(&operations[index]);
    }
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_committed_submit_survives_observation_failure_and_retries_echo(void) {
    tp_session *session = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(&client, session, &error));
    const tp_session_snapshot *before =
        gui_session_client_snapshot(&client);
    tp_operation operation =
        make_rename_operation(session, "echo-retry");
    const gui_session_submit_request request = {
        .operations = &operation,
        .operation_count = 1,
        .expected_revision = 0,
        .semantic_label = "atlas.rename",
        .retained_transaction_id =
            "edededededededededededededededed",
    };
    gui_session_submit_result result = {0};
    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &result, &error));
    TEST_ASSERT_TRUE(result.transaction.committed);
    TEST_ASSERT_TRUE(result.observation_pending);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, result.observation_status);
    TEST_ASSERT_EQUAL_PTR(
        before, gui_session_client_snapshot(&client));

    gui_session_submit_result retry = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_submit(
            &client, &request, &retry, &error));
    TEST_ASSERT_TRUE(retry.transaction.committed);
    TEST_ASSERT_TRUE(retry.observation_pending);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM, retry.observation_status);
    TEST_ASSERT_EQUAL_INT64(
        1, tp_session_revision(session));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_observe(&client, &error));
    TEST_ASSERT_NOT_EQUAL(
        before, gui_session_client_snapshot(&client));
    gui_session_submit_result_destroy(&result);
    gui_session_submit_result_destroy(&retry);
    tp_operation_free(&operation);
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_prepare_failure_preserves_attached_session_observation_and_generation(void) {
    tp_session *old_session = make_session();
    tp_session *candidate = make_session();
    gui_session_client client;
    gui_session_client_prepared prepared = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(
            &client, old_session, &error));
    const tp_session_snapshot *old_snapshot =
        gui_session_client_snapshot(&client);
    const uint64_t old_generation =
        gui_session_client_instance_generation(
            &client);

    gui_session_client__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_session_client_prepare(
            &client, candidate, &prepared,
            &error));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(
            &client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_session_client_instance_generation(
            &client));
    TEST_ASSERT_FALSE(prepared.ready);

    gui_session_client_detach(&client);
    tp_session_destroy(old_session);
    tp_session_destroy(candidate);
}

void test_prepare_rejects_active_session_alias_without_state_change(void) {
    tp_session *session = make_session();
    gui_session_client client;
    gui_session_client_prepared prepared =
        {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(
            &client, session, &error));
    const tp_session_snapshot *snapshot =
        gui_session_client_snapshot(&client);
    const uint64_t generation =
        gui_session_client_instance_generation(
            &client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_prepare(
            &client, session, &prepared,
            &error));
    TEST_ASSERT_FALSE(prepared.ready);
    TEST_ASSERT_EQUAL_PTR(
        session,
        gui_session_client_attached_session(
            &client));
    TEST_ASSERT_EQUAL_PTR(
        snapshot,
        gui_session_client_snapshot(
            &client));
    TEST_ASSERT_EQUAL_UINT64(
        generation,
        gui_session_client_instance_generation(
            &client));
    gui_session_client_detach(&client);
    tp_session_destroy(session);
}

void test_prepare_is_invisible_until_nonfallible_commit(void) {
    tp_session *old_session = make_session();
    tp_session *candidate = make_session();
    gui_session_client client;
    gui_session_client_prepared prepared = {0};
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(
            &client, old_session, &error));
    const tp_session_snapshot *old_snapshot =
        gui_session_client_snapshot(&client);
    const uint64_t old_generation =
        gui_session_client_instance_generation(
            &client);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_prepare(
            &client, candidate, &prepared,
            &error));
    TEST_ASSERT_TRUE(prepared.ready);
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(
            &client));
    TEST_ASSERT_EQUAL_PTR(
        old_snapshot,
        gui_session_client_snapshot(&client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_session_client_instance_generation(
            &client));

    tp_session *retired =
        gui_session_client_commit_prepared(
            &client, &prepared);
    TEST_ASSERT_EQUAL_PTR(old_session, retired);
    TEST_ASSERT_FALSE(prepared.ready);
    TEST_ASSERT_EQUAL_PTR(
        candidate,
        gui_session_client_attached_session(
            &client));
    TEST_ASSERT_EQUAL_UINT64(
        old_generation + 1U,
        gui_session_client_instance_generation(
            &client));
    TEST_ASSERT_NOT_EQUAL(
        old_snapshot,
        gui_session_client_snapshot(&client));

    gui_session_client_detach(&client);
    tp_session_destroy(retired);
    tp_session_destroy(candidate);
}

void test_attach_is_initial_empty_only(void) {
    tp_session *old_session = make_session();
    tp_session *candidate = make_session();
    gui_session_client client;
    tp_error error = {{0}};
    gui_session_client_init(&client);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_client_attach(
            &client, old_session, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_session_client_attach(
            &client, candidate, &error));
    TEST_ASSERT_EQUAL_PTR(
        old_session,
        gui_session_client_attached_session(
            &client));

    gui_session_client_detach(&client);
    tp_session_destroy(old_session);
    tp_session_destroy(candidate);
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
    RUN_TEST(
        test_submit_generates_full_id_and_publishes_common_echo);
    RUN_TEST(test_submit_no_change_is_terminal_without_event);
    RUN_TEST(
        test_submit_rejects_pinned_frame_before_rng_or_admission);
    RUN_TEST(
        test_submit_rng_failure_is_structured_and_non_mutating);
    RUN_TEST(
        test_retained_id_retry_commits_once_and_returns_duplicate_result);
    RUN_TEST(
        test_retained_id_retry_recovers_after_local_result_copy_failure);
    RUN_TEST(
        test_retained_id_requires_exact_view_and_draft_identity);
    RUN_TEST(
        test_successful_reattach_clears_receipts_but_failed_reattach_preserves_them);
    RUN_TEST(
        test_pending_receipt_waits_for_exact_echo_and_resync_reconciles_it);
    RUN_TEST(
        test_unobserved_receipt_capacity_rejects_before_core_mutation);
    RUN_TEST(
        test_generated_id_collision_regenerates_instead_of_replaying);
    RUN_TEST(
        test_retained_retry_replays_complete_committed_and_rejected_results);
    RUN_TEST(
        test_submit_100_operations_is_one_revision_event_and_undo_step);
    RUN_TEST(
        test_committed_submit_survives_observation_failure_and_retries_echo);
    RUN_TEST(
        test_prepare_failure_preserves_attached_session_observation_and_generation);
    RUN_TEST(
        test_prepare_rejects_active_session_alias_without_state_change);
    RUN_TEST(
        test_prepare_is_invisible_until_nonfallible_commit);
    RUN_TEST(test_attach_is_initial_empty_only);
    return UNITY_END();
}
