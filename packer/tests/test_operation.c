/* F2-01: operation catalog + closed field vocabulary + canonical byte-stable
 * encoder goldens + payload/reference validation (invalid id / type / range /
 * reference each rejected with the right structured status). */

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h"          /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_identity.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_test_model.h"
#include "unity.h"

/* det_fill / id_of: shared byte-identical fixtures, see tp_test_model.h
 * (tp_test_det_fill / tp_test_id_of). */

void setUp(void) {}
void tearDown(void) {}

/* ---- catalog ------------------------------------------------------------- */

void test_catalog_selfcheck(void) { TEST_ASSERT_TRUE(tp_op_catalog_selfcheck()); }

void test_catalog_wire_roundtrip(void) {
    for (int k = TP_OP_INVALID + 1; k < TP_OP_KIND_COUNT; k++) {
        const tp_op_info *info = tp_op_info_by_kind((tp_op_kind)k);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_TRUE(info->wire[0] != '\0');
        TEST_ASSERT_EQUAL_INT(k, (int)tp_op_kind_from_wire(info->wire));
        TEST_ASSERT_EQUAL_STRING(info->wire, tp_op_wire((tp_op_kind)k));
    }
    TEST_ASSERT_EQUAL_INT(TP_OP_INVALID, (int)tp_op_kind_from_wire("no.such.op"));
    TEST_ASSERT_NULL(tp_op_info_by_kind(TP_OP_INVALID));
    TEST_ASSERT_NULL(tp_op_info_by_kind(TP_OP_KIND_COUNT));
}

void test_catalog_classes(void) {
    TEST_ASSERT_EQUAL_STRING("create", tp_op_class_name(tp_op_info_by_kind(TP_OP_ATLAS_CREATE)->effect));
    TEST_ASSERT_EQUAL_STRING("remove", tp_op_class_name(tp_op_info_by_kind(TP_OP_ATLAS_REMOVE)->effect));
    TEST_ASSERT_EQUAL_STRING("move", tp_op_class_name(tp_op_info_by_kind(TP_OP_ANIMATION_FRAME_MOVE)->effect));
    TEST_ASSERT_EQUAL_STRING("set", tp_op_class_name(tp_op_info_by_kind(TP_OP_ATLAS_SETTINGS_SET)->effect));
}

void test_field_vocab_no_raw_patch(void) {
    TEST_ASSERT_TRUE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, "op"));
    TEST_ASSERT_TRUE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, "atlas_id"));
    TEST_ASSERT_TRUE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, "name"));
    /* No raw field-patch escape hatch: a key outside the closed vocab is refused. */
    TEST_ASSERT_FALSE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, "max_size"));
    TEST_ASSERT_FALSE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, "/etc/passwd"));
    TEST_ASSERT_FALSE(tp_op_field_allowed(TP_OP_ATLAS_CREATE, NULL));
}

/* ---- encoder byte-goldens ------------------------------------------------ */

void test_encode_golden_atlas_create(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = tp_test_id_of(0x11);
    op.u.atlas_create.name = (char *)"hero"; /* not freed: literal, op not passed to tp_operation_free */
    char *j = tp_operation_encode(&op);
    TEST_ASSERT_NOT_NULL(j);
    const char *golden = "{\n"
                         "  \"op\": \"atlas.create\",\n"
                         "  \"atlas_id\": \"atlas_11111111111111111111111111111111\",\n"
                         "  \"name\": \"hero\"\n"
                         "}\n";
    TEST_ASSERT_EQUAL_STRING(golden, j);
    free(j);
}

void test_encode_golden_settings_sparse(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = tp_test_id_of(0x11);
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_SHAPE; /* sparse: only these two emit */
    op.u.atlas_settings.max_size = 1024;
    op.u.atlas_settings.shape = 2;
    char *j = tp_operation_encode(&op);
    TEST_ASSERT_NOT_NULL(j);
    const char *golden = "{\n"
                         "  \"op\": \"atlas.settings.set\",\n"
                         "  \"atlas_id\": \"atlas_11111111111111111111111111111111\",\n"
                         "  \"max_size\": 1024,\n"
                         "  \"shape\": 2\n"
                         "}\n";
    TEST_ASSERT_EQUAL_STRING(golden, j);
    free(j);
}

void test_encode_animation_frame_keeps_canonical_source_identity(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_ADD;
    op.atlas_id = tp_test_id_of(0x11);
    op.u.anim_frame_add.anim_id = tp_test_id_of(0x22);
    op.u.anim_frame_add.frame.source_id = tp_test_id_of(0x33);
    op.u.anim_frame_add.frame.src_key = (char *)"shared.png";
    op.u.anim_frame_add.index = -1;
    char *json = tp_operation_encode(&op);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"source_id\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"src_key\": \"shared.png\""));
    free(json);
}

void test_encode_result_committed(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = tp_test_id_of(0x11);
    op.u.atlas_create.name = (char *)"hero";
    char *j = tp_op_result_encode(&op, NULL);
    TEST_ASSERT_NOT_NULL(j);
    const char *golden = "{\n"
                         "  \"op\": \"atlas.create\",\n"
                         "  \"status\": \"committed\"\n"
                         "}\n";
    TEST_ASSERT_EQUAL_STRING(golden, j);
    free(j);
}

void test_encode_result_rejected(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = tp_test_id_of(0x11);
    tp_op_reject rej;
    rej.status = TP_STATUS_OUT_OF_RANGE;
    (void)snprintf(rej.field, sizeof rej.field, "%s", "max_size");
    (void)snprintf(rej.message, sizeof rej.message, "%s", "max_size = 99999 must be in [1..16384]");
    char *j = tp_op_result_encode(&op, &rej);
    TEST_ASSERT_NOT_NULL(j);
    /* structured status id + offending field + prose, keys ascending */
    TEST_ASSERT_NOT_NULL(strstr(j, "\"code\": \"out_of_range\""));
    TEST_ASSERT_NOT_NULL(strstr(j, "\"field\": \"max_size\""));
    TEST_ASSERT_NOT_NULL(strstr(j, "\"status\": \"rejected\""));
    /* code precedes field precedes op precedes status (ascending) */
    TEST_ASSERT_TRUE(strstr(j, "\"code\"") < strstr(j, "\"field\""));
    TEST_ASSERT_TRUE(strstr(j, "\"op\"") < strstr(j, "\"status\""));
    free(j);
}

/* ---- validation: invalid id / type / range / reference ------------------- */

/* A project with one default atlas whose id is promoted to a real (addressable)
 * value. Returns the atlas id via *out_aid. */
static tp_project *promoted_project(tp_id128 *out_aid) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    if (out_aid) {
        *out_aid = p->atlases[0].id;
    }
    return p;
}

void test_validate_unknown_op(void) {
    tp_project *p = tp_project_create();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = (tp_op_kind)9999;
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_OP, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_UNKNOWN_OP, rej.status);
    tp_project_destroy(p);
}

void test_validate_invalid_id(void) {
    tp_project *p = tp_project_create();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE; /* nil atlas_id -> malformed */
    op.u.atlas_create.name = (char *)"x";
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("atlas_id", rej.field);
    tp_project_destroy(p);
}

void test_validate_bad_reference(void) {
    tp_project *p = tp_project_create();
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_test_id_of(0x55); /* no such atlas */
    op.u.atlas_rename.name = (char *)"x";
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("atlas_id", rej.field);
    tp_project_destroy(p);
}

void test_validate_out_of_range(void) {
    tp_id128 aid;
    tp_project *p = promoted_project(&aid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = aid;
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    op.u.atlas_settings.max_size = 99999; /* > the 16384 build cap */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, rej.status);
    TEST_ASSERT_EQUAL_STRING("max_size", rej.field);
    /* a valid value passes */
    op.u.atlas_settings.max_size = 2048;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

/* A project with one atlas holding one animation of N named frames, ids promoted.
 * *out_anid receives the animation's id. */
static tp_project *anim_project(tp_id128 *out_aid, tp_id128 *out_anid, int frames) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&p->atlases[0], "sprites"));
    tp_project_anim *an = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&p->atlases[0], "walk", &an));
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    const tp_id128 source_id = p->atlases[0].sources[0].id;
    const char *names[3] = {"a", "b", "c"};
    for (int i = 0; i < frames && i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_project_anim_add_frame(an, source_id, names[i]));
    }
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    if (out_aid) {
        *out_aid = p->atlases[0].id;
    }
    if (out_anid) {
        *out_anid = p->atlases[0].animations[0].id;
    }
    return p;
}

/* [1] a negative frame_count is rejected (would loop &frames[-1] in apply -> underflow),
 * for BOTH animation.create and animation.frames.set, never touching the NULL frames ptr. */
void test_validate_negative_frame_count(void) {
    tp_id128 aid;
    tp_id128 anid;
    tp_project *p = anim_project(&aid, &anid, 0);
    tp_op_reject rej;

    tp_operation op; /* animation.create: valid fps/playback so we REACH the frame check */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = aid;
    op.u.anim_create.anim_id = tp_test_id_of(0xC1);
    op.u.anim_create.name = (char *)"run";
    op.u.anim_create.fps = 12.0F;
    op.u.anim_create.playback = 0;
    op.u.anim_create.frames = NULL;
    op.u.anim_create.frame_count = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("frame_count", rej.field);

    memset(&op, 0, sizeof op); /* animation.frames.set on the existing anim */
    op.kind = TP_OP_ANIMATION_FRAMES_SET;
    op.atlas_id = aid;
    op.u.anim_frames_set.anim_id = anid;
    op.u.anim_frames_set.frames = NULL;
    op.u.anim_frames_set.frame_count = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("frame_count", rej.field);
    tp_project_destroy(p);
}

/* [1b] a positive frame_count with a NULL frames array is rejected (the apply loop would deref
 * frames[i] on a null pointer), for BOTH animation.create and animation.frames.set. */
void test_validate_null_frames_array(void) {
    tp_id128 aid;
    tp_id128 anid;
    tp_project *p = anim_project(&aid, &anid, 0);
    tp_op_reject rej;

    tp_operation op; /* animation.create: valid fps/playback so we REACH the frame check */
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_CREATE;
    op.atlas_id = aid;
    op.u.anim_create.anim_id = tp_test_id_of(0xC2);
    op.u.anim_create.name = (char *)"run";
    op.u.anim_create.fps = 12.0F;
    op.u.anim_create.playback = 0;
    op.u.anim_create.frames = NULL;
    op.u.anim_create.frame_count = 1; /* claims a frame but the array is null */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("frames", rej.field);

    memset(&op, 0, sizeof op); /* animation.frames.set on the existing anim */
    op.kind = TP_OP_ANIMATION_FRAMES_SET;
    op.atlas_id = aid;
    op.u.anim_frames_set.anim_id = anid;
    op.u.anim_frames_set.frames = NULL;
    op.u.anim_frames_set.frame_count = 2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("frames", rej.field);
    tp_project_destroy(p);
}

void test_validate_animation_frame_requires_normalized_source_key(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], "sprites"));
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(&project->atlases[0], "walk",
                                       &animation));
    uint8_t counter = 43U;
    tp_rng rng = {tp_test_det_fill, &counter};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &error));

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_FRAME_ADD;
    operation.atlas_id = project->atlases[0].id;
    operation.u.anim_frame_add.anim_id = animation->id;
    operation.u.anim_frame_add.frame.source_id =
        project->atlases[0].sources[0].id;
    operation.u.anim_frame_add.frame.src_key = (char *)"./hero.png";
    operation.u.anim_frame_add.index = -1;
    tp_op_reject reject;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("frame", reject.field);
    tp_project_destroy(project);
}

/* [3] atlas.create rejects a duplicate NAME (CLI `atlas add` policy). */
void test_validate_atlas_create_dup_name(void) {
    tp_id128 aid;
    tp_project *p = promoted_project(&aid); /* default atlas name "atlas1" */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = tp_test_id_of(0xA5);
    op.u.atlas_create.name = (char *)"atlas1";
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("name", rej.field);
    op.u.atlas_create.name = (char *)"brandnew"; /* a fresh name passes */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

/* [4] atlas.rename rejects a collision with ANOTHER atlas; rename-to-self is allowed. */
void test_validate_atlas_rename_collision(void) {
    tp_project *p = tp_project_create(); /* atlas[0] = "atlas1" */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_add_atlas(p, "atlas2", NULL));
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = p->atlases[0].id;
    op.u.atlas_rename.name = (char *)"atlas2"; /* collides with the other atlas */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("name", rej.field);
    op.u.atlas_rename.name = (char *)"atlas1"; /* rename to self: allowed */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.atlas_rename.name = (char *)"atlas3"; /* fresh name: allowed */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

void test_validate_atlas_name_shape_is_core_owned(void) {
    tp_id128 aid;
    tp_project *p = promoted_project(&aid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = aid;
    tp_op_reject rej;
    op.u.atlas_rename.name = (char *)"bad/name";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("name", rej.field);
    op.u.atlas_rename.name = (char *)"...";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));
    op.u.atlas_rename.name = (char *)"safe.name";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

/* H/P1-2: animation.rename rejects an empty name, an unknown anim_id, and a collision with
 * ANOTHER animation (INVALID_ARGUMENT); rename-to-self and a fresh name are allowed. */
void test_validate_anim_rename(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_anim *a1 = NULL;
    tp_project_anim *a2 = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&p->atlases[0], "walk", &a1));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(&p->atlases[0], "run", &a2));
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    tp_id128 aid = p->atlases[0].id;
    tp_id128 walk_id = p->atlases[0].animations[0].id;

    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_RENAME;
    op.atlas_id = aid;
    op.u.anim_rename.anim_id = walk_id;
    tp_op_reject rej;

    op.u.anim_rename.name = (char *)""; /* empty -> invalid_argument */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("name", rej.field);

    op.u.anim_rename.name = (char *)"run"; /* collides with the other animation */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("name", rej.field);

    op.u.anim_rename.name = (char *)"walk"; /* rename to self: allowed (no-op) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.anim_rename.name = (char *)"sprint"; /* a fresh name: allowed */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));

    op.u.anim_rename.anim_id = tp_test_id_of(0x99); /* unknown anim id -> not_found */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_NOT_FOUND, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("anim_id", rej.field);
    tp_project_destroy(p);
}

/* [2] source.add rejects a path that already exists (the mutator would silently dedupe
 * and strand the op's source_id). Backslash form normalizes to the same path. */
void test_validate_source_add_dup_path(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(&p->atlases[0], "sprites/hero"));
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = p->atlases[0].id;
    op.u.source_add.source_id = tp_test_id_of(0xB9);
    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    op.u.source_add.key = (char *)"sprites/hero"; /* exact duplicate */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("key", rej.field);
    op.u.source_add.key = (char *)"sprites\\hero"; /* normalizes to the same path -> also dup */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    op.u.source_add.key = (char *)"sprites/villain"; /* a genuinely new path passes */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

/* A saved project owns one canonical identity for source paths. Validation must
 * compare that identity, not the caller's relative spelling, because apply and
 * durable replay store the resolved path. */
void test_validate_source_add_dup_relative_to_saved_project(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    free(p->project_dir);
    static const char project_dir[] = "/tmp/ntpacker-project";
    p->project_dir = (char *)malloc(sizeof project_dir);
    TEST_ASSERT_NOT_NULL(p->project_dir);
    memcpy(p->project_dir, project_dir, sizeof project_dir);
    static const char source_base[] = "/tmp/ntpacker-project";
    p->source_base_dir = (char *)malloc(sizeof source_base);
    TEST_ASSERT_NOT_NULL(p->source_base_dir);
    memcpy(p->source_base_dir, source_base, sizeof source_base);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&p->atlases[0],
                                                      "sprites/hero"));
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));

    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = p->atlases[0].id;
    op.u.source_add.source_id = tp_test_id_of(0xB8);
    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    op.u.source_add.key = (char *)"sprites/hero";
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("key", rej.field);
    tp_project_destroy(p);
}

void test_validate_saved_source_keys_preserve_empty_and_replace_uniqueness(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    static const char project_dir[] = "/tmp/ntpacker-project";
    p->project_dir = (char *)malloc(sizeof project_dir);
    p->source_base_dir = (char *)malloc(sizeof project_dir);
    TEST_ASSERT_NOT_NULL(p->project_dir);
    TEST_ASSERT_NOT_NULL(p->source_base_dir);
    memcpy(p->project_dir, project_dir, sizeof project_dir);
    memcpy(p->source_base_dir, project_dir, sizeof project_dir);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&p->atlases[0], "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&p->atlases[0], "two"));
    uint8_t ctr = 9;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));

    tp_operation op = {0};
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = p->atlases[0].id;
    op.u.source_add.source_id = tp_test_id_of(0xB7);
    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    op.u.source_add.key = (char *)"";
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));

    op.kind = TP_OP_SOURCE_REPLACE;
    op.u.source_ref.source_id = p->atlases[0].sources[0].id;
    op.u.source_ref.key = (char *)"";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));
    op.u.source_ref.key = (char *)"two";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("key", rej.field);
    tp_project_destroy(p);
}

/* [2b] source.add rejects a kind outside the currently-valid set {FOLDER, FILE}: apply stores kind
 * verbatim and it re-serializes by token, so an out-of-range kind (99, or the reserved ATLAS=2)
 * would silently coerce to "folder" on reload -> live-vs-disk divergence. FOLDER/FILE still pass. */
void test_validate_source_add_bad_kind(void) {
    tp_id128 aid;
    tp_project *p = promoted_project(&aid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = aid;
    op.u.source_add.source_id = tp_test_id_of(0xBA);
    op.u.source_add.key = (char *)"sprites/new";
    tp_op_reject rej;

    op.u.source_add.kind = (tp_source_kind)2; /* reserved ATLAS -- not valid until Epic B1 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("kind", rej.field);
    op.u.source_add.kind = (tp_source_kind)99; /* garbage from a direct-built op */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("kind", rej.field);

    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER; /* the two valid kinds still pass */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.source_add.kind = TP_SOURCE_KIND_FILE;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

void test_validate_source_paths_fail_closed(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_source(&project->atlases[0], "existing"));
    uint8_t counter = 23;
    tp_rng rng = {tp_test_det_fill, &counter};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));

    char oversized[TP_IDENTITY_PATH_MAX + 1U];
    memset(oversized, 'x', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    tp_operation op = {0};
    op.kind = TP_OP_SOURCE_ADD;
    op.atlas_id = project->atlases[0].id;
    op.u.source_add.source_id = tp_test_id_of(0xB6);
    op.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    op.u.source_add.key = oversized;
    tp_op_reject reject;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_operation_validate(project, &op, &reject));
    TEST_ASSERT_EQUAL_STRING("key", reject.field);

    op.kind = TP_OP_SOURCE_REPLACE;
    op.u.source_ref.source_id = project->atlases[0].sources[0].id;
    op.u.source_ref.key = oversized;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_operation_validate(project, &op, &reject));
    TEST_ASSERT_EQUAL_STRING("key", reject.field);
    tp_project_destroy(project);
}

void test_source_attached_sprite_ops_preserve_structural_identity(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(&project->atlases[0], "sprites"));
    uint8_t counter = 19U;
    tp_rng rng = {tp_test_det_fill, &counter};
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(project, &rng, &err));
    const tp_id128 atlas_id = project->atlases[0].id;
    const tp_id128 source_id = project->atlases[0].sources[0].id;

    tp_operation operation;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas_id;
    operation.u.sprite_set.source_id = source_id;
    operation.u.sprite_set.src_key = (char *)"characters/hero.png";
    operation.u.sprite_set.mask = TP_SPF_ORIGIN;
    operation.u.sprite_set.origin_x = 0.25F;
    operation.u.sprite_set.origin_y = 0.75F;
    tp_op_reject reject;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &operation, &reject));
    tp_project_sprite *sprite =
        tp_project_atlas_find_sprite_by_source_key(
            &project->atlases[0], source_id, "characters/hero.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, sprite->source_ref));
    TEST_ASSERT_EQUAL_STRING("characters/hero.png", sprite->src_key);

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_NAME_SET;
    operation.atlas_id = atlas_id;
    operation.u.sprite_name.source_id = source_id;
    operation.u.sprite_name.src_key = (char *)"characters/hero.png";
    operation.u.sprite_name.name = (char *)"hero_final";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &operation, &reject));
    sprite = tp_project_atlas_find_sprite_by_source_key(
        &project->atlases[0], source_id, "characters/hero.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, sprite->source_ref));
    TEST_ASSERT_EQUAL_STRING("characters/hero.png", sprite->src_key);
    TEST_ASSERT_EQUAL_STRING("hero_final", sprite->rename);

    /* Clearing one canonical identity must not alter a neighboring identity. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_set_sprite_rename_by_source_key(
            &project->atlases[0], source_id, "characters/enemy-other.png",
            "enemy_final"));
    sprite = tp_project_atlas_find_sprite_by_source_key(
        &project->atlases[0], source_id, "characters/enemy-other.png");
    TEST_ASSERT_NOT_NULL(sprite);
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    operation.atlas_id = atlas_id;
    operation.u.sprite_clear.source_id = source_id;
    operation.u.sprite_clear.src_key = (char *)"characters/enemy.png";
    operation.u.sprite_clear.mask = TP_SPF_ORIGIN;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_operation_apply(project, &operation, &reject));
    sprite = tp_project_atlas_find_sprite_by_source_key(
        &project->atlases[0], source_id, "characters/enemy-other.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_EQUAL_STRING("enemy_final", sprite->rename);
    TEST_ASSERT_NULL(tp_project_atlas_find_sprite_by_source_key(
        &project->atlases[0], source_id, "characters/enemy.png"));
    tp_project_destroy(project);
}

/* Atlas spacing is bounded by the effective page size. This is a builder safety
 * contract, not a UI preset: every client must be unable to persist a project
 * that reaches nt_builder's padding/margin assertions. */
void test_validate_knob_bounds_match_cli(void) {
    tp_id128 aid;
    tp_project *p = promoted_project(&aid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = aid;
    tp_op_reject rej;
    const int page_size = p->atlases[0].max_size;

    op.u.atlas_settings.mask = TP_AF_PADDING;
    op.u.atlas_settings.padding = page_size;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.atlas_settings.padding = page_size + 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("padding", rej.field);
    op.u.atlas_settings.padding = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("padding", rej.field);

    op.u.atlas_settings.mask = TP_AF_MARGIN;
    op.u.atlas_settings.margin = page_size + 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("margin", rej.field);

    op.u.atlas_settings.mask = TP_AF_EXTRUDE | TP_AF_SHAPE;
    op.u.atlas_settings.extrude = page_size + 1;
    op.u.atlas_settings.shape = TP_PACK_SHAPE_MIN;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("extrude", rej.field);

    /* Shrinking max_size must also validate unchanged effective spacing. */
    p->atlases[0].padding = 10;
    op.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    op.u.atlas_settings.max_size = 9;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("max_size", rej.field);
    p->atlases[0].padding = 0;

    op.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    op.u.atlas_settings.max_size = 8192; /* a shipped GUI preset, <= the build cap: now accepted (F4) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.atlas_settings.max_size = 99999; /* far over any page cap: still rejected */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("max_size", rej.field);
    tp_project_destroy(p);
}

void test_validate_sprite_spacing_override_respects_page_size(void) {
    tp_project *project = tp_test_base_project();
    tp_project_atlas *atlas = &project->atlases[0];
    tp_operation operation = {0};
    tp_op_reject reject;

    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas->id;
    operation.u.sprite_set.source_id = atlas->sources[0].id;
    operation.u.sprite_set.src_key = (char *)"hero.png";
    operation.u.sprite_set.mask = TP_SPF_MARGIN;
    operation.u.sprite_set.ov_margin = atlas->max_size + 1;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_RANGE,
        tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_margin", reject.field);

    operation.u.sprite_set.mask = TP_SPF_EXTRUDE;
    operation.u.sprite_set.ov_extrude = atlas->max_size + 1;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_RANGE,
        tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_extrude", reject.field);

    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "hero.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->ov_margin = 100;
    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ATLAS_SETTINGS_SET;
    operation.atlas_id = atlas->id;
    operation.u.atlas_settings.mask = TP_AF_MAX_SIZE;
    operation.u.atlas_settings.max_size = 99;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_RANGE,
        tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("max_size", reject.field);
    tp_project_destroy(project);
}

void test_validate_sprite_overrides_match_pack_descriptor_domain(void) {
    tp_project *project = tp_test_base_project();
    tp_project_atlas *atlas = &project->atlases[0];
    atlas->shape = TP_PACK_SHAPE_MIN;
    atlas->max_size = 512;

    tp_operation operation = {0};
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas->id;
    operation.u.sprite_set.source_id = atlas->sources[0].id;
    operation.u.sprite_set.src_key = (char *)"hero.png";
    tp_op_reject reject;

    operation.u.sprite_set.mask = TP_SPF_ALLOW_ROTATE;
    operation.u.sprite_set.ov_allow_rotate = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_allow_rotate", reject.field);

    operation.u.sprite_set.mask = TP_SPF_MAX_VERTICES;
    operation.u.sprite_set.ov_max_vertices = 257;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_max_vertices", reject.field);

    operation.u.sprite_set.mask = TP_SPF_MARGIN;
    operation.u.sprite_set.ov_margin = 257;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_margin", reject.field);
    operation.u.sprite_set.ov_margin = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));

    operation.u.sprite_set.mask = TP_SPF_EXTRUDE;
    operation.u.sprite_set.ov_extrude = 257;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("ov_extrude", reject.field);
    operation.u.sprite_set.ov_extrude = 0;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE,
                          tp_operation_validate(project, &operation, &reject));

    tp_project_destroy(project);
}

void test_validate_rejects_invalid_utf8_in_persisted_strings(void) {
    tp_project *project = tp_test_base_project();
    tp_project_atlas *atlas = &project->atlases[0];
    char invalid_name[] = {'b', (char)0xC3, 'x', '\0'};
    tp_operation operation = {0};
    tp_op_reject reject;

    operation.kind = TP_OP_ATLAS_RENAME;
    operation.atlas_id = atlas->id;
    operation.u.atlas_rename.name = invalid_name;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("name", reject.field);

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_NAME_SET;
    operation.atlas_id = atlas->id;
    operation.u.sprite_name.source_id = atlas->sources[0].id;
    operation.u.sprite_name.src_key = (char *)"hero.png";
    operation.u.sprite_name.name = invalid_name;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("name", reject.field);

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_SET;
    operation.atlas_id = atlas->id;
    operation.u.target_set.target_id = atlas->targets[0].id;
    operation.u.target_set.mask = TP_TF_OUT_PATH;
    operation.u.target_set.out_path = invalid_name;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_UTF8,
                          tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("out_path", reject.field);

    tp_project_destroy(project);
}

void test_validate_rejects_unknown_presence_mask_bits(void) {
    tp_project *project = tp_test_base_project();
    tp_project_atlas *atlas = &project->atlases[0];
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_animation(atlas, "walk", &animation));
    uint8_t counter = 91U;
    tp_rng rng = {tp_test_det_fill, &counter};
    tp_error assign_error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &assign_error));

    const uint32_t unknown = UINT32_C(1) << 31;
    tp_operation operation = {0};
    tp_op_reject reject;

    operation.kind = TP_OP_ATLAS_SETTINGS_SET;
    operation.atlas_id = atlas->id;
    operation.u.atlas_settings.mask = TP_AF_PADDING | unknown;
    operation.u.atlas_settings.padding = 1;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_operation_validate(project, &operation, &reject));

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_SET;
    operation.atlas_id = atlas->id;
    operation.u.sprite_set.source_id = atlas->sources[0].id;
    operation.u.sprite_set.src_key = (char *)"hero.png";
    operation.u.sprite_set.mask = TP_SPF_ORIGIN | unknown;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_operation_validate(project, &operation, &reject));

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    operation.atlas_id = atlas->id;
    operation.u.sprite_clear.source_id = atlas->sources[0].id;
    operation.u.sprite_clear.src_key = (char *)"hero.png";
    operation.u.sprite_clear.mask = TP_SPF_ORIGIN | unknown;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_operation_validate(project, &operation, &reject));
    TEST_ASSERT_EQUAL_STRING("fields", reject.field);

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_ANIMATION_SETTINGS_SET;
    operation.atlas_id = atlas->id;
    operation.u.anim_settings.anim_id = animation->id;
    operation.u.anim_settings.mask = TP_ANF_FPS | unknown;
    operation.u.anim_settings.fps = 12.0F;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_operation_validate(project, &operation, &reject));

    memset(&operation, 0, sizeof operation);
    operation.kind = TP_OP_TARGET_SET;
    operation.atlas_id = atlas->id;
    operation.u.target_set.target_id = atlas->targets[0].id;
    operation.u.target_set.mask = TP_TF_ENABLED | unknown;
    operation.u.target_set.enabled = false;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_operation_validate(project, &operation, &reject));

    tp_project_destroy(project);
}

/* [6] frame.move accepts an unbounded to_index (CLI clamps); an invalid from is rejected. */
void test_validate_frame_move_to_index_unbounded(void) {
    tp_id128 aid;
    tp_id128 anid;
    tp_project *p = anim_project(&aid, &anid, 3);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ANIMATION_FRAME_MOVE;
    op.atlas_id = aid;
    op.u.anim_frame_move.anim_id = anid;
    op.u.anim_frame_move.from_index = 0;
    op.u.anim_frame_move.to_index = 99; /* large: accepted (was OUT_OF_BOUNDS before fix) */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.anim_frame_move.to_index = -5; /* negative: also accepted (clamps to front) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.anim_frame_move.from_index = 9; /* from out of range: still rejected */
    op.u.anim_frame_move.to_index = 1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_STRING("from_index", rej.field);
    tp_project_destroy(p);
}

/* C1: a project whose atlas[0] holds one target {json_neotolis, "out/orig", enabled}, ids
 * promoted. out_aid and out_tid receive the atlas + target ids. */
static tp_project *target_project(tp_id128 *out_aid, tp_id128 *out_tid) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_target *t = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_target(&p->atlases[0], TP_EXPORTER_ID_JSON_NEOTOLIS, "out/orig", &t));
    t->enabled = true;
    uint8_t ctr = 7;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_assign_missing_ids(p, &rng, &err));
    if (out_aid) {
        *out_aid = p->atlases[0].id;
    }
    if (out_tid) {
        *out_tid = p->atlases[0].targets[0].id;
    }
    return p;
}

/* C1: a MASKED target.set applies ONLY the flagged field; unmasked fields keep their
 * current value. mask = TP_TF_ENABLED changes enabled alone (exporter + out_path kept). */
void test_target_set_mask_enabled_only(void) {
    tp_id128 aid;
    tp_id128 tid;
    tp_project *p = target_project(&aid, &tid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = aid;
    op.u.target_set.target_id = tid;
    op.u.target_set.mask = TP_TF_ENABLED; /* only enabled -- exporter/out_path left unset (NULL) */
    op.u.target_set.enabled = false;
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_target *t = tp_project_atlas_find_target_by_id(&p->atlases[0], tid);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(t->enabled);                                         /* changed */
    TEST_ASSERT_EQUAL_STRING(TP_EXPORTER_ID_JSON_NEOTOLIS, t->exporter_id); /* preserved */
    TEST_ASSERT_EQUAL_STRING("out/orig", t->out_path);                     /* preserved */
    tp_project_destroy(p);
}

/* C1: mask = TP_TF_OUT_PATH with a valid non-empty path changes out_path alone; exporter
 * and enabled are preserved even though the op carries junk/opposite values for them. */
void test_target_set_mask_out_path_only(void) {
    tp_id128 aid;
    tp_id128 tid;
    tp_project *p = target_project(&aid, &tid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = aid;
    op.u.target_set.target_id = tid;
    op.u.target_set.mask = TP_TF_OUT_PATH;         /* only out_path */
    op.u.target_set.out_path = (char *)"out/new";
    op.u.target_set.exporter_id = NULL;            /* unset: must be ignored (bit not set) */
    op.u.target_set.enabled = false;               /* unset: must be ignored (bit not set) */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_apply(p, &op, &rej));
    tp_project_target *t = tp_project_atlas_find_target_by_id(&p->atlases[0], tid);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_STRING("out/new", t->out_path);                      /* changed */
    TEST_ASSERT_EQUAL_STRING(TP_EXPORTER_ID_JSON_NEOTOLIS, t->exporter_id); /* preserved */
    TEST_ASSERT_TRUE(t->enabled);                                          /* preserved (still initial true) */
    tp_project_destroy(p);
}

/* C1: with the OUT_PATH bit UNSET, the non-empty-out_path check is skipped -- a mask =
 * TP_TF_ENABLED op with a NULL/empty out_path validates OK. An all-zero mask names no
 * field and is rejected (mirrors atlas/anim settings). */
void test_target_set_mask_skips_unset_field_checks(void) {
    tp_id128 aid;
    tp_id128 tid;
    tp_project *p = target_project(&aid, &tid);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = aid;
    op.u.target_set.target_id = tid;
    op.u.target_set.mask = TP_TF_ENABLED;
    op.u.target_set.enabled = true;
    op.u.target_set.out_path = NULL;     /* empty out_path: NOT rejected -- OUT_PATH bit unset */
    op.u.target_set.exporter_id = NULL;  /* unknown exporter: NOT rejected -- EXPORTER bit unset */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.target_set.out_path = (char *)""; /* explicitly empty: still fine (bit unset) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
    op.u.target_set.mask = 0; /* names no field -> rejected */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, tp_operation_validate(p, &op, &rej));
    tp_project_destroy(p);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_catalog_selfcheck);
    RUN_TEST(test_catalog_wire_roundtrip);
    RUN_TEST(test_catalog_classes);
    RUN_TEST(test_field_vocab_no_raw_patch);
    RUN_TEST(test_encode_golden_atlas_create);
    RUN_TEST(test_encode_golden_settings_sparse);
    RUN_TEST(test_encode_animation_frame_keeps_canonical_source_identity);
    RUN_TEST(test_encode_result_committed);
    RUN_TEST(test_encode_result_rejected);
    RUN_TEST(test_validate_unknown_op);
    RUN_TEST(test_validate_invalid_id);
    RUN_TEST(test_validate_bad_reference);
    RUN_TEST(test_validate_out_of_range);
    RUN_TEST(test_validate_negative_frame_count);
    RUN_TEST(test_validate_null_frames_array);
    RUN_TEST(test_validate_animation_frame_requires_normalized_source_key);
    RUN_TEST(test_validate_atlas_create_dup_name);
    RUN_TEST(test_validate_atlas_rename_collision);
    RUN_TEST(test_validate_atlas_name_shape_is_core_owned);
    RUN_TEST(test_validate_anim_rename);
    RUN_TEST(test_validate_source_add_dup_path);
    RUN_TEST(test_validate_source_add_dup_relative_to_saved_project);
    RUN_TEST(test_validate_saved_source_keys_preserve_empty_and_replace_uniqueness);
    RUN_TEST(test_validate_source_add_bad_kind);
    RUN_TEST(test_validate_source_paths_fail_closed);
    RUN_TEST(test_source_attached_sprite_ops_preserve_structural_identity);
    RUN_TEST(test_validate_knob_bounds_match_cli);
    RUN_TEST(test_validate_sprite_spacing_override_respects_page_size);
    RUN_TEST(test_validate_sprite_overrides_match_pack_descriptor_domain);
    RUN_TEST(test_validate_rejects_invalid_utf8_in_persisted_strings);
    RUN_TEST(test_validate_rejects_unknown_presence_mask_bits);
    RUN_TEST(test_validate_frame_move_to_index_unbounded);
    RUN_TEST(test_target_set_mask_enabled_only);
    RUN_TEST(test_target_set_mask_out_path_only);
    RUN_TEST(test_target_set_mask_skips_unset_field_checks);
    return UNITY_END();
}
