/* F2-03: semantic diff / exact inverse (Undo) + redo replay + a minimal in-memory
 * history, proven by an ORACLE suite (the ENGINE; the GUI Undo cutover is F3-02):
 *   - per-op oracle for EVERY op kind: A -> forward transaction -> B -> inverse -> A'
 *     is BYTE-IDENTICAL to A, and equals the legacy FULL-SNAPSHOT restore
 *     (tp_project_save_buffer/load_buffer). redo -> B' is byte-identical to B;
 *   - reference integrity: atlas.remove removes its whole subtree; source.remove
 *     is rejected while sprite/frame references still target that source;
 *   - a 100-animation batch committed as ONE transaction -> ONE inverse restores A;
 *   - inverse-apply allocation failure at EVERY staging depth -> ROLLBACK (model
 *     byte-unchanged, cursor unchanged), no leak (ASan/LSan in CI);
 *   - capture-alloc failure during commit fails the whole commit cleanly (F2-02
 *     atomicity preserved: model byte-unchanged, no history entry);
 *   - redo + redo-branch discard (a new transaction after Undo drops the redo steps);
 *   - a corrupted/hostile diff (stale/unknown entity id, out-of-range position) ->
 *     a STRUCTURED error, never a crash; the model is left byte-unchanged;
 *   - dirty stays identity-derived: an Undo to the saved baseline is clean even at a
 *     higher revision; label/author carry into the history record.
 */

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_diff.h"
#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h" /* diff alloc seam + record/history internals (corrupt + rollback) */
#include "tp_history_codec_internal.h"
#include "tp_journal_internal.h" /* bounded binary codec helpers for hostile HISTORY fixtures */
#include "tp_model_seam.h"
#include "tp_project_identity_internal.h"
#include "tp_test_model.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {
    tp_diff__test_set_alloc_fail(-1);
    tp_diff__test_reset_alloc_count();
    tp_history__test_set_limits(0, 0U, 0U);
}

/* ---- fixtures ------------------------------------------------------------- */

/* tp_test_det_fill / tp_test_id_of / tp_test_serialize_project: shared
 * byte-identical fixtures, see tp_test_model.h. next_txn_id/s_id_ctr and
 * make_base()/fresh() below stay local: make_base() is a larger 2-atlas +
 * animation fixture, not the same as tp_test_base_project(). */
static int s_id_ctr = 0;
static void next_txn_id(char *buf) { (void)snprintf(buf, 33, "%032x", (unsigned)(++s_id_ctr)); }

/* A CRC-independent, syntactically valid HISTORY frames.set transition whose
 * key is deliberately noncanonical. The decoder accepts it (string + ids are
 * well formed); final project validation must reject it before publication. */
static size_t noncanonical_frame_transition(uint8_t *out, size_t cap,
                                            tp_id128 atlas_id, tp_id128 anim_id,
                                            tp_id128 source_id) {
    static const char key[] = "a/../b.png";
    const size_t need = 8U + 1U + 16U + 16U + 4U + 16U + 4U + sizeof key - 1U;
    if (!out || cap < need) return 0U;
    size_t off = 0U;
    tp_jrn_put_u32(out + off, TP_HISTORY_CODEC_VERSION);
    off += 4U;
    tp_jrn_put_u32(out + off, 1U);
    off += 4U;
    out[off++] = (uint8_t)TP_DIFF_SHAPE_FRAMES_LIST;
    memcpy(out + off, atlas_id.bytes, sizeof atlas_id.bytes);
    off += sizeof atlas_id.bytes;
    memcpy(out + off, anim_id.bytes, sizeof anim_id.bytes);
    off += sizeof anim_id.bytes;
    tp_jrn_put_u32(out + off, 1U);
    off += 4U;
    memcpy(out + off, source_id.bytes, sizeof source_id.bytes);
    off += sizeof source_id.bytes;
    tp_jrn_put_u32(out + off, (uint32_t)(sizeof key - 1U));
    off += 4U;
    memcpy(out + off, key, sizeof key - 1U);
    return off + sizeof key - 1U;
}

/* A0 "atlas1" {sources "sprites" + unreferenced "unused", target json-neotolis
 * "out/a", anim "walk"[hero,hero2]}
 * + A1 "atlas2" (empty). All structural ids promoted (deterministic). */
static tp_project *make_base(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a0 = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a0, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a0, "unused"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a0, "walk", &an));
    int idx = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", &idx));
    a0 = &p->atlases[0];
    an = &a0->animations[0];
    uint8_t ctr = 1;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(p, &rng, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(an, a0->sources[0].id,
                                                    "hero.png"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(an, a0->sources[0].id,
                                                    "hero2.png"));
    return p;
}

static tp_model *fresh(void) {
    tp_model *m = tp_model_wrap(make_base());
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    return m;
}

/* ids of the base entities (from the live project). */
static tp_id128 a0_id(tp_model *m) { return tp_model_project(m)->atlases[0].id; }
static tp_id128 a1_id(tp_model *m) { return tp_model_project(m)->atlases[1].id; }
static tp_id128 src0_id(tp_model *m) { return tp_model_project(m)->atlases[0].sources[0].id; }
static tp_id128 src1_id(tp_model *m) { return tp_model_project(m)->atlases[0].sources[1].id; }
static tp_id128 tgt0_id(tp_model *m) { return tp_model_project(m)->atlases[0].targets[0].id; }
static tp_id128 anim0_id(tp_model *m) { return tp_model_project(m)->atlases[0].animations[0].id; }

/* ---- the oracle: A -> forward -> B -> inverse -> A' + redo -> B' ---------- */

static void run_oracle(tp_model *m, tp_operation *ops, int n) {
    tp_error err;
    char *A = tp_test_serialize_project(tp_model_project(m));
    /* legacy full-snapshot of A -- the comparison oracle. */
    char *snapA = NULL;
    size_t snapAlen = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save_buffer(tp_model_project(m), &snapA, &snapAlen, &err));
    int64_t rev0 = tp_model_revision(m);
    int depth0 = tp_model_undo_depth(m); /* tolerate any pre-seeded history */

    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = rev0;
    req.ops = ops;
    req.op_count = n;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    tp_txn_result_free(&res);
    TEST_ASSERT_EQUAL_INT64(rev0 + 1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(depth0 + 1, tp_model_undo_depth(m)); /* one new undoable step */
    TEST_ASSERT_TRUE(tp_model_can_undo(m));
    char *B = tp_test_serialize_project(tp_model_project(m));

    /* The durable history codec is a one-way projection of the same semantic
     * record. Exercise it for every operation kind covered by this oracle. */
    tp_history *history = tp_model_history(m);
    TEST_ASSERT_NOT_NULL(history);
    const tp_diff_record *record = history->records[history->pos - 1];
    tp_project *codec_project = tp_project_clone(tp_model_project(m));
    TEST_ASSERT_NOT_NULL(codec_project);
    tp_history_transition_blob transition = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_ERROR;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_history_transition_encode(record, true, codec_project, SIZE_MAX,
                                     &transition, &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_history_transition_apply(codec_project,
                                                      transition.data,
                                                      transition.len, NULL,
                                                      &err));
    char *codec_A = tp_test_serialize_project(codec_project);
    TEST_ASSERT_EQUAL_STRING(A, codec_A);
    tp_history_transition_blob_free(&transition);

    outcome = TP_HISTORY_CODEC_ERROR;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_history_transition_encode(record, false, codec_project, SIZE_MAX,
                                     &transition, &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_history_transition_apply(codec_project,
                                                      transition.data,
                                                      transition.len, NULL,
                                                      &err));
    char *codec_B = tp_test_serialize_project(codec_project);
    TEST_ASSERT_EQUAL_STRING(B, codec_B);
    tp_history_transition_blob_free(&transition);
    tp_project_destroy(codec_project);

    /* inverse (Undo) -> A' */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_INT64(rev0 + 2, tp_model_revision(m)); /* Undo bumps the revision */
    char *Ap = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(A, Ap); /* diff restore is byte-identical to A */

    /* legacy full-snapshot restore of A == the diff restore. */
    tp_project *legacy = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(snapA, snapAlen, &legacy, &err));
    char *Aleg = tp_test_serialize_project(legacy);
    TEST_ASSERT_EQUAL_STRING(Aleg, Ap); /* diff restore == full-snapshot restore */
    tp_project_destroy(legacy);

    /* redo -> B' */
    TEST_ASSERT_TRUE(tp_model_can_redo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
    TEST_ASSERT_EQUAL_INT64(rev0 + 3, tp_model_revision(m));
    char *Bp = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(B, Bp);

    free(A);
    free(B);
    free(Ap);
    free(Bp);
    free(Aleg);
    free(codec_A);
    free(codec_B);
    free(snapA);
}

void test_history_codec_atlas_name_roundtrip_both_directions(void) {
    tp_model *m = fresh();
    tp_project *before = tp_project_clone(tp_model_project(m));
    TEST_ASSERT_NOT_NULL(before);

    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"codec-name";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result result = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &result, &err));
    TEST_ASSERT_TRUE(result.committed);
    tp_txn_result_free(&result);

    tp_project *after = tp_project_clone(tp_model_project(m));
    TEST_ASSERT_NOT_NULL(after);
    tp_history *history = tp_model_history(m);
    TEST_ASSERT_NOT_NULL(history);
    TEST_ASSERT_EQUAL_INT(1, history->count);
    const tp_diff_record *record = history->records[0];

    tp_history_transition_blob undo = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_ERROR;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_history_transition_encode(record, true, after, SIZE_MAX, &undo,
                                     &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_UINT32(1U, undo.op_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_history_transition_apply(after, undo.data,
                                                      undo.len, NULL, &err));
    char *expected_before = tp_test_serialize_project(before);
    char *actual_before = tp_test_serialize_project(after);
    TEST_ASSERT_EQUAL_STRING(expected_before, actual_before);

    tp_history_transition_blob redo = {0};
    outcome = TP_HISTORY_CODEC_ERROR;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_history_transition_encode(record, false, before, SIZE_MAX, &redo,
                                     &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_history_transition_apply(before, redo.data,
                                                      redo.len, NULL, &err));
    char *expected_after = tp_test_serialize_project(tp_model_project(m));
    char *actual_after = tp_test_serialize_project(before);
    TEST_ASSERT_EQUAL_STRING(expected_after, actual_after);

    free(expected_before);
    free(actual_before);
    free(expected_after);
    free(actual_after);
    tp_history_transition_blob_free(&undo);
    tp_history_transition_blob_free(&redo);
    tp_project_destroy(before);
    tp_project_destroy(after);
    tp_model_destroy(m);
}

/* Compact history has exactly two non-error fallbacks: a future shape the
 * current codec cannot express, or a record that genuinely exceeds its byte
 * budget. A malformed value in a known shape is a hard error -- silently
 * checkpointing it would hide a core corruption bug. */
void test_history_codec_rejects_malformed_known_shape_but_falls_back_for_unknown_shape(void) {
    tp_model *m = fresh();
    tp_error err = {0};
    tp_history_transition_blob blob = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_OK;

    tp_diff_op malformed = {0};
    malformed.shape = TP_DIFF_SHAPE_ATLAS_NAME;
    malformed.atlas_id = a0_id(m);
    malformed.name_after = NULL; /* known shape, invalid wire value */
    tp_diff_record bad = {.ops = &malformed, .op_count = 1};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_history_transition_encode(&bad, false, tp_model_project(m), SIZE_MAX,
                                     &blob, &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_ERROR, outcome);
    TEST_ASSERT_NULL(blob.data);

    tp_diff_op future = {0};
    future.shape = (tp_diff_shape)99;
    tp_diff_record unknown = {.ops = &future, .op_count = 1};
    outcome = TP_HISTORY_CODEC_ERROR;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_history_transition_encode(&unknown, false, tp_model_project(m),
                                     SIZE_MAX, &blob, &outcome, &err));
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_UNSUPPORTED, outcome);
    TEST_ASSERT_NULL(blob.data);

    tp_model_destroy(m);
}

static void assert_hostile_collection_remove_rejected(tp_diff_coll coll,
                                                       bool reverse) {
    tp_project *project = make_base();
    tp_project_atlas *atlas = &project->atlases[0];
    tp_project_anim fake_animation = {.id = tp_test_id_of(0xE1)};
    tp_project_target fake_target = {.id = tp_test_id_of(0xE2)};
    tp_diff_op operation = {
        .shape = TP_DIFF_SHAPE_COLL,
        .coll = coll,
        .atlas_id = atlas->id,
        .anim_id = atlas->animations[0].id,
        .position = 0,
        .created = reverse,
    };
    switch (coll) {
        case TP_DIFF_COLL_ATLAS:
            operation.elem = &project->atlases[1];
            break;
        case TP_DIFF_COLL_SOURCE:
            operation.elem = &atlas->sources[1];
            break;
        case TP_DIFF_COLL_ANIM:
            operation.elem = &fake_animation;
            break;
        case TP_DIFF_COLL_TARGET:
            operation.elem = &fake_target;
            break;
        case TP_DIFF_COLL_FRAME:
            operation.elem = &atlas->animations[0].frames[1];
            break;
    }
    char *before = tp_test_serialize_project(project);
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_diff_op_apply(project, &operation, reverse,
                                           &error));
    char *after = tp_test_serialize_project(project);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_project_destroy(project);
}

void test_hostile_collection_removals_require_position_identity(void) {
    for (int coll = TP_DIFF_COLL_ATLAS; coll <= TP_DIFF_COLL_FRAME; ++coll) {
        assert_hostile_collection_remove_rejected((tp_diff_coll)coll, false);
        assert_hostile_collection_remove_rejected((tp_diff_coll)coll, true);
    }
}

static void assert_hostile_sprite_record_rejected(bool reverse,
                                                  bool replacement) {
    tp_project *project = make_base();
    tp_project_atlas *atlas = &project->atlases[0];
    tp_project_sprite *current = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "hero.png", &current));
    TEST_ASSERT_NOT_NULL(current);
    tp_project_sprite other = {
        .source_ref = atlas->sources[0].id,
        .src_key = (char *)"enemy.png",
    };
    const tp_project_sprite current_identity = *current;
    tp_diff_op operation = {
        .shape = TP_DIFF_SHAPE_SPRITE_RECORD,
        .atlas_id = atlas->id,
    };
    if (reverse) {
        operation.spr_after_present = true;
        operation.spr_after_index = 0;
        operation.spr_after = replacement ? current_identity : other;
        operation.spr_before_present = replacement;
        operation.spr_before_index = replacement ? 0 : -1;
        operation.spr_before = other;
    } else {
        operation.spr_before_present = true;
        operation.spr_before_index = 0;
        operation.spr_before = replacement ? current_identity : other;
        operation.spr_after_present = replacement;
        operation.spr_after_index = replacement ? 0 : -1;
        operation.spr_after = other;
    }
    char *before = tp_test_serialize_project(project);
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_diff_op_apply(project, &operation, reverse,
                                           &error));
    char *after = tp_test_serialize_project(project);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_project_destroy(project);
}

void test_hostile_sprite_remove_and_replace_require_position_identity(void) {
    assert_hostile_sprite_record_rejected(false, false);
    assert_hostile_sprite_record_rejected(true, false);
    assert_hostile_sprite_record_rejected(false, true);
    assert_hostile_sprite_record_rejected(true, true);
}

void test_history_codec_atomic_apply_rejects_noncanonical_result(void) {
    tp_model *m = fresh();
    const tp_project_atlas *atlas = &tp_model_project(m)->atlases[0];
    uint8_t transition[96];
    const size_t transition_len = noncanonical_frame_transition(
        transition, sizeof transition, atlas->id, atlas->animations[0].id,
        atlas->sources[0].id);
    TEST_ASSERT_GREATER_THAN_size_t(0U, transition_len);
    char *before = tp_test_serialize_project(tp_model_project(m));
    /* The test owns this model and exercises the codec's project-level atomic
     * helper directly; the public accessor is intentionally const. */
    tp_project *live = (tp_project *)(void *)tp_model_project(m);
    tp_error err = {0};
    uint32_t op_count = 0U;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_history_transition_apply(live,
                                                      transition, transition_len,
                                                      &op_count, &err));
    char *after = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(after);
    free(before);
    tp_model_destroy(m);
}

/* seed a sparse sprite override on {src, key} so a *.clear has something to remove. */
static void seed_sprite(tp_model *m, const char *key) {
    tp_error err;
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = a0_id(m);
    op.u.sprite_set.source_id = src0_id(m);
    op.u.sprite_set.src_key = (char *)key;
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    op.u.sprite_set.origin_x = 0.25F;
    op.u.sprite_set.origin_y = 0.75F;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
}

/* ---- per-op oracle: every catalog kind ----------------------------------- */

void test_oracle_atlas_create(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = tp_test_id_of(0x51);
    op.u.atlas_create.name = (char *)"created";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_atlas_remove(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a1_id(m); /* remove the SECOND atlas (mid/tail position) */
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_atlas_rename(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"renamed";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_atlas_settings_set(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a0_id(m);
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_PADDING | TP_AF_SHAPE | TP_AF_POWER_OF_TWO;
    op.u.atlas_settings.max_size = 2048;
    op.u.atlas_settings.padding = 7;
    op.u.atlas_settings.shape = 2;
    op.u.atlas_settings.power_of_two = true;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_source_add(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = a0_id(m);
    op.u.source_add.source_id = tp_test_id_of(0x52);
    op.u.source_add.kind = TP_SOURCE_KIND_FILE;
    op.u.source_add.key = (char *)"extra/tiles.png";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_source_remove(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.source_ref.source_id = src1_id(m); /* no persistent references */
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_source_replace(void) { /* reserved op */
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REPLACE;
    op.atlas_id = a0_id(m);
    op.u.source_ref.source_id = src0_id(m);
    op.u.source_ref.key = (char *)"sprites_v2";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_sprite_override_set(void) { /* creates a new sparse record */
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = a0_id(m);
    op.u.sprite_set.source_id = src0_id(m);
    op.u.sprite_set.src_key = (char *)"hero.png";
    op.u.sprite_set.mask = TP_SPF_ORIGIN | TP_SPF_SLICE9;
    op.u.sprite_set.origin_x = 0.1F;
    op.u.sprite_set.origin_y = 0.2F;
    op.u.sprite_set.slice9[0] = 3;
    op.u.sprite_set.slice9[1] = 4;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_sprite_override_clear(void) { /* prunes an existing record (remove) */
    tp_model *m = fresh();
    seed_sprite(m, "villain.png"); /* part of A: a record to clear */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    op.atlas_id = a0_id(m);
    op.u.sprite_clear.source_id = src0_id(m);
    op.u.sprite_clear.src_key = (char *)"villain.png";
    op.u.sprite_clear.mask = TP_SPF_ORIGIN; /* clears origin -> record becomes default -> pruned */
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_sprite_name_set(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_NAME_SET;
    op.atlas_id = a0_id(m);
    op.u.sprite_name.source_id = src0_id(m);
    op.u.sprite_name.src_key = (char *)"hero.png";
    op.u.sprite_name.name = (char *)"HeroExport";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_create(void) {
    tp_model *m = fresh();
    tp_op_sprite_ref frames[2] = {
        {src0_id(m), (char *)"hero.png"},
        {src0_id(m), (char *)"hero2.png"}};
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = a0_id(m);
    op.u.anim_create.anim_id = tp_test_id_of(0x53);
    op.u.anim_create.name = (char *)"run";
    op.u.anim_create.fps = 24.0F;
    op.u.anim_create.playback = 1;
    op.u.anim_create.frames = frames;
    op.u.anim_create.frame_count = 2;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_remove(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.anim_ref.anim_id = anim0_id(m);
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

/* H/P1-2: the anim NAME field is set non-default and the oracle proves rename's diff
 * (fwd + inverse + redo) is byte-complete -- i.e. animation rename is genuinely undoable. */
void test_oracle_animation_rename(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_RENAME;
    op.atlas_id = a0_id(m);
    op.u.anim_rename.anim_id = anim0_id(m);
    op.u.anim_rename.name = (char *)"renamed";
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

/* H/P1-2 explicit: an anim rename reverts its NAME on undo and re-applies it on redo. */
void test_anim_rename_undo_redo_name(void) {
    tp_model *m = fresh();
    tp_error err;
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_RENAME;
    op.atlas_id = a0_id(m);
    op.u.anim_rename.anim_id = anim0_id(m);
    op.u.anim_rename.name = (char *)"sprint";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_EQUAL_STRING("sprint", tp_model_project(m)->atlases[0].animations[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("walk", tp_model_project(m)->atlases[0].animations[0].name); /* reverted */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
    TEST_ASSERT_EQUAL_STRING("sprint", tp_model_project(m)->atlases[0].animations[0].name); /* re-applied */
    tp_model_destroy(m);
}

void test_oracle_animation_settings_set(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = a0_id(m);
    op.u.anim_settings.anim_id = anim0_id(m);
    op.u.anim_settings.mask = TP_ANF_FPS | TP_ANF_PLAYBACK | TP_ANF_FLIP_H;
    op.u.anim_settings.fps = 15.0F;
    op.u.anim_settings.playback = 2;
    op.u.anim_settings.flip_h = true;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_frames_set(void) { /* reserved bulk op */
    tp_model *m = fresh();
    tp_op_sprite_ref frames[3] = {
        {src0_id(m), (char *)"a.png"}, {src0_id(m), (char *)"b.png"},
        {src0_id(m), (char *)"c.png"}};
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAMES_SET;
    op.atlas_id = a0_id(m);
    op.u.anim_frames_set.anim_id = anim0_id(m);
    op.u.anim_frames_set.frames = frames;
    op.u.anim_frames_set.frame_count = 3;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_frame_add(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_ADD;
    op.atlas_id = a0_id(m);
    op.u.anim_frame_add.anim_id = anim0_id(m);
    op.u.anim_frame_add.frame = (tp_op_sprite_ref){src0_id(m), (char *)"hero3.png"};
    op.u.anim_frame_add.index = 1; /* insert in the middle */
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_frame_remove(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.anim_frame_rm.anim_id = anim0_id(m);
    op.u.anim_frame_rm.index = 0;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_animation_frame_move(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = a0_id(m);
    op.u.anim_frame_move.anim_id = anim0_id(m);
    op.u.anim_frame_move.from_index = 0;
    op.u.anim_frame_move.to_index = 1;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_target_create(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_CREATE;
    op.atlas_id = a0_id(m);
    op.u.target_create.target_id = tp_test_id_of(0x54);
    op.u.target_create.exporter_id = (char *)"defold";
    op.u.target_create.out_path = (char *)"out/defold";
    op.u.target_create.enabled = true;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_target_remove(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.target_ref.target_id = tgt0_id(m);
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

void test_oracle_target_set(void) {
    tp_model *m = fresh();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = a0_id(m);
    op.u.target_set.target_id = tgt0_id(m);
    op.u.target_set.mask = TP_TF_ALL; /* full replace (behavior-preserving) */
    op.u.target_set.exporter_id = (char *)"defold";
    op.u.target_set.out_path = (char *)"out/changed";
    op.u.target_set.enabled = false;
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

/* ---- reference cascade --------------------------------------------------- */

/* Removing a whole atlas drops its sources/sprites/animations/targets; the inverse
 * restores the entire subtree byte-identical (the coarse-remove case that favors
 * state-capture over inverse-as-operations). */
void test_cascade_atlas_remove_full_subtree(void) {
    tp_model *m = fresh();
    /* enrich A0 so the removed atlas carries every collection. */
    seed_sprite(m, "hero.png");
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a0_id(m); /* the RICH atlas at index 0 (sources+sprite+anim+target) */
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

/* Canonical v5 records cannot reference an unknown source. Removing a source
 * with sprite/frame references is rejected before staging and changes nothing. */
void test_source_remove_rejects_referenced_source_atomically(void) {
    tp_model *m = fresh();
    seed_sprite(m, "hero.png"); /* a sprite override bridging into the atlas */
    char *before = tp_test_serialize_project(tp_model_project(m));
    const int64_t before_revision = tp_model_revision(m);
    const int before_undo_depth = tp_model_undo_depth(m);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.source_ref.source_id = src0_id(m);
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = before_revision;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result result = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_model_apply(m, &req, &result, &err));
    TEST_ASSERT_FALSE(result.committed);
    tp_txn_result_free(&result);
    char *after = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_EQUAL_INT64(before_revision, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(before_undo_depth, tp_model_undo_depth(m));
    free(after);
    free(before);
    tp_model_destroy(m);
}

/* ---- a 100-animation batch as ONE transaction / ONE inverse -------------- */

void test_batch_100_animations_one_inverse(void) {
    tp_model *m = fresh();
    tp_id128 aid = a0_id(m);
    enum { N = 100 };
    tp_operation ops[N];
    char names[N][16];
    for (int i = 0; i < N; i++) {
        (void)snprintf(names[i], sizeof names[i], "anim_%d", i);
        memset(&ops[i], 0, sizeof ops[i]);
        ops[i].kind = TP_OP_ANIMATION_CREATE;
        ops[i].atlas_id = aid;
        ops[i].u.anim_create.anim_id = tp_test_id_of((uint8_t)(0x80 + i));
        ops[i].u.anim_create.name = names[i];
        ops[i].u.anim_create.fps = 30.0F;
    }
    run_oracle(m, ops, N); /* one transaction -> one record -> one inverse restores A */
    tp_model_destroy(m);
}

/* A single transaction whose later ops depend on an entity an earlier op created
 * (create the atlas, then add a source/animation into it, then rename it): capture
 * addresses the freshly-created atlas by id against the progressively-applied clone,
 * and the reverse-order inverse unwinds the whole batch back to A byte-identical. */
void test_batch_mixed_intrabatch(void) {
    tp_model *m = fresh();
    tp_id128 newa = tp_test_id_of(0x51);
    tp_operation ops[4];
    memset(ops, 0, sizeof ops);
    ops[0].kind = TP_OP_ATLAS_CREATE;
    ops[0].atlas_id = newa;
    ops[0].u.atlas_create.name = (char *)"born";
    ops[1].kind = TP_OP_SOURCE_ADD;
    ops[1].atlas_id = newa;
    ops[1].u.source_add.source_id = tp_test_id_of(0x52);
    ops[1].u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    ops[1].u.source_add.key = (char *)"born/sprites";
    ops[2].kind = TP_OP_ANIMATION_CREATE;
    ops[2].atlas_id = newa;
    ops[2].u.anim_create.anim_id = tp_test_id_of(0x53);
    ops[2].u.anim_create.name = (char *)"idle";
    ops[2].u.anim_create.fps = 12.0F;
    ops[3].kind = TP_OP_ATLAS_RENAME;
    ops[3].atlas_id = newa;
    ops[3].u.atlas_rename.name = (char *)"reborn";
    run_oracle(m, ops, 4);
    tp_model_destroy(m);
}

/* ---- inverse-apply allocation-failure rollback (sweep every staging depth) - */

void test_inverse_alloc_failure_rolls_back(void) {
    tp_model *m = fresh();
    seed_sprite(m, "hero.png"); /* rich atlas so the inverse (re-insert) allocates a lot */
    tp_error err;

    /* forward: remove the rich atlas -> B. Undo re-inserts the whole subtree. */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = a0_id(m);
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    char *B = tp_test_serialize_project(tp_model_project(m));
    int64_t revB = tp_model_revision(m);

    /* dry-count the diff allocations the inverse needs (on a throwaway clone). */
    tp_diff_record *r = tp_history_undo_record(tp_model_history(m));
    TEST_ASSERT_NOT_NULL(r);
    tp_project *dry = tp_project_clone(tp_model_project(m));
    TEST_ASSERT_NOT_NULL(dry);
    tp_diff__test_reset_alloc_count();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_diff_record_apply(dry, r, true, &err));
    int total = tp_diff__test_alloc_count();
    tp_project_destroy(dry);
    TEST_ASSERT_TRUE(total > 1);

    /* Fail at every staging depth: each undo must fail, roll back (model still B),
     * and leave the cursor undoable + revision unchanged. */
    for (int k = 0; k < total; k++) {
        tp_diff__test_set_alloc_fail(k);
        tp_status st = tp_model_undo(m, &err);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, st);
        TEST_ASSERT_EQUAL_INT64(revB, tp_model_revision(m)); /* unchanged */
        TEST_ASSERT_TRUE(tp_model_can_undo(m));              /* cursor unchanged */
        char *now = tp_test_serialize_project(tp_model_project(m));
        TEST_ASSERT_EQUAL_STRING(B, now); /* rolled back: model byte-unchanged */
        free(now);
    }
    /* a clean undo then succeeds. */
    tp_diff__test_set_alloc_fail(-1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    free(B);
    tp_model_destroy(m);
}

/* ---- capture-alloc failure during commit -> whole commit fails cleanly ---- */

void test_capture_alloc_failure_fails_commit_atomically(void) {
    tp_error err;
    /* count the diff allocations one capture-committing transaction needs. */
    tp_operation probe_op;
    memset(&probe_op, 0, sizeof probe_op);

    tp_model *probe = fresh();
    char *A = tp_test_serialize_project(tp_model_project(probe));
    probe_op.kind = TP_OP_SOURCE_ADD;
    probe_op.atlas_id = a0_id(probe);
    probe_op.u.source_add.source_id = tp_test_id_of(0x52);
    probe_op.u.source_add.kind = TP_SOURCE_KIND_FILE;
    probe_op.u.source_add.key = (char *)"extra/tiles.png";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = 0;
    req.ops = &probe_op;
    req.op_count = 1;
    tp_txn_result res;
    tp_diff__test_reset_alloc_count();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(probe, &req, &res, &err));
    tp_txn_result_free(&res);
    int total = tp_diff__test_alloc_count();
    tp_model_destroy(probe);
    TEST_ASSERT_TRUE(total > 1);

    /* Fail each diff allocation depth on a FRESH model: the commit must reject, the
     * model stay byte-unchanged, the revision stay 0, and NO history entry appear. */
    for (int k = 0; k < total; k++) {
        tp_model *m = fresh();
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_SOURCE_ADD;
        op.atlas_id = a0_id(m);
        op.u.source_add.source_id = tp_test_id_of(0x52);
        op.u.source_add.kind = TP_SOURCE_KIND_FILE;
        op.u.source_add.key = (char *)"extra/tiles.png";
        tp_txn_request r2 = {0};
        r2.schema = TP_TXN_SCHEMA;
        next_txn_id(r2.id_hex);
        r2.expected_revision = 0;
        r2.ops = &op;
        r2.op_count = 1;
        tp_txn_result res2;
        tp_diff__test_set_alloc_fail(k);
        tp_status st = tp_model_apply(m, &r2, &res2, &err);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, st);
        TEST_ASSERT_FALSE(res2.committed);
        TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* unchanged */
        TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(m)); /* no history entry */
        char *now = tp_test_serialize_project(tp_model_project(m));
        TEST_ASSERT_EQUAL_STRING(A, now); /* model byte-unchanged */
        free(now);
        tp_txn_result_free(&res2);
        tp_model_destroy(m);
    }
    free(A);
}

/* ---- corrupted / hostile diff -> structured error, model byte-unchanged --- */

void test_corrupted_diff_unknown_atlas(void) {
    tp_model *m = fresh();
    tp_error err;
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a0_id(m);
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    op.u.atlas_settings.max_size = 1234;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    char *B = tp_test_serialize_project(tp_model_project(m));

    /* corrupt the record: a stale/unknown atlas id. */
    tp_diff_record *r = tp_history_undo_record(tp_model_history(m));
    tp_id128 saved = r->ops[0].atlas_id;
    r->ops[0].atlas_id = tp_test_id_of(0xEE);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_model_undo(m, &err)); /* structured, no crash */
    char *now = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(B, now); /* model byte-unchanged */
    TEST_ASSERT_TRUE(tp_model_can_undo(m));
    free(now);

    /* repaired: a clean undo works. */
    r->ops[0].atlas_id = saved;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    free(B);
    tp_model_destroy(m);
}

void test_corrupted_diff_bad_position(void) {
    tp_model *m = fresh();
    tp_error err;
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE; /* undo re-inserts at `position` */
    op.atlas_id = a0_id(m);
    op.u.source_ref.source_id = src1_id(m);
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    char *B = tp_test_serialize_project(tp_model_project(m));

    tp_diff_record *r = tp_history_undo_record(tp_model_history(m));
    int saved = r->ops[0].position;
    r->ops[0].position = 999; /* out-of-range insert index */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_model_undo(m, &err));
    char *now = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(B, now);
    free(now);

    r->ops[0].position = saved;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    free(B);
    tp_model_destroy(m);
}

/* ---- redo-branch discard ------------------------------------------------- */

void test_redo_branch_discard(void) {
    tp_model *m = fresh();
    tp_error err;
    char *A = tp_test_serialize_project(tp_model_project(m));

    tp_operation op;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;

    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"one";
    req.expected_revision = tp_model_revision(m);
    next_txn_id(req.id_hex);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);

    op.u.atlas_rename.name = (char *)"two";
    req.expected_revision = tp_model_revision(m);
    next_txn_id(req.id_hex);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err)); /* -> "one" */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err)); /* -> A */
    TEST_ASSERT_EQUAL_INT(2, tp_model_redo_depth(m));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(m));

    /* a NEW transaction after the Undo discards the whole redo branch. */
    op.u.atlas_rename.name = (char *)"three";
    req.expected_revision = tp_model_revision(m);
    next_txn_id(req.id_hex);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_FALSE(tp_model_can_redo(m));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(m));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_STRING("three", tp_model_project(m)->atlases[0].name);

    /* undo the new step -> back to A byte-identical. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    char *Ap = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(A, Ap);
    free(A);
    free(Ap);
    tp_model_destroy(m);
}

/* ---- multi-step undo/redo (a 3-transaction stack) ------------------------ */

void test_multi_step_undo_redo(void) {
    tp_model *m = fresh();
    tp_error err;
    const char *names[3] = {"n1", "n2", "n3"};
    char *snaps[4];
    snaps[0] = tp_test_serialize_project(tp_model_project(m));
    tp_operation op;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    for (int i = 0; i < 3; i++) {
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_RENAME;
        op.atlas_id = a0_id(m);
        op.u.atlas_rename.name = (char *)names[i];
        req.expected_revision = tp_model_revision(m);
        next_txn_id(req.id_hex);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
        tp_txn_result_free(&res);
        snaps[i + 1] = tp_test_serialize_project(tp_model_project(m));
    }
    /* undo all the way down, checking each intermediate state byte-identical. */
    for (int i = 2; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
        char *cur = tp_test_serialize_project(tp_model_project(m));
        TEST_ASSERT_EQUAL_STRING(snaps[i], cur);
        free(cur);
    }
    TEST_ASSERT_FALSE(tp_model_can_undo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_model_undo(m, &err)); /* empty */
    /* redo all the way up. */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
        char *cur = tp_test_serialize_project(tp_model_project(m));
        TEST_ASSERT_EQUAL_STRING(snaps[i + 1], cur);
        free(cur);
    }
    TEST_ASSERT_FALSE(tp_model_can_redo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_model_redo(m, &err));
    for (int i = 0; i < 4; i++) {
        free(snaps[i]);
    }
    tp_model_destroy(m);
}

/* ---- dirty stays identity-derived across Undo; label/author carry through -- */

void test_dirty_clean_after_undo_to_saved_baseline(void) {
    tp_model *m = fresh();
    tp_error err;
    TEST_ASSERT_FALSE(tp_model_dirty(m));
    tp_model_mark_saved(m); /* baseline = A */

    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"dirtied";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.label = (char *)"rename atlas";
    req.author = (char *)"tester";
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_TRUE(tp_model_dirty(m));
    TEST_ASSERT_EQUAL_STRING("rename atlas", tp_model_undo_label(m)); /* metadata carried */
    TEST_ASSERT_EQUAL_STRING("tester", tp_model_undo_author(m));

    int64_t rev_dirty = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_TRUE(tp_model_revision(m) > rev_dirty); /* Undo is a HIGHER revision */
    TEST_ASSERT_FALSE(tp_model_dirty(m));               /* yet clean: identity, not revision */
    tp_model_destroy(m);
}

/* ---- no history attached => exactly the F2-02 behavior ------------------- */

void test_no_history_is_f2_02_behavior(void) {
    tp_model *m = tp_model_wrap(make_base()); /* history NOT enabled */
    tp_error err;
    TEST_ASSERT_FALSE(tp_model_has_history(m));
    TEST_ASSERT_FALSE(tp_model_can_undo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_model_undo(m, &err)); /* no history */

    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"x";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_FALSE(tp_model_can_undo(m)); /* still nothing captured */
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* ---- [1]/[2] defensive capture: enabling history NEVER changes the "invalid input
 *       => structured error, model byte-unchanged, no crash" behavior --------------- */

static char *dup_cstr(const char *s) {
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(c);
    memcpy(c, s, n);
    return c;
}

/* The observable result of applying one op -- for enabled-vs-disabled equality. */
typedef struct apply_probe {
    tp_status st;
    bool committed;
    int64_t revision;
    tp_status err0_code;
    int err0_op;
    char *serial; /* the model AFTER the apply (owned) */
} apply_probe;

static apply_probe apply_one(tp_model *m, tp_operation *op) {
    tp_error err;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = op;
    req.op_count = 1;
    tp_txn_result res;
    apply_probe pr = {0};
    pr.st = tp_model_apply(m, &req, &res, &err);
    pr.committed = res.committed;
    pr.revision = res.revision;
    pr.err0_code = res.error_count > 0 ? res.errors[0].code : TP_STATUS_OK;
    pr.err0_op = res.error_count > 0 ? res.errors[0].op_index : -999;
    tp_txn_result_free(&res);
    pr.serial = tp_test_serialize_project(tp_model_project(m));
    return pr;
}

/* The SAME hostile op applied to a history-ENABLED and a history-DISABLED model (both
 * a fresh, deterministically-promoted make_base, so entity ids match) must reject with
 * the SAME machine-contract result -- status, primary error code, offending op index,
 * committed=false -- and leave EACH model byte-unchanged with no captured history. */
static void assert_enabled_equals_disabled(tp_operation *op, tp_status want) {
    tp_model *me = tp_model_wrap(make_base());
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(me));
    tp_model *md = tp_model_wrap(make_base()); /* history off */
    char *before_e = tp_test_serialize_project(tp_model_project(me));
    char *before_d = tp_test_serialize_project(tp_model_project(md));

    apply_probe pe = apply_one(me, op);
    apply_probe pd = apply_one(md, op);

    TEST_ASSERT_EQUAL_INT(want, pd.st);        /* the F2-02 (history-less) baseline */
    TEST_ASSERT_EQUAL_INT(want, pe.st);        /* history on: identical status, no crash */
    TEST_ASSERT_FALSE(pd.committed);
    TEST_ASSERT_FALSE(pe.committed);
    TEST_ASSERT_EQUAL_INT(pd.err0_code, pe.err0_code); /* same primary error code */
    TEST_ASSERT_EQUAL_INT(want, pe.err0_code);
    TEST_ASSERT_EQUAL_INT(pd.err0_op, pe.err0_op);     /* same offending op index */
    TEST_ASSERT_EQUAL_STRING(before_d, pd.serial);     /* model byte-unchanged (baseline) */
    TEST_ASSERT_EQUAL_STRING(before_e, pe.serial);     /* model byte-unchanged (history on) */
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(me)); /* nothing captured */
    TEST_ASSERT_FALSE(tp_model_can_undo(me));

    free(before_e);
    free(before_d);
    free(pe.serial);
    free(pd.serial);
    tp_model_destroy(me);
    tp_model_destroy(md);
}

/* the deterministic base ids (make_base promotes with a fixed rng, so every instance
 * shares these). */
static tp_id128 base_atlas0_id(void) {
    tp_model *ref = tp_model_wrap(make_base());
    tp_id128 id = tp_model_project(ref)->atlases[0].id;
    tp_model_destroy(ref);
    return id;
}
static tp_id128 base_anim0_id(void) {
    tp_model *ref = tp_model_wrap(make_base());
    tp_id128 id = tp_model_project(ref)->atlases[0].animations[0].id;
    tp_model_destroy(ref);
    return id;
}

void test_capture_dangling_atlas_rename(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_test_id_of(0xEE); /* no such atlas */
    op.u.atlas_rename.name = (char *)"x";
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_validation_precedes_missing_parent(void) {
    char invalid_name[] = {'b', (char)0xC3, 'x', '\0'};
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_test_id_of(0xEE);
    op.u.atlas_rename.name = invalid_name;
    assert_enabled_equals_disabled(&op, TP_STATUS_INVALID_UTF8);
}

void test_capture_dangling_atlas_settings(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = tp_test_id_of(0xEE);
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    op.u.atlas_settings.max_size = 1234;
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_atlas_remove(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = tp_test_id_of(0xEE);
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_source_remove(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = base_atlas0_id(); /* valid atlas ... */
    op.u.source_ref.source_id = tp_test_id_of(0xEE); /* ... dangling source */
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_sprite_atlas(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = tp_test_id_of(0xEE); /* dangling atlas -> grab_sprite must not deref NULL */
    op.u.sprite_set.source_id = tp_test_id_of(0x01);
    op.u.sprite_set.src_key = (char *)"hero.png";
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_anim_settings(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = base_atlas0_id();
    op.u.anim_settings.anim_id = tp_test_id_of(0xEE); /* dangling animation */
    op.u.anim_settings.mask = TP_ANF_FPS;
    op.u.anim_settings.fps = 12.0F;
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_frame_remove_anim(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = base_atlas0_id();
    op.u.anim_frame_rm.anim_id = tp_test_id_of(0xEE); /* dangling animation */
    op.u.anim_frame_rm.index = 0;
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

void test_capture_dangling_target_set(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = base_atlas0_id();
    op.u.target_set.target_id = tp_test_id_of(0xEE); /* dangling target */
    op.u.target_set.mask = TP_TF_ALL;
    op.u.target_set.exporter_id = (char *)"defold";
    op.u.target_set.out_path = (char *)"out/x";
    op.u.target_set.enabled = true;
    assert_enabled_equals_disabled(&op, TP_STATUS_NOT_FOUND);
}

/* [2] the frame.remove index is read from the op BEFORE apply validates it: an
 * out-of-range index must yield OUT_OF_BOUNDS (not an OOB read), identical to the
 * history-less path. */
void test_capture_oob_frame_remove_index(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = base_atlas0_id();
    op.u.anim_frame_rm.anim_id = base_anim0_id(); /* walk: 2 frames */
    op.u.anim_frame_rm.index = 99;                /* out of range */
    assert_enabled_equals_disabled(&op, TP_STATUS_OUT_OF_BOUNDS);
}

/* ---- [3] reserve-OOM leaves the transaction id un-recorded / retryable ------------ */

void test_reserve_oom_leaves_id_retryable(void) {
    tp_model *m = fresh(); /* history enabled */
    tp_error err;
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)"renamed";

    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex); /* ONE fixed id, reused on the retry */
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    char *before = tp_test_serialize_project(tp_model_project(m));

    /* history_reserve OOMs on this commit: it must fail cleanly BEFORE idstore->record. */
    tp_history__test_fail_next_reserve();
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* unchanged */
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(m)); /* no history entry */
    char *now = tp_test_serialize_project(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, now); /* model byte-unchanged */
    free(now);
    tp_txn_result_free(&res);

    /* Retry the SAME id at the SAME revision: the id was NOT recorded, so it is not
     * poisoned to DUPLICATE_ID -- it commits. (With reserve AFTER record this rejects.) */
    tp_txn_result res2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res2, &err));
    TEST_ASSERT_TRUE(res2.committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(m));
    tp_txn_result_free(&res2);
    free(before);
    tp_model_destroy(m);
}

/* ---- M3 bounded history admission + post-ACK FIFO ----------------------- */

static tp_status apply_rename_result(tp_model *m, const char *name, tp_txn_result *res) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = a0_id(m);
    op.u.atlas_rename.name = (char *)name;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_error err = {0};
    memset(res, 0, sizeof *res);
    return tp_model_apply(m, &req, res, &err);
}

static void commit_rename_ok(tp_model *m, const char *name) {
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, apply_rename_result(m, name, &res));
    TEST_ASSERT_TRUE(res.committed);
    tp_txn_result_free(&res);
}

void test_history_record_exact_byte_boundary_and_oversize_reject(void) {
    /* First measure the real owned components of this semantic diff. */
    tp_model *probe = fresh();
    commit_rename_ok(probe, "bounded");
    const size_t bytes = tp_model_history(probe)->records[0]->bytes;
    TEST_ASSERT_GREATER_THAN(sizeof(tp_diff_record) + sizeof(tp_diff_op), bytes);
    TEST_ASSERT_EQUAL_UINT64(bytes, tp_model_history(probe)->bytes);
    tp_model_destroy(probe);

    /* Exact boundary is admitted. */
    tp_history__test_set_limits(1, bytes, bytes);
    tp_model *exact = fresh();
    commit_rename_ok(exact, "bounded");
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(exact));
    TEST_ASSERT_EQUAL_UINT64(bytes, tp_model_history(exact)->bytes);
    tp_model_destroy(exact);

    /* One byte below the measured record rejects before model publication. */
    tp_history__test_set_limits(1, bytes - 1U, bytes - 1U);
    tp_model *over = fresh();
    char *before = tp_test_serialize_project(tp_model_project(over));
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, apply_rename_result(over, "bounded", &res));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, res.errors[0].code);
    TEST_ASSERT_EQUAL_STRING("history", res.errors[0].field);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(over));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(over));
    char *after = tp_test_serialize_project(tp_model_project(over));
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(over);
}

void test_history_step_budget_discards_redo_then_evicts_fifo(void) {
    tp_history__test_set_limits(2, TP_HISTORY_MAX_BYTES, TP_HISTORY_MAX_RECORD_BYTES);
    tp_model *m = fresh();
    tp_error err = {0};

    commit_rename_ok(m, "A");
    commit_rename_ok(m, "B");
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m)); /* exact step boundary */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("A", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(1, tp_model_redo_depth(m));

    commit_rename_ok(m, "C"); /* atomically drops B's redo branch */
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT(0, tp_model_redo_depth(m));
    commit_rename_ok(m, "D"); /* FIFO-evicts A; retained records are C,D */
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("C", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("A", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_FALSE(tp_model_can_undo(m));
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(TP_HISTORY_MAX_BYTES, tp_model_history(m)->bytes);
    tp_model_destroy(m);
}

void test_history_total_byte_budget_evicts_fifo_at_exact_boundary(void) {
    tp_model *probe = fresh();
    commit_rename_ok(probe, "AAAAAA"); /* same byte length as the original atlas1 */
    const size_t record_bytes = tp_model_history(probe)->records[0]->bytes;
    tp_model_destroy(probe);

    tp_history__test_set_limits(10, record_bytes * 2U, record_bytes);
    tp_model *m = fresh();
    commit_rename_ok(m, "AAAAAA");
    commit_rename_ok(m, "BBBBBB");
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_UINT64(record_bytes * 2U, tp_model_history(m)->bytes); /* exact total boundary */

    commit_rename_ok(m, "CCCCCC"); /* byte budget, not step budget, evicts oldest */
    TEST_ASSERT_EQUAL_INT(2, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_UINT64(record_bytes * 2U, tp_model_history(m)->bytes);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("BBBBBB", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("AAAAAA", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_FALSE(tp_model_can_undo(m));
    tp_model_destroy(m);
}

/* ---- [5a] all-fields completeness oracle: the safety net for the deep-copy fork ---- */

/* An atlas whose EVERY persistent field -- and every field of every child entity kind
 * (source, sparse sprite, animation + frames, target), including a RESOLVED sprite and
 * frame (source_ref + src_key non-default) -- is non-default. Removing it captures the
 * whole subtree through fill_atlas + every per-kind fork copy; a field the fork forgets
 * to copy makes the inverse restore non-byte-identical and FAILS the oracle here. */
static tp_project *make_maximal(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    int keep = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "keep", &keep)); /* realloc BEFORE we cache a */
    tp_project_atlas *a = &p->atlases[0];
    free(a->name);
    a->name = dup_cstr("maxatlas");
    a->max_size = 4096;
    a->padding = 9;
    a->margin = 3;
    a->extrude = 5;
    a->alpha_threshold = 123;
    a->max_vertices = 7;
    a->shape = 2;
    a->allow_transform = false; /* default true  */
    a->power_of_two = true;     /* default false */
    a->pixels_per_unit = 3.5F;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source_kind(a, "art/hero", TP_SOURCE_KIND_FOLDER));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source_kind(a, "art/tiles.png", TP_SOURCE_KIND_FILE));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source_kind(a, "art/unused.png", TP_SOURCE_KIND_FILE));

    /* Give sources stable ids before their canonical {source,key} refs are
     * constructed. Structural entities added below receive their ids in the
     * second assignment pass. */
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    const tp_id128 source_folder = a->sources[0].id;
    const tp_id128 source_file = a->sources[1].id;

    /* sprite A: canonical {source,key} with every other field non-default. */
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_sprite_by_source_key(
                              a, source_folder, "hero/walk_01", &sp));
    sp->origin_x = 0.25F;
    sp->origin_y = 0.75F;
    sp->slice9_lrtb[0] = 4;
    sp->slice9_lrtb[1] = 5;
    sp->slice9_lrtb[2] = 8;
    sp->slice9_lrtb[3] = 9;
    sp->rename = dup_cstr("player_walk_01");
    sp->ov_shape = 0;
    sp->ov_allow_rotate = 0;
    sp->ov_max_vertices = 6;
    sp->ov_margin = 3;
    sp->ov_extrude = 5;
    /* sprite B: exercises copy_sprite_fields' canonical source/key duplicate. */
    tp_project_sprite *sp2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_sprite_by_source_key(
                              a, source_file, "grass.png", &sp2));
    sp2->origin_x = 0.1F;

    /* animation: every field non-default + canonical frames. */
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(an, source_folder,
                                                    "hero/walk_01"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(an, source_folder,
                                                    "hero/walk_02"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_anim_add_frame(an, source_file,
                                                    "grass.png"));
    an->fps = 24.0F;
    an->playback = 2;
    an->flip_h = true;
    an->flip_v = true;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out/hero.json", NULL));
    tp_project_target *t2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "defold", "out/hero.tpinfo", &t2));
    t2->enabled = false; /* default true */

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    return p;
}

static void run_remove_oracle(tp_op_kind kind, tp_id128 anim_id, int frame_or_pos_unused) {
    (void)frame_or_pos_unused;
    tp_model *m = tp_model_wrap(make_maximal());
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    const tp_project_atlas *a = &tp_model_project(m)->atlases[0];
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = kind;
    op.atlas_id = a->id;
    switch (kind) {
        case TP_OP_SOURCE_REMOVE: op.u.source_ref.source_id = a->sources[2].id; break; /* the unreferenced FILE source */
        case TP_OP_ANIMATION_REMOVE: op.u.anim_ref.anim_id = anim_id; break;
        case TP_OP_TARGET_REMOVE: op.u.target_ref.target_id = a->targets[1].id; break; /* the disabled target */
        default: break; /* atlas.remove: atlas_id only */
    }
    run_oracle(m, &op, 1);
    tp_model_destroy(m);
}

/* atlas.remove of the maximal atlas: fill_atlas + every per-kind fork copy at once. */
void test_completeness_oracle_atlas_remove(void) { run_remove_oracle(TP_OP_ATLAS_REMOVE, tp_id128_nil(), 0); }
/* each kind's STANDALONE copy_elem fork copy, all fields non-default. */
void test_completeness_oracle_source_remove(void) { run_remove_oracle(TP_OP_SOURCE_REMOVE, tp_id128_nil(), 0); }
void test_completeness_oracle_target_remove(void) { run_remove_oracle(TP_OP_TARGET_REMOVE, tp_id128_nil(), 0); }
void test_completeness_oracle_anim_remove(void) {
    tp_model *ref = tp_model_wrap(make_maximal());
    tp_id128 anid = tp_model_project(ref)->atlases[0].animations[0].id;
    tp_model_destroy(ref);
    run_remove_oracle(TP_OP_ANIMATION_REMOVE, anid, 0);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_history_codec_atlas_name_roundtrip_both_directions);
    RUN_TEST(test_history_codec_rejects_malformed_known_shape_but_falls_back_for_unknown_shape);
    RUN_TEST(test_hostile_collection_removals_require_position_identity);
    RUN_TEST(test_hostile_sprite_remove_and_replace_require_position_identity);
    RUN_TEST(test_history_codec_atomic_apply_rejects_noncanonical_result);
    RUN_TEST(test_oracle_atlas_create);
    RUN_TEST(test_oracle_atlas_remove);
    RUN_TEST(test_oracle_atlas_rename);
    RUN_TEST(test_oracle_atlas_settings_set);
    RUN_TEST(test_oracle_source_add);
    RUN_TEST(test_oracle_source_remove);
    RUN_TEST(test_oracle_source_replace);
    RUN_TEST(test_oracle_sprite_override_set);
    RUN_TEST(test_oracle_sprite_override_clear);
    RUN_TEST(test_oracle_sprite_name_set);
    RUN_TEST(test_oracle_animation_create);
    RUN_TEST(test_oracle_animation_remove);
    RUN_TEST(test_oracle_animation_rename);
    RUN_TEST(test_anim_rename_undo_redo_name);
    RUN_TEST(test_oracle_animation_settings_set);
    RUN_TEST(test_oracle_animation_frames_set);
    RUN_TEST(test_oracle_animation_frame_add);
    RUN_TEST(test_oracle_animation_frame_remove);
    RUN_TEST(test_oracle_animation_frame_move);
    RUN_TEST(test_oracle_target_create);
    RUN_TEST(test_oracle_target_remove);
    RUN_TEST(test_oracle_target_set);
    RUN_TEST(test_cascade_atlas_remove_full_subtree);
    RUN_TEST(test_source_remove_rejects_referenced_source_atomically);
    RUN_TEST(test_batch_100_animations_one_inverse);
    RUN_TEST(test_batch_mixed_intrabatch);
    RUN_TEST(test_inverse_alloc_failure_rolls_back);
    RUN_TEST(test_capture_alloc_failure_fails_commit_atomically);
    RUN_TEST(test_corrupted_diff_unknown_atlas);
    RUN_TEST(test_corrupted_diff_bad_position);
    RUN_TEST(test_redo_branch_discard);
    RUN_TEST(test_multi_step_undo_redo);
    RUN_TEST(test_dirty_clean_after_undo_to_saved_baseline);
    RUN_TEST(test_no_history_is_f2_02_behavior);
    /* [1]/[2] defensive capture: history on == history off on hostile input, no crash */
    RUN_TEST(test_capture_dangling_atlas_rename);
    RUN_TEST(test_capture_validation_precedes_missing_parent);
    RUN_TEST(test_capture_dangling_atlas_settings);
    RUN_TEST(test_capture_dangling_atlas_remove);
    RUN_TEST(test_capture_dangling_source_remove);
    RUN_TEST(test_capture_dangling_sprite_atlas);
    RUN_TEST(test_capture_dangling_anim_settings);
    RUN_TEST(test_capture_dangling_frame_remove_anim);
    RUN_TEST(test_capture_dangling_target_set);
    RUN_TEST(test_capture_oob_frame_remove_index);
    /* [3] reserve-OOM keeps the transaction id retryable */
    RUN_TEST(test_reserve_oom_leaves_id_retryable);
    RUN_TEST(test_history_record_exact_byte_boundary_and_oversize_reject);
    RUN_TEST(test_history_step_budget_discards_redo_then_evicts_fifo);
    RUN_TEST(test_history_total_byte_budget_evicts_fifo_at_exact_boundary);
    /* [5a] all-fields completeness oracle for the deep-copy fork */
    RUN_TEST(test_completeness_oracle_atlas_remove);
    RUN_TEST(test_completeness_oracle_source_remove);
    RUN_TEST(test_completeness_oracle_target_remove);
    RUN_TEST(test_completeness_oracle_anim_remove);
    return UNITY_END();
}
