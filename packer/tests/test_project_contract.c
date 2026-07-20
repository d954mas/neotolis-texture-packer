/* Independent canonical-v5 contract oracle.
 *
 * The writer is checked against checked-in bytes built without invoking the
 * loader.  The loader is checked against field assertions without re-saving.
 * Keeping the two directions independent prevents a matching parser/writer
 * drift from looking like a successful round trip. */

#define _CRT_SECURE_NO_WARNINGS

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "unity.h"

static const char *g_fixture_path;

void setUp(void) {}
void tearDown(void) {}

static char *copy_text(const char *text) {
    const size_t size = strlen(text) + 1U;
    char *copy = (char *)malloc(size);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, size);
    return copy;
}

static tp_id128 fixed_id(unsigned char last_byte) {
    tp_id128 id = {{0U}};
    id.bytes[15] = last_byte;
    return id;
}

static void assert_id(unsigned char last_byte, tp_id128 actual) {
    TEST_ASSERT_TRUE(tp_id128_eq(fixed_id(last_byte), actual));
}

static char *read_fixture(size_t *out_len) {
    FILE *file = fopen(g_fixture_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    const long length = ftell(file);
    TEST_ASSERT_TRUE(length >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *bytes = (char *)malloc((size_t)length + 1U);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_size_t((size_t)length,
                             fread(bytes, 1U, (size_t)length, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    bytes[length] = '\0';
    *out_len = (size_t)length;
    return bytes;
}

static tp_project *build_expected_project(void) {
    tp_project *project = tp_project_create();
    TEST_ASSERT_NOT_NULL(project);
    tp_project_atlas *hero = tp_project_get_atlas(project, 0);
    TEST_ASSERT_NOT_NULL(hero);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_set_atlas_name(hero, "Герой α"));
    hero->id = fixed_id(0x11U);
    hero->max_size = 1024;
    hero->padding = 4;
    hero->margin = 3;
    hero->extrude = 2;
    hero->alpha_threshold = 123;
    hero->max_vertices = 12;
    hero->shape = 1;
    hero->allow_transform = false;
    hero->power_of_two = false;
    hero->pixels_per_unit = 2.5F;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_atlas_add_source_kind(
                          hero, "арт/герой", TP_SOURCE_KIND_FOLDER));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_atlas_add_source_kind(
                          hero, "art/ui icon.png", TP_SOURCE_KIND_FILE));
    hero->sources[0].id = fixed_id(0x21U);
    hero->sources[1].id = fixed_id(0x22U);

    tp_project_sprite *sprite = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_atlas_add_sprite_by_source_key(
                          hero, hero->sources[0].id,
                          "ходьба/кадр_01.png", &sprite));
    TEST_ASSERT_NOT_NULL(sprite);
    sprite->origin_x = 0.25F;
    sprite->origin_y = 0.75F;
    sprite->slice9_lrtb[0] = 4U;
    sprite->slice9_lrtb[1] = 5U;
    sprite->slice9_lrtb[2] = 6U;
    sprite->slice9_lrtb[3] = 7U;
    sprite->rename = copy_text("Игрок\n№1");
    sprite->ov_shape = 0;
    sprite->ov_allow_rotate = 0;
    sprite->ov_max_vertices = 6;
    sprite->ov_margin = 3;
    sprite->ov_extrude = 5;

    tp_project_anim *animation = NULL;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_animation(
                                             hero, "бег", &animation));
    TEST_ASSERT_NOT_NULL(animation);
    animation->id = fixed_id(0x31U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_anim_add_frame(
                          animation, hero->sources[0].id,
                          "ходьба/кадр_01.png"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_anim_add_frame(
                          animation, hero->sources[1].id, "ui/button.png"));
    animation->fps = 12.5F;
    animation->playback = 2;
    animation->flip_h = true;

    tp_project_target *json_target = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_atlas_add_target(
                          hero, "json-neotolis", "out/герой.json",
                          &json_target));
    json_target->id = fixed_id(0x41U);
    tp_project_target *defold_target = NULL;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, tp_project_atlas_add_target(
                          hero, "defold", "out/hero.atlas", &defold_target));
    defold_target->id = fixed_id(0x42U);
    defold_target->enabled = false;

    int ui_index = -1;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_add_atlas(project, "UI ✨", &ui_index));
    tp_project_atlas *ui = tp_project_get_atlas(project, ui_index);
    TEST_ASSERT_NOT_NULL(ui);
    ui->id = fixed_id(0x12U);
    return project;
}

void test_writer_matches_checked_in_canonical_bytes(void) {
    tp_project *project = build_expected_project();
    char *actual = NULL;
    size_t actual_len = 0U;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_save_buffer(project, &actual, &actual_len, &error),
        error.msg);
    size_t expected_len = 0U;
    char *expected = read_fixture(&expected_len);
    TEST_ASSERT_EQUAL_size_t(expected_len, actual_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, expected_len);
    free(expected);
    free(actual);
    tp_project_destroy(project);
}

void test_loader_maps_checked_in_bytes_to_expected_model(void) {
    size_t fixture_len = 0U;
    char *fixture = read_fixture(&fixture_len);
    tp_project *project = NULL;
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_project_load_buffer(fixture, fixture_len, &project, &error),
        error.msg);
    free(fixture);
    TEST_ASSERT_NOT_NULL(project);
    TEST_ASSERT_EQUAL_INT(2, project->atlas_count);

    const tp_project_atlas *hero = &project->atlases[0];
    assert_id(0x11U, hero->id);
    TEST_ASSERT_EQUAL_STRING("Герой α", hero->name);
    TEST_ASSERT_EQUAL_INT(1024, hero->max_size);
    TEST_ASSERT_EQUAL_INT(4, hero->padding);
    TEST_ASSERT_EQUAL_INT(3, hero->margin);
    TEST_ASSERT_EQUAL_INT(2, hero->extrude);
    TEST_ASSERT_EQUAL_INT(123, hero->alpha_threshold);
    TEST_ASSERT_EQUAL_INT(12, hero->max_vertices);
    TEST_ASSERT_EQUAL_INT(1, hero->shape);
    TEST_ASSERT_FALSE(hero->allow_transform);
    TEST_ASSERT_FALSE(hero->power_of_two);
    TEST_ASSERT_TRUE(fabsf(hero->pixels_per_unit - 2.5F) < 0.0001F);

    TEST_ASSERT_EQUAL_INT(2, hero->source_count);
    assert_id(0x21U, hero->sources[0].id);
    TEST_ASSERT_EQUAL_INT(TP_SOURCE_KIND_FOLDER, hero->sources[0].kind);
    TEST_ASSERT_EQUAL_STRING("арт/герой", hero->sources[0].path);
    assert_id(0x22U, hero->sources[1].id);
    TEST_ASSERT_EQUAL_INT(TP_SOURCE_KIND_FILE, hero->sources[1].kind);
    TEST_ASSERT_EQUAL_STRING("art/ui icon.png", hero->sources[1].path);

    TEST_ASSERT_EQUAL_INT(1, hero->sprite_count);
    const tp_project_sprite *sprite = &hero->sprites[0];
    TEST_ASSERT_TRUE(tp_id128_eq(hero->sources[0].id, sprite->source_ref));
    TEST_ASSERT_EQUAL_STRING("ходьба/кадр_01.png", sprite->src_key);
    TEST_ASSERT_TRUE(fabsf(sprite->origin_x - 0.25F) < 0.0001F);
    TEST_ASSERT_TRUE(fabsf(sprite->origin_y - 0.75F) < 0.0001F);
    TEST_ASSERT_EQUAL_UINT16(4U, sprite->slice9_lrtb[0]);
    TEST_ASSERT_EQUAL_UINT16(5U, sprite->slice9_lrtb[1]);
    TEST_ASSERT_EQUAL_UINT16(6U, sprite->slice9_lrtb[2]);
    TEST_ASSERT_EQUAL_UINT16(7U, sprite->slice9_lrtb[3]);
    TEST_ASSERT_EQUAL_STRING("Игрок\n№1", sprite->rename);
    TEST_ASSERT_EQUAL_INT(0, sprite->ov_shape);
    TEST_ASSERT_EQUAL_INT(0, sprite->ov_allow_rotate);
    TEST_ASSERT_EQUAL_INT(6, sprite->ov_max_vertices);
    TEST_ASSERT_EQUAL_INT(3, sprite->ov_margin);
    TEST_ASSERT_EQUAL_INT(5, sprite->ov_extrude);

    TEST_ASSERT_EQUAL_INT(1, hero->animation_count);
    const tp_project_anim *animation = &hero->animations[0];
    assert_id(0x31U, animation->id);
    TEST_ASSERT_EQUAL_STRING("бег", animation->name);
    TEST_ASSERT_EQUAL_INT(2, animation->frame_count);
    TEST_ASSERT_TRUE(tp_id128_eq(hero->sources[0].id,
                                animation->frames[0].source_ref));
    TEST_ASSERT_EQUAL_STRING("ходьба/кадр_01.png",
                             animation->frames[0].src_key);
    TEST_ASSERT_TRUE(tp_id128_eq(hero->sources[1].id,
                                animation->frames[1].source_ref));
    TEST_ASSERT_EQUAL_STRING("ui/button.png", animation->frames[1].src_key);
    TEST_ASSERT_TRUE(fabsf(animation->fps - 12.5F) < 0.0001F);
    TEST_ASSERT_EQUAL_INT(2, animation->playback);
    TEST_ASSERT_TRUE(animation->flip_h);
    TEST_ASSERT_FALSE(animation->flip_v);

    TEST_ASSERT_EQUAL_INT(2, hero->target_count);
    assert_id(0x41U, hero->targets[0].id);
    TEST_ASSERT_EQUAL_STRING("json-neotolis",
                             hero->targets[0].exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/герой.json", hero->targets[0].out_path);
    TEST_ASSERT_TRUE(hero->targets[0].enabled);
    assert_id(0x42U, hero->targets[1].id);
    TEST_ASSERT_EQUAL_STRING("defold", hero->targets[1].exporter_id);
    TEST_ASSERT_EQUAL_STRING("out/hero.atlas", hero->targets[1].out_path);
    TEST_ASSERT_FALSE(hero->targets[1].enabled);

    const tp_project_atlas *ui = &project->atlases[1];
    tp_pack_settings defaults;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_pack_settings_defaults(&defaults));
    assert_id(0x12U, ui->id);
    TEST_ASSERT_EQUAL_STRING("UI ✨", ui->name);
    TEST_ASSERT_EQUAL_INT(defaults.max_size, ui->max_size);
    TEST_ASSERT_EQUAL_INT(defaults.padding, ui->padding);
    TEST_ASSERT_EQUAL_INT(defaults.margin, ui->margin);
    TEST_ASSERT_EQUAL_INT(defaults.extrude, ui->extrude);
    TEST_ASSERT_EQUAL_INT(defaults.alpha_threshold, ui->alpha_threshold);
    TEST_ASSERT_EQUAL_INT(defaults.max_vertices, ui->max_vertices);
    TEST_ASSERT_EQUAL_INT(defaults.shape, ui->shape);
    TEST_ASSERT_EQUAL_INT(defaults.allow_transform, ui->allow_transform);
    TEST_ASSERT_EQUAL_INT(defaults.power_of_two, ui->power_of_two);
    TEST_ASSERT_TRUE(fabsf(defaults.pixels_per_unit - ui->pixels_per_unit) <
                          0.0001F);
    TEST_ASSERT_EQUAL_INT(0, ui->source_count);
    TEST_ASSERT_EQUAL_INT(0, ui->sprite_count);
    TEST_ASSERT_EQUAL_INT(0, ui->animation_count);
    TEST_ASSERT_EQUAL_INT(0, ui->target_count);
    tp_project_destroy(project);
}

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    g_fixture_path = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_writer_matches_checked_in_canonical_bytes);
    RUN_TEST(test_loader_maps_checked_in_bytes_to_expected_model);
    return UNITY_END();
}
