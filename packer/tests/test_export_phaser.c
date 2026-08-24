#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "core/nt_assert.h"
#include "tp_core/tp_arena.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_export_internal.h"
#include "tp_format_binding_proto_internal.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_lua_export_adapter_internal.h"
#include "unity.h"

static tp_format_catalog *g_catalog;
static tp_export_page g_pages[2];
static tp_export_sprite g_sprites[4];
static tp_export_anim g_animation;
static tp_export_ir g_ir;

void setUp(void) {}
void tearDown(void) {}

static char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(file, path);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    const long length = ftell(file);
    TEST_ASSERT_TRUE(length >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *bytes = malloc((size_t)length + 1U);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t(
        (size_t)length, fread(bytes, 1, (size_t)length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    bytes[length] = '\0';
    *size = (size_t)length;
    return bytes;
}

static tp_format_catalog *open_phaser_catalog(void) {
    char descriptor_path[TP_IDENTITY_PATH_MAX];
    char source_path[TP_IDENTITY_PATH_MAX];
    TEST_ASSERT_TRUE(snprintf(
        descriptor_path, sizeof descriptor_path,
        "%s/phaser-3-multiatlas/format.json",
        TP_FORMAT_BUNDLED_ROOT) > 0);
    TEST_ASSERT_TRUE(snprintf(
        source_path, sizeof source_path,
        "%s/phaser-3-multiatlas/export.lua",
        TP_FORMAT_BUNDLED_ROOT) > 0);
    size_t descriptor_size = 0U;
    size_t source_size = 0U;
    char *descriptor = read_file(descriptor_path, &descriptor_size);
    char *source = read_file(source_path, &source_size);
    tp_format_descriptor_parse_result parsed = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_format_descriptor_v1_parse(
            (const unsigned char *)descriptor, descriptor_size,
            &parsed, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(
        TP_FORMAT_DESCRIPTOR_ADMITTED, parsed.outcome);

    char *package_path = malloc(sizeof "formats/phaser-3-multiatlas");
    TEST_ASSERT_NOT_NULL(package_path);
    memcpy(package_path, "formats/phaser-3-multiatlas",
           sizeof "formats/phaser-3-multiatlas");
    tp_format_binding_proto_binding binding = {
        .implementation = TP_FORMAT_IMPLEMENTATION_LUA,
        .descriptor = tp_format_owned_descriptor_view(
            parsed.owned_descriptor),
        .owned_descriptor = parsed.owned_descriptor,
        .api_version = TP_FORMAT_API_VERSION,
        .package_path = package_path,
        .descriptor_bytes = (const unsigned char *)descriptor,
        .descriptor_byte_count = descriptor_size,
        .source_bytes = (const unsigned char *)source,
        .source_byte_count = source_size,
    };
    memset(binding.fingerprint, '2', 32U);
    binding.fingerprint[32] = '\0';
    tp_format_binding_proto_value value = {
        .bindings = &binding,
        .binding_count = 1U,
        .preview = {.kind = TP_FORMAT_BINDING_RESOLUTION_BINDING,
                    .binding_index = 0U},
    };
    tp_format_catalog *catalog = NULL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_lua_export_catalog_create_worker(
            &value, NULL, "phaser-3-multiatlas", &catalog, &error),
        error.msg);
    return catalog;
}

static tp_export_sprite sprite(
    const char *name, int page, int x, int y, int width, int height) {
    tp_export_sprite result = {0};
    result.final_name = name;
    result.data.name = name;
    result.data.page = page;
    result.data.frame.x = x;
    result.data.frame.y = y;
    result.data.frame.w = width;
    result.data.frame.h = height;
    result.data.spriteSourceSize.w = width;
    result.data.spriteSourceSize.h = height;
    result.data.sourceSize.w = width;
    result.data.sourceSize.h = height;
    result.data.pivot.x = 0.5F;
    result.data.pivot.y = 0.5F;
    result.data.alias_of = -1;
    return result;
}

static void build_fixture(void) {
    g_pages[0] = (tp_export_page){.artifact_id = 0, .w = 64, .h = 32};
    g_pages[1] = (tp_export_page){.artifact_id = 1, .w = 32, .h = 64};

    g_sprites[0] = sprite("alias", 0, 10, 2, 16, 20);
    g_sprites[0].data.trimmed = true;
    g_sprites[0].data.spriteSourceSize.x = 2;
    g_sprites[0].data.spriteSourceSize.y = 3;
    g_sprites[0].data.spriteSourceSize.w = 16;
    g_sprites[0].data.spriteSourceSize.h = 20;
    g_sprites[0].data.sourceSize.w = 24;
    g_sprites[0].data.sourceSize.h = 30;
    g_sprites[0].data.pivot.x = 0.25F;
    g_sprites[0].data.pivot.y = 0.75F;
    g_sprites[0].data.alias_of = 1;

    g_sprites[1] = g_sprites[0];
    g_sprites[1].final_name = "hero";
    g_sprites[1].data.name = "hero";
    g_sprites[1].data.alias_of = -1;

    g_sprites[2] = sprite("panel", 1, 0, 20, 24, 32);
    g_sprites[2].data.sourceSize.w = 40;
    g_sprites[2].data.sourceSize.h = 48;
    g_sprites[2].data.spriteSourceSize.w = 24;
    g_sprites[2].data.spriteSourceSize.h = 32;
    g_sprites[2].data.trimmed = true;
    g_sprites[2].data.slice9_lrtb[0] = 8;
    g_sprites[2].data.slice9_lrtb[1] = 6;
    g_sprites[2].data.slice9_lrtb[2] = 5;
    g_sprites[2].data.slice9_lrtb[3] = 7;

    g_sprites[3] = sprite("rotate", 1, 5, 6, 10, 20);
    g_sprites[3].data.transform =
        (uint8_t)(TP_TRANSFORM_DIAGONAL | TP_TRANSFORM_FLIP_H);

    static const char *animation_frames[] = {"hero", "rotate"};
    g_animation = (tp_export_anim){
        .id = "walk", .frames = animation_frames, .frame_count = 2,
        .fps = 12.0F, .playback = 1,
    };
    g_ir = (tp_export_ir){
        .version = TP_EXPORT_IR_VERSION,
        .atlas_name = "phaser-atlas",
        .pixels_per_unit = 1.0F,
        .pages = g_pages,
        .page_count = 2,
        .sprites = g_sprites,
        .sprite_count = 4,
    };
}

static tp_export_document_batch serialize_ir(
    const tp_export_ir *ir, tp_format_diagnostic_report **diagnostics,
    tp_status *status) {
    const tp_exporter *exporter = tp_format_catalog_exporter_find(
        g_catalog, "phaser-3-multiatlas");
    TEST_ASSERT_NOT_NULL(exporter);
    tp_arena *arena = tp_arena_create(0);
    TEST_ASSERT_NOT_NULL(arena);
    tp_export_artifact_plan plan = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_export_artifact_plan_build(
            exporter->format, ir, "phaser-atlas", arena, &plan, &error),
        error.msg);
    tp_export_document_batch batch = {0};
    *status = tp_export_serialize_and_validate_documents(
        exporter, ir, &plan, NULL, diagnostics, &batch,
        NULL, NULL, &error);
    tp_arena_destroy(arena);
    return batch;
}

static void assert_keys(
    const cJSON *object, const char *const *keys, size_t count) {
    const cJSON *member = object ? object->child : NULL;
    for (size_t index = 0U; index < count; ++index) {
        TEST_ASSERT_NOT_NULL(member);
        TEST_ASSERT_EQUAL_STRING(keys[index], member->string);
        member = member->next;
    }
    TEST_ASSERT_NULL(member);
}

static void assert_number(const cJSON *object, const char *key, int value) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    TEST_ASSERT_TRUE(cJSON_IsNumber(item));
    TEST_ASSERT_EQUAL_INT(value, item->valueint);
}

static void assert_rect(
    const cJSON *frame, const char *key,
    int x, int y, int width, int height) {
    static const char *const keys[] = {"x", "y", "w", "h"};
    const cJSON *rect = cJSON_GetObjectItemCaseSensitive(frame, key);
    TEST_ASSERT_TRUE(cJSON_IsObject(rect));
    assert_keys(rect, keys, 4U);
    assert_number(rect, "x", x);
    assert_number(rect, "y", y);
    assert_number(rect, "w", width);
    assert_number(rect, "h", height);
}

static void test_exact_document_and_reserved_names(void) {
    tp_status first_status = TP_STATUS_OK;
    tp_status second_status = TP_STATUS_OK;
    tp_export_document_batch first =
        serialize_ir(&g_ir, NULL, &first_status);
    tp_export_document_batch second =
        serialize_ir(&g_ir, NULL, &second_status);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, first_status);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, second_status);
    TEST_ASSERT_EQUAL_INT(1, first.document_count);
    TEST_ASSERT_EQUAL_UINT64(
        first.documents[0].size, second.documents[0].size);
    TEST_ASSERT_EQUAL_MEMORY(
        first.documents[0].data, second.documents[0].data,
        first.documents[0].size);
    TEST_ASSERT_TRUE(first.documents[0].size > 1U);
    const unsigned char *first_bytes = first.documents[0].data;
    TEST_ASSERT_EQUAL_CHAR(
        '\n', first_bytes[first.documents[0].size - 1U]);
    TEST_ASSERT_NULL(memchr(
        first.documents[0].data, '\r', first.documents[0].size));
    TEST_ASSERT_NULL(memchr(
        first.documents[0].data, '\0', first.documents[0].size));
    TEST_ASSERT_NOT_NULL(strstr(
        (const char *)first.documents[0].data,
        "      \"frames\": [\n        {\n          \"filename\": \"alias\""));
    tp_export_document_batch_destroy(&second);
    tp_export_document_batch_destroy(&first);

    static const char *const reserved[] = {
        "__BASE", "__proto__", "hasOwnProperty"};
    const char *saved_name = g_sprites[0].final_name;
    for (size_t index = 0U; index < 3U; ++index) {
        g_sprites[0].final_name = reserved[index];
        tp_format_diagnostic_report *diagnostics = NULL;
        tp_status failure = TP_STATUS_OK;
        tp_export_document_batch rejected =
            serialize_ir(&g_ir, &diagnostics, &failure);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, failure);
        TEST_ASSERT_EQUAL_INT(0, rejected.document_count);
        TEST_ASSERT_NOT_NULL(diagnostics);
        TEST_ASSERT_EQUAL_UINT64(
            1U, tp_format_diagnostic_report_count(diagnostics));
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(diagnostics, 0U);
        TEST_ASSERT_NOT_NULL(diagnostic);
        TEST_ASSERT_EQUAL_INT(
            TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED, diagnostic->code);
        TEST_ASSERT_NOT_NULL(strstr(diagnostic->message, reserved[index]));
        tp_format_diagnostic_report_destroy(diagnostics);
        tp_export_document_batch_destroy(&rejected);
    }
    g_sprites[0].final_name = saved_name;
}

static void test_capability_projection_and_notices(void) {
    const tp_format_descriptor *format =
        tp_format_catalog_find_available(g_catalog, "phaser-3-multiatlas");
    TEST_ASSERT_NOT_NULL(format);
    TEST_ASSERT_EQUAL_HEX8(0x21, format->caps.transform_mask);
    TEST_ASSERT_FALSE(format->caps.polygons);
    TEST_ASSERT_TRUE(format->caps.pivot);
    TEST_ASSERT_TRUE(format->caps.slice9);
    TEST_ASSERT_TRUE(format->caps.multipage);
    TEST_ASSERT_TRUE(format->caps.aliases);
    TEST_ASSERT_FALSE(format->caps.animations);

    tp_pack_settings requested = {0};
    tp_pack_settings_defaults(&requested);
    requested.shape = 2;
    requested.allowed_transforms = TP_EXPORT_TRANSFORMS_ALL;
    tp_pack_settings effective = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_effective_settings(
            &requested, &format->caps, &effective));
    TEST_ASSERT_EQUAL_INT(0, effective.shape);
    TEST_ASSERT_EQUAL_HEX8(0x21, effective.allowed_transforms);

    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = &project->atlases[0];
    atlas->shape = 2;
    atlas->allow_transform = true;
    tp_export_ir with_animation = g_ir;
    with_animation.animations = &g_animation;
    with_animation.animation_count = 1;
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_export_predict_loss(
            project, 0, &format->caps, format->id,
            &with_animation, &notices, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(3, notices.count);
    TEST_ASSERT_EQUAL_INT(TP_NOTICE_FIELD_TRANSFORM, notices.items[0].field_id);
    TEST_ASSERT_EQUAL_INT(TP_NOTICE_FIELD_POLYGON, notices.items[1].field_id);
    TEST_ASSERT_EQUAL_INT(TP_NOTICE_FIELD_ANIMATION, notices.items[2].field_id);
    for (int index = 0; index < notices.count; ++index) {
        TEST_ASSERT_EQUAL_INT(
            TP_NOTICE_REASON_CAPS_UNSUPPORTED,
            notices.items[index].reason_id);
    }
    tp_export_notices_free(&notices);
    tp_project_destroy(project);
}

static void test_independent_consumer_parser(void) {
    tp_status status = TP_STATUS_OK;
    tp_export_document_batch batch = serialize_ir(&g_ir, NULL, &status);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, status);
    cJSON *root = cJSON_ParseWithLength(
        (const char *)batch.documents[0].data,
        batch.documents[0].size);
    TEST_ASSERT_NOT_NULL(root);
    static const char *const root_keys[] = {"textures"};
    static const char *const page_keys[] = {
        "image", "format", "size", "scale", "frames"};
    static const char *const frame_keys[] = {
        "filename", "frame", "rotated", "trimmed",
        "spriteSourceSize", "sourceSize", "pivot"};
    static const char *const slice_frame_keys[] = {
        "filename", "frame", "rotated", "trimmed",
        "spriteSourceSize", "sourceSize", "pivot", "scale9Borders"};
    assert_keys(root, root_keys, 1U);
    cJSON *textures = cJSON_GetObjectItemCaseSensitive(root, "textures");
    TEST_ASSERT_TRUE(cJSON_IsArray(textures));
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(textures));

    const char *const expected_names[][2] = {
        {"alias", "hero"}, {"panel", "rotate"}};
    const char *last_name = NULL;
    for (int page_index = 0; page_index < 2; ++page_index) {
        cJSON *page = cJSON_GetArrayItem(textures, page_index);
        assert_keys(page, page_keys, 5U);
        char expected_image[32];
        TEST_ASSERT_TRUE(snprintf(
            expected_image, sizeof expected_image,
            "phaser-atlas-%d.png", page_index) > 0);
        TEST_ASSERT_EQUAL_STRING(
            expected_image,
            cJSON_GetObjectItemCaseSensitive(page, "image")->valuestring);
        TEST_ASSERT_EQUAL_STRING(
            "RGBA8888",
            cJSON_GetObjectItemCaseSensitive(page, "format")->valuestring);
        assert_number(page, "scale", 1);
        cJSON *size = cJSON_GetObjectItemCaseSensitive(page, "size");
        static const char *const size_keys[] = {"w", "h"};
        assert_keys(size, size_keys, 2U);
        assert_number(size, "w", g_pages[page_index].w);
        assert_number(size, "h", g_pages[page_index].h);
        cJSON *frames = cJSON_GetObjectItemCaseSensitive(page, "frames");
        TEST_ASSERT_TRUE(cJSON_IsArray(frames));
        TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(frames));
        for (int local = 0; local < 2; ++local) {
            cJSON *frame = cJSON_GetArrayItem(frames, local);
            const int sprite_index = page_index * 2 + local;
            const tp_sprite *expected = &g_sprites[sprite_index].data;
            assert_keys(
                frame,
                sprite_index == 2 ? slice_frame_keys : frame_keys,
                sprite_index == 2 ? 8U : 7U);
            TEST_ASSERT_EQUAL_STRING(
                expected_names[page_index][local],
                cJSON_GetObjectItemCaseSensitive(
                    frame, "filename")->valuestring);
            const char *actual_name = cJSON_GetObjectItemCaseSensitive(
                frame, "filename")->valuestring;
            TEST_ASSERT_TRUE(strcmp(actual_name, "__BASE") != 0);
            TEST_ASSERT_TRUE(strcmp(actual_name, "__proto__") != 0);
            TEST_ASSERT_TRUE(strcmp(actual_name, "hasOwnProperty") != 0);
            if (last_name) {
                TEST_ASSERT_TRUE(strcmp(last_name, actual_name) < 0);
            }
            last_name = actual_name;
            assert_rect(frame, "frame", expected->frame.x, expected->frame.y,
                        expected->frame.w, expected->frame.h);
            TEST_ASSERT_EQUAL_INT(
                expected->transform != 0,
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                    frame, "rotated")));
            TEST_ASSERT_EQUAL_INT(
                expected->trimmed,
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(
                    frame, "trimmed")));
            assert_rect(frame, "spriteSourceSize",
                        expected->spriteSourceSize.x,
                        expected->spriteSourceSize.y,
                        expected->spriteSourceSize.w,
                        expected->spriteSourceSize.h);
            TEST_ASSERT_TRUE(expected->spriteSourceSize.x >= 0);
            TEST_ASSERT_TRUE(expected->spriteSourceSize.y >= 0);
            TEST_ASSERT_TRUE(
                expected->spriteSourceSize.x +
                    expected->spriteSourceSize.w <=
                expected->sourceSize.w);
            TEST_ASSERT_TRUE(
                expected->spriteSourceSize.y +
                    expected->spriteSourceSize.h <=
                expected->sourceSize.h);
            cJSON *source_size = cJSON_GetObjectItemCaseSensitive(
                frame, "sourceSize");
            assert_keys(source_size, size_keys, 2U);
            assert_number(source_size, "w", expected->sourceSize.w);
            assert_number(source_size, "h", expected->sourceSize.h);
            cJSON *pivot = cJSON_GetObjectItemCaseSensitive(frame, "pivot");
            static const char *const pivot_keys[] = {"x", "y"};
            assert_keys(pivot, pivot_keys, 2U);
            TEST_ASSERT_TRUE(
                cJSON_GetObjectItemCaseSensitive(pivot, "x")->valuedouble ==
                (double)expected->pivot.x);
            TEST_ASSERT_TRUE(
                cJSON_GetObjectItemCaseSensitive(pivot, "y")->valuedouble ==
                (double)expected->pivot.y);
            TEST_ASSERT_EQUAL_INT(
                sprite_index == 2,
                cJSON_HasObjectItem(frame, "scale9Borders"));
            const int footprint_w = expected->transform != 0
                ? expected->frame.h : expected->frame.w;
            const int footprint_h = expected->transform != 0
                ? expected->frame.w : expected->frame.h;
            TEST_ASSERT_TRUE(expected->frame.x + footprint_w <=
                             g_pages[page_index].w);
            TEST_ASSERT_TRUE(expected->frame.y + footprint_h <=
                             g_pages[page_index].h);
        }
    }
    cJSON *panel = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(
            cJSON_GetArrayItem(textures, 1), "frames"), 0);
    assert_rect(panel, "scale9Borders", 8, 5, 26, 36);
    cJSON_Delete(root);
    tp_export_document_batch_destroy(&batch);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    if (argc != 2) {
        return 2;
    }
    build_fixture();
    g_catalog = open_phaser_catalog();
    NT_ASSERT(g_catalog);
    UNITY_BEGIN();
    if (strcmp(argv[1], "export_contract") == 0) {
        RUN_TEST(test_exact_document_and_reserved_names);
    } else if (strcmp(argv[1], "capability_matrix") == 0) {
        RUN_TEST(test_capability_projection_and_notices);
    } else if (strcmp(argv[1], "consumer_validate") == 0) {
        RUN_TEST(test_independent_consumer_parser);
    } else {
        tp_format_catalog_release(g_catalog);
        return 2;
    }
    const int result = UNITY_END();
    tp_format_catalog_release(g_catalog);
    return result;
}
