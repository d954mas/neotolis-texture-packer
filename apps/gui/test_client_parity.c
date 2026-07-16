#include "gui_session_adapter.h"

#include <stdio.h>
#include <string.h>

#include "tp_core/tp_client_capability.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

typedef enum corpus_adapter {
    CORPUS_GUI,
    CORPUS_HEADLESS
} corpus_adapter;

typedef struct corpus_result {
    tp_status success;
    tp_status validation;
    tp_status conflict;
    tp_status duplicate;
    int64_t revision;
    uint64_t model_generation;
    uint64_t event_sequence;
    tp_id128 semantic_identity;
    char atlas_name[64];
    tp_session_event events[4];
    size_t event_count;
    bool event_resync;
} corpus_result;

typedef struct invalid_corpus_result {
    tp_status empty_atlas_name;
    tp_status empty_animation_name;
    tp_status empty_source_path;
    tp_status invalid_source_kind;
    int64_t revision;
    uint64_t event_sequence;
    char messages[4][256];
} invalid_corpus_result;

static int deterministic_fill(void *ctx, uint8_t *dst, size_t count) {
    uint8_t *value = (uint8_t *)ctx;
    for (size_t i = 0; i < count; i++) {
        dst[i] = (*value)++;
    }
    return (int)count;
}

void setUp(void) {}
void tearDown(void) {}

static tp_status headless_apply(tp_session *session, tp_operation *operation,
                                int64_t expected_revision, const char *transaction_id,
                                tp_error *err) {
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s", transaction_id);
    request.expected_revision = expected_revision;
    request.ops = operation;
    request.op_count = 1;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    const tp_status status = tp_session_apply(session, &request, &result, err);
    if (status != TP_STATUS_OK && result.error_count > 0 &&
        result.errors[0].message[0] != '\0') {
        (void)tp_error_set(err, status, "%s", result.errors[0].message);
    }
    tp_txn_result_free(&result);
    return status;
}

static tp_status apply_rename(corpus_adapter adapter, tp_session *session,
                              tp_id128 atlas_id, int64_t expected_revision,
                              const char *name, const char *transaction_id,
                              tp_error *err) {
    if (adapter == CORPUS_GUI) {
        return gui_session_rename_atlas(session, atlas_id, expected_revision,
                                        name, transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas_id;
    operation.u.atlas_rename.name = (char *)name;
    return headless_apply(session, &operation, expected_revision,
                          transaction_id, err);
}

static tp_status apply_padding(corpus_adapter adapter, tp_session *session,
                               tp_id128 atlas_id, int64_t expected_revision,
                               int padding, const char *transaction_id,
                               tp_error *err) {
    tp_op_atlas_settings settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_AF_PADDING;
    settings.padding = padding;
    if (adapter == CORPUS_GUI) {
        return gui_session_set_atlas_settings(session, atlas_id,
                                              expected_revision, &settings,
                                              transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_SETTINGS_SET;
    operation.atlas_id = atlas_id;
    operation.u.atlas_settings = settings;
    return headless_apply(session, &operation, expected_revision,
                          transaction_id, err);
}

static corpus_result run_corpus(corpus_adapter adapter) {
    uint8_t seed = 17U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));

    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &initial, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(initial, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    tp_session_snapshot_destroy(initial);

    corpus_result out;
    memset(&out, 0, sizeof out);
    out.success = apply_rename(adapter, session, atlas_id, 0, "golden",
                               "11111111111111111111111111111111", &err);
    out.validation = apply_padding(adapter, session, atlas_id, 1, -1,
                                   "22222222222222222222222222222222", &err);
    out.conflict = apply_rename(adapter, session, atlas_id, 0, "stale",
                                "33333333333333333333333333333333", &err);
    out.duplicate = apply_rename(adapter, session, atlas_id, 0, "retry",
                                 "11111111111111111111111111111111", &err);

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *final_atlas =
        tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(final_atlas);
    (void)snprintf(out.atlas_name, sizeof out.atlas_name, "%s", final_atlas->name);
    out.revision = tp_session_snapshot_revision(snapshot);
    out.model_generation = tp_session_snapshot_model_generation(snapshot);
    out.event_sequence = tp_session_snapshot_event_sequence(snapshot);
    out.semantic_identity = tp_session_snapshot_semantic_identity(snapshot);
    tp_session_snapshot_destroy(snapshot);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_events_after(session, 0, out.events,
                                                  sizeof out.events / sizeof out.events[0],
                                                  &out.event_count, &out.event_resync, &err));
    tp_session_destroy(session);
    return out;
}

static invalid_corpus_result run_invalid_corpus(corpus_adapter adapter) {
    uint8_t seed = 37U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));
    tp_session_snapshot *initial = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &initial, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(initial, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    tp_session_snapshot_destroy(initial);

    const tp_id128 animation_id = {{
        0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41,
        0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42, 0x42}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_session_create_animation(session, atlas_id, animation_id, 0,
                                     "walk", NULL, 0,
                                     "41414141414141414141414141414141", &err));

    invalid_corpus_result out;
    memset(&out, 0, sizeof out);
    err = (tp_error){{0}};
    out.empty_atlas_name = apply_rename(
        adapter, session, atlas_id, 1, "",
        "42424242424242424242424242424242", &err);
    (void)snprintf(out.messages[0], sizeof out.messages[0], "%s", err.msg);

    err = (tp_error){{0}};
    if (adapter == CORPUS_GUI) {
        out.empty_animation_name = gui_session_rename_animation(
            session, atlas_id, animation_id, 1, "",
            "43434343434343434343434343434343", &err);
    } else {
        tp_operation operation;
        memset(&operation, 0, sizeof operation);
        operation.kind = TP_OP_ANIMATION_RENAME;
        operation.atlas_id = atlas_id;
        operation.u.anim_rename.anim_id = animation_id;
        operation.u.anim_rename.name = "";
        out.empty_animation_name = headless_apply(
            session, &operation, 1,
            "43434343434343434343434343434343", &err);
    }
    (void)snprintf(out.messages[1], sizeof out.messages[1], "%s", err.msg);

    const tp_id128 source_id = {{
        0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51, 0x51,
        0x52, 0x52, 0x52, 0x52, 0x52, 0x52, 0x52, 0x52}};
    const tp_id128 source_ids[1] = {source_id};
    const char *empty_paths[1] = {""};
    err = (tp_error){{0}};
    if (adapter == CORPUS_GUI) {
        out.empty_source_path = gui_session_add_sources(
            session, atlas_id, source_ids, empty_paths, 1,
            TP_SNAPSHOT_SOURCE_FILE, 1,
            "44444444444444444444444444444444", &err);
    } else {
        tp_operation operation;
        memset(&operation, 0, sizeof operation);
        operation.kind = TP_OP_SOURCE_ADD;
        operation.atlas_id = atlas_id;
        operation.u.source_add.source_id = source_id;
        operation.u.source_add.kind = TP_SOURCE_KIND_FILE;
        operation.u.source_add.key = "";
        out.empty_source_path = headless_apply(
            session, &operation, 1,
            "44444444444444444444444444444444", &err);
    }
    (void)snprintf(out.messages[2], sizeof out.messages[2], "%s", err.msg);

    const char *valid_paths[1] = {"sprites/coin.png"};
    err = (tp_error){{0}};
    if (adapter == CORPUS_GUI) {
        out.invalid_source_kind = gui_session_add_sources(
            session, atlas_id, source_ids, valid_paths, 1,
            (tp_snapshot_source_kind)99, 1,
            "45454545454545454545454545454545", &err);
    } else {
        tp_operation operation;
        memset(&operation, 0, sizeof operation);
        operation.kind = TP_OP_SOURCE_ADD;
        operation.atlas_id = atlas_id;
        operation.u.source_add.source_id = source_id;
        operation.u.source_add.kind = (tp_source_kind)99;
        operation.u.source_add.key = "sprites/coin.png";
        out.invalid_source_kind = headless_apply(
            session, &operation, 1,
            "45454545454545454545454545454545", &err);
    }
    (void)snprintf(out.messages[3], sizeof out.messages[3], "%s", err.msg);

    tp_session_snapshot *after = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &after, &err));
    out.revision = tp_session_snapshot_revision(after);
    out.event_sequence = tp_session_snapshot_event_sequence(after);
    tp_session_snapshot_destroy(after);
    tp_session_destroy(session);
    return out;
}

void test_capability_matrix_is_typed_and_exact(void) {
    static const tp_client_capability_availability expected[3][6] = {
        {TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
         TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE},
        {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE},
        {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE}
    };
    for (int client = TP_CLIENT_FILE_CLI; client <= TP_CLIENT_LIVE_HEADLESS; client++) {
        for (int capability = TP_CLIENT_CAPABILITY_TRANSACTION;
             capability <= TP_CLIENT_CAPABILITY_LIVE_JOBS; capability++) {
            tp_client_capability_result result;
            memset(&result, 0, sizeof result);
            const tp_status status = tp_client_capability_query(
                (tp_client_kind)client, (tp_client_capability)capability, &result);
            const tp_client_capability_availability want =
                expected[client - TP_CLIENT_FILE_CLI]
                        [capability - TP_CLIENT_CAPABILITY_TRANSACTION];
            TEST_ASSERT_EQUAL_INT(client, result.client);
            TEST_ASSERT_EQUAL_INT(capability, result.capability);
            TEST_ASSERT_EQUAL_INT(want, result.availability);
            TEST_ASSERT_EQUAL_INT(want == TP_CLIENT_CAPABILITY_AVAILABLE
                                      ? TP_STATUS_OK
                                      : TP_STATUS_UNSUPPORTED_CAPABILITY,
                                  status);
        }
    }
    TEST_ASSERT_EQUAL_STRING("available", tp_client_capability_availability_id(
                                              TP_CLIENT_CAPABILITY_AVAILABLE));
    TEST_ASSERT_EQUAL_STRING("not_applicable", tp_client_capability_availability_id(
                                                  TP_CLIENT_CAPABILITY_NOT_APPLICABLE));
    TEST_ASSERT_EQUAL_STRING("not_implemented", tp_client_capability_availability_id(
                                                   TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED));
    TEST_ASSERT_EQUAL_STRING("unknown_availability",
                             tp_client_capability_availability_id(
                                 (tp_client_capability_availability)99));

    tp_client_capability_result invalid;
    memset(&invalid, 0, sizeof invalid);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_client_capability_query((tp_client_kind)0,
                                                     TP_CLIENT_CAPABILITY_EVENTS,
                                                     &invalid));
}

void test_live_headless_job_capability_has_snapshot_owned_input_seam(void) {
    uint8_t seed = 71U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));
    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_pack_settings settings;
    memset(&settings, 0, sizeof settings);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_pack_settings_build_snapshot(snapshot, atlas->id,
                                                          &settings, &err));
    tp_pack_input input;
    memset(&input, 0, sizeof input);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_pack_input_build_snapshot(snapshot, atlas->id,
                                                       &input, &err));
    TEST_ASSERT_EQUAL_INT(0, input.count);
    tp_pack_input_free(&input);
    tp_session_snapshot_destroy(snapshot);
    tp_session_destroy(session);
}

void test_live_headless_runs_real_session_owned_pack_job(void) {
    uint8_t seed = 91U;
    const tp_rng rng = {deterministic_fill, &seed};
    tp_error err = {{0}};
    tp_session *session = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create_default_project(&rng, &session, &err));

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    tp_session_snapshot_destroy(snapshot);

    char source_path[1024];
    (void)snprintf(source_path, sizeof source_path,
                   "%s/apps/cli/testdata/sprites/coin.png",
                   TP_TEST_SOURCE_DIR);
    tp_operation source;
    memset(&source, 0, sizeof source);
    source.kind = TP_OP_SOURCE_ADD;
    source.atlas_id = atlas_id;
    source.u.source_add.kind = TP_SOURCE_KIND_FILE;
    source.u.source_add.source_id = (tp_id128){{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    source.u.source_add.key = source_path;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          headless_apply(session, &source, 0,
                                         "91919191919191919191919191919191",
                                         &err));

    tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = ".",
        .preview_exporter_id = NULL,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    TEST_ASSERT_TRUE(tp_session_job_active(session));

    tp_session_job_progress progress;
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, progress.kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, progress.state);
    TEST_ASSERT_EQUAL_INT(1, progress.current);
    TEST_ASSERT_EQUAL_INT(1, progress.total);

    tp_session_job_result result;
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_PACK, result.kind);
    TEST_ASSERT_NOT_NULL(result.pack.result);
    TEST_ASSERT_EQUAL_INT(1, result.pack.result->sprite_count);
    TEST_ASSERT_FALSE(tp_session_job_active(session));
    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
}

void test_gui_and_headless_share_golden_transaction_session_corpus(void) {
    const corpus_result gui = run_corpus(CORPUS_GUI);
    const corpus_result headless = run_corpus(CORPUS_HEADLESS);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui.success);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, gui.validation);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_REVISION_CONFLICT, gui.conflict);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, gui.duplicate);
    TEST_ASSERT_EQUAL_INT(gui.success, headless.success);
    TEST_ASSERT_EQUAL_INT(gui.validation, headless.validation);
    TEST_ASSERT_EQUAL_INT(gui.conflict, headless.conflict);
    TEST_ASSERT_EQUAL_INT(gui.duplicate, headless.duplicate);

    TEST_ASSERT_EQUAL_INT64(gui.revision, headless.revision);
    TEST_ASSERT_EQUAL_UINT64(gui.model_generation, headless.model_generation);
    TEST_ASSERT_EQUAL_UINT64(gui.event_sequence, headless.event_sequence);
    TEST_ASSERT_EQUAL_MEMORY(&gui.semantic_identity, &headless.semantic_identity,
                             sizeof gui.semantic_identity);
    TEST_ASSERT_EQUAL_STRING(gui.atlas_name, headless.atlas_name);
    TEST_ASSERT_EQUAL_STRING("golden", gui.atlas_name);
    TEST_ASSERT_EQUAL_UINT64(1, gui.event_sequence);
    TEST_ASSERT_EQUAL_UINT(1, gui.event_count);
    TEST_ASSERT_EQUAL_UINT(gui.event_count, headless.event_count);
    TEST_ASSERT_FALSE(gui.event_resync);
    TEST_ASSERT_EQUAL_INT(gui.event_resync, headless.event_resync);
    TEST_ASSERT_EQUAL_MEMORY(gui.events, headless.events,
                             gui.event_count * sizeof gui.events[0]);
}

void test_gui_invalid_intents_are_classified_by_the_shared_core(void) {
    const invalid_corpus_result gui = run_invalid_corpus(CORPUS_GUI);
    const invalid_corpus_result headless = run_invalid_corpus(CORPUS_HEADLESS);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, gui.empty_atlas_name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, gui.empty_animation_name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, gui.empty_source_path);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, gui.invalid_source_kind);
    TEST_ASSERT_EQUAL_INT(gui.empty_atlas_name, headless.empty_atlas_name);
    TEST_ASSERT_EQUAL_INT(gui.empty_animation_name, headless.empty_animation_name);
    TEST_ASSERT_EQUAL_INT(gui.empty_source_path, headless.empty_source_path);
    TEST_ASSERT_EQUAL_INT(gui.invalid_source_kind, headless.invalid_source_kind);
    TEST_ASSERT_EQUAL_MEMORY(gui.messages, headless.messages, sizeof gui.messages);
    TEST_ASSERT_EQUAL_INT64(1, gui.revision);
    TEST_ASSERT_EQUAL_INT64(gui.revision, headless.revision);
    TEST_ASSERT_EQUAL_UINT64(1, gui.event_sequence);
    TEST_ASSERT_EQUAL_UINT64(gui.event_sequence, headless.event_sequence);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_capability_matrix_is_typed_and_exact);
    RUN_TEST(test_live_headless_job_capability_has_snapshot_owned_input_seam);
    RUN_TEST(test_live_headless_runs_real_session_owned_pack_job);
    RUN_TEST(test_gui_and_headless_share_golden_transaction_session_corpus);
    RUN_TEST(test_gui_invalid_intents_are_classified_by_the_shared_core);
    return UNITY_END();
}
