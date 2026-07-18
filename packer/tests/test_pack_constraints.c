#include <string.h>

#include "tp_pack_constraints_internal.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static tp_pack_atlas_constraint_input valid_atlas(void) {
    tp_pack_atlas_constraint_input input = {
        .max_size = 32,
        .padding = 0,
        .margin = 0,
        .extrude = 0,
        .alpha_threshold = 0,
        .max_vertices = 4,
        .shape = TP_PACK_SHAPE_MIN,
        .pixels_per_unit = 1.0F,
    };
    return input;
}

void test_atlas_constraint_facts_cover_raw_boundaries(void) {
    tp_pack_atlas_constraint_input input = valid_atlas();
    tp_pack_atlas_constraint_facts facts =
        tp_pack_atlas_constraint_facts_of(&input);
    tp_pack_atlas_constraint_facts none = {0};
    TEST_ASSERT_EQUAL_MEMORY(&none, &facts, sizeof facts);

    input.padding = -1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.padding_negative);
    TEST_ASSERT_FALSE(facts.padding_exceeds_max_size);

    input = valid_atlas();
    input.padding = input.max_size;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_FALSE(facts.padding_exceeds_max_size);
    input.padding++;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.padding_exceeds_max_size);

    input = valid_atlas();
    input.margin = 31;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_FALSE(facts.margin_exceeds_max_size);
    input.margin = 32;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_FALSE(facts.margin_exceeds_max_size);
    input.margin = 33;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.margin_exceeds_max_size);

    input = valid_atlas();
    input.extrude = -1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_negative);
    input.extrude = 33;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_exceeds_max_size);

    input = valid_atlas();
    input.max_size = 0;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.max_size_out_of_range);
    input.max_size = TP_PACK_MAX_PAGE_DIM + 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.max_size_out_of_range);

    input = valid_atlas();
    input.shape = TP_PACK_SHAPE_MAX;
    input.extrude = 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_requires_rect);

    input = valid_atlas();
    input.alpha_threshold = -1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.alpha_threshold_out_of_range);
    input.alpha_threshold = TP_PACK_ALPHA_MAX + 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.alpha_threshold_out_of_range);

    input = valid_atlas();
    input.max_vertices = 0;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.max_vertices_out_of_range);
    input.max_vertices = TP_PACK_MAX_VERTICES + 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.max_vertices_out_of_range);

    input = valid_atlas();
    input.shape = TP_PACK_SHAPE_MIN - 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.shape_out_of_range);
    input.shape = TP_PACK_SHAPE_MAX + 1;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.shape_out_of_range);

    input = valid_atlas();
    input.pixels_per_unit = 0.0F;
    facts = tp_pack_atlas_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.pixels_per_unit_out_of_range);
}

static tp_pack_sprite_constraint_input valid_sprite(void) {
    tp_pack_sprite_constraint_input input = {
        .atlas_max_size = 32,
        .atlas_shape = TP_PACK_SHAPE_MAX,
        .atlas_extrude = 0,
    };
    return input;
}

void test_sprite_constraint_facts_separate_wire_and_effective_domains(void) {
    tp_pack_sprite_constraint_input input = valid_sprite();
    tp_pack_sprite_constraint_facts facts =
        tp_pack_sprite_constraint_facts_of(&input);
    tp_pack_sprite_constraint_facts none = {0};
    TEST_ASSERT_EQUAL_MEMORY(&none, &facts, sizeof facts);

    input.has_margin = true;
    input.margin = 0;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.margin_not_wire_representable);
    input.margin = 32;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_FALSE(facts.margin_not_wire_representable);
    TEST_ASSERT_FALSE(facts.margin_exceeds_max_size);
    input.margin = 33;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.margin_exceeds_max_size);
    input.margin = 256;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.margin_not_wire_representable);

    input = valid_sprite();
    input.has_extrude = true;
    input.extrude = 0;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_not_wire_representable);
    input.extrude = 33;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_exceeds_max_size);
    input.extrude = 256;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.extrude_not_wire_representable);

    input = valid_sprite();
    input.has_shape = true;
    input.shape = TP_PACK_SHAPE_MAX + 1;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.shape_not_wire_representable);
    input = valid_sprite();
    input.has_allow_rotate = true;
    input.allow_rotate = 1;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.allow_rotate_not_wire_representable);
    input = valid_sprite();
    input.has_max_vertices = true;
    input.max_vertices = TP_PACK_MAX_VERTICES + 1;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.max_vertices_not_wire_representable);

    input = valid_sprite();
    input.has_shape = true;
    input.shape = TP_PACK_SHAPE_MAX;
    input.has_slice9 = true;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.slice9_shape_conflict);

    input = valid_sprite();
    input.has_extrude = true;
    input.extrude = 1;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_TRUE(facts.effective_extrude_requires_rect);
    input.has_slice9 = true;
    facts = tp_pack_sprite_constraint_facts_of(&input);
    TEST_ASSERT_FALSE(facts.effective_extrude_requires_rect);
}

void test_sprite_wire_predicates_cover_exact_storage_edges(void) {
    TEST_ASSERT_FALSE(tp_pack_sprite_shape_wire_representable(-1));
    TEST_ASSERT_TRUE(tp_pack_sprite_shape_wire_representable(
        TP_PACK_SHAPE_MIN));
    TEST_ASSERT_TRUE(tp_pack_sprite_shape_wire_representable(
        TP_PACK_SHAPE_MAX));
    TEST_ASSERT_FALSE(tp_pack_sprite_shape_wire_representable(
        TP_PACK_SHAPE_MAX + 1));

    TEST_ASSERT_FALSE(tp_pack_sprite_rotate_wire_representable(-1));
    TEST_ASSERT_TRUE(tp_pack_sprite_rotate_wire_representable(0));
    TEST_ASSERT_FALSE(tp_pack_sprite_rotate_wire_representable(1));

    TEST_ASSERT_FALSE(tp_pack_sprite_max_vertices_wire_representable(0));
    TEST_ASSERT_TRUE(tp_pack_sprite_max_vertices_wire_representable(1));
    TEST_ASSERT_TRUE(tp_pack_sprite_max_vertices_wire_representable(
        TP_PACK_MAX_VERTICES));
    TEST_ASSERT_FALSE(tp_pack_sprite_max_vertices_wire_representable(
        TP_PACK_MAX_VERTICES + 1));

    TEST_ASSERT_FALSE(tp_pack_sprite_spacing_wire_representable(0));
    TEST_ASSERT_TRUE(tp_pack_sprite_spacing_wire_representable(1));
    TEST_ASSERT_TRUE(tp_pack_sprite_spacing_wire_representable(UINT8_MAX));
    TEST_ASSERT_FALSE(tp_pack_sprite_spacing_wire_representable(
        UINT8_MAX + 1));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_atlas_constraint_facts_cover_raw_boundaries);
    RUN_TEST(test_sprite_constraint_facts_separate_wire_and_effective_domains);
    RUN_TEST(test_sprite_wire_predicates_cover_exact_storage_edges);
    return UNITY_END();
}
