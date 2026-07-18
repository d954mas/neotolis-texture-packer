#ifndef TP_PACK_CONSTRAINTS_INTERNAL_H
#define TP_PACK_CONSTRAINTS_INTERNAL_H

#include <stdbool.h>

#include "tp_core/tp_pack.h"

typedef struct tp_pack_atlas_constraint_input {
    int max_size;
    int padding;
    int margin;
    int extrude;
    int alpha_threshold;
    int max_vertices;
    int shape;
    float pixels_per_unit;
} tp_pack_atlas_constraint_input;

typedef struct tp_pack_atlas_constraint_facts {
    bool max_size_out_of_range;
    bool padding_negative;
    bool margin_negative;
    bool extrude_negative;
    bool padding_exceeds_max_size;
    bool margin_exceeds_max_size;
    bool extrude_exceeds_max_size;
    bool alpha_threshold_out_of_range;
    bool max_vertices_out_of_range;
    bool shape_out_of_range;
    bool pixels_per_unit_out_of_range;
    bool extrude_requires_rect;
} tp_pack_atlas_constraint_facts;

typedef struct tp_pack_sprite_constraint_input {
    int atlas_max_size;
    int atlas_shape;
    int atlas_extrude;
    bool has_slice9;
    bool has_shape;
    int shape;
    bool has_allow_rotate;
    int allow_rotate;
    bool has_max_vertices;
    int max_vertices;
    bool has_margin;
    int margin;
    bool has_extrude;
    int extrude;
} tp_pack_sprite_constraint_input;

typedef struct tp_pack_sprite_constraint_facts {
    bool shape_not_wire_representable;
    bool allow_rotate_not_wire_representable;
    bool max_vertices_not_wire_representable;
    bool margin_not_wire_representable;
    bool extrude_not_wire_representable;
    bool margin_exceeds_max_size;
    bool extrude_exceeds_max_size;
    bool slice9_shape_conflict;
    bool effective_extrude_requires_rect;
} tp_pack_sprite_constraint_facts;

tp_pack_atlas_constraint_facts tp_pack_atlas_constraint_facts_of(
    const tp_pack_atlas_constraint_input *input);
tp_pack_sprite_constraint_facts tp_pack_sprite_constraint_facts_of(
    const tp_pack_sprite_constraint_input *input);

#endif /* TP_PACK_CONSTRAINTS_INTERNAL_H */
