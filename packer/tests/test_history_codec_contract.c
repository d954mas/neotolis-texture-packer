/* Fixed v1 history-transition wire contract. Checked-in hex fixtures were
 * written directly from the big-endian grammar; no production wire helper
 * constructs the expected bytes. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"
#include "tp_diff_internal.h"
#include "tp_history_codec_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_test_model.h"
#include "unity.h"

static const char *g_forward_path;
static const char *g_reverse_path;
static const char *g_collections_forward_path;
static const char *g_collections_reverse_path;

void setUp(void) {}
void tearDown(void) {}

static tp_id128 id_last(unsigned char last_byte) {
    tp_id128 id = {{0U}};
    id.bytes[15] = last_byte;
    return id;
}

static char *copy_text(const char *text) {
    const size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, size);
    return copy;
}

static int hex_digit(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static uint8_t *read_hex(const char *path, size_t *out_len) {
    FILE *file = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    const long file_len = ftell(file);
    TEST_ASSERT_TRUE(file_len >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *text = (char *)malloc((size_t)file_len + 1U);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_size_t((size_t)file_len,
                             fread(text, 1U, (size_t)file_len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    text[file_len] = '\0';

    uint8_t *bytes = (uint8_t *)malloc((size_t)file_len / 2U + 1U);
    TEST_ASSERT_NOT_NULL(bytes);
    int high = -1;
    size_t count = 0U;
    for (long i = 0; i < file_len; i++) {
        const int digit = hex_digit((unsigned char)text[i]);
        if (digit < 0) {
            TEST_ASSERT_TRUE(text[i] == ' ' || text[i] == '\t' ||
                             text[i] == '\r' || text[i] == '\n');
            continue;
        }
        if (high < 0) {
            high = digit;
        } else {
            bytes[count++] = (uint8_t)((unsigned)high * 16U +
                                       (unsigned)digit);
            high = -1;
        }
    }
    TEST_ASSERT_EQUAL_INT(-1, high);
    free(text);
    *out_len = count;
    return bytes;
}

static tp_project *build_before_project(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *atlas = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    atlas->id = id_last(0x11U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_set_atlas_name(atlas, "old"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, "base"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_atlas_add_source(atlas, "keep"));
    atlas->sources[0].id = id_last(0x23U);
    atlas->sources[1].id = id_last(0x22U);
    static const char *const keys[] = {"s0.png", "s1.png", "s2.png"};
    for (int i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK, tp_project_atlas_add_sprite_by_source_key(
                              atlas, atlas->sources[0].id, keys[i], NULL));
    }
    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(
                                             atlas, "walk", &animation));
    animation->id = id_last(0x31U);
    tp_project_target *target = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_atlas_add_target(atlas, "json", "out/a", &target));
    target->id = id_last(0x41U);
    return project;
}

static tp_diff_record build_record(tp_diff_op ops[10]) {
    static tp_project_source added_source;
    static tp_project_frame after_frames[2];
    memset(ops, 0, 10U * sizeof *ops);
    added_source = (tp_project_source){id_last(0x21U),
                                       TP_SOURCE_KIND_FILE,
                                       (char *)"relative/file.png"};
    after_frames[0] = (tp_project_frame){NULL, id_last(0x21U),
                                         (char *)"a.png"};
    after_frames[1] = (tp_project_frame){NULL, id_last(0x22U),
                                         (char *)"b.png"};

    ops[0] = (tp_diff_op){.shape = TP_DIFF_SHAPE_COLL,
                          .atlas_id = id_last(0x11U),
                          .coll = TP_DIFF_COLL_SOURCE,
                          .position = 2,
                          .created = true,
                          .elem = &added_source};
    ops[1] = (tp_diff_op){.shape = TP_DIFF_SHAPE_ATLAS_NAME,
                          .atlas_id = id_last(0x11U),
                          .name_before = (char *)"old",
                          .name_after = (char *)"new"};
    ops[2] = (tp_diff_op){
        .shape = TP_DIFF_SHAPE_ATLAS_KNOBS,
        .atlas_id = id_last(0x11U),
        .knobs_before = {2048, 2, 0, 0, 1, 8, 2, true, true, 1.0F},
        .knobs_after = {1024, 4, 3, 2, 123, 12, 1, false, false, 2.5F}};
    ops[3] = (tp_diff_op){.shape = TP_DIFF_SHAPE_SOURCE_PATH,
                          .atlas_id = id_last(0x11U),
                          .entity_id = id_last(0x21U),
                          .path_before = (char *)"old/path",
                          .path_after = (char *)"new/path"};
    ops[4] = (tp_diff_op){.shape = TP_DIFF_SHAPE_TARGET_FIELDS,
                          .atlas_id = id_last(0x11U),
                          .entity_id = id_last(0x41U),
                          .exporter_before = (char *)"json",
                          .out_before = (char *)"out/a",
                          .enabled_before = true,
                          .exporter_after = (char *)"defold",
                          .out_after = (char *)"out/b",
                          .enabled_after = false};
    ops[5] = (tp_diff_op){.shape = TP_DIFF_SHAPE_ANIM_SETTINGS,
                          .atlas_id = id_last(0x11U),
                          .anim_id = id_last(0x31U),
                          .anim_before = {30.0F, 0, false, false},
                          .anim_after = {12.5F, 2, true, false}};
    ops[6] = (tp_diff_op){
        .shape = TP_DIFF_SHAPE_SPRITE_RECORD,
        .atlas_id = id_last(0x11U),
        .spr_before_present = false,
        .spr_before_index = -1,
        .spr_after_present = true,
        .spr_after_index = 3,
        .spr_after = {.src_key = (char *)"hero.png",
                      .origin_x = 0.25F,
                      .origin_y = 0.75F,
                      .slice9_lrtb = {1U, 2U, 3U, 4U},
                      .rename = NULL,
                      .ov_shape = 0,
                      .ov_allow_rotate = 0,
                      .ov_max_vertices = 6,
                      .ov_margin = 3,
                      .ov_extrude = 5}};
    ops[6].spr_after.source_ref = id_last(0x21U);
    ops[7] = (tp_diff_op){.shape = TP_DIFF_SHAPE_FRAMES_LIST,
                          .atlas_id = id_last(0x11U),
                          .anim_id = id_last(0x31U),
                          .frames_before_count = 0,
                          .frames_after = after_frames,
                          .frames_after_count = 2};
    ops[8] = (tp_diff_op){.shape = TP_DIFF_SHAPE_FRAME_MOVE,
                          .atlas_id = id_last(0x11U),
                          .anim_id = id_last(0x31U),
                          .from_index = 0,
                          .to_index = 1};
    ops[9] = (tp_diff_op){.shape = TP_DIFF_SHAPE_ANIM_NAME,
                          .atlas_id = id_last(0x11U),
                          .anim_id = id_last(0x31U),
                          .name_before = (char *)"walk",
                          .name_after = (char *)"run"};
    return (tp_diff_record){.ops = ops, .op_count = 10};
}

static tp_diff_record build_collections_record(tp_diff_op ops[5]) {
    static tp_project_atlas atlas;
    static tp_project_source source;
    static tp_project_anim animation;
    static tp_project_frame frame;
    static tp_project_target target;
    memset(ops, 0, 5U * sizeof *ops);
    atlas = (tp_project_atlas){.id = {{0U}},
                               .name = (char *)"added",
                               .max_size = 2048,
                               .padding = 2,
                               .alpha_threshold = 1,
                               .max_vertices = 8,
                               .shape = 2,
                               .allow_transform = true,
                               .power_of_two = true,
                               .pixels_per_unit = 1.0F};
    atlas.id = id_last(0x12U);
    source = (tp_project_source){id_last(0x21U), TP_SOURCE_KIND_FILE,
                                 (char *)"relative/file.png"};
    animation = (tp_project_anim){.id = {{0U}},
                                   .name = (char *)"idle",
                                   .fps = 30.0F};
    animation.id = id_last(0x32U);
    frame = (tp_project_frame){NULL, id_last(0x22U), (char *)"frame.png"};
    target = (tp_project_target){id_last(0x42U), (char *)"defold",
                                 (char *)"out/c", false};
    static const tp_diff_coll collections[] = {
        TP_DIFF_COLL_ATLAS, TP_DIFF_COLL_SOURCE, TP_DIFF_COLL_ANIM,
        TP_DIFF_COLL_FRAME, TP_DIFF_COLL_TARGET};
    void *const elements[] = {&atlas, &source, &animation, &frame, &target};
    static const int positions[] = {1, 2, 1, 0, 1};
    for (int i = 0; i < 5; i++) {
        ops[i] = (tp_diff_op){.shape = TP_DIFF_SHAPE_COLL,
                              .atlas_id = id_last(
                                  collections[i] == TP_DIFF_COLL_ATLAS
                                      ? 0x12U
                                      : 0x11U),
                              .anim_id = collections[i] == TP_DIFF_COLL_FRAME
                                             ? id_last(0x31U)
                                             : tp_id128_nil(),
                              .coll = collections[i],
                              .position = positions[i],
                              .created = true,
                              .elem = elements[i]};
    }
    return (tp_diff_record){.ops = ops, .op_count = 5};
}

static void assert_fixture_is_valid(const uint8_t *bytes, size_t len) {
    uint32_t op_count = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_validate(bytes, len, &op_count, &error),
        error.msg);
    TEST_ASSERT_EQUAL_UINT32(10U, op_count);
}

void test_encoder_matches_fixed_forward_and_reverse_bytes(void) {
    size_t forward_len = 0U, reverse_len = 0U;
    uint8_t *forward = read_hex(g_forward_path, &forward_len);
    uint8_t *reverse = read_hex(g_reverse_path, &reverse_len);
    tp_diff_op ops[10];
    const tp_diff_record record = build_record(ops);
    tp_history_transition_blob blob = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_ERROR;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_encode(&record, false, NULL, SIZE_MAX, &blob,
                                     &outcome, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_size_t(forward_len, blob.len);
    TEST_ASSERT_EQUAL_MEMORY(forward, blob.data, forward_len);
    tp_history_transition_blob_free(&blob);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_encode(&record, true, NULL, SIZE_MAX, &blob,
                                     &outcome, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_EQUAL_size_t(reverse_len, blob.len);
    TEST_ASSERT_EQUAL_MEMORY(reverse, blob.data, reverse_len);
    tp_history_transition_blob_free(&blob);
    free(forward);
    free(reverse);
}

void test_all_collection_variants_match_fixed_bytes_and_apply_both_ways(void) {
    size_t forward_len = 0U, reverse_len = 0U;
    uint8_t *forward = read_hex(g_collections_forward_path, &forward_len);
    uint8_t *reverse = read_hex(g_collections_reverse_path, &reverse_len);
    tp_diff_op ops[5];
    const tp_diff_record record = build_collections_record(ops);
    tp_history_transition_blob blob = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_ERROR;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_encode(&record, false, NULL, SIZE_MAX, &blob,
                                     &outcome, &error), error.msg);
    TEST_ASSERT_EQUAL_size_t(forward_len, blob.len);
    TEST_ASSERT_EQUAL_MEMORY(forward, blob.data, forward_len);
    tp_history_transition_blob_free(&blob);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_encode(&record, true, NULL, SIZE_MAX, &blob,
                                     &outcome, &error), error.msg);
    TEST_ASSERT_EQUAL_size_t(reverse_len, blob.len);
    TEST_ASSERT_EQUAL_MEMORY(reverse, blob.data, reverse_len);
    tp_history_transition_blob_free(&blob);

    tp_project *project = build_before_project();
    char *before = tp_test_serialize_project(project);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_apply(project, forward, forward_len, NULL,
                                    &error), error.msg);
    TEST_ASSERT_EQUAL_INT(2, project->atlas_count);
    TEST_ASSERT_EQUAL_INT(3, project->atlases[0].source_count);
    TEST_ASSERT_EQUAL_INT(2, project->atlases[0].animation_count);
    TEST_ASSERT_EQUAL_INT(1, project->atlases[0].animations[0].frame_count);
    TEST_ASSERT_EQUAL_INT(2, project->atlases[0].target_count);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_apply(project, reverse, reverse_len, NULL,
                                    &error), error.msg);
    char *restored = tp_test_serialize_project(project);
    TEST_ASSERT_EQUAL_STRING(before, restored);
    free(before);
    free(restored);
    tp_project_destroy(project);
    free(forward);
    free(reverse);
}

void test_fixed_bytes_decode_apply_and_reverse_byte_exactly(void) {
    size_t forward_len = 0U, reverse_len = 0U;
    uint8_t *forward = read_hex(g_forward_path, &forward_len);
    uint8_t *reverse = read_hex(g_reverse_path, &reverse_len);
    assert_fixture_is_valid(forward, forward_len);
    assert_fixture_is_valid(reverse, reverse_len);
    tp_project *project = build_before_project();
    char *before = tp_test_serialize_project(project);
    tp_error error = {0};
    uint32_t op_count = 0U;
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_apply(project, forward, forward_len, &op_count,
                                    &error), error.msg);
    TEST_ASSERT_EQUAL_UINT32(10U, op_count);
    const tp_project_atlas *atlas = &project->atlases[0];
    TEST_ASSERT_EQUAL_STRING("new", atlas->name);
    TEST_ASSERT_EQUAL_INT(3, atlas->source_count);
    TEST_ASSERT_TRUE(tp_id128_eq(id_last(0x21U), atlas->sources[2].id));
    TEST_ASSERT_EQUAL_STRING("new/path", atlas->sources[2].path);
    TEST_ASSERT_EQUAL_INT(4, atlas->sprite_count);
    TEST_ASSERT_EQUAL_STRING("hero.png", atlas->sprites[3].src_key);
    TEST_ASSERT_NULL(atlas->sprites[3].rename);
    TEST_ASSERT_EQUAL_STRING("defold", atlas->targets[0].exporter_id);
    TEST_ASSERT_FALSE(atlas->targets[0].enabled);
    TEST_ASSERT_EQUAL_STRING("run", atlas->animations[0].name);
    TEST_ASSERT_EQUAL_INT(2, atlas->animations[0].frame_count);
    TEST_ASSERT_EQUAL_STRING("b.png",
                             atlas->animations[0].frames[0].src_key);
    TEST_ASSERT_EQUAL_STRING("a.png",
                             atlas->animations[0].frames[1].src_key);

    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_apply(project, reverse, reverse_len, &op_count,
                                    &error), error.msg);
    char *restored = tp_test_serialize_project(project);
    TEST_ASSERT_EQUAL_STRING(before, restored);
    free(before);
    free(restored);
    tp_project_destroy(project);
    free(forward);
    free(reverse);
}

static void assert_all_truncations_and_trailing_rejected(const char *path) {
    size_t len = 0U;
    uint8_t *bytes = read_hex(path, &len);
    for (size_t prefix = 0U; prefix < len; prefix++) {
        uint32_t count = 0U;
        tp_error error = {0};
        const tp_status expected = prefix < 8U ? TP_STATUS_OUT_OF_BOUNDS
                                               : TP_STATUS_INVALID_ARGUMENT;
        TEST_ASSERT_EQUAL_INT(
            expected,
            tp_history_transition_validate(bytes, prefix, &count, &error));
    }
    uint8_t *with_tail = (uint8_t *)malloc(len + 1U);
    TEST_ASSERT_NOT_NULL(with_tail);
    memcpy(with_tail, bytes, len);
    with_tail[len] = 0xA5U;
    uint32_t count = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        tp_history_transition_validate(with_tail, len + 1U, &count, &error));
    free(with_tail);
    free(bytes);
}

void test_fixed_bytes_reject_every_truncation_and_trailing_data(void) {
    assert_all_truncations_and_trailing_rejected(g_forward_path);
    assert_all_truncations_and_trailing_rejected(g_reverse_path);
}

void test_required_string_rejects_null_and_empty_wire_sentinels(void) {
    static const uint8_t null_name[] = {
        0x00U, 0x00U, 0x00U, 0x01U, /* version */
        0x00U, 0x00U, 0x00U, 0x01U, /* one op */
        0x02U,                       /* ATLAS_NAME */
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x11U,
        0xffU, 0xffU, 0xffU, 0xffU /* NULL string */};
    uint8_t empty_name[sizeof null_name];
    memcpy(empty_name, null_name, sizeof null_name);
    memset(empty_name + sizeof empty_name - 4U, 0, 4U); /* length zero */
    const struct {
        const uint8_t *bytes;
        size_t len;
    } invalid[] = {{null_name, sizeof null_name},
                   {empty_name, sizeof empty_name}};
    for (size_t i = 0U; i < sizeof invalid / sizeof invalid[0]; i++) {
        uint32_t count = 0U;
        tp_error error = {0};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_INVALID_ARGUMENT,
            tp_history_transition_validate(invalid[i].bytes, invalid[i].len,
                                             &count, &error));
    }
}

static bool contains_bytes(const uint8_t *haystack, size_t haystack_len,
                           const char *needle) {
    const size_t needle_len = strlen(needle);
    if (needle_len > haystack_len) return false;
    for (size_t i = 0U; i <= haystack_len - needle_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0) return true;
    }
    return false;
}

void test_encoder_resolves_source_path_against_path_context(void) {
    tp_diff_op ops[10];
    tp_diff_record record = build_record(ops);
    record.op_count = 1;
    tp_project *context = tp_project_create();
    TEST_ASSERT_NOT_NULL(context);
#ifdef _WIN32
    static const char root[] = "C:/history-contract-root";
#else
    static const char root[] = "/history-contract-root";
#endif
    context->source_base_dir = copy_text(root);
    char expected[TP_IDENTITY_PATH_MAX];
    const int written = snprintf(expected, sizeof expected,
                                 "%s/relative/file.png", root);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof expected);
    tp_history_transition_blob blob = {0};
    tp_history_codec_outcome outcome = TP_HISTORY_CODEC_ERROR;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_history_transition_encode(&record, false, context, SIZE_MAX, &blob,
                                     &outcome, &error), error.msg);
    TEST_ASSERT_EQUAL_INT(TP_HISTORY_CODEC_OK, outcome);
    TEST_ASSERT_TRUE(contains_bytes(blob.data, blob.len, expected));
    tp_history_transition_blob_free(&blob);
    tp_project_destroy(context);
}

int main(int argc, char **argv) {
    if (argc != 5) return 2;
    g_forward_path = argv[1];
    g_reverse_path = argv[2];
    g_collections_forward_path = argv[3];
    g_collections_reverse_path = argv[4];
    UNITY_BEGIN();
    RUN_TEST(test_encoder_matches_fixed_forward_and_reverse_bytes);
    RUN_TEST(test_all_collection_variants_match_fixed_bytes_and_apply_both_ways);
    RUN_TEST(test_fixed_bytes_decode_apply_and_reverse_byte_exactly);
    RUN_TEST(test_fixed_bytes_reject_every_truncation_and_trailing_data);
    RUN_TEST(test_required_string_rejects_null_and_empty_wire_sentinels);
    RUN_TEST(test_encoder_resolves_source_path_against_path_context);
    return UNITY_END();
}
