#include <stdint.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static int sequence_fill(void *context, uint8_t *out, size_t length) {
    uint8_t *sequence = (uint8_t *)context;
    for (size_t i = 0U; i < length; i++) {
        out[i] = (uint8_t)(*sequence + (uint8_t)i + 1U);
    }
    (*sequence)++;
    return (int)length;
}

static int fail_fill(void *context, uint8_t *out, size_t length) {
    (void)context;
    (void)out;
    (void)length;
    return -1;
}

static int constant_fill(void *context, uint8_t *out, size_t length) {
    (void)context;
    memset(out, 0x5a, length);
    return (int)length;
}

static tp_project *make_project_with_entities(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_animation(atlas, "idle", NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(atlas, "json-neotolis", "out", NULL));
    return project;
}

void test_assign_missing_ids_is_atomic_and_idempotent(void) {
    tp_project *project = make_project_with_entities();
    uint8_t sequence = 1U;
    tp_rng rng = {sequence_fill, &sequence};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &error), error.msg);

    tp_project_atlas *atlas = &project->atlases[0];
    const tp_id128 atlas_id = atlas->id;
    const tp_id128 source_id = atlas->sources[0].id;
    const tp_id128 animation_id = atlas->animations[0].id;
    const tp_id128 target_id = atlas->targets[0].id;
    TEST_ASSERT_FALSE(tp_id128_is_nil(atlas_id));
    TEST_ASSERT_FALSE(tp_id128_is_nil(source_id));
    TEST_ASSERT_FALSE(tp_id128_is_nil(animation_id));
    TEST_ASSERT_FALSE(tp_id128_is_nil(target_id));

    tp_rng must_not_run = {fail_fill, NULL};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &must_not_run, &error));
    TEST_ASSERT_TRUE(tp_id128_eq(atlas_id, atlas->id));
    TEST_ASSERT_TRUE(tp_id128_eq(source_id, atlas->sources[0].id));
    TEST_ASSERT_TRUE(tp_id128_eq(animation_id, atlas->animations[0].id));
    TEST_ASSERT_TRUE(tp_id128_eq(target_id, atlas->targets[0].id));
    tp_project_destroy(project);
}

void test_rng_failure_leaves_every_id_unchanged(void) {
    tp_project *project = make_project_with_entities();
    tp_rng rng = {fail_fill, NULL};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_RNG_FAILED,
        tp_project_assign_missing_ids(project, &rng, &error));
    const tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->sources[0].id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->animations[0].id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->targets[0].id));
    tp_project_destroy(project);
}

void test_generated_collision_leaves_every_id_unchanged(void) {
    tp_project *project = make_project_with_entities();
    tp_rng rng = {constant_fill, NULL};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_DUPLICATE_ID,
        tp_project_assign_missing_ids(project, &rng, &error));
    const tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->sources[0].id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->animations[0].id));
    TEST_ASSERT_TRUE(tp_id128_is_nil(atlas->targets[0].id));
    tp_project_destroy(project);
}

static tp_status load_json(const char *json, tp_project **out,
                           tp_error *error) {
    return tp_project_load_buffer(json, strlen(json), out, error);
}

void test_loaded_structural_ids_are_required_and_unique(void) {
    static const char *const missing =
        "{\"version\":5,\"atlases\":[{\"name\":\"a\"}]}";
    static const char *const duplicate =
        "{\"version\":5,\"atlases\":["
        "{\"id\":\"atlas_11111111111111111111111111111111\",\"name\":\"a\"},"
        "{\"id\":\"atlas_11111111111111111111111111111111\",\"name\":\"b\"}]}";
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                          load_json(missing, &project, &error));
    TEST_ASSERT_NULL(project);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          load_json(duplicate, &project, &error));
    TEST_ASSERT_NULL(project);
}

void test_loaded_structural_id_text_is_strict(void) {
    static const char *const invalid[] = {
        "{\"version\":5,\"atlases\":[{\"id\":\"atlas_0000000000000000000000000000000g\",\"name\":\"a\"}]}",
        "{\"version\":5,\"atlases\":[{\"id\":\"atlas_00000000000000000000000000000000\",\"name\":\"a\"}]}",
        "{\"version\":5,\"atlases\":[{\"id\":\"source_00000000000000000000000000000001\",\"name\":\"a\"}]}",
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        tp_project *project = NULL;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                              load_json(invalid[i], &project, &error));
        TEST_ASSERT_NULL(project);
        TEST_ASSERT_TRUE(error.msg[0] != '\0');
    }
}

void test_loaded_structural_ids_are_unique_across_entity_kinds(void) {
    static const char json[] =
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_11111111111111111111111111111111\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_11111111111111111111111111111111\","
        "\"path\":\"sprites\"}]}]}";
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          load_json(json, &project, &error));
    TEST_ASSERT_NULL(project);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_assign_missing_ids_is_atomic_and_idempotent);
    RUN_TEST(test_rng_failure_leaves_every_id_unchanged);
    RUN_TEST(test_generated_collision_leaves_every_id_unchanged);
    RUN_TEST(test_loaded_structural_ids_are_required_and_unique);
    RUN_TEST(test_loaded_structural_id_text_is_strict);
    RUN_TEST(test_loaded_structural_ids_are_unique_across_entity_kinds);
    return UNITY_END();
}
