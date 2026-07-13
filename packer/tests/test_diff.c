/* F2-03: semantic diff / exact inverse (Undo) + redo replay + a minimal in-memory
 * history, proven by an ORACLE suite (the ENGINE; the GUI Undo cutover is F3-02):
 *   - per-op oracle for EVERY op kind: A -> forward transaction -> B -> inverse -> A'
 *     is BYTE-IDENTICAL to A, and equals the legacy FULL-SNAPSHOT restore
 *     (tp_project_save_buffer/load_buffer). redo -> B' is byte-identical to B;
 *   - reference cascade: a coarse atlas.remove (whole subtree) and a source.remove
 *     that sprites/frames reference -> inverse restores everything byte-identical;
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
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h" /* diff alloc seam + record/history internals (corrupt + rollback) */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {
    tp_diff__test_set_alloc_fail(-1);
    tp_diff__test_reset_alloc_count();
}

/* ---- fixtures ------------------------------------------------------------- */

static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}

static tp_id128 id_of(uint8_t b) {
    tp_id128 x;
    for (int i = 0; i < 16; i++) {
        x.bytes[i] = b;
    }
    return x;
}

static int s_id_ctr = 0;
static void next_txn_id(char *buf) { (void)snprintf(buf, 33, "%032x", (unsigned)(++s_id_ctr)); }

static char *serialize(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save_buffer(p, &buf, &len, &err));
    return buf;
}

/* A0 "atlas1" {source "sprites", target json-neotolis "out/a", anim "walk"[hero,hero2]}
 * + A1 "atlas2" (empty). All structural ids promoted (deterministic). */
static tp_project *make_base(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a0 = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a0, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a0, TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a0, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero2"));
    int idx = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", &idx));
    uint8_t ctr = 1;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
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
static tp_id128 tgt0_id(tp_model *m) { return tp_model_project(m)->atlases[0].targets[0].id; }
static tp_id128 anim0_id(tp_model *m) { return tp_model_project(m)->atlases[0].animations[0].id; }

/* ---- the oracle: A -> forward -> B -> inverse -> A' + redo -> B' ---------- */

static void run_oracle(tp_model *m, tp_operation *ops, int n) {
    tp_error err;
    char *A = serialize(tp_model_project(m));
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
    char *B = serialize(tp_model_project(m));

    /* inverse (Undo) -> A' */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_INT64(rev0 + 2, tp_model_revision(m)); /* Undo bumps the revision */
    char *Ap = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(A, Ap); /* diff restore is byte-identical to A */

    /* legacy full-snapshot restore of A == the diff restore. */
    tp_project *legacy = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(snapA, snapAlen, &legacy, &err));
    char *Aleg = serialize(legacy);
    TEST_ASSERT_EQUAL_STRING(Aleg, Ap); /* diff restore == full-snapshot restore */
    tp_project_destroy(legacy);

    /* redo -> B' */
    TEST_ASSERT_TRUE(tp_model_can_redo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
    TEST_ASSERT_EQUAL_INT64(rev0 + 3, tp_model_revision(m));
    char *Bp = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(B, Bp);

    free(A);
    free(B);
    free(Ap);
    free(Bp);
    free(Aleg);
    free(snapA);
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
    op.atlas_id = id_of(0x51);
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
    op.u.source_add.source_id = id_of(0x52);
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
    op.u.source_ref.source_id = src0_id(m);
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
    static char *frames[2] = {(char *)"hero", (char *)"hero2"};
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = a0_id(m);
    op.u.anim_create.anim_id = id_of(0x53);
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
    static char *frames[3] = {(char *)"a", (char *)"b", (char *)"c"};
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
    op.u.anim_frame_add.frame = (char *)"hero3";
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
    op.u.target_create.target_id = id_of(0x54);
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

/* Removing a source that a sprite override + an animation frame reference: the
 * dependent records orphan (keyed by id) but are byte-unchanged; the inverse
 * restores the source so the references resolve again. */
void test_cascade_source_remove_with_references(void) {
    tp_model *m = fresh();
    seed_sprite(m, "hero.png"); /* a sprite override bridging into the atlas */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = a0_id(m);
    op.u.source_ref.source_id = src0_id(m);
    run_oracle(m, &op, 1);
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
        ops[i].u.anim_create.anim_id = id_of((uint8_t)(0x80 + i));
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
    tp_id128 newa = id_of(0x51);
    tp_operation ops[4];
    memset(ops, 0, sizeof ops);
    ops[0].kind = TP_OP_ATLAS_CREATE;
    ops[0].atlas_id = newa;
    ops[0].u.atlas_create.name = (char *)"born";
    ops[1].kind = TP_OP_SOURCE_ADD;
    ops[1].atlas_id = newa;
    ops[1].u.source_add.source_id = id_of(0x52);
    ops[1].u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    ops[1].u.source_add.key = (char *)"born/sprites";
    ops[2].kind = TP_OP_ANIMATION_CREATE;
    ops[2].atlas_id = newa;
    ops[2].u.anim_create.anim_id = id_of(0x53);
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
    char *B = serialize(tp_model_project(m));
    int64_t revB = tp_model_revision(m);

    /* dry-count the diff allocations the inverse needs (on a throwaway clone). */
    tp_diff_record *r = tp_history_undo_record(m->history);
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
        char *now = serialize(tp_model_project(m));
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
    char *A = serialize(tp_model_project(probe));
    probe_op.kind = TP_OP_SOURCE_ADD;
    probe_op.atlas_id = a0_id(probe);
    probe_op.u.source_add.source_id = id_of(0x52);
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
        op.u.source_add.source_id = id_of(0x52);
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
        char *now = serialize(tp_model_project(m));
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
    char *B = serialize(tp_model_project(m));

    /* corrupt the record: a stale/unknown atlas id. */
    tp_diff_record *r = tp_history_undo_record(m->history);
    tp_id128 saved = r->ops[0].atlas_id;
    r->ops[0].atlas_id = id_of(0xEE);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_model_undo(m, &err)); /* structured, no crash */
    char *now = serialize(tp_model_project(m));
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
    op.u.source_ref.source_id = src0_id(m);
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    next_txn_id(req.id_hex);
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    char *B = serialize(tp_model_project(m));

    tp_diff_record *r = tp_history_undo_record(m->history);
    int saved = r->ops[0].position;
    r->ops[0].position = 999; /* out-of-range insert index */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_model_undo(m, &err));
    char *now = serialize(tp_model_project(m));
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
    char *A = serialize(tp_model_project(m));

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
    char *Ap = serialize(tp_model_project(m));
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
    snaps[0] = serialize(tp_model_project(m));
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
        snaps[i + 1] = serialize(tp_model_project(m));
    }
    /* undo all the way down, checking each intermediate state byte-identical. */
    for (int i = 2; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
        char *cur = serialize(tp_model_project(m));
        TEST_ASSERT_EQUAL_STRING(snaps[i], cur);
        free(cur);
    }
    TEST_ASSERT_FALSE(tp_model_can_undo(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_model_undo(m, &err)); /* empty */
    /* redo all the way up. */
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
        char *cur = serialize(tp_model_project(m));
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

int main(void) {
    UNITY_BEGIN();
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
    RUN_TEST(test_oracle_animation_settings_set);
    RUN_TEST(test_oracle_animation_frames_set);
    RUN_TEST(test_oracle_animation_frame_add);
    RUN_TEST(test_oracle_animation_frame_remove);
    RUN_TEST(test_oracle_animation_frame_move);
    RUN_TEST(test_oracle_target_create);
    RUN_TEST(test_oracle_target_remove);
    RUN_TEST(test_oracle_target_set);
    RUN_TEST(test_cascade_atlas_remove_full_subtree);
    RUN_TEST(test_cascade_source_remove_with_references);
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
    return UNITY_END();
}
