/*
 * F2-01 task 1: the append-only operation catalog + closed field vocabulary.
 * Promoted from the accepted C0-02 spike (packer/spike/c0/src/tp_c0_op_catalog.c)
 * onto the production tp_id_kind. Row order matches the enum so index == kind;
 * tp_op_catalog_selfcheck() pins that a reorder can't silently mis-map a kind.
 *
 * Every current ntpacker mutation verb appears in the cli_verb column (a compound
 * verb -- `sprite set`, `new` -- names the verb whose canonical lowering is that
 * single op; see docs/decisions and C0-02-contract.md §1). Reserved rows
 * (cli_verb == NULL) are spec-listed ops with no current verb. There is
 * deliberately NO raw field-patch escape hatch (§6.2).
 */

#include "tp_core/tp_operation.h"

#include <stdlib.h>
#include <string.h>

static const tp_op_info k_ops[TP_OP_KIND_COUNT] = {
    {TP_OP_INVALID, "", TP_OP_CLASS_SET, TP_ID_KIND_INVALID, NULL},

    {TP_OP_ATLAS_CREATE, "atlas.create", TP_OP_CLASS_CREATE, TP_ID_KIND_ATLAS, "atlas add"},
    {TP_OP_ATLAS_REMOVE, "atlas.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ATLAS, "atlas remove"},
    {TP_OP_ATLAS_RENAME, "atlas.rename", TP_OP_CLASS_SET, TP_ID_KIND_ATLAS, "atlas rename"},
    {TP_OP_ATLAS_SETTINGS_SET, "atlas.settings.set", TP_OP_CLASS_SET, TP_ID_KIND_ATLAS, "set"},

    {TP_OP_SOURCE_ADD, "source.add", TP_OP_CLASS_CREATE, TP_ID_KIND_SOURCE, "add"},
    {TP_OP_SOURCE_REMOVE, "source.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_SOURCE, "remove"},
    {TP_OP_SOURCE_REPLACE, "source.replace", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, NULL},

    {TP_OP_SPRITE_OVERRIDE_SET, "sprite.override.set", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite set"},
    {TP_OP_SPRITE_OVERRIDE_CLEAR, "sprite.override.clear", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite unset"},
    {TP_OP_SPRITE_NAME_SET, "sprite.name.set", TP_OP_CLASS_SET, TP_ID_KIND_SOURCE, "sprite set"},

    {TP_OP_ANIMATION_CREATE, "animation.create", TP_OP_CLASS_CREATE, TP_ID_KIND_ANIM, "anim create"},
    {TP_OP_ANIMATION_REMOVE, "animation.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ANIM, "anim remove"},
    {TP_OP_ANIMATION_SETTINGS_SET, "animation.settings.set", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, "anim set"},
    {TP_OP_ANIMATION_FRAMES_SET, "animation.frames.set", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, NULL},
    {TP_OP_ANIMATION_FRAME_ADD, "animation.frame.add", TP_OP_CLASS_CREATE, TP_ID_KIND_ANIM, "anim add-frame"},
    {TP_OP_ANIMATION_FRAME_REMOVE, "animation.frame.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_ANIM, "anim remove-frame"},
    {TP_OP_ANIMATION_FRAME_MOVE, "animation.frame.move", TP_OP_CLASS_MOVE, TP_ID_KIND_ANIM, "anim move-frame"},

    {TP_OP_TARGET_CREATE, "target.create", TP_OP_CLASS_CREATE, TP_ID_KIND_TARGET, "target add"},
    {TP_OP_TARGET_REMOVE, "target.remove", TP_OP_CLASS_REMOVE, TP_ID_KIND_TARGET, "target remove"},
    {TP_OP_TARGET_SET, "target.set", TP_OP_CLASS_SET, TP_ID_KIND_TARGET, "target set"},

    /* H/P1-2: appended (APPEND-ONLY catalog) -- animation rename is a first-class undoable+journaled op. */
    {TP_OP_ANIMATION_RENAME, "animation.rename", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, "anim rename"},
};

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

/* Closed per-op canonical field vocabulary (task 1). Addressing `*_id` keys +
 * typed payload keys. Fixed-arity tuples are scalar fields (origin_x/origin_y,
 * slice9_l..b); only genuinely variable-length lists (frames, the clear `fields`
 * list) are JSON arrays. Sprite ops address by the canonical {source_id, src_key}
 * identity (from which sprite_id derives) -- see docs/decisions. */
static const char *const f_atlas_create[] = {"atlas_id", "name"};
static const char *const f_atlas_remove[] = {"atlas_id"};
static const char *const f_atlas_rename[] = {"atlas_id", "name"};
static const char *const f_atlas_settings[] = {"atlas_id",     "max_size",     "padding",         "margin",
                                               "extrude",      "alpha_threshold", "max_vertices", "shape",
                                               "allow_transform", "power_of_two", "pixels_per_unit"};
static const char *const f_source_add[] = {"atlas_id", "source_id", "key", "kind"};
static const char *const f_source_remove[] = {"atlas_id", "source_id"};
static const char *const f_source_replace[] = {"atlas_id", "source_id", "key"};
static const char *const f_sprite_ov_set[] = {"atlas_id",     "source_id",    "src_key",  "origin_x",
                                              "origin_y",     "slice9_l",     "slice9_r", "slice9_t",
                                              "slice9_b",     "ov_shape",     "ov_allow_rotate",
                                              "ov_max_vertices", "ov_margin", "ov_extrude"};
static const char *const f_sprite_ov_clear[] = {"atlas_id", "source_id", "src_key", "fields"};
static const char *const f_sprite_name[] = {"atlas_id", "source_id", "src_key", "name"};
static const char *const f_anim_create[] = {"atlas_id", "anim_id", "name",   "fps",
                                            "playback", "flip_h",  "flip_v", "frames"};
static const char *const f_anim_remove[] = {"atlas_id", "anim_id"};
static const char *const f_anim_settings[] = {"atlas_id", "anim_id", "fps", "playback", "flip_h", "flip_v"};
static const char *const f_anim_frames_set[] = {"atlas_id", "anim_id", "frames"};
static const char *const f_anim_frame_add[] = {"atlas_id", "anim_id", "frame", "index"};
static const char *const f_anim_frame_remove[] = {"atlas_id", "anim_id", "index"};
static const char *const f_anim_frame_move[] = {"atlas_id", "anim_id", "from_index", "to_index"};
static const char *const f_target_create[] = {"atlas_id", "target_id", "exporter_id", "out_path", "enabled"};
static const char *const f_target_remove[] = {"atlas_id", "target_id"};
static const char *const f_target_set[] = {"atlas_id", "target_id", "exporter_id", "out_path", "enabled"};
static const char *const f_anim_rename[] = {"atlas_id", "anim_id", "name"};

#define FV(arr) (arr), (int)(sizeof(arr) / sizeof((arr)[0]))
static const struct {
    tp_op_kind kind;
    const char *const *keys;
    int count;
} k_fields[TP_OP_KIND_COUNT] = {
    {TP_OP_INVALID, NULL, 0},
    {TP_OP_ATLAS_CREATE, FV(f_atlas_create)},
    {TP_OP_ATLAS_REMOVE, FV(f_atlas_remove)},
    {TP_OP_ATLAS_RENAME, FV(f_atlas_rename)},
    {TP_OP_ATLAS_SETTINGS_SET, FV(f_atlas_settings)},
    {TP_OP_SOURCE_ADD, FV(f_source_add)},
    {TP_OP_SOURCE_REMOVE, FV(f_source_remove)},
    {TP_OP_SOURCE_REPLACE, FV(f_source_replace)},
    {TP_OP_SPRITE_OVERRIDE_SET, FV(f_sprite_ov_set)},
    {TP_OP_SPRITE_OVERRIDE_CLEAR, FV(f_sprite_ov_clear)},
    {TP_OP_SPRITE_NAME_SET, FV(f_sprite_name)},
    {TP_OP_ANIMATION_CREATE, FV(f_anim_create)},
    {TP_OP_ANIMATION_REMOVE, FV(f_anim_remove)},
    {TP_OP_ANIMATION_SETTINGS_SET, FV(f_anim_settings)},
    {TP_OP_ANIMATION_FRAMES_SET, FV(f_anim_frames_set)},
    {TP_OP_ANIMATION_FRAME_ADD, FV(f_anim_frame_add)},
    {TP_OP_ANIMATION_FRAME_REMOVE, FV(f_anim_frame_remove)},
    {TP_OP_ANIMATION_FRAME_MOVE, FV(f_anim_frame_move)},
    {TP_OP_TARGET_CREATE, FV(f_target_create)},
    {TP_OP_TARGET_REMOVE, FV(f_target_remove)},
    {TP_OP_TARGET_SET, FV(f_target_set)},
    {TP_OP_ANIMATION_RENAME, FV(f_anim_rename)},
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

const char *const *tp_op_fields(tp_op_kind kind, int *count) {
    if (kind <= TP_OP_INVALID || kind >= TP_OP_KIND_COUNT) {
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

bool tp_op_field_allowed(tp_op_kind kind, const char *key) {
    if (!key) {
        return false;
    }
    if (strcmp(key, "op") == 0) {
        return true;
    }
    int n = 0;
    const char *const *keys = tp_op_fields(kind, &n);
    for (int i = 0; i < n; i++) {
        if (strcmp(keys[i], key) == 0) {
            return true;
        }
    }
    return false;
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
