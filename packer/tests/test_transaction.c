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
#include "tp_core/tp_diff.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"
#include "tp_txn_internal.h"            /* clone fault seam */
#include "tp_op_internal.h"             /* tp_op__test_set_alloc_fail */
#include "tp_project_internal.h"        /* checkpoint-size traversal seam */
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

static char *txn_json_with_author_size(size_t total_len) {
    static const char prefix[] =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
        "\"expected_revision\":0,\"author\":\"";
    static const char suffix[] = "\",\"operations\":[]}}";
    TEST_ASSERT_TRUE(total_len >= (sizeof prefix - 1U) + (sizeof suffix - 1U));
    char *json = (char *)malloc(total_len + 1U);
    TEST_ASSERT_NOT_NULL(json);
    memcpy(json, prefix, sizeof prefix - 1U);
    const size_t author_len = total_len - (sizeof prefix - 1U) - (sizeof suffix - 1U);
    memset(json + sizeof prefix - 1U, 'a', author_len);
    memcpy(json + sizeof prefix - 1U + author_len, suffix, sizeof suffix);
    return json;
}

static char *txn_json_with_remove_ops(int op_count) {
    static const char prefix[] =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
        "\"expected_revision\":0,\"operations\":[";
    static const char op[] =
        "{\"op\":\"atlas.remove\",\"atlas_id\":\"atlas_11111111111111111111111111111111\"}";
    static const char suffix[] = "]}}";
    TEST_ASSERT_TRUE(op_count >= 0);
    const size_t separators = (op_count > 0) ? (size_t)(op_count - 1) : 0U;
    const size_t total_len = (sizeof prefix - 1U) + (size_t)op_count * (sizeof op - 1U) + separators +
                             (sizeof suffix - 1U);
    TEST_ASSERT_TRUE(total_len <= TP_TXN_MAX_REQUEST_BYTES);
    char *json = (char *)malloc(total_len + 1U);
    TEST_ASSERT_NOT_NULL(json);
    size_t off = 0;
    memcpy(json + off, prefix, sizeof prefix - 1U);
    off += sizeof prefix - 1U;
    for (int i = 0; i < op_count; i++) {
        if (i > 0) {
            json[off++] = ',';
        }
        memcpy(json + off, op, sizeof op - 1U);
        off += sizeof op - 1U;
    }
    memcpy(json + off, suffix, sizeof suffix);
    TEST_ASSERT_EQUAL_size_t(total_len, off + sizeof suffix - 1U);
    return json;
}

static char *txn_json_with_unknown_ops(int op_count) {
    static const char prefix[] =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
        "\"expected_revision\":0,\"operations\":[";
    static const char op[] = "{\"op\":\"unknown.operation\"}";
    static const char suffix[] = "]}}";
    TEST_ASSERT_TRUE(op_count >= 0);
    const size_t separators = (op_count > 0) ? (size_t)(op_count - 1) : 0U;
    const size_t total_len = (sizeof prefix - 1U) + (size_t)op_count * (sizeof op - 1U) + separators +
                             (sizeof suffix - 1U);
    TEST_ASSERT_TRUE(total_len <= TP_TXN_MAX_REQUEST_BYTES);
    char *json = (char *)malloc(total_len + 1U);
    TEST_ASSERT_NOT_NULL(json);
    size_t off = 0U;
    memcpy(json + off, prefix, sizeof prefix - 1U);
    off += sizeof prefix - 1U;
    for (int i = 0; i < op_count; ++i) {
        if (i > 0) json[off++] = ',';
        memcpy(json + off, op, sizeof op - 1U);
        off += sizeof op - 1U;
    }
    memcpy(json + off, suffix, sizeof suffix);
    TEST_ASSERT_EQUAL_size_t(total_len, off + sizeof suffix - 1U);
    return json;
}

static char *txn_json_with_nested_operations(int depth, size_t *out_len) {
    static const char prefix[] =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000000\","
        "\"expected_revision\":0,\"operations\":[";
    static const char nested[] = "{\"operations\":[";
    static const char suffix[] = "]}}";
    TEST_ASSERT_TRUE(depth > 0);
    const size_t len = (sizeof prefix - 1U) + (size_t)depth * (sizeof nested - 1U) + 1U +
                       (size_t)depth * 2U + (sizeof suffix - 1U);
    char *json = (char *)malloc(len + 1U);
    TEST_ASSERT_NOT_NULL(json);
    size_t off = 0U;
    memcpy(json + off, prefix, sizeof prefix - 1U);
    off += sizeof prefix - 1U;
    for (int i = 0; i < depth; ++i) {
        memcpy(json + off, nested, sizeof nested - 1U);
        off += sizeof nested - 1U;
    }
    json[off++] = '0';
    for (int i = 0; i < depth; ++i) {
        json[off++] = ']';
        json[off++] = '}';
    }
    memcpy(json + off, suffix, sizeof suffix);
    TEST_ASSERT_EQUAL_size_t(len, off + sizeof suffix - 1U);
    if (out_len) {
        *out_len = len;
    }
    return json;
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
    TEST_ASSERT_GREATER_THAN_size_t(sizeof *c,
                                    tp_project__test_clone_allocation_bytes());
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
    tp_id128 source_id = p->atlases[0].sources[0].id;
    tp_op_sprite_ref frames[2] = {{source_id, (char *)"a"},
                                  {source_id, (char *)"b"}};
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

    tp_operation later_op;
    op_atlas_rename(&later_op, aid, "later");
    tp_txn_request later_req = req;
    later_req.ops = &later_op;
    later_req.expected_revision = 1;
    (void)snprintf(later_req.id_hex, sizeof later_req.id_hex, "%s", "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_apply(m, &later_req, &res, &err)); /* rev -> 2 */
    tp_txn_result_free(&res);

    char *before = serialize(tp_model_project(m));
    /* Retry the original id after another commit, with the original stale revision:
     * duplicate detection precedes revision and returns the CURRENT revision. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, tp_model_apply(m, &req, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(2, res.revision);
    TEST_ASSERT_EQUAL_INT(1, res.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, res.errors[0].code);
    TEST_ASSERT_EQUAL_STRING("id", res.errors[0].field);
    TEST_ASSERT_TRUE(err.msg[0] != '\0');
    TEST_ASSERT_EQUAL_INT64(2, tp_model_revision(m));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_txn_result_free(&res);
    tp_model_destroy(m);
}

static tp_status commit_numbered_change(tp_model *model, unsigned id_value,
                                        int64_t expected_revision,
                                        tp_txn_result *result, tp_error *err) {
    tp_txn_request request = {0};
    tp_operation operation;
    char name[32];
    (void)snprintf(name, sizeof name, "atlas-%u", id_value);
    op_atlas_rename(&operation, model->project->atlases[0].id, name);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%032x", id_value);
    request.expected_revision = expected_revision;
    request.ops = &operation;
    request.op_count = 1;
    return tp_model_apply(model, &request, result, err);
}

void test_idempotency_retention_window_evicts_fifo(void) {
    tp_model *model = tp_model_create();
    TEST_ASSERT_NOT_NULL(model);
    model->project->atlases[0].id = id_of(0x77);
    model->project->atlases[0].id_synthetic = false;
    tp_txn_result result;
    tp_error err = {0};

    for (unsigned i = 0U; i < (unsigned)TP_TXN_RETAINED_ID_CAP; ++i) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              commit_numbered_change(model, i + 1U,
                                                     (int64_t)i, &result,
                                                     &err));
        tp_txn_result_free(&result);
    }

    /* The boundary element is retained and duplicate detection still precedes
     * its deliberately stale expected revision. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_numbered_change(model, 1U, 0, &result, &err));
    TEST_ASSERT_EQUAL_INT64(TP_TXN_RETAINED_ID_CAP, result.revision);
    tp_txn_result_free(&result);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_numbered_change(
                              model, (unsigned)TP_TXN_RETAINED_ID_CAP + 1U,
                              TP_TXN_RETAINED_ID_CAP, &result, &err));
    tp_txn_result_free(&result);

    /* One successful commit evicts exactly the oldest ID, not its successor. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_numbered_change(model, 2U, 0, &result, &err));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_numbered_change(
                              model, 1U, TP_TXN_RETAINED_ID_CAP + 1,
                              &result, &err));
    tp_txn_result_free(&result);

    tp_model_destroy(model);
}

void test_semantic_no_change_is_not_committed_or_retained(void) {
    tp_project *project = base_project();
    const tp_id128 atlas_id = project->atlases[0].id;
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));

    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, id_of(0xA5));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_attach_journal(model, journal, &error));
    const int64_t journal_bytes_before = io.length(io.ctx);

    tp_operation operation;
    op_atlas_rename(&operation, atlas_id, "atlas1");
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee", 33U);
    request.expected_revision = 0;
    request.ops = &operation;
    request.op_count = 1;
    tp_txn_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_TRUE(result.no_change);
    TEST_ASSERT_EQUAL_INT64(0, result.revision);
    TEST_ASSERT_EQUAL_INT(0, result.error_count);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(model));
    TEST_ASSERT_FALSE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT(0, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT64(journal_bytes_before, io.length(io.ctx));
    char *encoded = tp_txn_result_encode(&result);
    TEST_ASSERT_NOT_NULL(encoded);
    TEST_ASSERT_NOT_NULL(strstr(encoded, "\"status\": \"no_change\""));
    free(encoded);
    tp_txn_result_free(&result);

    /* A no-change request is not a retained committed ID. Retrying it returns
     * the same typed outcome, never duplicate_id. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_TRUE(result.no_change);
    TEST_ASSERT_EQUAL_INT(0, result.error_count);
    TEST_ASSERT_EQUAL_INT64(journal_bytes_before, io.length(io.ctx));
    tp_txn_result_free(&result);
    tp_model_destroy(model);
}

void test_semantic_no_change_preserves_redo_branch(void) {
    tp_project *project = base_project();
    const tp_id128 atlas_id = project->atlases[0].id;
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    tp_error error = {0};

    tp_operation change;
    op_atlas_rename(&change, atlas_id, "changed");
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "dddddddddddddddddddddddddddddddd", 33U);
    request.ops = &change;
    request.op_count = 1;
    tp_txn_result result;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &error));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    TEST_ASSERT_TRUE(tp_model_can_redo(model));
    const int redo_depth = tp_model_redo_depth(model);
    const int64_t revision = tp_model_revision(model);

    tp_operation no_change;
    op_atlas_rename(&no_change, atlas_id, "atlas1");
    request.ops = &no_change;
    request.expected_revision = revision;
    memcpy(request.id_hex, "cccccccccccccccccccccccccccccccc", 33U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_TRUE(result.no_change);
    TEST_ASSERT_EQUAL_INT64(revision, tp_model_revision(model));
    TEST_ASSERT_EQUAL_INT(redo_depth, tp_model_redo_depth(model));
    TEST_ASSERT_TRUE(tp_model_can_redo(model));
    tp_txn_result_free(&result);
    tp_model_destroy(model);
}

void test_journal_less_history_avoids_checkpoint_size_traversals(void) {
    tp_project *project = base_project();
    const tp_id128 atlas_id = project->atlases[0].id;
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));

    tp_operation change;
    op_atlas_rename(&change, atlas_id, "history-without-journal");
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "abababababababababababababababab", 33U);
    request.ops = &change;
    request.op_count = 1;
    tp_txn_result result = {0};
    tp_error error = {0};

    tp_project__test_serialization_stats_reset();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_TRUE(result.committed);
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_size_query_calls());
    tp_txn_result_free(&result);

    tp_project__test_serialization_stats_reset();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &error));
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_size_query_calls());
    tp_model_destroy(model);
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

void test_json_request_byte_limit_is_inclusive_and_preparse(void) {
    char *at_limit = txn_json_with_author_size((size_t)TP_TXN_MAX_REQUEST_BYTES);
    tp_txn_request *req = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_txn_request_decode_n(at_limit, (size_t)TP_TXN_MAX_REQUEST_BYTES, &req, &err));
    TEST_ASSERT_NOT_NULL(req);
    tp_txn_request_free(req);
    req = NULL;

    char *over_limit = txn_json_with_author_size((size_t)TP_TXN_MAX_REQUEST_BYTES + 1U);
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_txn_request_decode_n(over_limit, (size_t)TP_TXN_MAX_REQUEST_BYTES + 1U,
                                                  &req, &err));
    TEST_ASSERT_NULL(req);
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_txn_request_decode(over_limit, &req, &err));
    TEST_ASSERT_NULL(req); /* legacy C-string path uses the same bounded admission */

    /* Invalid JSON above the byte cap still reports the cap, proving rejection occurs
     * before cJSON is allowed to parse/materialize attacker-controlled input. */
    memset(over_limit, '{', (size_t)TP_TXN_MAX_REQUEST_BYTES + 1U);
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_txn_request_decode_n(over_limit, (size_t)TP_TXN_MAX_REQUEST_BYTES + 1U,
                                                  &req, &err));
    TEST_ASSERT_NULL(req);
    free(at_limit);
    free(over_limit);
}

void test_json_operation_count_limit_is_inclusive(void) {
    char *at_limit = txn_json_with_remove_ops(TP_TXN_MAX_OPS);
    tp_txn_request *req = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode_n(at_limit, strlen(at_limit), &req, &err));
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_INT(TP_TXN_MAX_OPS, req->op_count);
    tp_txn_request_free(req);
    free(at_limit);

    char *over_limit = txn_json_with_remove_ops(TP_TXN_MAX_OPS + 1);
    req = NULL;
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_txn_request_decode_n(over_limit, strlen(over_limit), &req, &err));
    TEST_ASSERT_NULL(req);

    /* The count gate runs before cJSON: even a syntactically incomplete suffix
     * cannot force materialization of an already oversized operations array. */
    const size_t malformed_len = strlen(over_limit);
    over_limit[malformed_len - 1U] = 'x';
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_txn_request_decode_n(over_limit, malformed_len, &req, &err));
    TEST_ASSERT_NULL(req);
    free(over_limit);
}

void test_json_precheck_rejects_duplicate_counted_envelope_keys(void) {
    const char *duplicate_operations =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000001\","
        "\"expected_revision\":0,\"operations\":[{}],\"operations\":[]}}";
    const char *duplicate_transaction =
        "{\"schema\":1,\"transaction\":{\"id\":\"00000000000000000000000000000001\","
        "\"expected_revision\":0,\"operations\":[{}]},\"transaction\":{\"operations\":[]}}";
    tp_error err = {0};
    int count = -1;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_txn__count_operations_json_n(duplicate_operations,
                                        strlen(duplicate_operations), &count, &err));
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "duplicate operations"));

    memset(&err, 0, sizeof err);
    count = -1;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_txn__count_operations_json_n(duplicate_transaction,
                                        strlen(duplicate_transaction), &count, &err));
    TEST_ASSERT_EQUAL_INT(0, count);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "duplicate transaction"));
}

void test_json_operation_precheck_is_single_pass_under_adversarial_nesting(void) {
    size_t json_len = 0U;
    char *json = txn_json_with_nested_operations(256, &json_len);
    tp_txn_request *req = NULL;
    tp_error err = {0};
    size_t steps = 0U;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_txn__test_json_precheck(json, json_len, &steps, &err));
    TEST_ASSERT_GREATER_THAN_size_t(0U, steps);
    TEST_ASSERT_LESS_OR_EQUAL_size_t(json_len, steps);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_txn_request_decode_n(json, json_len, &req, &err));
    TEST_ASSERT_NULL(req);
    free(json);
}

void test_json_max_ops_walk_and_error_growth_are_linear(void) {
    char *valid = txn_json_with_remove_ops(TP_TXN_MAX_OPS);
    tp_txn_request *req = NULL;
    tp_error err = {0};
    tp_txn__test_complexity_reset();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_txn_request_decode_n(valid, strlen(valid), &req, &err));
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_INT(TP_TXN_MAX_OPS, req->op_count);
    TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)TP_TXN_MAX_OPS * 3U,
                                     tp_txn__test_op_walk_steps());
    tp_txn_request_free(req);
    free(valid);

    char *faults = txn_json_with_unknown_ops(TP_TXN_MAX_OPS);
    tp_model *model = tp_model_wrap(base_project());
    tp_txn_result result;
    memset(&err, 0, sizeof err);
    tp_txn__test_complexity_reset();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_OP,
                          tp_model_apply_json_n(model, faults, strlen(faults), &result, &err));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT(TP_TXN_MAX_OPS, result.error_count);
    TEST_ASSERT_EQUAL_INT(0, result.errors[0].op_index);
    TEST_ASSERT_EQUAL_INT(TP_TXN_MAX_OPS - 1,
                          result.errors[result.error_count - 1].op_index);
    TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)TP_TXN_MAX_OPS * 3U,
                                     tp_txn__test_op_walk_steps());
    TEST_ASSERT_EQUAL_size_t(1U, tp_txn__test_error_allocations());
    tp_txn_result_free(&result);
    tp_model_destroy(model);
    free(faults);
}

void test_length_aware_json_requires_exact_span_consumption(void) {
    const char *valid = "{\"schema\":1,\"transaction\":{"
                        "\"id\":\"00000000000000000000000000000000\","
                        "\"expected_revision\":0,\"operations\":[]}}";
    const size_t valid_len = strlen(valid);
    char *buffer = (char *)malloc(valid_len + 5U);
    TEST_ASSERT_NOT_NULL(buffer);
    memcpy(buffer, valid, valid_len);
    memcpy(buffer + valid_len, "junk", 4U);
    buffer[valid_len + 4U] = '\0';

    tp_txn_request *req = NULL;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_txn_request_decode_n(buffer, valid_len + 4U, &req, &err));
    TEST_ASSERT_NULL(req);

    buffer[valid_len] = '\0';
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_txn_request_decode_n(buffer, valid_len + 4U, &req, &err));
    TEST_ASSERT_NULL(req);

    memcpy(buffer + valid_len, " \r\n\t", 4U);
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_txn_request_decode_n(buffer, valid_len + 4U, &req, &err));
    TEST_ASSERT_NOT_NULL(req);
    tp_txn_request_free(req);
    free(buffer);
}

void test_operation_count_rejects_before_clone_for_json_and_typed_apply(void) {
    tp_model *m = tp_model_create();
    TEST_ASSERT_NOT_NULL(m);
    char *over_limit = txn_json_with_remove_ops(TP_TXN_MAX_OPS + 1);
    tp_txn_result res;
    tp_error err = {0};

    tp_project__test_set_clone_alloc_fail(0);
    char *bytes_over = txn_json_with_author_size((size_t)TP_TXN_MAX_REQUEST_BYTES + 1U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_model_apply_json_n(m, bytes_over,
                                                (size_t)TP_TXN_MAX_REQUEST_BYTES + 1U, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, res.revision);
    tp_txn_result_free(&res);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_model_apply_json_n(m, over_limit, strlen(over_limit), &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, res.revision);
    tp_txn_result_free(&res);

    tp_txn_request typed = {0};
    typed.schema = TP_TXN_SCHEMA;
    (void)snprintf(typed.id_hex, sizeof typed.id_hex, "%s", "11111111111111111111111111111111");
    typed.expected_revision = 0;
    typed.op_count = TP_TXN_MAX_OPS + 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_model_apply(m, &typed, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, res.revision);
    tp_txn_result_free(&res);

    typed.op_count = 0;
    typed.label = (char *)malloc((size_t)TP_TXN_MAX_REQUEST_BYTES + 1U);
    TEST_ASSERT_NOT_NULL(typed.label);
    memset(typed.label, 'x', (size_t)TP_TXN_MAX_REQUEST_BYTES);
    typed.label[TP_TXN_MAX_REQUEST_BYTES] = '\0';
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_model_apply(m, &typed, &res, &err));
    TEST_ASSERT_FALSE(res.committed);
    TEST_ASSERT_EQUAL_INT64(0, res.revision);
    tp_txn_result_free(&res);
    free(typed.label);
    typed.label = NULL;

    /* The first clone allocation still fails: no over-limit path reached clone. */
    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = tp_model_project(m)->atlases[0].id;
    typed.ops = &op;
    typed.op_count = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, tp_model_apply(m, &typed, &res, &err));
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));
    tp_txn_result_free(&res);

    free(bytes_over);
    free(over_limit);
    tp_model_destroy(m);
}

void test_typed_byte_limit_rejects_before_encode_or_clone_without_journal(void) {
    tp_project *project = base_project();
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    char *author = malloc((size_t)TP_TXN_MAX_REQUEST_BYTES + 1U);
    TEST_ASSERT_NOT_NULL(author);
    memset(author, 'a', (size_t)TP_TXN_MAX_REQUEST_BYTES);
    author[TP_TXN_MAX_REQUEST_BYTES] = '\0';

    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    memcpy(request.id_hex, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", 33U);
    request.author = author;
    tp_txn_result result;
    tp_error error = {0};
    tp_txn__test_encode_stats_reset();
    tp_project__test_set_clone_alloc_fail(0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_model_apply(model, &request, &result, &error));
    TEST_ASSERT_EQUAL_UINT64(0, tp_txn__test_request_encode_calls());
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(model));
    tp_txn_result_free(&result);
    free(author);
    tp_model_destroy(model);
}

void test_typed_positive_op_count_requires_operations_before_clone(void) {
    tp_model *m = tp_model_wrap(base_project());
    TEST_ASSERT_NOT_NULL(m);
    char *before = serialize(tp_model_project(m));

    tp_txn_request typed = {0};
    typed.schema = TP_TXN_SCHEMA;
    (void)snprintf(typed.id_hex, sizeof typed.id_hex, "%s",
                   "12121212121212121212121212121212");
    typed.expected_revision = 0;
    typed.ops = NULL;
    typed.op_count = 1;
    TEST_ASSERT_NULL(tp_txn_request_encode(&typed));

    tp_txn_result result = {0};
    tp_error err = {0};
    tp_project__test_set_clone_alloc_fail(0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_model_apply(m, &typed, &result, &err));
    tp_project__test_set_clone_alloc_fail(-1);

    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT64(0, result.revision);
    TEST_ASSERT_EQUAL_INT(1, result.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, result.errors[0].code);
    TEST_ASSERT_EQUAL_STRING("operations", result.errors[0].field);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);

    free(after);
    free(before);
    tp_txn_result_free(&result);
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

/* Sprite transport fields stay wide through decode; the shared operation validator
 * owns the storage/domain range and rejects before the apply-side narrowing cast.
 * This prevents 65535 wrapping to -1 == INHERIT while keeping every frontend's
 * structured error semantics identical. */
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
        TEST_ASSERT_EQUAL_INT(1, res.error_count);
        TEST_ASSERT_EQUAL_STRING("ov_margin", res.errors[0].field);
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

/* R2a: JSON target.set is FIELD-PRESENCE (like every other SET op). An object carrying only
 * exporter_id + out_path (NO "enabled") decodes to mask == EXPORTER|OUT_PATH with the ENABLED bit
 * UNSET -- so tp_operation_encode -> decode round-trips faithfully and replay never re-adds an unsent
 * field. (Before R2a this pinned TP_TF_ALL and defaulted enabled=true -- the exact wire-form data loss
 * the diff-recovery journal replay hits.) A full object still yields TP_TF_ALL (old contract = subset). */
void test_json_target_set_presence(void) {
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
    /* presence-derived: exporter + out_path present, "enabled" absent -> its bit is NOT set */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(TP_TF_EXPORTER | TP_TF_OUT_PATH), rd->ops[0].u.target_set.mask);
    tp_txn_request_free(rd);
}

/* R2a: a single-field target.set ("enabled" only) decodes to mask == ENABLED with exporter/out_path
 * absent -- proving a lone non-string field survives the JSON round-trip unchanged. */
void test_json_target_set_enabled_only(void) {
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                       "\"target_id\":\"target_22222222222222222222222222222222\","
                       "\"enabled\":false}"
                       "]}}";
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT(1, rd->op_count);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_TF_ENABLED, rd->ops[0].u.target_set.mask);
    TEST_ASSERT_FALSE(rd->ops[0].u.target_set.enabled);
    tp_txn_request_free(rd);
}

/* R2a: the partial mask survives a full encode -> decode round-trip -- the exact fidelity the
 * diff-recovery journal (R2b) relies on when it re-encodes committed ops and replays them. */
void test_json_target_set_roundtrip(void) {
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                       "\"target_id\":\"target_22222222222222222222222222222222\","
                       "\"out_path\":\"out/y.json\"}" /* only out_path */
                       "]}}";
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    const uint32_t mask0 = rd->ops[0].u.target_set.mask;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_TF_OUT_PATH, mask0);
    char *reenc = tp_txn_request_encode(rd);
    TEST_ASSERT_NOT_NULL(reenc);
    tp_txn_request *rd2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(reenc, &rd2, &err));
    TEST_ASSERT_NOT_NULL(rd2);
    TEST_ASSERT_EQUAL_UINT32(mask0, rd2->ops[0].u.target_set.mask); /* partial mask preserved across the round-trip */
    free(reenc);
    tp_txn_request_free(rd);
    tp_txn_request_free(rd2);
}

/* R2a backward-compat: a FULL 3-field target.set JSON still yields TP_TF_ALL, so the pre-R2a
 * full-replace contract is a strict subset (guards the mask derivation + TP_TF_ALL's value). */
void test_json_target_set_full_is_all(void) {
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                       "\"target_id\":\"target_22222222222222222222222222222222\","
                       "\"exporter_id\":\"defold\",\"out_path\":\"out/x.json\",\"enabled\":true}"
                       "]}}";
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_TF_ALL, rd->ops[0].u.target_set.mask);
    TEST_ASSERT_TRUE(rd->ops[0].u.target_set.enabled);
    tp_txn_request_free(rd);
}

/* R2a: the ENABLED bit AND its boolean value survive an encode->decode round-trip -- the exact
 * fidelity format-B recovery replay depends on for a target enable/disable edit. */
void test_json_target_set_roundtrip_enabled(void) {
    const char *json = "{\"schema\":1,\"transaction\":{"
                       "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                       "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                       "\"target_id\":\"target_22222222222222222222222222222222\","
                       "\"out_path\":\"out/z.json\",\"enabled\":true}"
                       "]}}";
    tp_txn_request *rd = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(json, &rd, &err));
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(TP_TF_OUT_PATH | TP_TF_ENABLED), rd->ops[0].u.target_set.mask);
    TEST_ASSERT_TRUE(rd->ops[0].u.target_set.enabled);
    char *reenc = tp_txn_request_encode(rd);
    TEST_ASSERT_NOT_NULL(reenc);
    tp_txn_request *rd2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(reenc, &rd2, &err));
    TEST_ASSERT_NOT_NULL(rd2);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(TP_TF_OUT_PATH | TP_TF_ENABLED), rd2->ops[0].u.target_set.mask);
    TEST_ASSERT_TRUE(rd2->ops[0].u.target_set.enabled); /* the bool value survives, not just the bit */
    free(reenc);
    tp_txn_request_free(rd);
    tp_txn_request_free(rd2);
}

/* R2a lowering edges: a target.set naming NO mutable field lowers to mask==0 (validate then rejects it
 * -- see test_operation.c's mask==0 case), and a present-but-empty out_path still SETS the OUT_PATH bit,
 * so validate rejects the empty value instead of the field being silently dropped. */
void test_json_target_set_lower_edges(void) {
    tp_error err;
    const char *empty = "{\"schema\":1,\"transaction\":{"
                        "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                        "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                        "\"target_id\":\"target_22222222222222222222222222222222\"}"
                        "]}}";
    tp_txn_request *r0 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(empty, &r0, &err));
    TEST_ASSERT_NOT_NULL(r0);
    TEST_ASSERT_EQUAL_UINT32(0u, r0->ops[0].u.target_set.mask);
    tp_txn_request_free(r0);
    const char *empty_path = "{\"schema\":1,\"transaction\":{"
                             "\"id\":\"00000000000000000000000000000000\",\"expected_revision\":0,\"operations\":["
                             "{\"op\":\"target.set\",\"atlas_id\":\"atlas_11111111111111111111111111111111\","
                             "\"target_id\":\"target_22222222222222222222222222222222\",\"out_path\":\"\"}"
                             "]}}";
    tp_txn_request *r1 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_txn_request_decode(empty_path, &r1, &err));
    TEST_ASSERT_NOT_NULL(r1);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_TF_OUT_PATH, r1->ops[0].u.target_set.mask);
    tp_txn_request_free(r1);
}

void test_prechecked_recovery_decode_verifies_operation_count(void) {
    const char *json =
        "{\"schema\":1,\"transaction\":{"
        "\"id\":\"00000000000000000000000000000000\","
        "\"expected_revision\":0,\"operations\":["
        "{\"op\":\"atlas.rename\","
        "\"atlas_id\":\"atlas_11111111111111111111111111111111\","
        "\"name\":\"renamed\"}]}}";
    tp_error err = {0};
    int count = 0;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_txn__count_operations_json_n(json, strlen(json), &count, &err));
    TEST_ASSERT_EQUAL_INT(1, count);
    tp_txn_request *request = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_txn__decode_prechecked_json_n(json, strlen(json), count, &request,
                                         &err));
    TEST_ASSERT_NOT_NULL(request);
    TEST_ASSERT_EQUAL_INT(count, request->op_count);
    tp_txn_request_free(request);

    request = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_txn__decode_prechecked_json_n(json, strlen(json), count + 1,
                                         &request, &err));
    TEST_ASSERT_NULL(request);
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
    RUN_TEST(test_idempotency_retention_window_evicts_fifo);
    RUN_TEST(test_semantic_no_change_is_not_committed_or_retained);
    RUN_TEST(test_semantic_no_change_preserves_redo_branch);
    RUN_TEST(test_journal_less_history_avoids_checkpoint_size_traversals);
    RUN_TEST(test_dirty_is_semantic_identity);
    RUN_TEST(test_identity_excludes_runtime);
    RUN_TEST(test_identity_order_normalized);
    RUN_TEST(test_identity_frames_order_semantic);
    RUN_TEST(test_json_structural_fail_fast);
    RUN_TEST(test_json_request_byte_limit_is_inclusive_and_preparse);
    RUN_TEST(test_json_operation_count_limit_is_inclusive);
    RUN_TEST(test_json_precheck_rejects_duplicate_counted_envelope_keys);
    RUN_TEST(test_json_operation_precheck_is_single_pass_under_adversarial_nesting);
    RUN_TEST(test_json_max_ops_walk_and_error_growth_are_linear);
    RUN_TEST(test_length_aware_json_requires_exact_span_consumption);
    RUN_TEST(test_operation_count_rejects_before_clone_for_json_and_typed_apply);
    RUN_TEST(test_typed_byte_limit_rejects_before_encode_or_clone_without_journal);
    RUN_TEST(test_typed_positive_op_count_requires_operations_before_clone);
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
    RUN_TEST(test_json_target_set_presence);
    RUN_TEST(test_json_target_set_enabled_only);
    RUN_TEST(test_json_target_set_roundtrip);
    RUN_TEST(test_json_target_set_full_is_all);
    RUN_TEST(test_json_target_set_roundtrip_enabled);
    RUN_TEST(test_json_target_set_lower_edges);
    RUN_TEST(test_prechecked_recovery_decode_verifies_operation_count);
    return UNITY_END();
}
