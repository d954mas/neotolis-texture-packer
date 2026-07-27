/*
 * The append-only operation catalog + closed field vocabulary. Row order
 * matches the enum so index == kind;
 * tp_op_catalog_selfcheck() pins that a reorder can't silently mis-map a kind.
 *
 * Every current ntpacker mutation verb appears in the cli_verb column (a compound
 * verb -- `sprite set`, `new` -- names the verb whose canonical lowering is that
 * single op; see decision 0010 §1). Reserved rows
 * (cli_verb == NULL) are spec-listed ops with no current verb. There is
 * deliberately NO raw field-patch escape hatch (§6.2).
 */

#include "tp_op_internal.h"

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

    /* Appended (APPEND-ONLY catalog) -- animation rename is a first-class undoable+journaled op. */
    {TP_OP_ANIMATION_RENAME, "animation.rename", TP_OP_CLASS_SET, TP_ID_KIND_ANIM, "anim rename"},
};

/* ---- the field registry (spec §6) --------------------------------------- *
 * Row order per family is the JSON-lowering order; for sprite it is also the
 * clear-token emission order. Grouped bits occupy CONSECUTIVE rows -- the
 * arity and clear walkers rely on that. */
#define OP_OFF(TYPE, MEMBER) (uint16_t)offsetof(TYPE, MEMBER)
/* The wire key IS the payload member's spelling for every row below, so
 * stringizing MEMBER pins key<->member: they cannot drift apart. */
#define ROW(OPT, RECT, BIT, TYPE, MEMBER, TOKEN, GROUP, RESET) \
    {(BIT), #MEMBER, (TYPE), OP_OFF(OPT, MEMBER), OP_OFF(RECT, MEMBER), (TOKEN), (GROUP), (RESET)}
#define ATLAS_ROW(BIT, TYPE, MEMBER) \
    ROW(tp_op_atlas_settings, tp_project_atlas, BIT, TYPE, MEMBER, NULL, NULL, 0.0)
#define ANIM_ROW(BIT, TYPE, MEMBER) \
    ROW(tp_op_anim_settings, tp_project_anim, BIT, TYPE, MEMBER, NULL, NULL, 0.0)
#define TARGET_ROW(BIT, TYPE, MEMBER) \
    ROW(tp_op_target_set, tp_project_target, BIT, TYPE, MEMBER, NULL, NULL, 0.0)
#define SPRITE_OV_ROW(BIT, MEMBER, TOKEN) \
    ROW(tp_op_sprite_set, tp_project_sprite, BIT, TP_FIELD_INT_I16, MEMBER, TOKEN, NULL, TP_PROJECT_OV_INHERIT)
#define SPRITE_ORIGIN_ROW(MEMBER, TOKEN)                                          \
    ROW(tp_op_sprite_set, tp_project_sprite, TP_SPF_ORIGIN, TP_FIELD_FLOAT,       \
        MEMBER, TOKEN, "origin_x and origin_y", (double)TP_PROJECT_ORIGIN_DEFAULT)
/* slice9 is the one row family whose op and record spellings differ. */
#define SPRITE_SLICE9_ROW(SLOT, SIDE, TOKEN)                                      \
    {TP_SPF_SLICE9, "slice9_" #SIDE, TP_FIELD_INT_U16,                            \
     OP_OFF(tp_op_sprite_set, slice9[SLOT]),                                      \
     OP_OFF(tp_project_sprite, slice9_lrtb[SLOT]), (TOKEN), "slice9_l/r/t/b", 0.0}

static const tp_field_row k_atlas_field_rows[] = {
    ATLAS_ROW(TP_AF_MAX_SIZE, TP_FIELD_INT, max_size),
    ATLAS_ROW(TP_AF_PADDING, TP_FIELD_INT, padding),
    ATLAS_ROW(TP_AF_MARGIN, TP_FIELD_INT, margin),
    ATLAS_ROW(TP_AF_EXTRUDE, TP_FIELD_INT, extrude),
    ATLAS_ROW(TP_AF_ALPHA_THRESHOLD, TP_FIELD_INT, alpha_threshold),
    ATLAS_ROW(TP_AF_MAX_VERTICES, TP_FIELD_INT, max_vertices),
    ATLAS_ROW(TP_AF_SHAPE, TP_FIELD_INT, shape),
    ATLAS_ROW(TP_AF_ALLOW_TRANSFORM, TP_FIELD_BOOL, allow_transform),
    ATLAS_ROW(TP_AF_POWER_OF_TWO, TP_FIELD_BOOL, power_of_two),
    ATLAS_ROW(TP_AF_PIXELS_PER_UNIT, TP_FIELD_FLOAT, pixels_per_unit),
};

/* The CLI sets origin/slice9 as a unit, but the canonical encoder emits the
 * underlying scalar keys -- so each group bit spends several rows. */
static const tp_field_row k_sprite_field_rows[] = {
    SPRITE_ORIGIN_ROW(origin_x, "origin"),
    SPRITE_ORIGIN_ROW(origin_y, NULL),
    SPRITE_SLICE9_ROW(0, l, "slice9"),
    SPRITE_SLICE9_ROW(1, r, NULL),
    SPRITE_SLICE9_ROW(2, t, NULL),
    SPRITE_SLICE9_ROW(3, b, NULL),
    SPRITE_OV_ROW(TP_SPF_SHAPE, ov_shape, "shape"),
    SPRITE_OV_ROW(TP_SPF_ALLOW_ROTATE, ov_allow_rotate, "allow_rotate"),
    SPRITE_OV_ROW(TP_SPF_MAX_VERTICES, ov_max_vertices, "max_vertices"),
    SPRITE_OV_ROW(TP_SPF_MARGIN, ov_margin, "margin"),
    SPRITE_OV_ROW(TP_SPF_EXTRUDE, ov_extrude, "extrude"),
};

static const tp_field_row k_anim_field_rows[] = {
    ANIM_ROW(TP_ANF_FPS, TP_FIELD_FLOAT, fps),
    ANIM_ROW(TP_ANF_PLAYBACK, TP_FIELD_INT, playback),
    ANIM_ROW(TP_ANF_FLIP_H, TP_FIELD_BOOL, flip_h),
    ANIM_ROW(TP_ANF_FLIP_V, TP_FIELD_BOOL, flip_v),
};

static const tp_field_row k_target_field_rows[] = {
    TARGET_ROW(TP_TF_EXPORTER, TP_FIELD_STR, exporter_id),
    TARGET_ROW(TP_TF_OUT_PATH, TP_FIELD_STR, out_path),
    TARGET_ROW(TP_TF_ENABLED, TP_FIELD_BOOL, enabled),
};

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
 * {source_id, src_key} identity (from which sprite_id derives) -- see docs/decisions. */
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
    if (k_fields[kind].family == NO_FAMILY) {
        return false;
    }
    size_t rows_n = 0U;
    const tp_field_row *rows =
        tp_op_field_rows((tp_field_family)k_fields[kind].family, &rows_n);
    for (size_t i = 0U; i < rows_n; i++) {
        if (strcmp(rows[i].key, key) == 0) {
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
