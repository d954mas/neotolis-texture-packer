#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_diff.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_project_mutation_internal.h"
#include "tp_session_internal.h"
#include "tp_test_model.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef enum semantic_family {
    FAMILY_ATLAS = 0,
    FAMILY_SOURCE,
    FAMILY_SPRITE,
    FAMILY_ANIMATION,
    FAMILY_TARGET
} semantic_family;

static tp_model *make_rich_model(void) {
    tp_project *project = tp_test_base_project();
    tp_project_atlas *atlas = &project->atlases[0];
    const tp_id128 source_id = atlas->sources[0].id;
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(atlas, source_id,
                                                  "hero.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->origin_x = 0.25F;
    sprite->origin_y = 0.75F;

    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(atlas, "walk", &animation));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_anim_add_frame(animation, source_id, "hero.png"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_anim_add_frame(animation, source_id, "hero2.png"));

    uint8_t counter = 71U;
    tp_rng rng = {tp_test_det_fill, &counter};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &error));

    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    tp_model_mark_saved(model);
    return model;
}

static tp_operation family_operation(semantic_family family,
                                     const tp_model *model) {
    const tp_project_atlas *atlas = &tp_model_project(model)->atlases[0];
    tp_operation operation = {0};
    operation.atlas_id = atlas->id;
    switch (family) {
        case FAMILY_ATLAS:
            operation.kind = TP_OP_ATLAS_RENAME;
            operation.u.atlas_rename.name = (char *)"atlas-renamed";
            break;
        case FAMILY_SOURCE:
            operation.kind = TP_OP_SOURCE_REPLACE;
            operation.u.source_ref.source_id = atlas->sources[0].id;
            operation.u.source_ref.key = (char *)"sprites-v2";
            break;
        case FAMILY_SPRITE:
            operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
            operation.u.sprite_set.source_id = atlas->sources[0].id;
            operation.u.sprite_set.src_key = (char *)"hero.png";
            operation.u.sprite_set.mask = TP_SPF_ORIGIN;
            operation.u.sprite_set.origin_x = 0.125F;
            operation.u.sprite_set.origin_y = 0.875F;
            break;
        case FAMILY_ANIMATION:
            operation.kind = TP_OP_ANIMATION_SETTINGS_SET;
            operation.u.anim_settings.anim_id = atlas->animations[0].id;
            operation.u.anim_settings.mask = TP_ANF_FPS | TP_ANF_FLIP_H;
            operation.u.anim_settings.fps = 24.0F;
            operation.u.anim_settings.flip_h = true;
            break;
        case FAMILY_TARGET:
            operation.kind = TP_OP_TARGET_SET;
            operation.u.target_set.target_id = atlas->targets[0].id;
            operation.u.target_set.mask = TP_TF_OUT_PATH | TP_TF_ENABLED;
            operation.u.target_set.out_path = (char *)"out/changed";
            operation.u.target_set.enabled = false;
            break;
    }
    return operation;
}

static void apply_model_operation(tp_model *model, tp_operation *operation,
                                  unsigned transaction_number) {
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%032x",
                   transaction_number);
    request.expected_revision = tp_model_revision(model);
    request.ops = operation;
    request.op_count = 1;
    tp_txn_result result = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
}

static void assert_family_history_oracle(semantic_family family) {
    tp_model *model = make_rich_model();
    char *bytes_a = tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_FALSE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(model));

    tp_operation operation = family_operation(family, model);
    apply_model_operation(model, &operation, (unsigned)family + 1U);
    char *bytes_b = tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(bytes_a, bytes_b));
    TEST_ASSERT_TRUE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(model));

    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    char *bytes_undo = tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(bytes_a, bytes_undo);
    TEST_ASSERT_FALSE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT64(2, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(1, tp_model_redo_depth(model));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(model, &error));
    char *bytes_redo = tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(bytes_b, bytes_redo);
    TEST_ASSERT_TRUE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT64(3, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(model));

    free(bytes_redo);
    free(bytes_undo);
    free(bytes_b);
    free(bytes_a);
    tp_model_destroy(model);
}

void test_atlas_family_history_contract(void) {
    assert_family_history_oracle(FAMILY_ATLAS);
}
void test_source_family_history_contract(void) {
    assert_family_history_oracle(FAMILY_SOURCE);
}
void test_sprite_family_history_contract(void) {
    assert_family_history_oracle(FAMILY_SPRITE);
}
void test_animation_family_history_contract(void) {
    assert_family_history_oracle(FAMILY_ANIMATION);
}
void test_target_family_history_contract(void) {
    assert_family_history_oracle(FAMILY_TARGET);
}

void test_new_apply_after_undo_discards_only_the_redo_branch(void) {
    tp_model *model = make_rich_model();
    char *bytes_a = tp_test_serialize_project(tp_model_project(model));
    tp_operation first = family_operation(FAMILY_ATLAS, model);
    first.u.atlas_rename.name = (char *)"first";
    apply_model_operation(model, &first, 11U);
    char *bytes_b = tp_test_serialize_project(tp_model_project(model));

    tp_operation second = family_operation(FAMILY_ATLAS, model);
    second.u.atlas_rename.name = (char *)"second";
    apply_model_operation(model, &second, 12U);
    TEST_ASSERT_EQUAL_INT64(2, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(model));

    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    char *bytes_after_undo =
        tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(bytes_b, bytes_after_undo);
    TEST_ASSERT_EQUAL_INT64(3, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(1, tp_model_redo_depth(model));

    tp_operation branch = family_operation(FAMILY_TARGET, model);
    apply_model_operation(model, &branch, 13U);
    char *bytes_branch = tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_INT64(4, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(model));
    TEST_ASSERT_FALSE(tp_model_can_redo(model));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND,
                          tp_model_redo(model, &error));
    char *bytes_after_rejected_redo =
        tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(bytes_branch, bytes_after_rejected_redo);
    TEST_ASSERT_EQUAL_INT64(4, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(model));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    char *bytes_back_at_baseline =
        tp_test_serialize_project(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(bytes_a, bytes_back_at_baseline);
    TEST_ASSERT_FALSE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT64(6, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(2, tp_model_redo_depth(model));

    free(bytes_back_at_baseline);
    free(bytes_after_rejected_redo);
    free(bytes_branch);
    free(bytes_after_undo);
    free(bytes_b);
    free(bytes_a);
    tp_model_destroy(model);
}

static int deterministic_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *next = (uint8_t *)context;
    for (size_t i = 0U; i < length; ++i) {
        out[i] = (uint8_t)(*next + (uint8_t)i);
    }
    *next = (uint8_t)(*next + 17U);
    return (int)length;
}

typedef struct session_observation {
    char *bytes;
    int64_t revision;
    uint64_t model_generation;
    uint64_t event_sequence;
    bool dirty;
    int undo_depth;
    int redo_depth;
} session_observation;

static session_observation observe_session(const tp_session *session) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &snapshot, &error));
    session_observation observation = {
        tp_test_serialize_project(tp_session_snapshot_project_internal(snapshot)),
        tp_session_snapshot_revision(snapshot),
        tp_session_snapshot_model_generation(snapshot),
        tp_session_snapshot_event_sequence(snapshot),
        tp_session_snapshot_dirty(snapshot),
        tp_session_undo_depth(session),
        tp_session_redo_depth(session)};
    tp_session_snapshot_destroy(snapshot);
    return observation;
}

static void free_observation(session_observation *observation) {
    free(observation->bytes);
    observation->bytes = NULL;
}

static tp_status session_rename(tp_session *session, tp_id128 atlas_id,
                                const char *transaction_id, const char *name,
                                tp_txn_result *result, tp_error *error) {
    tp_operation operation = {0};
    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas_id;
    operation.u.atlas_rename.name = (char *)name;
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   transaction_id);
    request.expected_revision = tp_session_revision(session);
    request.ops = &operation;
    request.op_count = 1;
    memset(result, 0, sizeof *result);
    return tp_session_apply(session, &request, result, error);
}

void test_journal_failure_preserves_live_apply_undo_redo_publication(void) {
    static const char transaction_id[] =
        "91000000000000000000000000000001";
    uint8_t seed = 1U;
    tp_rng rng = {deterministic_fill, &seed};
    tp_session *session = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_session_create(&rng, &session, &error));
    tp_session_snapshot *identity = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(session, &identity, &error));
    const tp_id128 atlas_id =
        tp_session_snapshot_atlas_at(identity, 0)->id;
    tp_session_snapshot_destroy(identity);

    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    const tp_id128 journal_key = tp_test_id_of(0x91);
    tp_journal *journal = tp_journal_create(io, journal_key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_attach_journal(session, journal, &error));
    TEST_ASSERT_EQUAL_INT(0, tp_journal_id_count(journal));

    session_observation a = observe_session(session);
    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        session_rename(session, atlas_id, transaction_id, "changed", &result,
                       &error));
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);
    session_observation b = observe_session(session);
    TEST_ASSERT_NOT_EQUAL(0, strcmp(a.bytes, b.bytes));
    TEST_ASSERT_EQUAL_INT64(1, b.revision);
    TEST_ASSERT_EQUAL_UINT64(1U, b.event_sequence);
    TEST_ASSERT_TRUE(b.dirty);
    TEST_ASSERT_EQUAL_INT(1, b.undo_depth);
    TEST_ASSERT_EQUAL_INT(0, b.redo_depth);
    TEST_ASSERT_FALSE(tp_journal_contains(journal, transaction_id));
    TEST_ASSERT_EQUAL_INT(0, tp_journal_id_count(journal));
    TEST_ASSERT_FALSE(tp_session_recovery_available(session));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_DUPLICATE_ID,
        session_rename(session, atlas_id, transaction_id, "changed", &result,
                       &error));
    TEST_ASSERT_FALSE(result.committed);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_undo(session, &error));
    session_observation undone = observe_session(session);
    TEST_ASSERT_EQUAL_STRING(a.bytes, undone.bytes);
    TEST_ASSERT_EQUAL_INT64(2, undone.revision);
    TEST_ASSERT_EQUAL_UINT64(2U, undone.event_sequence);
    TEST_ASSERT_FALSE(undone.dirty);
    TEST_ASSERT_EQUAL_INT(0, undone.undo_depth);
    TEST_ASSERT_EQUAL_INT(1, undone.redo_depth);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_session_redo(session, &error));
    session_observation redone = observe_session(session);
    TEST_ASSERT_EQUAL_STRING(b.bytes, redone.bytes);
    TEST_ASSERT_EQUAL_INT64(3, redone.revision);
    TEST_ASSERT_EQUAL_UINT64(3U, redone.event_sequence);
    TEST_ASSERT_TRUE(redone.dirty);
    TEST_ASSERT_EQUAL_INT(1, redone.undo_depth);
    TEST_ASSERT_EQUAL_INT(0, redone.redo_depth);
    TEST_ASSERT_EQUAL_INT(0, tp_journal_id_count(journal));

    tp_session_event events[3] = {0};
    size_t event_count = 0U;
    bool resync_required = true;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_events_after(session, 0U, events, 3U, &event_count,
                                &resync_required, &error));
    TEST_ASSERT_FALSE(resync_required);
    TEST_ASSERT_EQUAL_size_t(3U, event_count);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_MODEL_COMMITTED, events[0].kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_UNDONE, events[1].kind);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_EVENT_REDONE, events[2].kind);

    free_observation(&redone);
    free_observation(&undone);
    free_observation(&b);
    free_observation(&a);
    tp_session_destroy(session);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_atlas_family_history_contract);
    RUN_TEST(test_source_family_history_contract);
    RUN_TEST(test_sprite_family_history_contract);
    RUN_TEST(test_animation_family_history_contract);
    RUN_TEST(test_target_family_history_contract);
    RUN_TEST(test_new_apply_after_undo_discards_only_the_redo_branch);
    RUN_TEST(test_journal_failure_preserves_live_apply_undo_redo_publication);
    return UNITY_END();
}
