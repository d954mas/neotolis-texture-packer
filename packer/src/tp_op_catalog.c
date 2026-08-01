/*
 * The append-only operation catalog + closed field vocabulary. Row order
 * matches the enum so index == kind;
 * tp_op_catalog_selfcheck() pins that a reorder can't silently mis-map a kind.
 *
 * Every current ntpacker mutation verb appears in the cli_verb column (a compound
 * verb -- `sprite set`, `new` -- names the verb whose canonical lowering is that
 * single op). Reserved rows
 * (cli_verb == NULL) are spec-listed ops with no current verb. There is
 * deliberately NO raw field-patch escape hatch (§6.2).
 */

#include "tp_op_internal.h"

#include <float.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_pack.h" /* the packing-knob bounds the schema REFERENCES */

/* OP(...) keeps the label columns aligned with the wire columns; a new op cannot
 * be added without giving it a palette label + history template (§6). */
#define OP(KIND, WIRE, CLASS, IDKIND, VERB, LABEL, TEMPLATE) \
    {(KIND), (WIRE), (CLASS), (IDKIND), (VERB), (LABEL), (TEMPLATE)}

static const tp_op_info k_ops[TP_OP_KIND_COUNT] = {
    OP(TP_OP_INVALID, "", TP_OP_CLASS_SET, TP_ID_KIND_INVALID, NULL, "", ""),

    OP(TP_OP_ATLAS_CREATE, "atlas.create", TP_OP_CLASS_CREATE, TP_ID_KIND_ATLAS, "atlas add",
       "Create atlas", "Create atlas {name}"),
    OP(TP_OP_ATLAS_REMOVE, "atlas.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ATLAS, "atlas remove",
       "Remove atlas", "Remove atlas"),
    OP(TP_OP_ATLAS_RENAME, "atlas.rename", TP_OP_CLASS_SET, TP_ID_KIND_ATLAS, "atlas rename",
       "Rename atlas", "Rename atlas to {name}"),
    OP(TP_OP_ATLAS_SETTINGS_SET, "atlas.settings.set", TP_OP_CLASS_SET, TP_ID_KIND_ATLAS, "set",
       "Set atlas setting", "Set {field} {value}"),

    OP(TP_OP_SOURCE_ADD, "source.add", TP_OP_CLASS_CREATE, TP_ID_KIND_SOURCE, "add",
       "Add source", "Add source {key}"),
    OP(TP_OP_SOURCE_REMOVE, "source.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_SOURCE, "remove",
       "Remove source", "Remove source"),
    OP(TP_OP_SOURCE_REPLACE, "source.replace", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, NULL,
       "Relink source", "Relink source to {key}"),

    OP(TP_OP_SPRITE_OVERRIDE_SET, "sprite.override.set", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite set",
       "Set sprite override", "Set {field} {value}"),
    OP(TP_OP_SPRITE_OVERRIDE_CLEAR, "sprite.override.clear", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite unset",
       "Clear sprite override", "Clear {fields}"),
    OP(TP_OP_SPRITE_NAME_SET, "sprite.name.set", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite set",
       "Rename sprite", "Rename sprite to {name}"),

    OP(TP_OP_ANIMATION_CREATE, "animation.create", TP_OP_CLASS_CREATE, TP_ID_KIND_ANIM, "anim create",
       "Create animation", "Create animation {name}"),
    OP(TP_OP_ANIMATION_REMOVE, "animation.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ANIM, "anim remove",
       "Remove animation", "Remove animation"),
    OP(TP_OP_ANIMATION_SETTINGS_SET, "animation.settings.set", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, "anim set",
       "Set animation setting", "Set {field} {value}"),
    OP(TP_OP_ANIMATION_FRAMES_SET, "animation.frames.set", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, NULL,
       "Set animation frames", "Set animation frames"),
    OP(TP_OP_ANIMATION_FRAME_ADD, "animation.frame.add", TP_OP_CLASS_CREATE, TP_ID_KIND_ANIM, "anim add-frame",
       "Add frame", "Add frame at {index}"),
    OP(TP_OP_ANIMATION_FRAME_REMOVE, "animation.frame.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ANIM,
       "anim remove-frame", "Remove frame", "Remove frame {index}"),
    OP(TP_OP_ANIMATION_FRAME_MOVE, "animation.frame.move", TP_OP_CLASS_MOVE, TP_ID_KIND_ANIM, "anim move-frame",
       "Move frame", "Move frame {from_index} to {to_index}"),

    OP(TP_OP_TARGET_CREATE, "target.create", TP_OP_CLASS_CREATE, TP_ID_KIND_TARGET, "target add",
       "Create export target", "Create export target {exporter_id}"),
    OP(TP_OP_TARGET_REMOVE, "target.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_TARGET, "target remove",
       "Remove export target", "Remove export target"),
    OP(TP_OP_TARGET_SET, "target.set", TP_OP_CLASS_SET, TP_ID_KIND_TARGET, "target set",
       "Set export target", "Set {field} {value}"),

    /* Appended (APPEND-ONLY catalog) -- animation rename is a first-class undoable+journaled op. */
    OP(TP_OP_ANIMATION_RENAME, "animation.rename", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, "anim rename",
       "Rename animation", "Rename animation to {name}"),
};

#undef OP

/* ---- the field registry (spec §6) --------------------------------------- *
 * Row order per family is the JSON-lowering order; for sprite it is also the
 * clear-token emission order. Grouped bits occupy CONSECUTIVE rows -- the
 * arity and clear walkers rely on that. Each row also carries its SCHEMA
 * columns (label + range/enum domain), which is what makes the registry
 * palette-ready instead of codec-only.
 *
 * ---- closed value sets ("range/enum") ------------------------------------ *
 * The wire keeps carrying the integer; the token is the stable machine spelling.
 * The value set is derived from the SAME named constants the validate families
 * check against, so an added shape/playback id is one row here, not a client
 * switch arm. */
static const tp_field_enum_value k_shape_values[] = {
    {TP_PACK_SHAPE_RECT, "rect", "Rectangle"},
    {TP_PACK_SHAPE_CONVEX_HULL, "convex_hull", "Convex hull"},
    {TP_PACK_SHAPE_CONCAVE_CONTOUR, "concave_contour", "Concave contour"},
};

/* The engine has no force-rotate, so the only representable override is "off". */
static const tp_field_enum_value k_allow_rotate_values[] = {
    {TP_PACK_OV_ALLOW_ROTATE_OFF, "no_rotate", "Never rotate"},
};

static const tp_field_enum_value k_playback_values[] = {
    {TP_PROJECT_ANIM_PLAYBACK_ONCE_FORWARD, "once_forward", "Once forward"},
    {TP_PROJECT_ANIM_PLAYBACK_LOOP_FORWARD, "loop_forward", "Loop forward"},
    {TP_PROJECT_ANIM_PLAYBACK_ONCE_BACKWARD, "once_backward", "Once backward"},
    {TP_PROJECT_ANIM_PLAYBACK_LOOP_BACKWARD, "loop_backward", "Loop backward"},
    {TP_PROJECT_ANIM_PLAYBACK_ONCE_PINGPONG, "once_pingpong", "Once ping-pong"},
    {TP_PROJECT_ANIM_PLAYBACK_LOOP_PINGPONG, "loop_pingpong", "Loop ping-pong"},
    {TP_PROJECT_ANIM_PLAYBACK_NONE, "none", "None"},
};

#define OP_OFF(TYPE, MEMBER) (uint16_t)offsetof(TYPE, MEMBER)
/* The wire key IS the payload member's spelling for every row below, so
 * stringizing MEMBER pins key<->member: they cannot drift apart. The variadic
 * tail carries the row's SCHEMA designators (label/domain/range/enum), so a new
 * schema column is one designator per row, never a positional reshuffle. */
#define ROW(OPT, RECT, BIT, TYPE, MEMBER, TOKEN, GROUP, RESET, ...)          \
    {.bit = (BIT), .key = #MEMBER, .type = (TYPE),                           \
     .op_off = OP_OFF(OPT, MEMBER), .rec_off = OP_OFF(RECT, MEMBER),         \
     .clear_token = (TOKEN), .group = (GROUP), .reset = (RESET), __VA_ARGS__}
#define ATLAS_ROW(BIT, TYPE, MEMBER, ...)                                    \
    ROW(tp_op_atlas_settings, tp_project_atlas, BIT, TYPE, MEMBER, NULL,     \
        NULL, 0.0, __VA_ARGS__)
#define ANIM_ROW(BIT, TYPE, MEMBER, ...)                                     \
    ROW(tp_op_anim_settings, tp_project_anim, BIT, TYPE, MEMBER, NULL, NULL, \
        0.0, __VA_ARGS__)
#define TARGET_ROW(BIT, TYPE, MEMBER, ...)                                   \
    ROW(tp_op_target_set, tp_project_target, BIT, TYPE, MEMBER, NULL, NULL,  \
        0.0, __VA_ARGS__)
/* Every override row shares one unset semantics: TP_PROJECT_OV_INHERIT is both
 * the sentinel a client may send and the value a clear writes back. */
#define SPRITE_OV_ROW(BIT, MEMBER, TOKEN, ...)                               \
    ROW(tp_op_sprite_set, tp_project_sprite, BIT, TP_FIELD_INT_I16, MEMBER,  \
        TOKEN, NULL, TP_PROJECT_OV_INHERIT, .has_unset = true, __VA_ARGS__)
#define SPRITE_ORIGIN_ROW(MEMBER, TOKEN, ...)                                     \
    ROW(tp_op_sprite_set, tp_project_sprite, TP_SPF_ORIGIN, TP_FIELD_FLOAT,       \
        MEMBER, TOKEN, "origin_x and origin_y", (double)TP_PROJECT_ORIGIN_DEFAULT,\
        RANGE(-FLT_MAX, FLT_MAX), __VA_ARGS__)
/* slice9 is the one row family whose op and record spellings differ. */
#define SPRITE_SLICE9_ROW(SLOT, SIDE, TOKEN, ...)                                 \
    {.bit = TP_SPF_SLICE9, .key = "slice9_" #SIDE, .type = TP_FIELD_INT_U16,      \
     .op_off = OP_OFF(tp_op_sprite_set, slice9[SLOT]),                            \
     .rec_off = OP_OFF(tp_project_sprite, slice9_lrtb[SLOT]),                     \
     .clear_token = (TOKEN), .group = "slice9_l/r/t/b", .reset = 0.0,             \
     RANGE(TP_PROJECT_SLICE9_MIN, TP_PROJECT_SLICE9_MAX), __VA_ARGS__}

/* Schema shorthands. A RANGE's endpoints are always the named constant the
 * validate family uses -- never a re-typed number. */
#define RANGE(MIN, MAX)                                       \
    .domain = TP_FIELD_DOMAIN_RANGE, .range_min = (double)(MIN), \
    .range_max = (double)(MAX)
#define ENUM_OF(TABLE)                          \
    .domain = TP_FIELD_DOMAIN_ENUM, .values = (TABLE), \
    .value_count = (uint8_t)(sizeof(TABLE) / sizeof((TABLE)[0]))
/* A positive finite float (tp_pack_pixels_per_unit_valid / tp_project_anim_fps_valid). */
#define POSITIVE_FINITE \
    RANGE(0.0, FLT_MAX), .min_exclusive = true

static const tp_field_row k_atlas_field_rows[] = {
    ATLAS_ROW(TP_AF_MAX_SIZE, TP_FIELD_INT, max_size, .label = "Max page size",
              RANGE(TP_PACK_MIN_PAGE_DIM, TP_PACK_MAX_PAGE_DIM)),
    ATLAS_ROW(TP_AF_PADDING, TP_FIELD_INT, padding, .label = "Padding",
              RANGE(TP_PACK_ATLAS_SPACING_MIN, TP_PACK_MAX_PAGE_DIM),
              .cap_key = "max_size"),
    ATLAS_ROW(TP_AF_MARGIN, TP_FIELD_INT, margin, .label = "Margin",
              RANGE(TP_PACK_ATLAS_SPACING_MIN, TP_PACK_MAX_PAGE_DIM),
              .cap_key = "max_size"),
    ATLAS_ROW(TP_AF_EXTRUDE, TP_FIELD_INT, extrude, .label = "Extrude",
              RANGE(TP_PACK_ATLAS_SPACING_MIN, TP_PACK_MAX_PAGE_DIM),
              .cap_key = "max_size"),
    ATLAS_ROW(TP_AF_ALPHA_THRESHOLD, TP_FIELD_INT, alpha_threshold,
              .label = "Alpha threshold",
              RANGE(TP_PACK_ALPHA_MIN, TP_PACK_ALPHA_MAX)),
    ATLAS_ROW(TP_AF_MAX_VERTICES, TP_FIELD_INT, max_vertices,
              .label = "Max vertices",
              RANGE(TP_PROJECT_MIN_VERTICES, TP_PROJECT_MAX_VERTICES)),
    ATLAS_ROW(TP_AF_SHAPE, TP_FIELD_INT, shape, .label = "Shape",
              ENUM_OF(k_shape_values)),
    ATLAS_ROW(TP_AF_ALLOW_TRANSFORM, TP_FIELD_BOOL, allow_transform,
              .label = "Allow transform"),
    ATLAS_ROW(TP_AF_POWER_OF_TWO, TP_FIELD_BOOL, power_of_two,
              .label = "Power of two"),
    ATLAS_ROW(TP_AF_PIXELS_PER_UNIT, TP_FIELD_FLOAT, pixels_per_unit,
              .label = "Pixels per unit", POSITIVE_FINITE),
};

/* The CLI sets origin/slice9 as a unit, but the canonical encoder emits the
 * underlying scalar keys -- so each group bit spends several rows. */
static const tp_field_row k_sprite_field_rows[] = {
    SPRITE_ORIGIN_ROW(origin_x, "origin", .label = "Origin X"),
    SPRITE_ORIGIN_ROW(origin_y, NULL, .label = "Origin Y"),
    SPRITE_SLICE9_ROW(0, l, "slice9", .label = "Slice 9 left"),
    SPRITE_SLICE9_ROW(1, r, NULL, .label = "Slice 9 right"),
    SPRITE_SLICE9_ROW(2, t, NULL, .label = "Slice 9 top"),
    SPRITE_SLICE9_ROW(3, b, NULL, .label = "Slice 9 bottom"),
    SPRITE_OV_ROW(TP_SPF_SHAPE, ov_shape, "shape", .label = "Shape",
                  ENUM_OF(k_shape_values)),
    SPRITE_OV_ROW(TP_SPF_ALLOW_ROTATE, ov_allow_rotate, "allow_rotate",
                  .label = "Allow rotate", ENUM_OF(k_allow_rotate_values)),
    SPRITE_OV_ROW(TP_SPF_MAX_VERTICES, ov_max_vertices, "max_vertices",
                  .label = "Max vertices",
                  RANGE(TP_PROJECT_MIN_VERTICES, TP_PROJECT_MAX_VERTICES)),
    SPRITE_OV_ROW(TP_SPF_MARGIN, ov_margin, "margin", .label = "Margin",
                  RANGE(TP_PACK_SPRITE_SPACING_MIN, TP_PACK_SPRITE_SPACING_MAX),
                  .cap_key = "max_size"),
    SPRITE_OV_ROW(TP_SPF_EXTRUDE, ov_extrude, "extrude", .label = "Extrude",
                  RANGE(TP_PACK_SPRITE_SPACING_MIN, TP_PACK_SPRITE_SPACING_MAX),
                  .cap_key = "max_size"),
};

static const tp_field_row k_anim_field_rows[] = {
    ANIM_ROW(TP_ANF_FPS, TP_FIELD_FLOAT, fps, .label = "FPS", POSITIVE_FINITE),
    ANIM_ROW(TP_ANF_PLAYBACK, TP_FIELD_INT, playback, .label = "Playback",
             ENUM_OF(k_playback_values)),
    ANIM_ROW(TP_ANF_FLIP_H, TP_FIELD_BOOL, flip_h, .label = "Flip horizontally"),
    ANIM_ROW(TP_ANF_FLIP_V, TP_FIELD_BOOL, flip_v, .label = "Flip vertically"),
};

static const tp_field_row k_target_field_rows[] = {
    TARGET_ROW(TP_TF_EXPORTER, TP_FIELD_STR, exporter_id, .label = "Exporter",
               .domain = TP_FIELD_DOMAIN_EXPORTER_ID, .nonempty = true),
    TARGET_ROW(TP_TF_OUT_PATH, TP_FIELD_STR, out_path, .label = "Output path",
               .nonempty = true),
    TARGET_ROW(TP_TF_ENABLED, TP_FIELD_BOOL, enabled, .label = "Enabled"),
};

#undef POSITIVE_FINITE
#undef ENUM_OF
#undef RANGE
#undef SPRITE_SLICE9_ROW
#undef SPRITE_ORIGIN_ROW
#undef SPRITE_OV_ROW
#undef TARGET_ROW
#undef ANIM_ROW
#undef ATLAS_ROW
#undef ROW
#undef OP_OFF

#define FAMILY_ROWS(ARR)                            \
    do {                                            \
        rows = (ARR);                               \
        n = sizeof(ARR) / sizeof((ARR)[0]);         \
    } while (0)

const tp_field_row *tp_op_field_rows(tp_field_family family, size_t *count) {
    const tp_field_row *rows = NULL;
    size_t n = 0U;
    switch (family) {
        case TP_FIELD_FAMILY_ATLAS: FAMILY_ROWS(k_atlas_field_rows); break;
        case TP_FIELD_FAMILY_SPRITE: FAMILY_ROWS(k_sprite_field_rows); break;
        case TP_FIELD_FAMILY_ANIM: FAMILY_ROWS(k_anim_field_rows); break;
        case TP_FIELD_FAMILY_TARGET: FAMILY_ROWS(k_target_field_rows); break;
    }
    if (count) {
        *count = n;
    }
    return rows;
}

#undef FAMILY_ROWS

/* ---- schema walk: operations -> fields -> type/range/enum/label ---------- */

const tp_field_row *tp_op_field_row_by_key(tp_field_family family,
                                           const char *key) {
    if (!key) {
        return NULL;
    }
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(family, &count);
    for (size_t i = 0U; i < count; i++) {
        if (strcmp(rows[i].key, key) == 0) {
            return &rows[i];
        }
    }
    return NULL;
}

const char *tp_field_enum_token(const tp_field_row *row, int value) {
    if (!row || row->domain != TP_FIELD_DOMAIN_ENUM) {
        return NULL;
    }
    for (uint8_t i = 0U; i < row->value_count; i++) {
        if (row->values[i].value == value) {
            return row->values[i].token;
        }
    }
    return NULL;
}

bool tp_field_enum_lookup(const tp_field_row *row, const char *token,
                          int *out_value) {
    if (!row || !token || row->domain != TP_FIELD_DOMAIN_ENUM) {
        return false;
    }
    for (uint8_t i = 0U; i < row->value_count; i++) {
        if (strcmp(row->values[i].token, token) == 0) {
            if (out_value) {
                *out_value = row->values[i].value;
            }
            return true;
        }
    }
    return false;
}

bool tp_field_value_admissible(const tp_field_row *row, double value) {
    if (!row) {
        return false;
    }
    if (value != value) { /* NaN is never a value, whatever the domain */
        return false;
    }
    if (row->has_unset && value == row->reset) {
        return true; /* the unset/inherit sentinel is always admissible */
    }
    switch (row->domain) {
        case TP_FIELD_DOMAIN_RANGE:
            return (row->min_exclusive ? value > row->range_min
                                       : value >= row->range_min) &&
                   value <= row->range_max;
        case TP_FIELD_DOMAIN_ENUM:
            return tp_field_enum_token(row, (int)value) != NULL;
        case TP_FIELD_DOMAIN_EXPORTER_ID: /* a string domain: not value-checkable */
            return false;
        case TP_FIELD_DOMAIN_ANY:
            return true;
    }
    return false;
}

bool tp_op__sprite_clear_bit(const char *token, uint32_t *bit) {
    if (!token) {
        return false;
    }
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(TP_FIELD_FAMILY_SPRITE, &count);
    for (size_t i = 0U; i < count; i++) {
        if (rows[i].clear_token && strcmp(rows[i].clear_token, token) == 0) {
            if (bit) {
                *bit = rows[i].bit;
            }
            return true;
        }
    }
    return false;
}

/* ---- the generic field walkers ------------------------------------------ *
 * Each has exactly ONE default-less switch over tp_field_type, so adding a
 * type is a compile error at every walker (-Wswitch) rather than a silent drop. */
void tp_op__fields_apply(tp_field_family family, const void *payload,
                         uint32_t mask, void *record) {
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(family, &count);
    for (size_t i = 0U; i < count; i++) {
        const tp_field_row *row = &rows[i];
        if ((mask & row->bit) == 0U) {
            continue;
        }
        const void *src = (const char *)payload + row->op_off;
        void *dst = (char *)record + row->rec_off;
        switch (row->type) {
            case TP_FIELD_INT: *(int *)dst = *(const int *)src; break;
            case TP_FIELD_INT_I16: *(int16_t *)dst = (int16_t)*(const int *)src; break;
            case TP_FIELD_INT_U16: *(uint16_t *)dst = (uint16_t)*(const int *)src; break;
            case TP_FIELD_BOOL: *(bool *)dst = *(const bool *)src; break;
            case TP_FIELD_FLOAT: *(float *)dst = *(const float *)src; break;
            /* An owned string is swapped by its op's stage-then-commit path
             * (an OOM must leave the record byte-unchanged), never copied here. */
            case TP_FIELD_STR: break;
        }
    }
}

void tp_op__fields_clear(tp_field_family family, uint32_t mask, void *record) {
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(family, &count);
    for (size_t i = 0U; i < count; i++) {
        const tp_field_row *row = &rows[i];
        if ((mask & row->bit) == 0U) {
            continue;
        }
        void *dst = (char *)record + row->rec_off;
        switch (row->type) {
            case TP_FIELD_INT: *(int *)dst = (int)row->reset; break;
            case TP_FIELD_INT_I16: *(int16_t *)dst = (int16_t)row->reset; break;
            case TP_FIELD_INT_U16: *(uint16_t *)dst = (uint16_t)row->reset; break;
            case TP_FIELD_BOOL: *(bool *)dst = row->reset != 0.0; break;
            case TP_FIELD_FLOAT: *(float *)dst = (float)row->reset; break;
            case TP_FIELD_STR: break; /* an owned string clears through its own free/swap */
        }
    }
}

bool tp_op__fields_match(tp_field_family family, const void *payload,
                         uint32_t mask, const void *record, bool *needs_fold) {
    bool match = true;
    size_t count = 0U;
    const tp_field_row *rows = tp_op_field_rows(family, &count);
    for (size_t i = 0U; i < count; i++) {
        const tp_field_row *row = &rows[i];
        if ((mask & row->bit) == 0U) {
            continue;
        }
        const void *want = (const char *)payload + row->op_off;
        const void *have = (const char *)record + row->rec_off;
        /* A masked FLOAT never votes: semantic identity follows canonical %.9g
         * text, so a binary difference is deferred to the canonical fold. */
        switch (row->type) {
            case TP_FIELD_INT: match = match && *(const int *)want == *(const int *)have; break;
            case TP_FIELD_INT_I16: match = match && (int16_t)*(const int *)want == *(const int16_t *)have; break;
            case TP_FIELD_INT_U16: match = match && (uint16_t)*(const int *)want == *(const uint16_t *)have; break;
            case TP_FIELD_BOOL: match = match && *(const bool *)want == *(const bool *)have; break;
            case TP_FIELD_FLOAT:
                if (*(const float *)want != *(const float *)have && needs_fold) {
                    *needs_fold = true;
                }
                break;
            case TP_FIELD_STR: {
                const char *a = *(const char *const *)want;
                const char *b = *(const char *const *)have;
                match = match && a && b && strcmp(a, b) == 0;
                break;
            }
        }
    }
    return match;
}

const tp_op_info *tp_op_info_by_kind(tp_op_kind kind) {
    if (kind <= TP_OP_INVALID || kind >= TP_OP_KIND_COUNT) {
        return NULL;
    }
    return &k_ops[kind];
}

const tp_op_info *tp_op_info_by_wire(const char *wire) {
    if (!wire) {
        return NULL;
    }
    for (int i = TP_OP_INVALID + 1; i < TP_OP_KIND_COUNT; i++) {
        if (strcmp(k_ops[i].wire, wire) == 0) {
            return &k_ops[i];
        }
    }
    return NULL;
}

tp_op_kind tp_op_kind_from_wire(const char *wire) {
    const tp_op_info *info = tp_op_info_by_wire(wire);
    return info ? info->kind : TP_OP_INVALID;
}

const char *tp_op_wire(tp_op_kind kind) {
    const tp_op_info *info = tp_op_info_by_kind(kind);
    return info ? info->wire : "";
}

const char *tp_op_class_name(tp_op_class cls) {
    switch (cls) {
        case TP_OP_CLASS_CREATE: return "create";
        case TP_OP_CLASS_REMOVE: return "remove";
        case TP_OP_CLASS_MOVE: return "move";
        case TP_OP_CLASS_SET: return "set";
    }
    return "unknown";
}

/* Closed per-op canonical field vocabulary: the op's ADDRESSING keys, plus -- for
 * a field-presence SET op -- its registry family, which owns the payload keys so
 * they are never spelled twice. Fixed-arity tuples are scalar fields
 * (origin_x/origin_y, slice9_l..b); only genuinely variable-length lists (frames,
 * the clear `fields` list) are JSON arrays. Sprite ops address by the canonical
 * {source_id, src_key} identity (from which sprite_id derives) -- see
 * docs/architecture/model-operations-and-session.md. */
static const char *const f_atlas_create[] = {"atlas_id", "name"};
static const char *const f_atlas_only[] = {"atlas_id"};
static const char *const f_atlas_rename[] = {"atlas_id", "name"};
static const char *const f_source_add[] = {"atlas_id", "source_id", "key", "kind"};
static const char *const f_source_remove[] = {"atlas_id", "source_id"};
static const char *const f_source_replace[] = {"atlas_id", "source_id", "key"};
static const char *const f_sprite_addr[] = {"atlas_id", "source_id", "src_key"};
static const char *const f_sprite_ov_clear[] = {"atlas_id", "source_id", "src_key", "fields"};
static const char *const f_sprite_name[] = {"atlas_id", "source_id", "src_key", "name"};
static const char *const f_anim_create[] = {"atlas_id", "anim_id", "name",   "fps",
                                            "playback", "flip_h",  "flip_v", "frames"};
static const char *const f_anim_addr[] = {"atlas_id", "anim_id"};
static const char *const f_anim_frames_set[] = {"atlas_id", "anim_id", "frames"};
static const char *const f_anim_frame_add[] = {"atlas_id", "anim_id", "frame", "index"};
static const char *const f_anim_frame_remove[] = {"atlas_id", "anim_id", "index"};
static const char *const f_anim_frame_move[] = {"atlas_id", "anim_id", "from_index", "to_index"};
static const char *const f_target_create[] = {"atlas_id", "target_id", "exporter_id", "out_path", "enabled"};
static const char *const f_target_addr[] = {"atlas_id", "target_id"};
static const char *const f_anim_rename[] = {"atlas_id", "anim_id", "name"};

#define NO_FAMILY (-1) /* the op carries no field-presence mask */
#define FV(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))
static const struct {
    tp_op_kind kind;
    const char *const *keys;
    int count;
    int family; /* tp_field_family, or NO_FAMILY */
} k_fields[TP_OP_KIND_COUNT] = {
    {TP_OP_INVALID, NULL, 0, NO_FAMILY},
    {TP_OP_ATLAS_CREATE, FV(f_atlas_create), NO_FAMILY},
    {TP_OP_ATLAS_REMOVE, FV(f_atlas_only), NO_FAMILY},
    {TP_OP_ATLAS_RENAME, FV(f_atlas_rename), NO_FAMILY},
    {TP_OP_ATLAS_SETTINGS_SET, FV(f_atlas_only), TP_FIELD_FAMILY_ATLAS},
    {TP_OP_SOURCE_ADD, FV(f_source_add), NO_FAMILY},
    {TP_OP_SOURCE_REMOVE, FV(f_source_remove), NO_FAMILY},
    {TP_OP_SOURCE_REPLACE, FV(f_source_replace), NO_FAMILY},
    {TP_OP_SPRITE_OVERRIDE_SET, FV(f_sprite_addr), TP_FIELD_FAMILY_SPRITE},
    {TP_OP_SPRITE_OVERRIDE_CLEAR, FV(f_sprite_ov_clear), NO_FAMILY},
    {TP_OP_SPRITE_NAME_SET, FV(f_sprite_name), NO_FAMILY},
    {TP_OP_ANIMATION_CREATE, FV(f_anim_create), NO_FAMILY},
    {TP_OP_ANIMATION_REMOVE, FV(f_anim_addr), NO_FAMILY},
    {TP_OP_ANIMATION_SETTINGS_SET, FV(f_anim_addr), TP_FIELD_FAMILY_ANIM},
    {TP_OP_ANIMATION_FRAMES_SET, FV(f_anim_frames_set), NO_FAMILY},
    {TP_OP_ANIMATION_FRAME_ADD, FV(f_anim_frame_add), NO_FAMILY},
    {TP_OP_ANIMATION_FRAME_REMOVE, FV(f_anim_frame_remove), NO_FAMILY},
    {TP_OP_ANIMATION_FRAME_MOVE, FV(f_anim_frame_move), NO_FAMILY},
    {TP_OP_TARGET_CREATE, FV(f_target_create), NO_FAMILY},
    {TP_OP_TARGET_REMOVE, FV(f_target_addr), NO_FAMILY},
    {TP_OP_TARGET_SET, FV(f_target_addr), TP_FIELD_FAMILY_TARGET},
    {TP_OP_ANIMATION_RENAME, FV(f_anim_rename), NO_FAMILY},
};
#undef FV

bool tp_op_catalog_selfcheck(void) {
    for (int k = 0; k < TP_OP_KIND_COUNT; k++) {
        if (k_ops[k].kind != (tp_op_kind)k || k_fields[k].kind != (tp_op_kind)k) {
            return false;
        }
    }
    return true;
}

bool tp_op_field_allowed(tp_op_kind kind, const char *key) {
    if (!key || kind <= TP_OP_INVALID || kind >= TP_OP_KIND_COUNT) {
        return false;
    }
    if (strcmp(key, "op") == 0) { /* the discriminator, not a payload key */
        return true;
    }
    for (int i = 0; i < k_fields[kind].count; i++) {
        if (strcmp(k_fields[kind].keys[i], key) == 0) {
            return true;
        }
    }
    tp_field_family family;
    if (!tp_op_field_family_of(kind, &family)) {
        return false;
    }
    return tp_op_field_row_by_key(family, key) != NULL;
}

bool tp_op_field_family_of(tp_op_kind kind, tp_field_family *out_family) {
    if (kind <= TP_OP_INVALID || kind >= TP_OP_KIND_COUNT ||
        k_fields[kind].family == NO_FAMILY) {
        return false;
    }
    if (out_family) {
        *out_family = (tp_field_family)k_fields[kind].family;
    }
    return true;
}

const tp_field_row *tp_op_fields(tp_op_kind kind, size_t *count) {
    tp_field_family family;
    if (!tp_op_field_family_of(kind, &family)) {
        if (count) {
            *count = 0U;
        }
        return NULL;
    }
    return tp_op_field_rows(family, count);
}

/* tp_operation_free: release the active arm's malloc-owned strings/arrays. Freeing
 * NULL is safe; a zero-initialized op frees nothing. Kept beside the catalog (both
 * are pure metadata about op shape). */
static void free_frames(tp_op_sprite_ref *frames, int count) {
    if (!frames) {
        return;
    }
    for (int i = 0; i < count; i++) {
        free(frames[i].src_key);
    }
    free(frames);
}

void tp_operation_free(tp_operation *op) {
    if (!op) {
        return;
    }
    switch (op->kind) {
        case TP_OP_ATLAS_CREATE: free(op->u.atlas_create.name); break;
        case TP_OP_ATLAS_RENAME: free(op->u.atlas_rename.name); break;
        case TP_OP_ANIMATION_RENAME: free(op->u.anim_rename.name); break;
        case TP_OP_SOURCE_ADD: free(op->u.source_add.key); break;
        case TP_OP_SOURCE_REPLACE:
        case TP_OP_SOURCE_REMOVE: free(op->u.source_ref.key); break;
        case TP_OP_SPRITE_OVERRIDE_SET: free(op->u.sprite_set.src_key); break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR: free(op->u.sprite_clear.src_key); break;
        case TP_OP_SPRITE_NAME_SET:
            free(op->u.sprite_name.src_key);
            free(op->u.sprite_name.name);
            break;
        case TP_OP_ANIMATION_CREATE:
            free(op->u.anim_create.name);
            free_frames(op->u.anim_create.frames, op->u.anim_create.frame_count);
            break;
        case TP_OP_ANIMATION_FRAMES_SET:
            free_frames(op->u.anim_frames_set.frames, op->u.anim_frames_set.frame_count);
            break;
        case TP_OP_ANIMATION_FRAME_ADD: free(op->u.anim_frame_add.frame.src_key); break;
        case TP_OP_TARGET_CREATE:
            free(op->u.target_create.exporter_id);
            free(op->u.target_create.out_path);
            break;
        case TP_OP_TARGET_SET:
            free(op->u.target_set.exporter_id);
            free(op->u.target_set.out_path);
            break;
        case TP_OP_INVALID:
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_SETTINGS_SET:
        case TP_OP_ANIMATION_REMOVE:
        case TP_OP_ANIMATION_SETTINGS_SET:
        case TP_OP_ANIMATION_FRAME_REMOVE:
        case TP_OP_ANIMATION_FRAME_MOVE:
        case TP_OP_TARGET_REMOVE:
        case TP_OP_KIND_COUNT: break; /* no owned strings */
    }
    memset(op, 0, sizeof *op);
}
