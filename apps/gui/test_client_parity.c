#include "gui_session_adapter.h"
#include "client_parity_manifest.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

#include "tp_core/tp_client_capability.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_transaction.h"
#include "tp_core/tp_build_worker.h"
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
    tp_status source_add;
    tp_status sprite_override;
    tp_status animation_create;
    tp_status target_create;
    int64_t revision;
    uint64_t model_generation;
    uint64_t event_sequence;
    tp_id128 semantic_identity;
    char atlas_name[64];
    char source_path[64];
    float sprite_origin_x;
    char animation_name[64];
    float animation_fps;
    int animation_frame_count;
    tp_id128 frame_source_id;
    char frame_source_key[64];
    char target_exporter_id[64];
    char target_out_path[64];
    bool target_enabled;
    tp_session_event events[12];
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

static void cleanup_export_fixture(void) {
    static const char *const relative_files[] = {
        "out/atlas1-0.png",
        "out/atlas1.json",
        "atlas1.h",
        "atlas1.ntpack",
        "live-export.ntpacker_project.ntpacker.lock",
        "live-export.ntpacker_project",
    };
    char path[1024];
    for (size_t i = 0U;
         i < sizeof relative_files / sizeof relative_files[0]; ++i) {
        (void)snprintf(path, sizeof path, "%s/%s", TP_TEST_BINARY_DIR,
                       relative_files[i]);
        (void)remove(path);
    }
    (void)snprintf(path, sizeof path, "%s/out", TP_TEST_BINARY_DIR);
    (void)test_rmdir(path);
    (void)test_rmdir(TP_TEST_BINARY_DIR);
}

void setUp(void) { cleanup_export_fixture(); }
void tearDown(void) { cleanup_export_fixture(); }

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

static tp_status apply_source_add(corpus_adapter adapter, tp_session *session,
                                  tp_id128 atlas_id, tp_id128 source_id,
                                  int64_t expected_revision, const char *path,
                                  const char *transaction_id, tp_error *err) {
    if (adapter == CORPUS_GUI) {
        const char *paths[1] = {path};
        return gui_session_add_sources(
            session, atlas_id, &source_id, paths, 1,
            TP_SNAPSHOT_SOURCE_FILE, expected_revision, transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SOURCE_ADD;
    operation.atlas_id = atlas_id;
    operation.u.source_add.source_id = source_id;
    operation.u.source_add.kind = TP_SOURCE_KIND_FILE;
    operation.u.source_add.key = (char *)path;
    return headless_apply(session, &operation, expected_revision,
                          transaction_id, err);
}

static tp_status apply_sprite_origin(corpus_adapter adapter,
                                     tp_session *session, tp_id128 atlas_id,
                                     tp_id128 source_id,
                                     int64_t expected_revision,
                                     const char *source_key, float origin_x,
                                     float origin_y,
                                     const char *transaction_id,
                                     tp_error *err) {
    tp_op_sprite_set settings;
    memset(&settings, 0, sizeof settings);
    settings.mask = TP_SPF_ORIGIN;
    settings.origin_x = origin_x;
    settings.origin_y = origin_y;
    if (adapter == CORPUS_GUI) {
        return gui_session_set_sprite_override(
            session, atlas_id, source_id, source_key, expected_revision,
            &settings, transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas_id;
    operation.u.sprite_set = settings;
    operation.u.sprite_set.source_id = source_id;
    operation.u.sprite_set.src_key = (char *)source_key;
    return headless_apply(session, &operation, expected_revision,
                          transaction_id, err);
}

static tp_status apply_animation_create(
    corpus_adapter adapter, tp_session *session, tp_id128 atlas_id,
    tp_id128 animation_id, tp_id128 source_id, int64_t expected_revision,
    const char *source_key, const char *transaction_id, tp_error *err) {
    tp_op_sprite_ref frame = {source_id, (char *)source_key};
    if (adapter == CORPUS_GUI) {
        return gui_session_create_animation(
            session, atlas_id, animation_id, expected_revision, "walk", &frame,
            1, transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.anim_create.anim_id = animation_id;
    operation.u.anim_create.name = "walk";
    operation.u.anim_create.fps = TP_PROJECT_ANIM_FPS_DEFAULT;
    operation.u.anim_create.playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
    operation.u.anim_create.frames = &frame;
    operation.u.anim_create.frame_count = 1;
    return headless_apply(session, &operation, expected_revision,
                          transaction_id, err);
}

static tp_status apply_target_create(corpus_adapter adapter,
                                     tp_session *session, tp_id128 atlas_id,
                                     tp_id128 target_id,
                                     int64_t expected_revision,
                                     const char *transaction_id,
                                     tp_error *err) {
    if (adapter == CORPUS_GUI) {
        return gui_session_create_target(
            session, atlas_id, target_id, expected_revision, "json-neotolis",
            "out/golden", true, transaction_id, err);
    }
    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_CREATE;
    operation.atlas_id = atlas_id;
    operation.u.target_create.target_id = target_id;
    operation.u.target_create.exporter_id = "json-neotolis";
    operation.u.target_create.out_path = "out/golden";
    operation.u.target_create.enabled = true;
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

    const tp_id128 source_id = {{
        0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61, 0x61,
        0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62, 0x62}};
    const tp_id128 animation_id = {{
        0x71, 0x71, 0x71, 0x71, 0x71, 0x71, 0x71, 0x71,
        0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72, 0x72}};
    const tp_id128 target_id = {{
        0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81,
        0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x82, 0x82}};
    static const char source_path[] = "sprites/hero.png";
    static const char source_key[] = "hero.png";

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
    out.source_add = apply_source_add(
        adapter, session, atlas_id, source_id, 1, source_path,
        "44444444444444444444444444444444", &err);
    out.sprite_override = apply_sprite_origin(
        adapter, session, atlas_id, source_id, 2, source_key, 0.25F, 0.75F,
        "66666666666666666666666666666666", &err);
    out.animation_create = apply_animation_create(
        adapter, session, atlas_id, animation_id, source_id, 3, source_key,
        "77777777777777777777777777777777", &err);
    out.target_create = apply_target_create(
        adapter, session, atlas_id, target_id, 4,
        "99999999999999999999999999999999", &err);

    tp_session_snapshot *snapshot = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_snapshot_atlas *final_atlas =
        tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(final_atlas);
    (void)snprintf(out.atlas_name, sizeof out.atlas_name, "%s", final_atlas->name);
    const tp_snapshot_source *source = tp_session_snapshot_source_by_id(
        snapshot, atlas_id, source_id);
    TEST_ASSERT_NOT_NULL(source);
    (void)snprintf(out.source_path, sizeof out.source_path, "%s", source->path);
    const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_by_key(
        snapshot, atlas_id, source_id, source_key);
    TEST_ASSERT_NOT_NULL(sprite);
    out.sprite_origin_x = sprite->origin_x;
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_by_id(snapshot, atlas_id, animation_id);
    TEST_ASSERT_NOT_NULL(animation);
    (void)snprintf(out.animation_name, sizeof out.animation_name, "%s",
                   animation->name);
    out.animation_fps = animation->fps;
    out.animation_frame_count = animation->frame_count;
    const tp_snapshot_frame *frame = tp_session_snapshot_animation_frame_at(
        snapshot, atlas_id, animation_id, 0);
    TEST_ASSERT_NOT_NULL(frame);
    out.frame_source_id = frame->source_id;
    (void)snprintf(out.frame_source_key, sizeof out.frame_source_key, "%s",
                   frame->source_key);
    const tp_snapshot_target *target = tp_session_snapshot_target_by_id(
        snapshot, atlas_id, target_id);
    TEST_ASSERT_NOT_NULL(target);
    (void)snprintf(out.target_exporter_id, sizeof out.target_exporter_id, "%s",
                   target->exporter_id);
    (void)snprintf(out.target_out_path, sizeof out.target_out_path, "%s",
                   target->out_path);
    out.target_enabled = target->enabled;
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
    static const tp_client_capability capabilities[9] = {
        TP_CLIENT_CAPABILITY_TRANSACTION,
        TP_CLIENT_CAPABILITY_PERSISTENCE,
        TP_CLIENT_CAPABILITY_EVENTS,
        TP_CLIENT_CAPABILITY_HISTORY,
        TP_CLIENT_CAPABILITY_RECOVERY,
        TP_CLIENT_CAPABILITY_PACK_JOB,
        TP_CLIENT_CAPABILITY_EXPORT_COMMAND,
        TP_CLIENT_CAPABILITY_INSPECT_ASYNC_JOB,
        TP_CLIENT_CAPABILITY_VALIDATE_ASYNC_JOB,
    };
    static const tp_client_capability_availability expected[3][9] = {
        {TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
         TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE},
        {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED,
         TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED},
        {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_AVAILABLE,
         TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED,
         TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED}
    };
    for (int client = TP_CLIENT_FILE_CLI; client <= TP_CLIENT_LIVE_HEADLESS; client++) {
        for (size_t capability_index = 0;
             capability_index < sizeof capabilities / sizeof capabilities[0];
             capability_index++) {
            const tp_client_capability capability =
                capabilities[capability_index];
            tp_client_capability_result result;
            memset(&result, 0, sizeof result);
            const tp_status status = tp_client_capability_query(
                (tp_client_kind)client, (tp_client_capability)capability, &result);
            const tp_client_capability_availability want =
                expected[client - TP_CLIENT_FILE_CLI]
                        [capability_index];
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

static bool is_named_executable_oracle(const char *oracle) {
    static const char *const known[] = {
        "cli_mutate_anim",   "cli_mutate_atlas",  "cli_mutate_set",
        "cli_mutate_source", "cli_mutate_sprite", "cli_mutate_target",
        "tp_client_parity_real",
    };
    if (!oracle || oracle[0] == '\0') {
        return false;
    }
    for (size_t i = 0U; i < sizeof known / sizeof known[0]; ++i) {
        if (strcmp(oracle, known[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *coverage_oracle(const client_parity_manifest_row *row,
                                   unsigned bit_index) {
    if (bit_index == 0U) {
        return row->cli_oracle;
    }
    if (bit_index == 1U) {
        return row->gui_oracle;
    }
    return row->dimension_oracles[bit_index];
}

void test_real_client_parity_manifest_covers_every_shipped_mutation(void) {
    TEST_ASSERT_EQUAL_UINT32((1U << CLIENT_PARITY_DIMENSION_COUNT) - 1U,
                             CLIENT_PARITY_REQUIRED_COVERAGE);
    uint32_t aggregate = 0U;
    size_t row_count = 0U;
    const client_parity_manifest_row *rows =
        client_parity_manifest_rows(&row_count);
    TEST_ASSERT_NOT_NULL(rows);
    TEST_ASSERT_EQUAL_UINT(19U, row_count);
    bool seen[TP_OP_KIND_COUNT];
    memset(seen, 0, sizeof seen);
    for (size_t i = 0U; i < row_count; ++i) {
        TEST_ASSERT_TRUE(rows[i].kind > TP_OP_INVALID);
        TEST_ASSERT_TRUE(rows[i].kind < TP_OP_KIND_COUNT);
        TEST_ASSERT_FALSE(seen[rows[i].kind]);
        seen[rows[i].kind] = true;
        TEST_ASSERT_NOT_NULL(rows[i].family);
        TEST_ASSERT_NOT_NULL(rows[i].cli_oracle);
        TEST_ASSERT_NOT_NULL(rows[i].gui_oracle);
        for (unsigned bit_index = 0U;
             bit_index < CLIENT_PARITY_DIMENSION_COUNT; ++bit_index) {
            const uint32_t bit = 1U << bit_index;
            const char *oracle = coverage_oracle(&rows[i], bit_index);
            if ((rows[i].coverage & bit) != 0U) {
                TEST_ASSERT_TRUE_MESSAGE(
                    is_named_executable_oracle(oracle),
                    "every claimed parity dimension needs a named executable oracle");
            } else {
                TEST_ASSERT_NULL_MESSAGE(
                    oracle,
                    "an unsupported parity dimension must not retain oracle evidence");
            }
        }
        aggregate |= rows[i].coverage;
    }
    for (int kind = TP_OP_ATLAS_CREATE; kind < TP_OP_KIND_COUNT; ++kind) {
        const bool reserved = kind == TP_OP_SOURCE_REPLACE ||
                              kind == TP_OP_ANIMATION_FRAMES_SET;
        TEST_ASSERT_EQUAL_INT(!reserved, seen[kind]);
    }

    const uint32_t outcome_dimensions =
        CLIENT_PARITY_ERROR | CLIENT_PARITY_NO_OP |
        CLIENT_PARITY_AMBIGUITY | CLIENT_PARITY_NOTICE |
        CLIENT_PARITY_EXIT_CODE;
    uint32_t seen_outcomes = 0U;
    size_t outcome_count = 0U;
    const client_parity_outcome_row *outcomes =
        client_parity_outcome_rows(&outcome_count);
    TEST_ASSERT_NOT_NULL(outcomes);
    TEST_ASSERT_EQUAL_UINT(5U, outcome_count);
    for (size_t i = 0U; i < outcome_count; ++i) {
        TEST_ASSERT_NOT_NULL(outcomes[i].family);
        TEST_ASSERT_TRUE((outcomes[i].dimension & outcome_dimensions) != 0U);
        TEST_ASSERT_EQUAL_UINT32(
            outcomes[i].dimension,
            outcomes[i].dimension & (0U - outcomes[i].dimension));
        TEST_ASSERT_EQUAL_UINT32(0U,
                                 seen_outcomes & outcomes[i].dimension);
        TEST_ASSERT_TRUE(outcomes[i].applicable_clients != 0U);
        TEST_ASSERT_EQUAL_UINT32(
            0U, outcomes[i].applicable_clients &
                    ~(uint32_t)(CLIENT_PARITY_REAL_CLI |
                                CLIENT_PARITY_REAL_GUI));
        if ((outcomes[i].applicable_clients & CLIENT_PARITY_REAL_CLI) != 0U) {
            TEST_ASSERT_TRUE(is_named_executable_oracle(
                outcomes[i].cli_oracle));
        } else {
            TEST_ASSERT_NULL(outcomes[i].cli_oracle);
        }
        if ((outcomes[i].applicable_clients & CLIENT_PARITY_REAL_GUI) != 0U) {
            TEST_ASSERT_TRUE(is_named_executable_oracle(
                outcomes[i].gui_oracle));
        } else {
            TEST_ASSERT_NULL(outcomes[i].gui_oracle);
        }
        seen_outcomes |= outcomes[i].dimension;
        aggregate |= outcomes[i].dimension;
    }
    TEST_ASSERT_EQUAL_UINT32(outcome_dimensions, seen_outcomes);
    TEST_ASSERT_EQUAL_UINT32(CLIENT_PARITY_REQUIRED_COVERAGE, aggregate);
}

void test_live_headless_runs_real_pack_job_and_export_command(void) {
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
    tp_mkdirs(TP_TEST_BINARY_DIR);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_session_input_token input_at_start =
        tp_session_snapshot_input_token(snapshot);
    tp_session_snapshot_destroy(snapshot);

    tp_pack_job_request request = {
        .atlas_id = atlas_id,
        .work_dir = TP_TEST_BINARY_DIR,
        .preview_exporter_id = NULL,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_pack_job_start(session, &request, &err));
    TEST_ASSERT_TRUE(tp_session_job_active(session));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_invalidate_sources(session, &err));

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
    TEST_ASSERT_EQUAL_UINT64(input_at_start.model_generation,
                             result.pack.input_token_at_start.model_generation);
    TEST_ASSERT_EQUAL_UINT64(input_at_start.source_generation,
                             result.pack.input_token_at_start.source_generation);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_snapshot_create(session, &snapshot, &err));
    const tp_session_input_token current_input =
        tp_session_snapshot_input_token(snapshot);
    TEST_ASSERT_EQUAL_UINT64(input_at_start.model_generation,
                             current_input.model_generation);
    TEST_ASSERT_EQUAL_UINT64(input_at_start.source_generation + 1U,
                             current_input.source_generation);
    tp_session_snapshot_destroy(snapshot);
    TEST_ASSERT_FALSE(tp_session_job_active(session));
    tp_session_job_result_destroy(&result);

    char project_path[1024];
    (void)snprintf(project_path, sizeof project_path,
                   "%s/live-export.ntpacker_project", TP_TEST_BINARY_DIR);
    tp_session_save_result save_result;
    memset(&save_result, 0, sizeof save_result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_save_as(session, project_path,
                                             &save_result, &err));
    const tp_export_command_request export_request = {
        .work_dir = TP_TEST_BINARY_DIR,
        .atlas_id = atlas_id,
    };
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_export_start(session, &export_request,
                                                  &err));
    do {
        memset(&progress, 0, sizeof progress);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_session_job_poll(session, &progress, &err));
    } while (progress.state == TP_SESSION_JOB_RUNNING);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, progress.kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, progress.state);
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_job_take_result(session, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_EXPORT, result.kind);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, result.status);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.targets);
    TEST_ASSERT_EQUAL_INT(1, result.export_result.atlases_ok);
    TEST_ASSERT_EQUAL_INT(0, result.export_result.atlases_failed);
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
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui.source_add);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui.sprite_override);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui.animation_create);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, gui.target_create);
    TEST_ASSERT_EQUAL_INT(gui.success, headless.success);
    TEST_ASSERT_EQUAL_INT(gui.validation, headless.validation);
    TEST_ASSERT_EQUAL_INT(gui.conflict, headless.conflict);
    TEST_ASSERT_EQUAL_INT(gui.duplicate, headless.duplicate);
    TEST_ASSERT_EQUAL_INT(gui.source_add, headless.source_add);
    TEST_ASSERT_EQUAL_INT(gui.sprite_override, headless.sprite_override);
    TEST_ASSERT_EQUAL_INT(gui.animation_create, headless.animation_create);
    TEST_ASSERT_EQUAL_INT(gui.target_create, headless.target_create);

    TEST_ASSERT_EQUAL_INT64(gui.revision, headless.revision);
    TEST_ASSERT_EQUAL_UINT64(gui.model_generation, headless.model_generation);
    TEST_ASSERT_EQUAL_UINT64(gui.event_sequence, headless.event_sequence);
    TEST_ASSERT_EQUAL_MEMORY(&gui.semantic_identity, &headless.semantic_identity,
                             sizeof gui.semantic_identity);
    TEST_ASSERT_EQUAL_STRING(gui.atlas_name, headless.atlas_name);
    TEST_ASSERT_EQUAL_STRING("golden", gui.atlas_name);
    TEST_ASSERT_EQUAL_STRING(gui.source_path, headless.source_path);
    TEST_ASSERT_EQUAL_STRING("sprites/hero.png", gui.source_path);
    TEST_ASSERT_TRUE(gui.sprite_origin_x == headless.sprite_origin_x);
    TEST_ASSERT_TRUE(gui.sprite_origin_x == 0.25F);
    TEST_ASSERT_EQUAL_STRING(gui.animation_name, headless.animation_name);
    TEST_ASSERT_EQUAL_STRING("walk", gui.animation_name);
    TEST_ASSERT_TRUE(gui.animation_fps == headless.animation_fps);
    TEST_ASSERT_TRUE(gui.animation_fps == TP_PROJECT_ANIM_FPS_DEFAULT);
    TEST_ASSERT_EQUAL_INT(gui.animation_frame_count,
                          headless.animation_frame_count);
    TEST_ASSERT_EQUAL_INT(1, gui.animation_frame_count);
    TEST_ASSERT_EQUAL_MEMORY(&gui.frame_source_id, &headless.frame_source_id,
                             sizeof gui.frame_source_id);
    TEST_ASSERT_FALSE(tp_id128_is_nil(gui.frame_source_id));
    TEST_ASSERT_EQUAL_STRING(gui.frame_source_key, headless.frame_source_key);
    TEST_ASSERT_EQUAL_STRING("hero.png", gui.frame_source_key);
    TEST_ASSERT_EQUAL_STRING(gui.target_exporter_id,
                             headless.target_exporter_id);
    TEST_ASSERT_EQUAL_STRING("json-neotolis", gui.target_exporter_id);
    TEST_ASSERT_EQUAL_STRING(gui.target_out_path, headless.target_out_path);
    TEST_ASSERT_EQUAL_STRING("out/golden", gui.target_out_path);
    TEST_ASSERT_EQUAL_INT(gui.target_enabled, headless.target_enabled);
    TEST_ASSERT_TRUE(gui.target_enabled);
    TEST_ASSERT_EQUAL_INT64(5, gui.revision);
    TEST_ASSERT_EQUAL_UINT64(5, gui.model_generation);
    TEST_ASSERT_EQUAL_UINT64(5, gui.event_sequence);
    TEST_ASSERT_EQUAL_UINT(5, gui.event_count);
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

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_capability_matrix_is_typed_and_exact);
    RUN_TEST(test_real_client_parity_manifest_covers_every_shipped_mutation);
    RUN_TEST(test_live_headless_runs_real_pack_job_and_export_command);
    RUN_TEST(test_gui_and_headless_share_golden_transaction_session_corpus);
    RUN_TEST(test_gui_invalid_intents_are_classified_by_the_shared_core);
    return UNITY_END();
}
