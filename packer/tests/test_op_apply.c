/* F2-01: id-only apply (forward + error per operation kind), stage-then-commit
 * allocator-fault safety, selector->operation builders, and a PARITY proof that an
 * op applied == the existing tp_project mutator's result (byte-identical serialized
 * project). This turns the engine from "dead groundwork" into "proven-equivalent
 * groundwork"; the shipping CLI/GUI cutover is F2-05. */

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_export.h"          /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_names.h"           /* tp_sprite_export_key (parity bridge) */
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_op_internal.h"             /* tp_op__test_set_alloc_fail */
#include "unity.h"

void setUp(void) {}
void tearDown(void) { tp_op__test_set_alloc_fail(-1); }

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

/* One default atlas + one folder source + one json-neotolis target, ids promoted
 * to real (addressable) values. */
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

/* ---- forward + error per kind: atlas ------------------------------------- */

void test_apply_atlas_ops(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* atlas.create */
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = id_of(0xA1);
    op.u.atlas_create.name = (char *)"extra";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(2, p->atlas_count);
    TEST_ASSERT_TRUE(tp_project_find_atlas_by_id(p, id_of(0xA1)) >= 0);

    memset(&op, 0, sizeof op); /* atlas.rename */
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = aid;
    op.u.atlas_rename.name = (char *)"renamed";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("renamed", p->atlases[0].name);

    memset(&op, 0, sizeof op); /* atlas.settings.set */
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = aid;
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_PADDING;
    op.u.atlas_settings.max_size = 2048;
    op.u.atlas_settings.padding = 7;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(2048, p->atlases[0].max_size);
    TEST_ASSERT_EQUAL_INT(7, p->atlases[0].padding);

    memset(&op, 0, sizeof op); /* atlas.remove (the extra atlas) */
    op.kind = TP_OP_ATLAS_REMOVE;
    op.atlas_id = id_of(0xA1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(1, p->atlas_count);

    /* error: rename a non-existent atlas */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = id_of(0x99);
    op.u.atlas_rename.name = (char *)"x";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_apply(p, &op, &rej));
    tp_project_destroy(p);
}

/* ---- forward + error per kind: source ------------------------------------ */

void test_apply_source_ops(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_id128 src0 = p->atlases[0].sources[0].id;
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* source.add */
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = aid;
    op.u.source_add.source_id = id_of(0xB1);
    op.u.source_add.kind = TP_SOURCE_KIND_FILE;
    op.u.source_add.key = (char *)"more/img.png";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(2, p->atlases[0].source_count);
    tp_project_source *added = tp_project_atlas_find_source_by_id(&p->atlases[0], id_of(0xB1));
    TEST_ASSERT_NOT_NULL(added);
    TEST_ASSERT_EQUAL_INT(TP_SOURCE_KIND_FILE, added->kind);

    memset(&op, 0, sizeof op); /* source.replace (reserved op) */
    op.kind = TP_OP_SOURCE_REPLACE;
    op.atlas_id = aid;
    op.u.source_ref.source_id = id_of(0xB1);
    op.u.source_ref.key = (char *)"other.png";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("other.png", tp_project_atlas_find_source_by_id(&p->atlases[0], id_of(0xB1))->path);

    memset(&op, 0, sizeof op); /* source.remove */
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = aid;
    op.u.source_ref.source_id = id_of(0xB1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].source_count);

    /* error: remove a non-existent source */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = aid;
    op.u.source_ref.source_id = id_of(0x77);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_apply(p, &op, &rej));
    (void)src0;
    tp_project_destroy(p);
}

/* ---- forward + error per kind: sprite ------------------------------------ */

void test_apply_sprite_ops(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_id128 src0 = p->atlases[0].sources[0].id;
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* sprite.override.set */
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = aid;
    op.u.sprite_set.source_id = src0;
    op.u.sprite_set.src_key = (char *)"hero.png";
    op.u.sprite_set.mask = TP_SPF_ORIGIN | TP_SPF_SLICE9;
    op.u.sprite_set.origin_x = 0.25F;
    op.u.sprite_set.origin_y = 0.75F;
    op.u.sprite_set.slice9[0] = 2;
    op.u.sprite_set.slice9[1] = 3;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_sprite *s = tp_project_atlas_find_sprite(&p->atlases[0], "hero"); /* bridge = strip_ext */
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->origin_x == 0.25F); /* exact in float; Unity float asserts are disabled here */
    TEST_ASSERT_EQUAL_UINT16(3, s->slice9_lrtb[1]);

    memset(&op, 0, sizeof op); /* sprite.name.set */
    op.kind = TP_OP_SPRITE_NAME_SET;
    op.atlas_id = aid;
    op.u.sprite_name.source_id = src0;
    op.u.sprite_name.src_key = (char *)"hero.png";
    op.u.sprite_name.name = (char *)"HERO";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    s = tp_project_atlas_find_sprite(&p->atlases[0], "hero");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("HERO", s->rename);

    memset(&op, 0, sizeof op); /* sprite.override.clear (partial: slice9 only) */
    op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    op.atlas_id = aid;
    op.u.sprite_clear.source_id = src0;
    op.u.sprite_clear.src_key = (char *)"hero.png";
    op.u.sprite_clear.mask = TP_SPF_SLICE9;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    s = tp_project_atlas_find_sprite(&p->atlases[0], "hero");
    TEST_ASSERT_NOT_NULL(s); /* still present: origin + rename remain */
    TEST_ASSERT_EQUAL_UINT16(0, s->slice9_lrtb[1]);

    memset(&op, 0, sizeof op); /* sprite.override.clear ALL -> drop record */
    op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    op.atlas_id = aid;
    op.u.sprite_clear.source_id = src0;
    op.u.sprite_clear.src_key = (char *)"hero.png";
    op.u.sprite_clear.mask = TP_SPF_ALL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite(&p->atlases[0], "hero"));

    /* error: override.set on a bad source reference */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = aid;
    op.u.sprite_set.source_id = id_of(0x66);
    op.u.sprite_set.src_key = (char *)"hero.png";
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("source_id", rej.field);
    tp_project_destroy(p);
}

/* F2-05a: a sprite op with a NIL source_id is a PENDING (name-keyed) override -- what
 * the CLI `sprite set`/`unset` builds on a SOURCE-LESS atlas (an override added by
 * export-key before any source scan). Apply keys it by the export bridge and leaves
 * the record pending; a NON-nil unknown source still rejects. */
void test_apply_sprite_pending_no_source(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_id128 aid = id_of(0x21);
    p->atlases[0].id = aid; /* address the source-less default atlas by a real id */
    p->atlases[0].id_synthetic = false;
    TEST_ASSERT_EQUAL_INT(0, p->atlases[0].source_count); /* no sources at all */
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* sprite.override.set, NIL source -> pending override */
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = aid;
    op.u.sprite_set.source_id = tp_id128_nil();
    op.u.sprite_set.src_key = (char *)"hero";
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    op.u.sprite_set.origin_x = 0.25F;
    op.u.sprite_set.origin_y = 0.75F;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_sprite *s = tp_project_atlas_find_sprite(&p->atlases[0], "hero");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->origin_x == 0.25F);
    TEST_ASSERT_TRUE(tp_id128_is_nil(s->source_ref)); /* stays pending (name-keyed) */

    memset(&op, 0, sizeof op); /* sprite.name.set, NIL source */
    op.kind = TP_OP_SPRITE_NAME_SET;
    op.atlas_id = aid;
    op.u.sprite_name.source_id = tp_id128_nil();
    op.u.sprite_name.src_key = (char *)"hero";
    op.u.sprite_name.name = (char *)"HERO";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    s = tp_project_atlas_find_sprite(&p->atlases[0], "hero");
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_STRING("HERO", s->rename);

    memset(&op, 0, sizeof op); /* sprite.override.clear ALL, NIL source -> drop record */
    op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    op.atlas_id = aid;
    op.u.sprite_clear.source_id = tp_id128_nil();
    op.u.sprite_clear.src_key = (char *)"hero";
    op.u.sprite_clear.mask = TP_SPF_ALL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite(&p->atlases[0], "hero"));
    /* clear on an absent record is an idempotent OK no-op. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));

    memset(&op, 0, sizeof op); /* a NON-nil unknown source still rejects (regression guard) */
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = aid;
    op.u.sprite_set.source_id = id_of(0x66);
    op.u.sprite_set.src_key = (char *)"hero";
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_apply(p, &op, &rej));
    tp_project_destroy(p);
}

/* ---- forward + error per kind: animation --------------------------------- */

void test_apply_anim_ops(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_op_reject rej;
    tp_operation op;
    char *frames2[] = {(char *)"a", (char *)"b"};

    memset(&op, 0, sizeof op); /* animation.create with initial frames */
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = aid;
    op.u.anim_create.anim_id = id_of(0xC1);
    op.u.anim_create.name = (char *)"walk";
    op.u.anim_create.fps = 12.0F;
    op.u.anim_create.playback = 1;
    op.u.anim_create.frames = frames2;
    op.u.anim_create.frame_count = 2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_anim *an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_NOT_NULL(an);
    TEST_ASSERT_EQUAL_INT(2, an->frame_count);
    TEST_ASSERT_TRUE(an->fps == 12.0F);

    memset(&op, 0, sizeof op); /* animation.settings.set */
    op.kind = TP_OP_ANIMATION_SETTINGS_SET;
    op.atlas_id = aid;
    op.u.anim_settings.anim_id = id_of(0xC1);
    op.u.anim_settings.mask = TP_ANF_FPS | TP_ANF_FLIP_H;
    op.u.anim_settings.fps = 24.0F;
    op.u.anim_settings.flip_h = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_TRUE(an->fps == 24.0F);
    TEST_ASSERT_TRUE(an->flip_h);

    memset(&op, 0, sizeof op); /* animation.frame.add (append) */
    op.kind = TP_OP_ANIMATION_FRAME_ADD;
    op.atlas_id = aid;
    op.u.anim_frame_add.anim_id = id_of(0xC1);
    op.u.anim_frame_add.frame = (char *)"c";
    op.u.anim_frame_add.index = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_EQUAL_INT(3, an->frame_count);
    TEST_ASSERT_EQUAL_STRING("c", an->frames[2].name);

    memset(&op, 0, sizeof op); /* animation.frame.move 0 -> 2 */
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = aid;
    op.u.anim_frame_move.anim_id = id_of(0xC1);
    op.u.anim_frame_move.from_index = 0;
    op.u.anim_frame_move.to_index = 2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_EQUAL_STRING("a", an->frames[2].name); /* 'a' moved to the end */

    memset(&op, 0, sizeof op); /* animation.frame.remove */
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = aid;
    op.u.anim_frame_rm.anim_id = id_of(0xC1);
    op.u.anim_frame_rm.index = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_EQUAL_INT(2, an->frame_count);

    memset(&op, 0, sizeof op); /* animation.frames.set (bulk replace) */
    char *one[] = {(char *)"x"};
    op.kind = TP_OP_ANIMATION_FRAMES_SET;
    op.atlas_id = aid;
    op.u.anim_frames_set.anim_id = id_of(0xC1);
    op.u.anim_frames_set.frames = one;
    op.u.anim_frames_set.frame_count = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    an = tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1));
    TEST_ASSERT_EQUAL_INT(1, an->frame_count);
    TEST_ASSERT_EQUAL_STRING("x", an->frames[0].name);

    /* error: frame.remove out of range */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
    op.atlas_id = aid;
    op.u.anim_frame_rm.anim_id = id_of(0xC1);
    op.u.anim_frame_rm.index = 9;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_operation_apply(p, &op, &rej));

    memset(&op, 0, sizeof op); /* animation.remove */
    op.kind = TP_OP_ANIMATION_REMOVE;
    op.atlas_id = aid;
    op.u.anim_ref.anim_id = id_of(0xC1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_NULL(tp_project_atlas_find_animation_by_id(&p->atlases[0], id_of(0xC1)));
    tp_project_destroy(p);
}

/* ---- forward + error per kind: target ------------------------------------ */

void test_apply_target_ops(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    tp_id128 t0 = p->atlases[0].targets[0].id;
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* target.create */
    op.kind = TP_OP_TARGET_CREATE;
    op.atlas_id = aid;
    op.u.target_create.target_id = id_of(0xD1);
    op.u.target_create.exporter_id = (char *)TP_EXPORTER_ID_JSON_NEOTOLIS;
    op.u.target_create.out_path = (char *)"out/x";
    op.u.target_create.enabled = true;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(2, p->atlases[0].target_count);

    memset(&op, 0, sizeof op); /* target.set on the base target */
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = aid;
    op.u.target_set.target_id = t0;
    op.u.target_set.exporter_id = (char *)TP_EXPORTER_ID_JSON_NEOTOLIS;
    op.u.target_set.out_path = (char *)"out/y";
    op.u.target_set.enabled = false;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_target *t = tp_project_atlas_find_target_by_id(&p->atlases[0], t0);
    TEST_ASSERT_EQUAL_STRING("out/y", t->out_path);
    TEST_ASSERT_FALSE(t->enabled);

    memset(&op, 0, sizeof op); /* target.remove */
    op.kind = TP_OP_TARGET_REMOVE;
    op.atlas_id = aid;
    op.u.target_ref.target_id = id_of(0xD1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].target_count);

    /* error: target.create with an unknown exporter id */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_CREATE;
    op.atlas_id = aid;
    op.u.target_create.target_id = id_of(0xD2);
    op.u.target_create.exporter_id = (char *)"no-such-exporter";
    op.u.target_create.out_path = (char *)"out/z";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("exporter_id", rej.field);
    tp_project_destroy(p);
}

/* ---- allocator failure BEFORE commit leaves the model BYTE-UNCHANGED ------ */

void test_alloc_fail_before_commit(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    char *before = serialize(p);

    char *frames3[] = {(char *)"a", (char *)"b", (char *)"c"};
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = aid;
    op.u.anim_create.anim_id = id_of(0xC1);
    op.u.anim_create.name = (char *)"walk";
    op.u.anim_create.fps = 30.0F;
    op.u.anim_create.frames = frames3;
    op.u.anim_create.frame_count = 3;

    tp_op_reject rej;
    tp_op__test_set_alloc_fail(2); /* fail the 3rd staging allocation (2nd frame dup) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(0, p->atlases[0].animation_count); /* no half-built animation */

    char *after = serialize(p);
    TEST_ASSERT_EQUAL_STRING(before, after); /* byte-unchanged */

    tp_op__test_set_alloc_fail(-1); /* now it succeeds */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].animation_count);
    TEST_ASSERT_EQUAL_INT(3, p->atlases[0].animations[0].frame_count);

    free(before);
    free(after);
    tp_project_destroy(p);
}

/* ---- PARITY: op apply == existing mutator (byte-identical project) -------- */

void test_parity_settings(void) {
    tp_project *seed = base_project();
    char *buf = serialize(seed);
    tp_project_destroy(seed);

    tp_project *a = NULL;
    tp_project *b = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &a, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &b, &err));

    /* engine path */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a->atlases[0].id;
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_SHAPE;
    op.u.atlas_settings.max_size = 2048;
    op.u.atlas_settings.shape = 1;
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(a, &op, &rej));

    /* existing-mutator path */
    b->atlases[0].max_size = 2048;
    b->atlases[0].shape = 1;

    char *sa = serialize(a);
    char *sb = serialize(b);
    TEST_ASSERT_EQUAL_STRING(sb, sa);

    free(buf);
    free(sa);
    free(sb);
    tp_project_destroy(a);
    tp_project_destroy(b);
}

void test_parity_sprite_override(void) {
    tp_project *seed = base_project();
    char *buf = serialize(seed);
    tp_project_destroy(seed);

    tp_project *a = NULL;
    tp_project *b = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &a, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &b, &err));
    tp_id128 src0 = a->atlases[0].sources[0].id;

    /* engine path: canonical {source_id, src_key} */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_SET;
    op.atlas_id = a->atlases[0].id;
    op.u.sprite_set.source_id = src0;
    op.u.sprite_set.src_key = (char *)"hero.png";
    op.u.sprite_set.mask = TP_SPF_ORIGIN;
    op.u.sprite_set.origin_x = 0.25F;
    op.u.sprite_set.origin_y = 0.75F;
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(a, &op, &rej));

    /* existing-mutator path: the name-keyed bridge, exactly as the CLI does */
    char bridge[512];
    tp_sprite_export_key("hero.png", bridge, sizeof bridge);
    tp_project_sprite *s = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_sprite(&b->atlases[0], bridge, &s));
    s->origin_x = 0.25F;
    s->origin_y = 0.75F;
    (void)tp_project_atlas_prune_sprite(&b->atlases[0], bridge);

    char *sa = serialize(a);
    char *sb = serialize(b);
    TEST_ASSERT_EQUAL_STRING(sb, sa);

    free(buf);
    free(sa);
    free(sb);
    tp_project_destroy(a);
    tp_project_destroy(b);
}

/* ---- selector -> operation builders (resolve / ambiguity / not-found) ----- */

void test_builders(void) {
    tp_project *p = base_project();
    tp_error err;
    tp_operation op;

    /* atlas.create (no selector) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_op_build_atlas_create(id_of(0xE1), "made", &op));
    TEST_ASSERT_EQUAL_INT(TP_OP_ATLAS_CREATE, (int)op.kind);
    TEST_ASSERT_EQUAL_STRING("made", op.u.atlas_create.name);
    tp_operation_free(&op);

    /* atlas.rename by name selector -> resolves to the atlas id */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_op_build_atlas_rename(p, "atlas1", "fresh", &op, NULL, &err));
    TEST_ASSERT_EQUAL_INT(TP_OP_ATLAS_RENAME, (int)op.kind);
    TEST_ASSERT_TRUE(tp_id128_eq(op.atlas_id, p->atlases[0].id));
    tp_operation_free(&op);

    /* not-found selector */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_op_build_atlas_rename(p, "nosuch", "x", &op, NULL, &err));

    /* ambiguous selector -> candidate list, never a first-match guess */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "twin", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "twin", NULL));
    tp_selector_candidates cand;
    memset(&cand, 0, sizeof cand);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_AMBIGUOUS_SELECTOR,
                          tp_op_build_atlas_rename(p, "twin", "x", &op, &cand, &err));
    TEST_ASSERT_EQUAL_INT(2, cand.count);
    tp_selector_candidates_free(&cand);
    tp_project_destroy(p);
}

/* ---- [1] a negative frame_count is rejected and leaves the model byte-unchanged --- */

void test_apply_negative_frames_unchanged(void) {
    tp_project *p = base_project();
    tp_id128 aid = p->atlases[0].id;
    char *before = serialize(p);
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* animation.create, frame_count = -1 */
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = aid;
    op.u.anim_create.anim_id = id_of(0xC1);
    op.u.anim_create.name = (char *)"walk";
    op.u.anim_create.fps = 12.0F;
    op.u.anim_create.frames = NULL;
    op.u.anim_create.frame_count = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(0, p->atlases[0].animation_count);

    char *after = serialize(p);
    TEST_ASSERT_EQUAL_STRING(before, after);
    free(before);
    free(after);
    tp_project_destroy(p);
}

/* ---- [2] source.add of a duplicate path is rejected (byte-unchanged); a real add
 *          still stamps the caller's source_id -------------------------------------- */

void test_apply_source_add_dup_rejected(void) {
    tp_project *p = base_project(); /* base has source "sprites" */
    tp_id128 aid = p->atlases[0].id;
    char *before = serialize(p);
    tp_op_reject rej;
    tp_operation op;

    memset(&op, 0, sizeof op); /* duplicate path -> rejected, model untouched */
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = aid;
    op.u.source_add.source_id = id_of(0xB1);
    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    op.u.source_add.key = (char *)"sprites";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("key", rej.field);
    TEST_ASSERT_EQUAL_INT(1, p->atlases[0].source_count);
    char *after = serialize(p);
    TEST_ASSERT_EQUAL_STRING(before, after);

    memset(&op, 0, sizeof op); /* a genuinely new path commits AND honors the id */
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = aid;
    op.u.source_add.source_id = id_of(0xB2);
    op.u.source_add.kind = TP_SOURCE_KIND_FILE;
    op.u.source_add.key = (char *)"more/img.png";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(2, p->atlases[0].source_count);
    TEST_ASSERT_NOT_NULL(tp_project_atlas_find_source_by_id(&p->atlases[0], id_of(0xB2)));

    free(before);
    free(after);
    tp_project_destroy(p);
}

/* ---- [7] PARITY: padding=8000 via op == via the mutator (byte-identical) ---------- */

void test_parity_padding_large(void) {
    tp_project *seed = base_project();
    char *buf = serialize(seed);
    tp_project_destroy(seed);

    tp_project *a = NULL;
    tp_project *b = NULL;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &a, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &b, &err));

    tp_operation op; /* engine path */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = a->atlases[0].id;
    op.u.atlas_settings.mask = TP_AF_PADDING;
    op.u.atlas_settings.padding = 8000; /* above the old artificial 4096 cap */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(a, &op, &rej));

    b->atlases[0].padding = 8000; /* existing-mutator path (what the CLI `set` does) */

    char *sa = serialize(a);
    char *sb = serialize(b);
    TEST_ASSERT_EQUAL_STRING(sb, sa);
    free(buf);
    free(sa);
    free(sb);
    tp_project_destroy(a);
    tp_project_destroy(b);
}

/* ---- [6] PARITY: frame.move {from:0,to:99} == the CLI move-to-end idiom ------------ */

void test_parity_frame_move_to_end(void) {
    tp_project *seed = tp_project_create();
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&seed->atlases[0], "walk", &an));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "a"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "b"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_add_frame(an, "c"));
    uint8_t ctr = 9;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(seed, &rng, &err));
    char *buf = serialize(seed);
    tp_project_destroy(seed);

    tp_project *a = NULL;
    tp_project *b = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &a, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_load_buffer(buf, strlen(buf), &b, &err));

    tp_operation op; /* engine op: move 0 -> 99 (large; clamps to end) */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = a->atlases[0].id;
    op.u.anim_frame_move.anim_id = a->atlases[0].animations[0].id;
    op.u.anim_frame_move.from_index = 0;
    op.u.anim_frame_move.to_index = 99;
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(a, &op, &rej));

    /* CLI idiom: tp_project_anim_move_frame(an, from, to - from) with a big `to` */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_anim_move_frame(&b->atlases[0].animations[0], 0, 99 - 0));

    char *sa = serialize(a);
    char *sb = serialize(b);
    TEST_ASSERT_EQUAL_STRING(sb, sa);
    TEST_ASSERT_EQUAL_STRING("a", a->atlases[0].animations[0].frames[2].name); /* moved to the end */
    free(buf);
    free(sa);
    free(sb);
    tp_project_destroy(a);
    tp_project_destroy(b);
}

/* ---- [5] builders scope the sub-entity to the RESOLVED atlas (no cross-atlas pair) - */

void test_builder_scopes_to_atlas(void) {
    tp_project *p = tp_project_create(); /* atlas[0] = "atlas1" */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&p->atlases[0], TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    tp_project_anim *an0 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&p->atlases[0], "walk", &an0));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", NULL));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&p->atlases[1], TP_EXPORTER_ID_JSON_NEOTOLIS, "out/b", NULL));
    tp_project_anim *an1 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&p->atlases[1], "run", &an1));
    uint8_t ctr = 3;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));

    tp_operation op;
    /* target "out/b" lives in atlas2; pairing it with atlas1 must NOT silently succeed */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        tp_op_build_target_set(p, "atlas1", "out/b", TP_EXPORTER_ID_JSON_NEOTOLIS, "out/b", true, &op, NULL, &err));
    /* the correct atlas resolves fine and pairs A+A */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_op_build_target_set(p, "atlas2", "out/b", TP_EXPORTER_ID_JSON_NEOTOLIS, "out/b2", true, &op, NULL, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(op.atlas_id, p->atlases[1].id));
    TEST_ASSERT_TRUE(tp_id128_eq(op.u.target_set.target_id, p->atlases[1].targets[0].id));
    tp_operation_free(&op);

    /* same scoping for animation.remove: anim "run" is in atlas2, not atlas1 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_op_build_anim_remove(p, "atlas1", "run", &op, NULL, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_op_build_anim_remove(p, "atlas2", "run", &op, NULL, &err));
    TEST_ASSERT_TRUE(tp_id128_eq(op.atlas_id, p->atlases[1].id));
    tp_operation_free(&op);
    tp_project_destroy(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_apply_atlas_ops);
    RUN_TEST(test_apply_source_ops);
    RUN_TEST(test_apply_sprite_ops);
    RUN_TEST(test_apply_sprite_pending_no_source);
    RUN_TEST(test_apply_anim_ops);
    RUN_TEST(test_apply_target_ops);
    RUN_TEST(test_alloc_fail_before_commit);
    RUN_TEST(test_parity_settings);
    RUN_TEST(test_parity_sprite_override);
    RUN_TEST(test_builders);
    RUN_TEST(test_apply_negative_frames_unchanged);
    RUN_TEST(test_apply_source_add_dup_rejected);
    RUN_TEST(test_parity_padding_large);
    RUN_TEST(test_parity_frame_move_to_end);
    RUN_TEST(test_builder_scopes_to_atlas);
    return UNITY_END();
}
