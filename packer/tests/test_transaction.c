/* F2-02: atomic transactions, revision, semantic dirty, idempotency, and the
 * versioned transaction request/result JSON contract. Proves the ENGINE (the
 * shipping CLI/GUI cutover is F2-05, not wired here):
 *   - deep-clone byte-identity + OOM-safe fault sweep;
 *   - atomicity: op N failing (validate or allocator) leaves ops 1..N-1 UNAPPLIED
 *     (live model byte-unchanged) and the revision unchanged;
 *   - clone-alloc fault at apply + per-op staging-alloc fault -> model byte-unchanged;
 *   - expected_revision below/above/equal; retry same id -> duplicate_id;
 *   - dirty = semantic identity (edit -> save -> edit -> inverse -> clean at a higher
 *     revision); mark-saved does not change revision;
 *   - full-batch validation ordering: structural fail-fast, revision short-circuit
 *     ALONE (op_index -1), per-op shape collect-all in (op_index, field) order;
 *   - CLI batch JSON golden: canonical byte-stable encode + the property that a
 *     decoded batch applies == the same ops applied one-by-one via F2-01;
 *   - UB-free number handling (5000000000 stable, 1e300 rejected, out-of-range int).
 */

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"          /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"
#include "tp_txn_internal.h"            /* clone fault seam */
#include "tp_op_internal.h"             /* tp_op__test_set_alloc_fail */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {
    tp_project__test_set_clone_alloc_fail(-1);
    tp_op__test_set_alloc_fail(-1);
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

/* One default atlas + one folder source + one json-neotolis target, ids promoted. */
static tp_project *base_project(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    uint8_t ctr = 1;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    return p;
}

static char *serialize(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save_buffer(p, &buf, &len, &err));
    return buf;
}

/* ---- deep-clone ---------------------------------------------------------- */

void test_clone_byte_identity(void) {
    tp_project *p = base_project();
    /* enrich: a sprite override + an animation with frames */
    tp_project_atlas *a = &p->atlases[0];
    tp_project_sprite *s = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "hero", &s));
    s->origin_x = 0.25F;
    s->slice9_lrtb[0] = 3;
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero2"));

    tp_project *c = tp_project_clone(p);
    TEST_ASSERT_NOT_NULL(c);
    char *bp = serialize(p);
    char *bc = serialize(c);
    TEST_ASSERT_EQUAL_STRING(bp, bc);

    /* independence: mutating the clone does not touch the original */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(&c->atlases[0], "changed"));
    char *bp2 = serialize(p);
    TEST_ASSERT_EQUAL_STRING(bp, bp2);

    free(bp);
    free(bc);
    free(bp2);
    tp_project_destroy(c);
    tp_project_destroy(p);
}

void test_clone_alloc_fault_sweep(void) {
    tp_project *p = base_project();
    /* Count the allocations a full clone needs. */
    tp_project *ok = tp_project_clone(p);
    TEST_ASSERT_NOT_NULL(ok);
    int total = tp_project__test_clone_alloc_count();
    tp_project_destroy(ok);
    TEST_ASSERT_TRUE(total > 1);

    /* Fail at every staging depth: each must return NULL (leak-checked by ASan/LSan
     * in CI) and leave the source usable. */
    for (int n = 0; n < total; n++) {
        tp_project__test_set_clone_alloc_fail(n);
        tp_project *c = tp_project_clone(p);
        TEST_ASSERT_NULL(c);
    }
    tp_project__test_set_clone_alloc_fail(-1);
    tp_project *c2 = tp_project_clone(p);
    TEST_ASSERT_NOT_NULL(c2);
    tp_project_destroy(c2);
    tp_project_destroy(p);
}

/* ---- revision precondition ----------------------------------------------- */

void test_revision_check(void) {
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_revision_check(3, 3, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_REVISION_CONFLICT, tp_revision_check(2, 3, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION, tp_revision_check(4, 3, &err));
}

/* ---- typed request builders (stack ops; literal strings; NOT heap-freed) -- */

static void op_atlas_settings(tp_operation *op, tp_id128 atlas, int max_size, int padding) {
    memset(op, 0, sizeof *op);
    op->kind = TP_OP_ATLAS_SETTINGS_SET;
    op->atlas_id = atlas;
    op->u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_PADDING;
    op->u.atlas_settings.max_size = max_size;
    op->u.atlas_settings.padding = padding;
}
static void op_atlas_rename(tp_operation *op, tp_id128 atlas, const char *name) {
    memset(op, 0, sizeof *op);
    op->kind = TP_OP_ATLAS_RENAME;
    op->atlas_id = atlas;
    op->u.atlas_rename.name = (char *)name;
}

/* ---- atomic commit + revision ------------------------------------------- */

void test_commit_and_revision(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));

    tp_operation ops[2];
    op_atlas_settings(&ops[0], aid, 2048, 4);
    op_atlas_rename(&ops[1], aid, "renamed");
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "0123456789abcdef0123456789abcdef");
    req.expected_revision = 0;
    req.ops = ops;
    req.op_count = 2;

    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT64(1, res.revision);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(2, res.op_count);
    TEST_ASSERT_EQUAL_STRING("atlas.settings.set", res.ops[0].wire);
    TEST_ASSERT_EQUAL_STRING("renamed", tp_model_project(m)->atlases[0].name);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* property: applying the batch == applying the same ops one-by-one via F2-01. */
void test_batch_equals_one_by_one(void) {
    tp_project *p1 = base_project();
    tp_id128 aid = p1->atlases[0].id;
    tp_operation ops[2];
    op_atlas_settings(&ops[0], aid, 1024, 2);
    op_atlas_rename(&ops[1], aid, "batched");

    tp_model *m = tp_model_wrap(p1);
    tp_txn_request req = {0};
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    req.schema = TP_TXN_SCHEMA;
    req.ops = ops;
    req.op_count = 2;
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    char *batched = serialize(tp_model_project(m));
    tp_txn_result_free(&res);

    tp_project *p2 = base_project(); /* identical initial state, same ids (deterministic) */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p2, &ops[0], &rej));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p2, &ops[1], &rej));
    char *onebyone = serialize(p2);

    TEST_ASSERT_EQUAL_STRING(onebyone, batched);
    free(batched);
    free(onebyone);
    tp_model_destroy(m);
    tp_project_destroy(p2);
}

/* atomicity: op N (here op1) fails semantic validation -> nothing applied. */
void test_atomicity_op_fails(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    char *before = serialize(tp_model_project(m));

    tp_operation ops[2];
    op_atlas_settings(&ops[0], aid, 2048, 4);   /* valid */
    memset(&ops[1], 0, sizeof ops[1]);          /* invalid: settings on a nonexistent atlas */
    ops[1].kind = TP_OP_ATLAS_SETTINGS_SET;
    ops[1].atlas_id = id_of(0xEE);
    ops[1].u.atlas_settings.mask = TP_AF_MAX_SIZE;
    ops[1].u.atlas_settings.max_size = 512;

    tp_txn_request req = {0};
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    req.schema = TP_TXN_SCHEMA;
    req.ops = ops;
    req.op_count = 2;
    tp_txn_result res;
    tp_error err;
    tp_status st = tp_model_apply(m, &req, &res, &err);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, st);
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(1, res.errors[0].op_index); /* op1 is the offender */
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* unchanged */

    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after); /* op0 NOT applied: model byte-unchanged */
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* clone-alloc fault at apply time -> model byte-unchanged, revision unchanged. */
void test_apply_clone_fault(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    char *before = serialize(tp_model_project(m));

    tp_operation ops[1];
    op_atlas_rename(&ops[0], aid, "x");
    tp_txn_request req = {0};
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "cccccccccccccccccccccccccccccccc");
    req.schema = TP_TXN_SCHEMA;
    req.ops = ops;
    req.op_count = 1;

    tp_project__test_set_clone_alloc_fail(0); /* fail the very first clone alloc */
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* per-op staging-alloc fault (animation.create frames) mid-batch -> unchanged. */
void test_apply_per_op_alloc_fault(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    char *before = serialize(tp_model_project(m));

    tp_operation ops[2];
    op_atlas_settings(&ops[0], aid, 2048, 4); /* op0 valid */
    memset(&ops[1], 0, sizeof ops[1]);        /* op1: anim.create with frames (compound staging) */
    ops[1].kind = TP_OP_ANIMATION_CREATE;
    ops[1].atlas_id = aid;
    ops[1].u.anim_create.anim_id = id_of(0xC7);
    ops[1].u.anim_create.name = (char *)"walk";
    ops[1].u.anim_create.fps = 12.0F;
    static char *frames[2] = {(char *)"a", (char *)"b"};
    ops[1].u.anim_create.frames = frames;
    ops[1].u.anim_create.frame_count = 2;

    tp_txn_request req = {0};
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "dddddddddddddddddddddddddddddddd");
    req.schema = TP_TXN_SCHEMA;
    req.ops = ops;
    req.op_count = 2;

    tp_op__test_set_alloc_fail(0); /* first staging alloc inside anim.create fails */
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after); /* op0 rolled back with the clone */
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* ---- expected_revision semantics ---------------------------------------- */

void test_expected_revision(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_operation op;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    tp_error err;

    op_atlas_rename(&op, aid, "one");
    req.expected_revision = 0;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "1000000000000000000000000000000a");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err)); /* rev -> 1 */
    tp_txn_result_free(&res);

    op_atlas_rename(&op, aid, "two");
    req.expected_revision = 0; /* stale */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "1000000000000000000000000000000b");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_REVISION_CONFLICT, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(-1, res.errors[0].op_index);
    tp_txn_result_free(&res);

    req.expected_revision = 5; /* never existed */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "1000000000000000000000000000000c");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);

    req.expected_revision = 1; /* current */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "1000000000000000000000000000000d");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err)); /* rev -> 2 */
    TEST_ASSERT_EQUAL_INT64(2, tp_model_revision(m));
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* ---- idempotency: retry the same id (with the STALE original revision) --- */

void test_idempotent_retry(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_operation op;
    op_atlas_rename(&op, aid, "renamed");
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    req.expected_revision = 0;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "abababababababababababababababab");
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err)); /* rev -> 1 */
    tp_txn_result_free(&res);

    char *before = serialize(tp_model_project(m));
    /* retry the SAME id with the SAME (now stale) expected_revision: idempotency is
     * checked BEFORE revision, so this is duplicate_id, not revision_conflict. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* ---- dirty = semantic identity (NOT revision) --------------------------- */

void test_dirty_is_semantic_identity(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_FALSE(tp_model_dirty(m)); /* freshly wrapped == clean */

    tp_operation op;
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    tp_error err;

    op_atlas_rename(&op, aid, "edited");
    req.expected_revision = 0;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "2000000000000000000000000000000a");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_TRUE(tp_model_dirty(m));

    int64_t rev_before_save = tp_model_revision(m);
    tp_model_mark_saved(m);
    TEST_ASSERT_FALSE(tp_model_dirty(m));
    TEST_ASSERT_EQUAL_INT64(rev_before_save, tp_model_revision(m)); /* save != revision bump */

    op_atlas_rename(&op, aid, "edited2");
    req.expected_revision = 1;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "2000000000000000000000000000000b");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_TRUE(tp_model_dirty(m));

    op_atlas_rename(&op, aid, "edited"); /* the INVERSE: back to the saved-baseline name */
    req.expected_revision = 2;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "2000000000000000000000000000000c");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    tp_txn_result_free(&res);
    TEST_ASSERT_TRUE(tp_model_revision(m) > rev_before_save); /* revision is HIGHER */
    TEST_ASSERT_FALSE(tp_model_dirty(m));                     /* yet clean: identity, not revision */
    tp_model_destroy(m);
}

void test_identity_excludes_runtime(void) {
    tp_project *p = base_project();
    tp_id128 before = tp_semantic_identity(p);
    /* the project file path is an identity KEY, not semantic content -> excluded. */
    free(p->project_dir);
    p->project_dir = NULL;
    char buf[8] = "some/x";
    p->project_dir = (char *)malloc(sizeof buf);
    memcpy(p->project_dir, buf, sizeof buf);
    tp_id128 after = tp_semantic_identity(p);
    TEST_ASSERT_TRUE(tp_id128_eq(before, after));

    /* schema_version is a serialization envelope -> excluded. */
    p->schema_version += 1;
    tp_id128 after2 = tp_semantic_identity(p);
    TEST_ASSERT_TRUE(tp_id128_eq(before, after2));
    tp_project_destroy(p);
}

/* identity is order-normalized for ID-keyed collections (targets). */
void test_identity_order_normalized(void) {
    tp_project *p = base_project();
    tp_project_atlas *a = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "defold", "out/b", NULL));
    /* promote the new target's id so it is addressable/stable */
    uint8_t ctr = 9;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    tp_id128 before = tp_semantic_identity(p);
    /* swap the two targets' array order: identity must not change */
    tp_project_target tmp = a->targets[0];
    a->targets[0] = a->targets[1];
    a->targets[1] = tmp;
    tp_id128 after = tp_semantic_identity(p);
    TEST_ASSERT_TRUE(tp_id128_eq(before, after));
    tp_project_destroy(p);
}

/* animation frame order IS semantic: reordering frames changes identity. */
void test_identity_frames_order_semantic(void) {
    tp_project *p = base_project();
    tp_project_atlas *a = &p->atlases[0];
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f0"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "f1"));
    tp_id128 before = tp_semantic_identity(p);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(an, 0, 1)); /* swap frame order */
    tp_id128 after = tp_semantic_identity(p);
    TEST_ASSERT_FALSE(tp_id128_eq(before, after));
    tp_project_destroy(p);
}

/* ---- JSON contract: validation ordering --------------------------------- */

void test_json_structural_fail_fast(void) {
    tp_model *m = tp_model_create();
    tp_txn_result res;
    tp_error err;
    /* malformed JSON */
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, "{not json", &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    tp_txn_result_free(&res);
    /* bad schema version */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION,
                          tp_model_apply_json(m, "{\"schema\":2,\"transaction\":{}}", &res, &err));
    tp_txn_result_free(&res);
    /* unknown envelope key */
    const char *bad = "{\"schema\":1,\"extra\":1,\"transaction\":{\"id\":\"" "00000000000000000000000000000000"
                      "\",\"expected_revision\":0,\"operations\":[]}}";
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, bad, &res, &err));
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* revision short-circuits ALONE (op_index -1) even with a malformed op present. */
void test_json_revision_short_circuit_alone(void) {
    tp_project *p = base_project();
    tp_model *m = tp_model_wrap(p);
    /* expected_revision 7 (> current 0) AND an unknown op: only the revision error. */
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":7,"
                       "\"operations\":[{\"op\":\"bogus.kind\"}]}}";
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION, tp_model_apply_json(m, json, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(-1, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION, res.errors[0].code);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* per-op shape faults collected in (op_index, field) order. */
void test_json_shape_collect_all(void) {
    tp_project *p = base_project();
    tp_model *m = tp_model_wrap(p);
    /* op0: known op with an UNKNOWN field; op1: known op with a MALFORMED atlas_id;
     * op2: UNKNOWN op wire. Expect three errors, in that order. */
    const char *json =
        "{\"schema\":1,\"transaction\":{"
        "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
        "{\"op\":\"atlas.remove\",\"atlas_id\":\"atlas_11111111111111111111111111111111\",\"bogus\":1},"
        "{\"op\":\"atlas.remove\",\"atlas_id\":\"atlas_xyz\"},"
        "{\"op\":\"not.a.real.op\"}"
        "]}}";
    tp_txn_result res;
    tp_error err;
    tp_status st = tp_model_apply_json(m, json, &res, &err);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, st);
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(3, res.error_count);
    TEST_ASSERT_EQUAL_INT(0, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_STRING("bogus", res.errors[0].field);
    TEST_ASSERT_EQUAL_INT(1, res.errors[1].op_index);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, res.errors[1].code);
    TEST_ASSERT_EQUAL_INT(2, res.errors[2].op_index);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_OP, res.errors[2].code);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* ---- CLI batch JSON golden: byte-stable encode + decode round-trip ------- */

void test_json_request_encode_golden(void) {
    tp_operation ops[2];
    op_atlas_settings(&ops[0], id_of(0xA1), 2048, 4);
    memset(&ops[1], 0, sizeof ops[1]);
    ops[1].kind = TP_OP_TARGET_CREATE;
    ops[1].atlas_id = id_of(0xA1);
    ops[1].u.target_create.target_id = id_of(0xB2);
    ops[1].u.target_create.exporter_id = (char *)"defold";
    ops[1].u.target_create.out_path = (char *)"out/x";
    ops[1].u.target_create.enabled = true;

    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "0123456789abcdef0123456789abcdef");
    req.expected_revision = 5000000000; /* > 2^32: PRId64 keeps it stable cross-OS */
    req.label = (char *)"batch";
    req.ops = ops;
    req.op_count = 2;

    char *json = tp_txn_request_encode(&req);
    TEST_ASSERT_NOT_NULL(json);
    static const char *golden =
        "{\n"
        "  \"schema\": 1,\n"
        "  \"transaction\": {\n"
        "    \"expected_revision\": 5000000000,\n"
        "    \"id\": \"0123456789abcdef0123456789abcdef\",\n"
        "    \"label\": \"batch\",\n"
        "    \"operations\": [\n"
        "      {\n"
        "        \"op\": \"atlas.settings.set\",\n"
        "        \"atlas_id\": \"atlas_a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1\",\n"
        "        \"max_size\": 2048,\n"
        "        \"padding\": 4\n"
        "      },\n"
        "      {\n"
        "        \"op\": \"target.create\",\n"
        "        \"atlas_id\": \"atlas_a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1\",\n"
        "        \"enabled\": true,\n"
        "        \"exporter_id\": \"defold\",\n"
        "        \"out_path\": \"out/x\",\n"
        "        \"target_id\": \"target_b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2\"\n"
        "      }\n"
        "    ]\n"
        "  }\n"
        "}\n";
    TEST_ASSERT_EQUAL_STRING(golden, json);

    /* decode -> re-encode is byte-identical (canonicalization is stable). */
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT64(5000000000, rd->expected_revision);
    char *json2 = tp_txn_request_encode(rd);
    TEST_ASSERT_EQUAL_STRING(json, json2);
    free(json);
    free(json2);
    tp_txn_request_free(rd);
}

/* the batch decoded from JSON applies == the same ops one-by-one via F2-01. */
void test_json_batch_equals_one_by_one(void) {
    tp_project *p1 = base_project();
    tp_id128 aid = p1->atlases[0].id;
    tp_operation ops[2];
    op_atlas_settings(&ops[0], aid, 1500, 3);
    op_atlas_rename(&ops[1], aid, "json-batched");
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "cafecafecafecafecafecafecafecafe");
    req.expected_revision = 0;
    req.ops = ops;
    req.op_count = 2;
    char *json = tp_txn_request_encode(&req);
    TEST_ASSERT_NOT_NULL(json);

    tp_model *m = tp_model_wrap(p1);
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply_json(m, json, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    char *batched = serialize(tp_model_project(m));
    /* the committed result encodes byte-stable too */
    char *rjson = tp_txn_result_encode(&res);
    TEST_ASSERT_NOT_NULL(rjson);
    TEST_ASSERT_NOT_NULL(strstr(rjson, "\"status\": \"committed\""));
    TEST_ASSERT_NOT_NULL(strstr(rjson, "\"revision\": 1"));
    tp_txn_result_free(&res);

    tp_project *p2 = base_project();
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p2, &ops[0], &rej));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p2, &ops[1], &rej));
    char *onebyone = serialize(p2);

    TEST_ASSERT_EQUAL_STRING(onebyone, batched);
    free(json);
    free(rjson);
    free(batched);
    free(onebyone);
    tp_model_destroy(m);
    tp_project_destroy(p2);
}

/* ---- UB-free number handling -------------------------------------------- */

void test_number_handling(void) {
    tp_model *m = tp_model_create();
    tp_txn_result res;
    tp_error err;
    /* expected_revision 1e300 is outside +/-2^53 -> structured reject, NO UB cast. */
    const char *big = "{\"schema\":1,\"transaction\":{"
                      "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":1e300,"
                      "\"operations\":[]}}";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_model_apply_json(m, big, &res, &err));
    tp_txn_result_free(&res);

    /* a fractional expected_revision is not an integer -> structured reject. */
    const char *frac = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":1.5,"
                       "\"operations\":[]}}";
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, frac, &res, &err));
    tp_txn_result_free(&res);
    tp_model_destroy(m);

    /* an int knob out of int range routes through the range-checked converter. */
    tp_project *p = base_project();
    char aid[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, p->atlases[0].id, aid, sizeof aid, &err));
    tp_model *m2 = tp_model_wrap(p);
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.settings.set\","
                   "\"atlas_id\":\"%s\",\"max_size\":9000000000}]}}",
                   aid);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_model_apply_json(m2, json, &res, &err));
    tp_txn_result_free(&res);
    tp_model_destroy(m2);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clone_byte_identity);
    RUN_TEST(test_clone_alloc_fault_sweep);
    RUN_TEST(test_revision_check);
    RUN_TEST(test_commit_and_revision);
    RUN_TEST(test_batch_equals_one_by_one);
    RUN_TEST(test_atomicity_op_fails);
    RUN_TEST(test_apply_clone_fault);
    RUN_TEST(test_apply_per_op_alloc_fault);
    RUN_TEST(test_expected_revision);
    RUN_TEST(test_idempotent_retry);
    RUN_TEST(test_dirty_is_semantic_identity);
    RUN_TEST(test_identity_excludes_runtime);
    RUN_TEST(test_identity_order_normalized);
    RUN_TEST(test_identity_frames_order_semantic);
    RUN_TEST(test_json_structural_fail_fast);
    RUN_TEST(test_json_revision_short_circuit_alone);
    RUN_TEST(test_json_shape_collect_all);
    RUN_TEST(test_json_request_encode_golden);
    RUN_TEST(test_json_batch_equals_one_by_one);
    RUN_TEST(test_number_handling);
    return UNITY_END();
}
