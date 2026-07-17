#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "unity.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"

void setUp(void) {}
void tearDown(void) {}

static tp_status load_text(const char *text, tp_project **out,
                           tp_error *error) {
    return tp_project_load_buffer(text, strlen(text), out, error);
}

static char *dup_text(const char *text) {
    const size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    if (copy) {
        memcpy(copy, text, size);
    }
    return copy;
}

void test_schema_version_is_exact_only(void) {
    static const int rejected[] = {1, 2, 3, 4, 6};
    for (size_t i = 0U; i < sizeof rejected / sizeof rejected[0]; i++) {
        char json[64];
        (void)snprintf(json, sizeof json,
                       "{\"version\":%d,\"atlases\":[]}", rejected[i]);
        tp_project *project = NULL;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_VERSION,
                              load_text(json, &project, &error));
        TEST_ASSERT_NULL(project);
    }

    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        load_text("{\"version\":5,\"atlases\":[]}", &project, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(project);
    tp_project_destroy(project);
}

void test_missing_or_wrong_version_is_bad_project(void) {
    static const char *const invalid[] = {
        "{\"atlases\":[]}",
        "{\"version\":\"5\",\"atlases\":[]}",
        "{\"version\":5.5,\"atlases\":[]}",
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        tp_project *project = NULL;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                              load_text(invalid[i], &project, &error));
        TEST_ASSERT_NULL(project);
    }
}

void test_loader_rejects_c_string_ambiguous_json(void) {
    static const char *const invalid[] = {
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"hero\\u0000suffix\"}]}",
        "{\"version\":5,\"version\":5,\"atlases\":[]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"first\",\"name\":\"shadow\"}]}",
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        tp_project *project = NULL;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                              load_text(invalid[i], &project, &error));
        TEST_ASSERT_NULL(project);
        TEST_ASSERT_TRUE(error.msg[0] != '\0');
    }

    static const char raw_nul[] =
        "{\"version\":5,\"atlases\":[]}\0shadow";
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_PROJECT,
        tp_project_load_buffer(raw_nul, sizeof raw_nul - 1U, &project,
                               &error));
    TEST_ASSERT_NULL(project);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "NUL"));
}

static tp_status load_project_with_name_bytes(
    const unsigned char *name_bytes, size_t name_len, tp_project **out,
    tp_error *error) {
    static const char prefix[] =
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"";
    static const char suffix[] = "\"}]}";
    char json[256];
    const size_t prefix_len = sizeof prefix - 1U;
    const size_t suffix_len = sizeof suffix - 1U;
    TEST_ASSERT_TRUE(prefix_len + name_len + suffix_len <= sizeof json);
    memcpy(json, prefix, prefix_len);
    memcpy(json + prefix_len, name_bytes, name_len);
    memcpy(json + prefix_len + name_len, suffix, suffix_len);
    return tp_project_load_buffer(json, prefix_len + name_len + suffix_len,
                                  out, error);
}

void test_loader_rejects_invalid_raw_utf8_before_json_lowering(void) {
    static const unsigned char stray_continuation[] = {0x80U};
    static const unsigned char overlong[] = {0xc0U, 0xafU};
    static const unsigned char truncated[] = {0xe2U, 0x82U};
    static const unsigned char bad_continuation[] = {0xe2U, 0x28U, 0xa1U};
    static const unsigned char surrogate[] = {0xedU, 0xa0U, 0x80U};
    static const unsigned char above_unicode_max[] = {0xf4U, 0x90U, 0x80U,
                                                       0x80U};
    static const struct {
        const unsigned char *bytes;
        size_t len;
    } invalid[] = {
        {stray_continuation, sizeof stray_continuation},
        {overlong, sizeof overlong},
        {truncated, sizeof truncated},
        {bad_continuation, sizeof bad_continuation},
        {surrogate, sizeof surrogate},
        {above_unicode_max, sizeof above_unicode_max},
    };

    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        tp_project *project = NULL;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_BAD_PROJECT,
            load_project_with_name_bytes(invalid[i].bytes, invalid[i].len,
                                         &project, &error));
        TEST_ASSERT_NULL(project);
        TEST_ASSERT_NOT_NULL(strstr(error.msg, "UTF-8"));
    }
}

void test_loader_accepts_well_formed_multibyte_utf8(void) {
    static const unsigned char name[] = {
        0xd0U, 0x90U,             /* Cyrillic A */
        0xe2U, 0x82U, 0xacU,      /* Euro sign */
        0xf0U, 0x9fU, 0x98U, 0x80U /* grinning face */
    };
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        load_project_with_name_bytes(name, sizeof name, &project, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(project);
    tp_project_destroy(project);
}

static void assert_unknown_project_key_rejected(const char *json) {
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          load_text(json, &project, &error));
    TEST_ASSERT_NULL(project);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "unknown"));
}

void test_loader_requires_root_atlases(void) {
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          load_text("{\"version\":5}", &project, &error));
    TEST_ASSERT_NULL(project);
    TEST_ASSERT_NOT_NULL(strstr(error.msg, "atlases"));
}

/* A canonical-v5 typo must be rejected at admission. Accepting it and then
 * saving would silently erase user intent, so every schema object is closed. */
void test_loader_rejects_unknown_keys_at_every_schema_object(void) {
    static const char *const invalid[] = {
        "{\"version\":5,\"atlases\":[],\"verison\":5}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"paddding\":3}]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"sprites\",\"knd\":\"file\"}]}]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"sprites\"}],\"sprites\":[{"
        "\"source\":\"source_00000000000000000000000000000002\","
        "\"key\":\"hero.png\",\"renmae\":\"hero\"}]}]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"animations\":[{"
        "\"id\":\"anim_00000000000000000000000000000002\","
        "\"name\":\"walk\",\"frames\":[],\"flp_h\":true}]}]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"sprites\"}],\"animations\":[{"
        "\"id\":\"anim_00000000000000000000000000000003\","
        "\"name\":\"walk\",\"frames\":[{"
        "\"source\":\"source_00000000000000000000000000000002\","
        "\"key\":\"hero.png\",\"ky\":\"lost.png\"}]}]}]}",
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"targets\":[{"
        "\"id\":\"target_00000000000000000000000000000002\","
        "\"exporter_id\":\"defold\",\"out_path\":\"out/atlas\","
        "\"enabeld\":false}]}]}",
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert_unknown_project_key_rejected(invalid[i]);
    }
}

static void assert_sprite_override_json_rejected(const char *override_json) {
    char json[1024];
    const int written = snprintf(
        json, sizeof json,
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"sprites\"}],\"sprites\":[{"
        "\"source\":\"source_00000000000000000000000000000002\","
        "\"key\":\"hero.png\",%s}]}]}",
        override_json);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof json);
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          load_text(json, &project, &error));
    TEST_ASSERT_NULL(project);
}

void test_loader_rejects_unrepresentable_sprite_override_domains(void) {
    static const char *const invalid[] = {
        "\"shape\":-2",          "\"shape\":3",
        "\"allow_rotate\":-2",  "\"allow_rotate\":1",
        "\"max_vertices\":0",   "\"max_vertices\":17",
        "\"margin\":0",         "\"margin\":257",
        "\"extrude\":0",        "\"extrude\":257",
    };
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        assert_sprite_override_json_rejected(invalid[i]);
    }
}

void test_loader_accepts_exact_sprite_override_domain_edges(void) {
    static const char json[] =
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":[{"
        "\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"sprites\"}],\"sprites\":[{"
        "\"source\":\"source_00000000000000000000000000000002\","
        "\"key\":\"hero.png\",\"shape\":2,\"allow_rotate\":0,"
        "\"max_vertices\":16,\"margin\":255,\"extrude\":255},{"
        "\"source\":\"source_00000000000000000000000000000002\","
        "\"key\":\"inherit.png\",\"shape\":-1,\"allow_rotate\":-1,"
        "\"max_vertices\":-1,\"margin\":-1,\"extrude\":-1}]}]}";
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  load_text(json, &project, &error),
                                  error.msg);
    TEST_ASSERT_NOT_NULL(project);
    const tp_project_sprite *sprite = &project->atlases[0].sprites[0];
    TEST_ASSERT_EQUAL_INT(2, sprite->ov_shape);
    TEST_ASSERT_EQUAL_INT(0, sprite->ov_allow_rotate);
    TEST_ASSERT_EQUAL_INT(16, sprite->ov_max_vertices);
    TEST_ASSERT_EQUAL_INT(255, sprite->ov_margin);
    TEST_ASSERT_EQUAL_INT(255, sprite->ov_extrude);
    sprite = &project->atlases[0].sprites[1];
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sprite->ov_shape);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sprite->ov_allow_rotate);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sprite->ov_max_vertices);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sprite->ov_margin);
    TEST_ASSERT_EQUAL_INT(TP_PROJECT_OV_INHERIT, sprite->ov_extrude);
    tp_project_destroy(project);
}

void test_save_rejects_noncanonical_ids(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    char *json = NULL;
    size_t len = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_ID_MALFORMED,
                          tp_project_save_buffer(project, &json, &len, &error));
    TEST_ASSERT_NULL(json);
    TEST_ASSERT_EQUAL_size_t(0U, len);
    tp_project_destroy(project);
}

void test_writer_always_emits_schema_v5(void) {
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        load_text("{\"version\":5,\"atlases\":[]}", &project, &error),
        error.msg);

    char *json = NULL;
    size_t len = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_save_buffer(project, &json, &len, &error),
        error.msg);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"version\": 5"));
    free(json);
    tp_project_destroy(project);
}

static tp_project *make_canonical_project(tp_project_atlas **out_atlas) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, "sprites"));
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_assign_missing_ids(project, &rng, &error), error.msg);
    if (out_atlas) {
        *out_atlas = atlas;
    }
    return project;
}

void test_save_rejects_unknown_source_reference(void) {
    tp_project_atlas *atlas = NULL;
    tp_project *project = make_canonical_project(&atlas);
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "hero.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->source_ref.bytes[0] ^= 0x5aU;

    char *json = NULL;
    size_t len = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_project_save_buffer(project, &json, &len, &error));
    TEST_ASSERT_NULL(json);
    tp_project_destroy(project);
}

void test_save_rejects_duplicate_sprite_identity(void) {
    tp_project_atlas *atlas = NULL;
    tp_project *project = make_canonical_project(&atlas);
    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_sprite_by_source_key(
            atlas, atlas->sources[0].id, "hero.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);

    tp_project_sprite *grown = (tp_project_sprite *)realloc(
        atlas->sprites, 2U * sizeof *atlas->sprites);
    TEST_ASSERT_NOT_NULL(grown);
    atlas->sprites = grown;
    atlas->sprite_cap = 2;
    atlas->sprites[1] = atlas->sprites[0];
    atlas->sprites[1].name = dup_text(atlas->sprites[0].name);
    atlas->sprites[1].src_key = dup_text(atlas->sprites[0].src_key);
    atlas->sprites[1].rename = NULL;
    TEST_ASSERT_NOT_NULL(atlas->sprites[1].name);
    TEST_ASSERT_NOT_NULL(atlas->sprites[1].src_key);
    atlas->sprite_count = 2;

    char *json = NULL;
    size_t len = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_project_save_buffer(project, &json, &len, &error));
    TEST_ASSERT_NULL(json);
    tp_project_destroy(project);
}

void test_loader_rejects_duplicate_normalized_source_path(void) {
    static const char json[] =
        "{\"version\":5,\"atlases\":[{"
        "\"id\":\"atlas_00000000000000000000000000000001\","
        "\"name\":\"a\",\"sources\":["
        "{\"id\":\"source_00000000000000000000000000000002\","
        "\"path\":\"art\\\\ui\"},"
        "{\"id\":\"source_00000000000000000000000000000003\","
        "\"path\":\"art/ui\"}]}]}";
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          load_text(json, &project, &error));
    TEST_ASSERT_NULL(project);
}

void test_save_rejects_duplicate_normalized_source_path(void) {
    tp_project_atlas *atlas = NULL;
    tp_project *project = make_canonical_project(&atlas);
    TEST_ASSERT_TRUE(atlas->source_cap >= 2);
    atlas->sources[1] = atlas->sources[0];
    atlas->sources[1].id = tp_id128_nil();
    atlas->sources[1].path = dup_text("sprites");
    TEST_ASSERT_NOT_NULL(atlas->sources[1].path);
    atlas->source_count = 2;
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_project_assign_missing_ids(project, &rng, &error),
        error.msg);

    char *json = NULL;
    size_t len = 0U;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_project_save_buffer(project, &json, &len, &error));
    TEST_ASSERT_NULL(json);
    tp_project_destroy(project);
}

void test_save_rejects_invalid_source_shape(void) {
    tp_project_atlas *atlas = NULL;
    tp_project *project = make_canonical_project(&atlas);
    free(atlas->sources[0].path);
    atlas->sources[0].path = dup_text("");
    TEST_ASSERT_NOT_NULL(atlas->sources[0].path);
    atlas->sources[0].kind = (tp_source_kind)99;

    char *json = NULL;
    size_t len = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BAD_PROJECT,
                          tp_project_save_buffer(project, &json, &len, &error));
    TEST_ASSERT_NULL(json);
    tp_project_destroy(project);
}

void test_assign_missing_ids_rejects_negative_counts(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    project->atlases[0].source_count = -1;
    tp_rng rng = tp_rng_os();
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_BAD_PROJECT,
        tp_project_assign_missing_ids(project, &rng, &error));
    project->atlases[0].source_count = 0;
    tp_project_destroy(project);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_schema_version_is_exact_only);
    RUN_TEST(test_missing_or_wrong_version_is_bad_project);
    RUN_TEST(test_loader_rejects_c_string_ambiguous_json);
    RUN_TEST(test_loader_rejects_invalid_raw_utf8_before_json_lowering);
    RUN_TEST(test_loader_accepts_well_formed_multibyte_utf8);
    RUN_TEST(test_loader_requires_root_atlases);
    RUN_TEST(test_loader_rejects_unknown_keys_at_every_schema_object);
    RUN_TEST(test_loader_rejects_unrepresentable_sprite_override_domains);
    RUN_TEST(test_loader_accepts_exact_sprite_override_domain_edges);
    RUN_TEST(test_save_rejects_noncanonical_ids);
    RUN_TEST(test_writer_always_emits_schema_v5);
    RUN_TEST(test_save_rejects_unknown_source_reference);
    RUN_TEST(test_save_rejects_duplicate_sprite_identity);
    RUN_TEST(test_loader_rejects_duplicate_normalized_source_path);
    RUN_TEST(test_save_rejects_duplicate_normalized_source_path);
    RUN_TEST(test_save_rejects_invalid_source_shape);
    RUN_TEST(test_assign_missing_ids_rejects_negative_counts);
    return UNITY_END();
}
