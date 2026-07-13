/* F2-01: operation catalog + closed field vocabulary + canonical byte-stable
 * encoder goldens + payload/reference validation (invalid id / type / range /
 * reference each rejected with the right structured status). */

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "unity.h"

/* Deterministic distinct non-nil bytes per call (mirrors test_project_ids). */
static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}

void setUp(void) {}
void tearDown(void) {}

static tp_id128 id_of(uint8_t b) {
    tp_id128 x;
    for (int i = 0; i < 16; i++) {
        x.bytes[i] = b;
    }
    return x;
}

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
    op.atlas_id = id_of(0x11);
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
    op.atlas_id = id_of(0x11);
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

void test_encode_result_committed(void) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_CREATE;
    op.atlas_id = id_of(0x11);
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
    op.atlas_id = id_of(0x11);
    tp_op_reject rej;
    rej.status = TP_STATUS_OUT_OF_RANGE;
    (void)snprintf(rej.field, sizeof rej.field, "%s", "max_size");
    (void)snprintf(rej.message, sizeof rej.message, "%s", "max_size = 99999 must be in [1..4096]");
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
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
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
    op.atlas_id = id_of(0x55); /* no such atlas */
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
    op.u.atlas_settings.max_size = 99999; /* > 4096 */
    tp_op_reject rej;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, tp_operation_validate(p, &op, &rej));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_RANGE, rej.status);
    TEST_ASSERT_EQUAL_STRING("max_size", rej.field);
    /* a valid value passes */
    op.u.atlas_settings.max_size = 2048;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_operation_validate(p, &op, &rej));
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
    RUN_TEST(test_encode_result_committed);
    RUN_TEST(test_encode_result_rejected);
    RUN_TEST(test_validate_unknown_op);
    RUN_TEST(test_validate_invalid_id);
    RUN_TEST(test_validate_bad_reference);
    RUN_TEST(test_validate_out_of_range);
    return UNITY_END();
}
