#include "tp_pack_constraints_internal.h"

tp_pack_atlas_constraint_facts tp_pack_atlas_constraint_facts_of(
    const tp_pack_atlas_constraint_input *input) {
    tp_pack_atlas_constraint_facts facts = {0};
    if (!input) {
        return facts;
    }
    facts.max_size_out_of_range =
        !tp_pack_max_size_valid(input->max_size);
    facts.padding_negative = input->padding < 0;
    facts.margin_negative = input->margin < 0;
    facts.extrude_negative = input->extrude < 0;
    facts.padding_exceeds_max_size = input->padding > input->max_size;
    facts.margin_exceeds_max_size = input->margin > input->max_size;
    facts.extrude_exceeds_max_size = input->extrude > input->max_size;
    facts.alpha_threshold_out_of_range =
        !tp_pack_alpha_threshold_valid(input->alpha_threshold);
    facts.max_vertices_out_of_range =
        !tp_pack_max_vertices_valid(input->max_vertices);
    facts.shape_out_of_range = !tp_pack_shape_valid(input->shape);
    facts.pixels_per_unit_out_of_range =
        !tp_pack_pixels_per_unit_valid(input->pixels_per_unit);
    facts.extrude_requires_rect =
        input->extrude > 0 && tp_pack_shape_valid(input->shape) &&
        input->shape != TP_PACK_SHAPE_MIN;
    return facts;
}

tp_pack_sprite_constraint_facts tp_pack_sprite_constraint_facts_of(
    const tp_pack_sprite_constraint_input *input) {
    tp_pack_sprite_constraint_facts facts = {0};
    if (!input) {
        return facts;
    }
    facts.shape_not_wire_representable =
        input->has_shape && !tp_pack_shape_valid(input->shape);
    facts.allow_rotate_not_wire_representable =
        input->has_allow_rotate && input->allow_rotate != 0;
    facts.max_vertices_not_wire_representable =
        input->has_max_vertices &&
        !tp_pack_max_vertices_valid(input->max_vertices);
    facts.margin_not_wire_representable =
        input->has_margin &&
        (input->margin < 1 || input->margin > UINT8_MAX);
    facts.extrude_not_wire_representable =
        input->has_extrude &&
        (input->extrude < 1 || input->extrude > UINT8_MAX);
    facts.margin_exceeds_max_size =
        input->has_margin && input->margin > input->atlas_max_size;
    facts.extrude_exceeds_max_size =
        input->has_extrude && input->extrude > input->atlas_max_size;
    facts.slice9_shape_conflict =
        input->has_slice9 && input->has_shape &&
        tp_pack_shape_valid(input->shape) &&
        input->shape != TP_PACK_SHAPE_MIN;

    int effective_shape = input->atlas_shape;
    if (input->has_slice9) {
        effective_shape = TP_PACK_SHAPE_MIN;
    } else if (input->has_shape && tp_pack_shape_valid(input->shape)) {
        effective_shape = input->shape;
    }
    const int effective_extrude =
        input->has_extrude ? input->extrude : input->atlas_extrude;
    facts.effective_extrude_requires_rect =
        effective_extrude > 0 && tp_pack_shape_valid(effective_shape) &&
        effective_shape != TP_PACK_SHAPE_MIN;
    return facts;
}
