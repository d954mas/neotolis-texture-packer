#include "tp_c0/tp_c0_op.h"

#include <string.h>

/* The append-only catalog. Row order matches the enum so index == kind. Every
 * current ntpacker mutation verb appears in the cli_verb column exactly once (a
 * compound verb that lowers to several ops -- `sprite set`, `new` -- is noted in
 * C0-02-contract.md §1; the column names the verb whose canonical lowering is
 * that single op). Reserved rows (cli_verb == NULL) are spec-listed ops with no
 * current verb. */
static const tp_c0_op_info k_ops[TP_C0_OP_KIND_COUNT] = {
    {TP_C0_OP_INVALID, "", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_INVALID, NULL},

    {TP_C0_OP_ATLAS_CREATE, "atlas.create", TP_C0_OP_CLASS_CREATE, TP_C0_ID_KIND_ATLAS, "atlas add"},
    {TP_C0_OP_ATLAS_REMOVE, "atlas.remove", TP_C0_OP_CLASS_REMOVE, TP_C0_ID_KIND_ATLAS, "atlas remove"},
    {TP_C0_OP_ATLAS_RENAME, "atlas.rename", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_ATLAS, "atlas rename"},
    {TP_C0_OP_ATLAS_SETTINGS_SET, "atlas.settings.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_ATLAS, "set"},

    {TP_C0_OP_SOURCE_ADD, "source.add", TP_C0_OP_CLASS_CREATE, TP_C0_ID_KIND_SOURCE, "add"},
    {TP_C0_OP_SOURCE_REMOVE, "source.remove", TP_C0_OP_CLASS_REMOVE, TP_C0_ID_KIND_SOURCE, "remove"},
    {TP_C0_OP_SOURCE_REPLACE, "source.replace", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_SOURCE, NULL},

    {TP_C0_OP_SPRITE_OVERRIDE_SET, "sprite.override.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_SOURCE, "sprite set"},
    {TP_C0_OP_SPRITE_OVERRIDE_CLEAR, "sprite.override.clear", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_SOURCE, "sprite unset"},
    {TP_C0_OP_SPRITE_NAME_SET, "sprite.name.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_SOURCE, "sprite set"},

    {TP_C0_OP_ANIMATION_CREATE, "animation.create", TP_C0_OP_CLASS_CREATE, TP_C0_ID_KIND_ANIM, "anim create"},
    {TP_C0_OP_ANIMATION_REMOVE, "animation.remove", TP_C0_OP_CLASS_REMOVE, TP_C0_ID_KIND_ANIM, "anim remove"},
    {TP_C0_OP_ANIMATION_SETTINGS_SET, "animation.settings.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_ANIM, "anim set"},
    {TP_C0_OP_ANIMATION_FRAMES_SET, "animation.frames.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_ANIM, NULL},
    {TP_C0_OP_ANIMATION_FRAME_ADD, "animation.frame.add", TP_C0_OP_CLASS_CREATE, TP_C0_ID_KIND_ANIM, "anim add-frame"},
    {TP_C0_OP_ANIMATION_FRAME_REMOVE, "animation.frame.remove", TP_C0_OP_CLASS_REMOVE, TP_C0_ID_KIND_ANIM,
     "anim remove-frame"},
    {TP_C0_OP_ANIMATION_FRAME_MOVE, "animation.frame.move", TP_C0_OP_CLASS_MOVE, TP_C0_ID_KIND_ANIM, "anim move-frame"},

    {TP_C0_OP_TARGET_CREATE, "target.create", TP_C0_OP_CLASS_CREATE, TP_C0_ID_KIND_TARGET, "target add"},
    {TP_C0_OP_TARGET_REMOVE, "target.remove", TP_C0_OP_CLASS_REMOVE, TP_C0_ID_KIND_TARGET, "target remove"},
    {TP_C0_OP_TARGET_SET, "target.set", TP_C0_OP_CLASS_SET, TP_C0_ID_KIND_TARGET, "target set"},
};

const tp_c0_op_info *tp_c0_op_info_by_kind(tp_c0_op_kind kind) {
    if (kind <= TP_C0_OP_INVALID || kind >= TP_C0_OP_KIND_COUNT) {
        return NULL;
    }
    return &k_ops[kind];
}

const tp_c0_op_info *tp_c0_op_info_by_wire(const char *wire) {
    if (!wire) {
        return NULL;
    }
    for (int i = TP_C0_OP_INVALID + 1; i < TP_C0_OP_KIND_COUNT; i++) {
        if (strcmp(k_ops[i].wire, wire) == 0) {
            return &k_ops[i];
        }
    }
    return NULL;
}

tp_c0_op_kind tp_c0_op_kind_from_wire(const char *wire) {
    const tp_c0_op_info *info = tp_c0_op_info_by_wire(wire);
    return info ? info->kind : TP_C0_OP_INVALID;
}

const char *tp_c0_op_wire(tp_c0_op_kind kind) {
    const tp_c0_op_info *info = tp_c0_op_info_by_kind(kind);
    return info ? info->wire : "";
}

const char *tp_c0_op_class_name(tp_c0_op_class cls) {
    switch (cls) {
        case TP_C0_OP_CLASS_CREATE: return "create";
        case TP_C0_OP_CLASS_REMOVE: return "remove";
        case TP_C0_OP_CLASS_MOVE: return "move";
        case TP_C0_OP_CLASS_SET: return "set";
    }
    return "unknown";
}

tp_c0_op_class tp_c0_op_class_from_name(const char *name, bool *ok) {
    if (name) {
        for (int c = TP_C0_OP_CLASS_CREATE; c <= TP_C0_OP_CLASS_SET; c++) {
            if (strcmp(tp_c0_op_class_name((tp_c0_op_class)c), name) == 0) {
                if (ok) {
                    *ok = true;
                }
                return (tp_c0_op_class)c;
            }
        }
    }
    if (ok) {
        *ok = false;
    }
    return TP_C0_OP_CLASS_SET;
}

/* Closed per-op canonical field vocabulary (task 2). Addressing `*_id` keys plus
 * typed payload keys. Fixed-arity tuples are scalar fields (origin_x/origin_y,
 * slice9_l..b) -- only genuinely variable-length lists (frames, clear `fields`)
 * are JSON arrays (contract §3). */
static const char *const f_atlas_create[] = {"atlas_id", "name"};
static const char *const f_atlas_remove[] = {"atlas_id"};
static const char *const f_atlas_rename[] = {"atlas_id", "name"};
static const char *const f_atlas_settings[] = {"atlas_id",     "max_size",        "padding",
                                               "margin",       "extrude",         "alpha_threshold",
                                               "max_vertices", "shape",           "allow_transform",
                                               "power_of_two", "pixels_per_unit"};
static const char *const f_source_add[] = {"atlas_id", "source_id", "key"};
static const char *const f_source_remove[] = {"source_id"};
static const char *const f_source_replace[] = {"source_id", "key"};
static const char *const f_sprite_ov_set[] = {"atlas_id",     "sprite_id", "origin_x", "origin_y",
                                              "slice9_l",     "slice9_r",  "slice9_t", "slice9_b",
                                              "ov_shape",     "ov_allow_rotate",
                                              "ov_max_vertices", "ov_margin", "ov_extrude"};
static const char *const f_sprite_ov_clear[] = {"atlas_id", "sprite_id", "fields"};
static const char *const f_sprite_name[] = {"atlas_id", "sprite_id", "name"};
static const char *const f_anim_create[] = {"atlas_id", "anim_id", "id",     "fps",
                                            "playback", "flip_h",  "flip_v", "frames"};
static const char *const f_anim_remove[] = {"anim_id"};
static const char *const f_anim_settings[] = {"anim_id", "fps", "playback", "flip_h", "flip_v"};
static const char *const f_anim_frames_set[] = {"anim_id", "frames"};
static const char *const f_anim_frame_add[] = {"anim_id", "frame", "index"};
static const char *const f_anim_frame_remove[] = {"anim_id", "frame", "index"};
static const char *const f_anim_frame_move[] = {"anim_id", "from_index", "to_index"};
static const char *const f_target_create[] = {"atlas_id", "target_id", "exporter_id", "out_path", "enabled"};
static const char *const f_target_remove[] = {"target_id"};
static const char *const f_target_set[] = {"target_id", "exporter_id", "out_path", "enabled"};

/* The `kind` column pins each row to its enum slot so a reorder is caught by
 * tp_c0_op_catalog_selfcheck instead of silently mis-mapping a kind to the wrong
 * field vocabulary (row order still must match the enum: index == kind). */
#define FV(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))
static const struct {
    tp_c0_op_kind kind;
    const char *const *keys;
    int count;
} k_fields[TP_C0_OP_KIND_COUNT] = {
    {TP_C0_OP_INVALID, NULL, 0},
    {TP_C0_OP_ATLAS_CREATE, FV(f_atlas_create)},
    {TP_C0_OP_ATLAS_REMOVE, FV(f_atlas_remove)},
    {TP_C0_OP_ATLAS_RENAME, FV(f_atlas_rename)},
    {TP_C0_OP_ATLAS_SETTINGS_SET, FV(f_atlas_settings)},
    {TP_C0_OP_SOURCE_ADD, FV(f_source_add)},
    {TP_C0_OP_SOURCE_REMOVE, FV(f_source_remove)},
    {TP_C0_OP_SOURCE_REPLACE, FV(f_source_replace)},
    {TP_C0_OP_SPRITE_OVERRIDE_SET, FV(f_sprite_ov_set)},
    {TP_C0_OP_SPRITE_OVERRIDE_CLEAR, FV(f_sprite_ov_clear)},
    {TP_C0_OP_SPRITE_NAME_SET, FV(f_sprite_name)},
    {TP_C0_OP_ANIMATION_CREATE, FV(f_anim_create)},
    {TP_C0_OP_ANIMATION_REMOVE, FV(f_anim_remove)},
    {TP_C0_OP_ANIMATION_SETTINGS_SET, FV(f_anim_settings)},
    {TP_C0_OP_ANIMATION_FRAMES_SET, FV(f_anim_frames_set)},
    {TP_C0_OP_ANIMATION_FRAME_ADD, FV(f_anim_frame_add)},
    {TP_C0_OP_ANIMATION_FRAME_REMOVE, FV(f_anim_frame_remove)},
    {TP_C0_OP_ANIMATION_FRAME_MOVE, FV(f_anim_frame_move)},
    {TP_C0_OP_TARGET_CREATE, FV(f_target_create)},
    {TP_C0_OP_TARGET_REMOVE, FV(f_target_remove)},
    {TP_C0_OP_TARGET_SET, FV(f_target_set)},
};
#undef FV

bool tp_c0_op_catalog_selfcheck(void) {
    for (int k = 0; k < TP_C0_OP_KIND_COUNT; k++) {
        if (k_ops[k].kind != (tp_c0_op_kind)k || k_fields[k].kind != (tp_c0_op_kind)k) {
            return false;
        }
    }
    return true;
}

const char *const *tp_c0_op_fields(tp_c0_op_kind kind, int *count) {
    if (kind <= TP_C0_OP_INVALID || kind >= TP_C0_OP_KIND_COUNT) {
        if (count) {
            *count = 0;
        }
        return NULL;
    }
    if (count) {
        *count = k_fields[kind].count;
    }
    return k_fields[kind].keys;
}

bool tp_c0_op_field_allowed(tp_c0_op_kind kind, const char *key) {
    if (!key) {
        return false;
    }
    if (strcmp(key, "op") == 0) {
        return true;
    }
    int n = 0;
    const char *const *keys = tp_c0_op_fields(kind, &n);
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i], key) == 0) {
            return true;
        }
    }
    return false;
}
