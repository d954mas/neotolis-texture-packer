/* Machine operation discovery must describe the same accepted vocabulary and
 * value domains as transaction admission, without advertising reserved slots. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "cJSON.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_transaction.h"
#include "unity.h"

static cJSON *catalog;
void setUp(void) {
    char *text = tp_op_catalog_encode();
    TEST_ASSERT_NOT_NULL(text);
    catalog = cJSON_Parse(text);
    free(text);
    TEST_ASSERT_NOT_NULL(catalog);
}
void tearDown(void) { cJSON_Delete(catalog); catalog = NULL; }

static const cJSON *member(const cJSON *object, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    TEST_ASSERT_NOT_NULL_MESSAGE(value, key);
    return value;
}
static const cJSON *descriptor(const char *wire) {
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, member(catalog, "operations")) {
        if (strcmp(member(row, "op")->valuestring, wire) == 0) return row;
    }
    TEST_FAIL_MESSAGE(wire);
    return NULL;
}
static const cJSON *schema(const char *wire) {
    return member(descriptor(wire), "input_schema");
}
static const cJSON *property(const char *wire, const char *key) {
    return member(member(schema(wire), "properties"), key);
}
static bool contains_string(const cJSON *array, const char *text) {
    const cJSON *value = NULL;
    cJSON_ArrayForEach(value, array) {
        if (cJSON_IsString(value) && strcmp(value->valuestring, text) == 0) return true;
    }
    return false;
}
static bool required(const char *wire, const char *key) {
    return contains_string(member(schema(wire), "required"), key);
}

static void test_complete_current_catalog_and_closed_vocabulary(void) {
    TEST_ASSERT_EQUAL_INT(1, member(catalog, "schema")->valueint);
    TEST_ASSERT_EQUAL_INT(TP_OP_KIND_COUNT - 3,
                          cJSON_GetArraySize(member(catalog, "operations")));
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, member(catalog, "operations")) {
        const char *wire = member(row, "op")->valuestring;
        const tp_op_info *info = tp_op_info_by_wire(wire);
        TEST_ASSERT_NOT_NULL(info);
        TEST_ASSERT_NOT_NULL(info->cli_verb);
        TEST_ASSERT_EQUAL_STRING(info->label, member(row, "label")->valuestring);
        TEST_ASSERT_EQUAL_STRING(info->label_template, member(row, "label_template")->valuestring);
        TEST_ASSERT_EQUAL_STRING(tp_op_class_name(info->effect), member(row, "effect")->valuestring);
        TEST_ASSERT_TRUE(cJSON_IsString(member(row, "target_kind")));
        const cJSON *input = member(row, "input_schema");
        TEST_ASSERT_EQUAL_STRING("https://json-schema.org/draft/2020-12/schema", member(input, "$schema")->valuestring);
        TEST_ASSERT_EQUAL_STRING("object", member(input, "type")->valuestring);
        TEST_ASSERT_TRUE(cJSON_IsFalse(member(input, "additionalProperties")));
        TEST_ASSERT_EQUAL_STRING(wire, member(property(wire, "op"), "const")->valuestring);
        TEST_ASSERT_TRUE(required(wire, "op"));
        TEST_ASSERT_TRUE(required(wire, "atlas_id"));
        const cJSON *prop = NULL;
        cJSON_ArrayForEach(prop, member(input, "properties")) {
            TEST_ASSERT_TRUE_MESSAGE(tp_op_field_allowed(info->kind, prop->string), prop->string);
        }
        /* The union of emitted keys also catches omissions of an allowed key. */
        const cJSON *other = NULL;
        cJSON_ArrayForEach(other, member(catalog, "operations")) {
            const cJSON *key = NULL;
            cJSON_ArrayForEach(key, member(member(other, "input_schema"), "properties")) {
                TEST_ASSERT_EQUAL_INT(tp_op_field_allowed(info->kind, key->string),
                    cJSON_HasObjectItem(member(input, "properties"), key->string));
            }
        }
    }
}

static void test_required_and_optional_arguments_follow_lowering(void) {
    TEST_ASSERT_TRUE(required("atlas.create", "name"));
    TEST_ASSERT_TRUE(required("source.add", "key"));
    TEST_ASSERT_FALSE(required("source.add", "kind"));
    TEST_ASSERT_EQUAL_STRING("string", member(property("source.add", "kind"), "type")->valuestring);
    TEST_ASSERT_TRUE(contains_string(member(property("source.add", "kind"), "enum"), "file"));
    TEST_ASSERT_TRUE(contains_string(member(property("source.add", "kind"), "enum"), "folder"));
    TEST_ASSERT_TRUE(required("animation.create", "anim_id"));
    TEST_ASSERT_TRUE(required("animation.create", "name"));
    TEST_ASSERT_FALSE(required("animation.create", "fps"));
    TEST_ASSERT_FALSE(required("animation.create", "frames"));
    TEST_ASSERT_TRUE(required("animation.frame.add", "frame"));
    TEST_ASSERT_FALSE(required("animation.frame.add", "index"));
    TEST_ASSERT_TRUE(required("animation.frame.remove", "index"));
    TEST_ASSERT_TRUE(required("animation.frame.move", "from_index"));
    TEST_ASSERT_TRUE(required("animation.frame.move", "to_index"));
    TEST_ASSERT_EQUAL_INT(-1, member(property("animation.frame.add", "index"), "minimum")->valueint);
    TEST_ASSERT_EQUAL_INT(0, member(property("animation.frame.remove", "index"), "minimum")->valueint);
    TEST_ASSERT_TRUE(member(property("animation.frame.move", "to_index"), "minimum")->valuedouble < 0);
    TEST_ASSERT_TRUE(required("target.create", "exporter_id"));
    TEST_ASSERT_TRUE(required("target.create", "out_path"));
    TEST_ASSERT_FALSE(required("target.create", "enabled"));
    TEST_ASSERT_TRUE(required("sprite.name.set", "name"));
    TEST_ASSERT_FALSE(cJSON_HasObjectItem(property("sprite.name.set", "name"), "minLength"));
}

static void test_field_metadata_and_group_arity_are_exposed(void) {
    const cJSON *padding = property("atlas.settings.set", "padding");
    size_t count = 0U;
    const tp_field_row *rows = tp_op_fields(TP_OP_ATLAS_SETTINGS_SET, &count);
    for (size_t i = 0; i < count; ++i) {
        const cJSON *field = property("atlas.settings.set", rows[i].key);
        TEST_ASSERT_EQUAL_STRING(rows[i].label, member(field, "title")->valuestring);
        if (rows[i].domain == TP_FIELD_DOMAIN_RANGE) {
            TEST_ASSERT_TRUE(rows[i].range_min ==
                member(field, rows[i].min_exclusive ? "exclusiveMinimum" : "minimum")->valuedouble);
            TEST_ASSERT_TRUE(rows[i].range_max == member(field, "maximum")->valuedouble);
        }
    }
    TEST_ASSERT_EQUAL_STRING("max_size", member(padding, "x-cap-key")->valuestring);
    const cJSON *inherit = property("sprite.override.set", "ov_shape");
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, member(inherit, "x-inherit")->valueint);
    TEST_ASSERT_EQUAL_STRING("shape", member(inherit, "x-clear")->valuestring);
    TEST_ASSERT_NOT_NULL(member(inherit, "anyOf"));
    const cJSON *deps = member(schema("sprite.override.set"), "dependentRequired");
    TEST_ASSERT_TRUE(contains_string(member(deps, "origin_x"), "origin_y"));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(member(deps, "slice9_l")));
    TEST_ASSERT_EQUAL_INT(1, member(property("sprite.override.clear", "fields"), "minItems")->valueint);
    TEST_ASSERT_EQUAL_INT(7, cJSON_GetArraySize(member(member(property("sprite.override.clear", "fields"), "items"), "enum")));
    const cJSON *frame = property("animation.frame.add", "frame");
    TEST_ASSERT_TRUE(cJSON_IsFalse(member(frame, "additionalProperties")));
    TEST_ASSERT_TRUE(contains_string(member(frame, "required"), "source_id"));
    TEST_ASSERT_TRUE(contains_string(member(frame, "required"), "src_key"));
}

static tp_status decode_operation(const char *op) {
    const char *prefix = "{\"schema\":1,\"transaction\":{\"id\":\"11111111111111111111111111111111\",\"expected_revision\":0,\"operations\":[";
    char *json = malloc(strlen(prefix) + strlen(op) + 5U);
    TEST_ASSERT_NOT_NULL(json);
    (void)sprintf(json, "%s%s]}}", prefix, op);
    tp_txn_request *request = NULL;
    tp_error error = {0};
    const tp_status status = tp_txn_request_decode(json, &request, &error);
    tp_txn_request_free(request);
    free(json);
    return status;
}
#define ATLAS "\"atlas_id\":\"atlas_11111111111111111111111111111111\""
#define ANIM "\"anim_id\":\"anim_22222222222222222222222222222222\""
#define SOURCE "\"source_id\":\"source_33333333333333333333333333333333\""
static void test_declared_shapes_match_real_decoder_examples(void) {
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, decode_operation("{\"op\":\"animation.create\"," ATLAS "," ANIM ",\"name\":\"walk\"}"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, decode_operation("{\"op\":\"source.add\"," ATLAS "," SOURCE ",\"key\":\"sprites\"}"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, decode_operation("{\"op\":\"sprite.name.set\"," ATLAS "," SOURCE ",\"src_key\":\"a.png\",\"name\":\"\"}"));
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, decode_operation("{\"op\":\"sprite.name.set\"," ATLAS "," SOURCE ",\"src_key\":\"a.png\",\"name\":null}"));
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, decode_operation("{\"op\":\"animation.frame.remove\"," ATLAS "," ANIM "}"));
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, decode_operation("{\"op\":\"sprite.override.set\"," ATLAS "," SOURCE ",\"src_key\":\"a.png\",\"origin_x\":0.5}"));
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, decode_operation("{\"op\":\"source.add\"," ATLAS "," SOURCE ",\"key\":\"sprites\",\"kind\":1}"));
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--catalog") == 0) {
        char *text = tp_op_catalog_encode();
        if (!text) return 1;
        (void)puts(text);
        free(text);
        return 0;
    }
    UNITY_BEGIN();
    RUN_TEST(test_complete_current_catalog_and_closed_vocabulary);
    RUN_TEST(test_required_and_optional_arguments_follow_lowering);
    RUN_TEST(test_field_metadata_and_group_arity_are_exposed);
    RUN_TEST(test_declared_shapes_match_real_decoder_examples);
    return UNITY_END();
}
