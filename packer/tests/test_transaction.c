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
#include "tp_project_clone_arena.h"     /* P-01: arena-backed clone under test */
#include "tp_core/tp_arena.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {
    tp_project__test_set_clone_alloc_fail(-1);
    tp_op__test_set_alloc_fail(-1);
    tp_txn__test_set_add_error_fail(-1);
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

/* ---- P-01 arena-backed clone: byte-identity pin --------------------------- */

static char *t_dup(const char *s) {
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    TEST_ASSERT_NOT_NULL(p);
    memcpy(p, s, n);
    return p;
}

/* A MAXIMAL project: every persistent field non-default -- the strongest possible
 * byte-identity pin for the clone fork (mirrors test_diff.c make_maximal). */
static tp_project *build_maximal(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(a, "maxatlas"));
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

    /* sprite A: pending name-bridge, every other field non-default (set FULLY before
     * the next add_sprite, which may realloc the sprites array and dangle `sp`). */
    tp_project_sprite *sp = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "hero/walk_01", &sp));
    sp->origin_x = 0.25F;
    sp->origin_y = 0.75F;
    sp->slice9_lrtb[0] = 4;
    sp->slice9_lrtb[1] = 5;
    sp->slice9_lrtb[2] = 8;
    sp->slice9_lrtb[3] = 9;
    sp->rename = t_dup("player_walk_01");
    sp->ov_shape = 0;
    sp->ov_allow_rotate = 0;
    sp->ov_max_vertices = 6;
    sp->ov_margin = 3;
    sp->ov_extrude = 5;
    /* sprite B: RESOLVED {source, key} -- exercises the src_key dup. */
    tp_project_sprite *sp2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(a, "grass", &sp2));
    sp2->origin_x = 0.1F;

    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(a, "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_01"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "hero/walk_02"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "grass")); /* -> resolved below */
    an->fps = 24.0F;
    an->playback = 2;
    an->flip_h = true;
    an->flip_v = true;

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "json-neotolis", "out/hero.json", NULL));
    tp_project_target *t2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, "defold", "out/hero.tpinfo", &t2));
    t2->enabled = false; /* default true */

    uint8_t ctr = 7;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    tp_id128 src_file = a->sources[1].id;
    sp2->source_ref = src_file;
    sp2->src_key = t_dup("grass.png");
    an->frames[2].source_ref = src_file;
    an->frames[2].src_key = t_dup("grass.png");
    return p;
}

/* The arena clone serializes BYTE-IDENTICAL to the source AND to the malloc clone,
 * on a maximal project; and is independent of later source mutation. No-leak is
 * structural: the whole clone is freed by one tp_arena_destroy (ASan/LSan in CI). */
void test_clone_arena_byte_identity(void) {
    tp_project *p = build_maximal();
    tp_project *mc = tp_project_clone(p); /* production malloc clone, cross-check */
    TEST_ASSERT_NOT_NULL(mc);

    size_t fp = tp_project_clone_arena_footprint(p);
    TEST_ASSERT_TRUE(fp > 0U);
    tp_arena *ar = tp_arena_create(fp);
    TEST_ASSERT_NOT_NULL(ar);
    tp_project *ac = tp_project_clone_into_arena(p, ar);
    TEST_ASSERT_NOT_NULL(ac);

    char *bp = serialize(p);
    char *bm = serialize(mc);
    char *ba = serialize(ac);
    TEST_ASSERT_EQUAL_STRING(bp, ba); /* arena clone == source: byte-identity is sacred */
    TEST_ASSERT_EQUAL_STRING(bm, ba); /* arena clone == malloc clone */

    /* independence: mutating the source does not perturb the arena clone's bytes. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_set_atlas_name(&p->atlases[0], "poked"));
    char *ba2 = serialize(ac);
    TEST_ASSERT_EQUAL_STRING(ba, ba2);

    free(bp);
    free(bm);
    free(ba);
    free(ba2);
    tp_arena_destroy(ar); /* one-shot free of the entire clone: no per-field leak */
    tp_project_destroy(mc);
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

/* H/P1-2: the new animation.rename op survives request encode -> decode -> re-encode
 * byte-identically (journal-safe: it round-trips through the closed wire vocabulary). */
void test_json_anim_rename_roundtrip(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_RENAME;
    op.atlas_id = id_of(0xA1);
    op.u.anim_rename.anim_id = id_of(0xC1);
    op.u.anim_rename.name = (char *)"renamed";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "0123456789abcdef0123456789abcdef");
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;

    char *json = tp_txn_request_encode(&req);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"op\": \"animation.rename\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"name\": \"renamed\""));

    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT(1, rd->op_count);
    TEST_ASSERT_EQUAL_INT(TP_OP_ANIMATION_RENAME, (int)rd->ops[0].kind);
    TEST_ASSERT_EQUAL_STRING("renamed", rd->ops[0].u.anim_rename.name);
    char *json2 = tp_txn_request_encode(rd);
    TEST_ASSERT_NOT_NULL(json2);
    TEST_ASSERT_EQUAL_STRING(json, json2); /* byte-identical re-encode */
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

/* ---- review-fix regressions (F2-02 fix) --------------------------------- */

/* [8] the typed path validates the transaction-id format (was: garbage id committed
 * and a second garbage id then collided on duplicate_id). */
void test_typed_malformed_id_rejected(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_operation op;
    op_atlas_rename(&op, aid, "x");
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    req.ops = &op;
    req.op_count = 1;
    req.expected_revision = 0;
    tp_txn_result res;
    tp_error err;

    const char *bad_ids[] = {
        "",                                  /* empty */
        "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz",   /* non-hex */
        "abcdef",                            /* too short */
        "0123456789abcdef0123456789abcde",   /* 31 (wrong length) */
        "0123456789ABCDEF0123456789ABCDEF",  /* uppercase: not lowercase hex */
    };
    for (size_t i = 0; i < sizeof bad_ids / sizeof bad_ids[0]; i++) {
        (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", bad_ids[i]);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_model_apply(m, &req, &res, &err));
        TEST_ASSERT_FALSE(res.committed);
        TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* nothing recorded, model untouched */
        tp_txn_result_free(&res);
    }
    /* nothing was recorded: a valid, distinct id still commits at revision 0. */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "00000000000000000000000000000000");
    op_atlas_rename(&op, aid, "y");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [1] tp_model_apply_json(m, json, NULL, err) never dereferences a NULL out, on any
 * reject path or the commit path. */
void test_json_null_out_no_crash(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    /* rejected: unknown envelope key */
    const char *envbad = "{\"schema\":1,\"extra\":1,\"transaction\":{"
                         "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":[]}}";
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, envbad, NULL, &err));
    /* rejected: per-op shape fault (unknown op) */
    const char *shapebad = "{\"schema\":1,\"transaction\":{"
                           "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,"
                           "\"operations\":[{\"op\":\"not.a.real.op\"}]}}";
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, shapebad, NULL, &err));
    /* rejected: malformed JSON */
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, "{not json", NULL, &err));
    /* committed with NULL out: the model still mutates + the revision still bumps. */
    char adhex[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, aid, adhex, sizeof adhex, &err));
    char json[400];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.rename\",\"atlas_id\":\"%s\","
                   "\"name\":\"z\"}]}}",
                   adhex);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply_json(m, json, NULL, &err));
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_STRING("z", tp_model_project(m)->atlases[0].name);
    tp_model_destroy(m);
}

/* [3] a malformed-JSON reject preserves the current revision (was: result.revision 0,
 * making the client believe the model reset). */
void test_json_malformed_preserves_revision(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    char adhex[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, aid, adhex, sizeof adhex, &err));
    char json[400];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.rename\",\"atlas_id\":\"%s\","
                   "\"name\":\"r1\"}]}}",
                   adhex);
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply_json(m, json, &res, &err)); /* revision -> 1 */
    TEST_ASSERT_EQUAL_INT64(1, res.revision);
    tp_txn_result_free(&res);

    /* malformed JSON at revision 1: result.revision must echo 1, not reset to 0. */
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, "{not json", &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(1, res.revision);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [6] present-but-non-string addressing ids are recorded in the collect-all pass, so
 * a whole batch's bad ids report together (was: only the first caught fail-fast in
 * lowering, one bad id per round-trip). */
void test_json_nonstring_id_collect_all(void) {
    tp_project *p = base_project();
    tp_model *m = tp_model_wrap(p);
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"atlas.remove\",\"atlas_id\":123},"
                       "{\"op\":\"atlas.remove\",\"atlas_id\":456}"
                       "]}}";
    tp_txn_result res;
    tp_error err;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, json, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(2, res.error_count); /* BOTH bad ids reported in one pass */
    TEST_ASSERT_EQUAL_INT(0, res.errors[0].op_index);
    TEST_ASSERT_EQUAL_STRING("atlas_id", res.errors[0].field);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, res.errors[0].code);
    TEST_ASSERT_EQUAL_INT(1, res.errors[1].op_index);
    TEST_ASSERT_EQUAL_STRING("atlas_id", res.errors[1].field);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, res.errors[1].code);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [0] sprite override int16 fields are range-checked BEFORE the narrowing cast, so an
 * out-of-range value rejects (was: 65535 wrapped to -1 == INHERIT and silently
 * dropped, apply COMMITTED). A valid value commits. */
void test_json_ov_int16_range(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_id128 src0 = p->atlases[0].sources[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    char adhex[TP_ID_TEXT_CAP];
    char sdhex[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, aid, adhex, sizeof adhex, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_SOURCE, src0, sdhex, sizeof sdhex, &err));

    const int bad[] = {65535, 65536, -40000};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char json[512];
        (void)snprintf(json, sizeof json,
                       "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                       "\"expected_revision\":0,\"operations\":[{\"op\":\"sprite.override.set\",\"atlas_id\":\"%s\","
                       "\"source_id\":\"%s\",\"src_key\":\"hero.png\",\"ov_margin\":%d}]}}",
                       adhex, sdhex, bad[i]);
        tp_txn_result res;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_model_apply_json(m, json, &res, &err));
        TEST_ASSERT_FALSE(res.committed);
        TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* nothing committed */
        tp_txn_result_free(&res);
    }
    /* a valid in-range override commits on the real source. */
    char okjson[512];
    (void)snprintf(okjson, sizeof okjson,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"sprite.override.set\",\"atlas_id\":\"%s\","
                   "\"source_id\":\"%s\",\"src_key\":\"hero.png\",\"ov_margin\":4}]}}",
                   adhex, sdhex);
    tp_txn_result res;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply_json(m, okjson, &res, &err));
    TEST_ASSERT_TRUE(res.committed);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [field] the rejected-result JSON emits the offending `field` (sparse: omitted when
 * ""), matching F2-01 tp_op_result_encode + canonical ascending key order. */
void test_json_result_error_field_golden(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    char adhex[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, aid, adhex, sizeof adhex, &err));
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.settings.set\",\"atlas_id\":\"%s\","
                   "\"bogus\":1}]}}",
                   adhex);
    tp_txn_result res;
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, json, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_STRING("bogus", res.errors[0].field);

    char *j = tp_txn_result_encode(&res);
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_NOT_NULL(strstr(j, "\"field\": \"bogus\""));
    /* ascending: code < field < message < op_index */
    TEST_ASSERT_TRUE(strstr(j, "\"code\"") < strstr(j, "\"field\""));
    TEST_ASSERT_TRUE(strstr(j, "\"field\"") < strstr(j, "\"message\""));
    TEST_ASSERT_TRUE(strstr(j, "\"message\"") < strstr(j, "\"op_index\""));
    free(j);
    tp_txn_result_free(&res);

    /* sparse: an envelope-level error with an empty field omits the "field" key. */
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_model_apply_json(m, "{not json", &res, &err));
    char *j2 = tp_txn_result_encode(&res);
    TEST_ASSERT_NOT_NULL(j2);
    TEST_ASSERT_NULL(strstr(j2, "\"field\"")); /* no field key when field == "" */
    free(j2);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [schema] a fractional/out-of-range schema is rejected (was: truncating
 * schema->valueint accepted {"schema":1.9} as 1). */
void test_json_fractional_schema_rejected(void) {
    tp_model *m = tp_model_create();
    tp_txn_result res;
    tp_error err;
    const char *frac = "{\"schema\":1.9,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":[]}}";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_model_apply_json(m, frac, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    tp_txn_result_free(&res);
    /* an integral but unknown version is still bad_version. */
    const char *two = "{\"schema\":2,\"transaction\":{"
                      "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":[]}}";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION, tp_model_apply_json(m, two, &res, &err));
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [OOM] a shape-faulted batch whose error record cannot be stored must REJECT, never
 * commit (was: add_error dropped the error, error_count 0, the batch committed). */
void test_json_shape_oom_must_not_commit(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    char adhex[TP_ID_TEXT_CAP];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_id_format(TP_ID_KIND_ATLAS, aid, adhex, sizeof adhex, &err));
    char *before = serialize(tp_model_project(m));
    /* a shape fault (unknown field "bogus") on an OTHERWISE valid+committable op: if
     * the fault record is dropped and error_count stays 0, the old code would lower
     * (ignoring the unknown field) and COMMIT. */
    char json[512];
    (void)snprintf(json, sizeof json,
                   "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
                   "\"expected_revision\":0,\"operations\":[{\"op\":\"atlas.settings.set\",\"atlas_id\":\"%s\","
                   "\"max_size\":2048,\"bogus\":1}]}}",
                   adhex);
    tp_txn__test_set_add_error_fail(0); /* the shape-fault error record alloc fails */
    tp_txn_result res;
    tp_status st = tp_model_apply_json(m, json, &res, &err);
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, st); /* rejected, NOT committed */
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m)); /* revision unchanged */
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after); /* model byte-unchanged */
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

/* [tokens] pin the sprite-clear field-token vocabulary: encode(mask) -> decode ->
 * mask is identity for every field, so the two hand-kept mask<->token lists
 * (tp_op_encode.c and tp_txn_lower.c) cannot silently disagree. */
void test_sprite_clear_field_roundtrip(void) {
    const uint32_t bits[] = {TP_SPF_ORIGIN,       TP_SPF_SLICE9, TP_SPF_SHAPE,  TP_SPF_ALLOW_ROTATE,
                             TP_SPF_MAX_VERTICES, TP_SPF_MARGIN, TP_SPF_EXTRUDE};
    for (size_t i = 0; i < sizeof bits / sizeof bits[0]; i++) {
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
        op.atlas_id = id_of(0xA1);
        op.u.sprite_clear.source_id = id_of(0xB2);
        op.u.sprite_clear.src_key = (char *)"hero.png";
        op.u.sprite_clear.mask = bits[i];

        tp_txn_request req = {0};
        req.schema = TP_TXN_SCHEMA;
        (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", "0123456789abcdef0123456789abcdef");
        req.ops = &op;
        req.op_count = 1;

        char *json = tp_txn_request_encode(&req);
        TEST_ASSERT_NOT_NULL(json);
        tp_txn_request *rd = NULL;
        tp_error err;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
        TEST_ASSERT_NOT_NULL(rd);
        TEST_ASSERT_EQUAL_INT(1, rd->op_count);
        TEST_ASSERT_EQUAL_UINT32(bits[i], rd->ops[0].u.sprite_clear.mask); /* round-trip identity */
        free(json);
        tp_txn_request_free(rd);
    }
}

/* [C1] JSON target.set lowering stays FULL-REPLACE: an object that OMITS "enabled" decodes to
 * mask == TP_TF_ALL with enabled defaulting to true -- the pre-mask contract. (The C1 field mask is an
 * internal mechanism for GUI partial edits built in C; partial-field target.set over JSON is a deliberate
 * future extension, not a silent effect of adding the struct mask.) */
void test_json_target_set_full_replace(void) {
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                       "\"target_id\":\"target_22222222222222222222222222222222\","
                       "\"exporter_id\":\"defold\",\"out_path\":\"out/x.json\"}" /* NO "enabled" key */
                       "]}}";
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT(1, rd->op_count);
    TEST_ASSERT_EQUAL_INT(TP_OP_TARGET_SET, rd->ops[0].kind);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_TF_ALL, rd->ops[0].u.target_set.mask); /* full replace, not presence-derived */
    TEST_ASSERT_TRUE(rd->ops[0].u.target_set.enabled);                           /* omitted enabled -> true default */
    tp_txn_request_free(rd);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_clone_byte_identity);
    RUN_TEST(test_clone_alloc_fault_sweep);
    RUN_TEST(test_clone_arena_byte_identity);
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
    RUN_TEST(test_json_anim_rename_roundtrip);
    RUN_TEST(test_json_batch_equals_one_by_one);
    RUN_TEST(test_number_handling);
    RUN_TEST(test_typed_malformed_id_rejected);
    RUN_TEST(test_json_null_out_no_crash);
    RUN_TEST(test_json_malformed_preserves_revision);
    RUN_TEST(test_json_nonstring_id_collect_all);
    RUN_TEST(test_json_ov_int16_range);
    RUN_TEST(test_json_result_error_field_golden);
    RUN_TEST(test_json_fractional_schema_rejected);
    RUN_TEST(test_json_shape_oom_must_not_commit);
    RUN_TEST(test_sprite_clear_field_roundtrip);
    RUN_TEST(test_json_target_set_full_replace);
    return UNITY_END();
}
