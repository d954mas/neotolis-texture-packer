/*
 * F2-01 tasks 4+5: operation payload + reference validation moved INTO core.
 * The range / name / exporter-id / reference rules that were duplicated in
 * apps/cli/cli_mutate.c (CLI-specific) and the apps/gui wrappers now live here,
 * once, producing a STRUCTURED status id + offending field + context (not prose).
 * Pure: never mutates `p`. Bounds-checked, UB-clean on arbitrary payloads.
 */

#include "tp_core/tp_operation.h"

#include <float.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "tp_core/tp_export.h"  /* tp_exporter_find (exporter-id validation) */
#include "tp_core/tp_project.h"
#include "tp_op_internal.h"

/* Reject NaN / +-inf; `pos` also rejects <= 0. No libm: comparisons only. */
static bool finite_any(float v) { return v == v && v >= -FLT_MAX && v <= FLT_MAX; }
static bool finite_pos(float v) { return v > 0.0F && v <= FLT_MAX; }

static const tp_project_atlas *find_atlas(const tp_project *p, tp_id128 id) {
    int ai = tp_project_find_atlas_by_id(p, id);
    return ai < 0 ? NULL : &p->atlases[ai];
}
/* Index of the atlas named `name` (exact, case-sensitive), or -1. Mirrors the CLI's
 * resolve_atlas so name-uniqueness rejections match the CLI oracle byte-for-byte. */
static int find_atlas_by_name(const tp_project *p, const char *name) {
    for (int i = 0; i < p->atlas_count; i++) {
        if (p->atlases[i].name && strcmp(p->atlases[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}
/* const adapters over the public id-addressed accessors (fix [8]): validate holds a
 * const project; the accessors take non-const and only READ, so the cast is safe. This
 * removes the lookup loops that duplicated the tp_project_atlas_find_*_by_id accessors. */
static const tp_project_source *find_source(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_source_by_id((tp_project_atlas *)a, id);
}
static const tp_project_anim *find_anim(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_animation_by_id((tp_project_atlas *)a, id);
}
static const tp_project_target *find_target(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_target_by_id((tp_project_atlas *)a, id);
}

/* Range-check one integer knob; returns OK or an OUT_OF_RANGE rejection. */
static tp_status range_i(tp_op_reject *rej, const char *field, long v, long lo, long hi) {
    if (v < lo || v > hi) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, field, "%s = %ld must be in [%ld..%ld]", field, v, lo, hi);
    }
    return TP_STATUS_OK;
}

/* Lower-bound-only check (no artificial upper cap). Mirrors the CLI `set` for the knobs
 * whose only constraint is `>= 0` -- the authoritative page-dim clamp lives in tp_pack. */
static tp_status min_i(tp_op_reject *rej, const char *field, long v, long lo) {
    if (v < lo) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, field, "%s = %ld must be >= %ld", field, v, lo);
    }
    return TP_STATUS_OK;
}

/* atlas.settings.set: check every masked knob against its range. */
static tp_status validate_atlas_settings(const tp_op_atlas_settings *s, tp_op_reject *rej) {
    tp_status st = TP_STATUS_OK;
    if (s->mask == 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "atlas.settings.set names no field");
    }
    if ((s->mask & TP_AF_MAX_SIZE) && (st = range_i(rej, "max_size", s->max_size, 1, TP_OP_MAX_PAGE_DIM))) {
        return st;
    }
    /* padding / margin / extrude: CLI `set` accepts any >= 0 (no upper cap here; tp_pack
     * clamps). Matching that keeps the op engine neither stricter nor looser than the CLI. */
    if ((s->mask & TP_AF_PADDING) && (st = min_i(rej, "padding", s->padding, 0))) {
        return st;
    }
    if ((s->mask & TP_AF_MARGIN) && (st = min_i(rej, "margin", s->margin, 0))) {
        return st;
    }
    if ((s->mask & TP_AF_EXTRUDE) && (st = min_i(rej, "extrude", s->extrude, 0))) {
        return st;
    }
    if ((s->mask & TP_AF_ALPHA_THRESHOLD) &&
        (st = range_i(rej, "alpha_threshold", s->alpha_threshold, 0, TP_OP_ALPHA_MAX))) {
        return st;
    }
    if ((s->mask & TP_AF_MAX_VERTICES) && (st = range_i(rej, "max_vertices", s->max_vertices, 1, TP_OP_MAX_VERTICES))) {
        return st;
    }
    if ((s->mask & TP_AF_SHAPE) && (st = range_i(rej, "shape", s->shape, 0, TP_OP_SHAPE_MAX))) {
        return st;
    }
    if ((s->mask & TP_AF_PIXELS_PER_UNIT) && !finite_pos(s->pixels_per_unit)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "pixels_per_unit", "pixels_per_unit must be positive finite");
    }
    return TP_STATUS_OK; /* allow_transform / power_of_two are booleans -- any value */
}

/* sprite.override.set: range-check each masked override field (INHERIT passes). */
static tp_status validate_sprite_set(const tp_op_sprite_set *s, tp_op_reject *rej) {
    tp_status st = TP_STATUS_OK;
    if (s->mask == 0) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "sprite.override.set names no field");
    }
    if ((s->mask & TP_SPF_ORIGIN) && (!finite_any(s->origin_x) || !finite_any(s->origin_y))) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "origin_x", "origin must be finite");
    }
    if ((s->mask & TP_SPF_SHAPE) && s->ov_shape != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_shape", s->ov_shape, 0, TP_OP_SHAPE_MAX))) {
        return st;
    }
    if ((s->mask & TP_SPF_ALLOW_ROTATE) && s->ov_allow_rotate != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_allow_rotate", s->ov_allow_rotate, 0, 1))) {
        return st;
    }
    if ((s->mask & TP_SPF_MAX_VERTICES) && s->ov_max_vertices != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_max_vertices", s->ov_max_vertices, 1, TP_OP_MAX_VERTICES))) {
        return st;
    }
    if ((s->mask & TP_SPF_MARGIN) && s->ov_margin != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_margin", s->ov_margin, 0, TP_OP_OV_MARGIN_MAX))) {
        return st;
    }
    if ((s->mask & TP_SPF_EXTRUDE) && s->ov_extrude != TP_PROJECT_OV_INHERIT &&
        (st = range_i(rej, "ov_extrude", s->ov_extrude, 0, TP_OP_OV_MARGIN_MAX))) {
        return st;
    }
    return TP_STATUS_OK; /* slice9 components are uint16 -- already in range */
}

/* fps + playback checks shared by animation.create / .settings.set. */
static tp_status validate_anim_knobs(bool check_fps, float fps, bool check_pb, int pb, tp_op_reject *rej) {
    if (check_fps && !finite_pos(fps)) {
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "fps", "fps must be positive finite");
    }
    if (check_pb) {
        return range_i(rej, "playback", pb, 0, TP_OP_PLAYBACK_MAX);
    }
    return TP_STATUS_OK;
}

static tp_status validate_frames(char *const *frames, int n, tp_op_reject *rej) {
    if (n < 0) { /* a negative count would loop &frames[-1] in apply -> heap underflow */
        return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "frame_count", "frame_count %d must be >= 0", n);
    }
    for (int i = 0; i < n; i++) {
        if (!frames[i] || frames[i][0] == '\0') {
            return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "frames", "frame %d is empty", i);
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_operation_validate(const tp_project *p, const tp_operation *op, tp_op_reject *rej) {
    tp_op__reject_ok(rej);
    if (!p || !op) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "null project or operation");
    }
    if (tp_op_info_by_kind(op->kind) == NULL) {
        return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not in the catalog", (int)op->kind);
    }

    /* atlas.create is the one op that must NOT already resolve its atlas_id. */
    if (op->kind == TP_OP_ATLAS_CREATE) {
        if (tp_id128_is_nil(op->atlas_id)) {
            return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "atlas_id", "atlas.create needs a real atlas id");
        }
        if (find_atlas(p, op->atlas_id)) {
            return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "atlas_id", "an atlas with that id already exists");
        }
        const char *nm = op->u.atlas_create.name;
        if (!nm || nm[0] == '\0') {
            return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "atlas name must be non-empty");
        }
        if (find_atlas_by_name(p, nm) >= 0) { /* CLI `atlas add` rejects a duplicate name */
            return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "an atlas named '%s' already exists", nm);
        }
        return TP_STATUS_OK;
    }

    /* Every other op addresses an existing parent atlas. */
    const tp_project_atlas *a = find_atlas(p, op->atlas_id);
    if (!a) {
        return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "atlas_id", "no atlas with that id");
    }

    switch (op->kind) {
        case TP_OP_ATLAS_REMOVE: return TP_STATUS_OK;
        case TP_OP_ATLAS_RENAME: {
            const char *nm = op->u.atlas_rename.name;
            if (!nm || nm[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "atlas name must be non-empty");
            }
            /* CLI `atlas rename` rejects a collision with ANOTHER atlas; renaming to the
             * same name (self) is allowed (a no-op). */
            int other = find_atlas_by_name(p, nm);
            if (other >= 0 && &p->atlases[other] != a) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "an atlas named '%s' already exists", nm);
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ATLAS_SETTINGS_SET: return validate_atlas_settings(&op->u.atlas_settings, rej);

        case TP_OP_SOURCE_ADD:
            if (tp_id128_is_nil(op->u.source_add.source_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "source_id", "source.add needs a real source id");
            }
            if (find_source(a, op->u.source_add.source_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "source_id", "a source with that id already exists");
            }
            if (!op->u.source_add.key || op->u.source_add.key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            /* The apply mutator dedupes a matching '/'-normalized path (count unchanged),
             * which would strand the op's source_id. The id-contract ("create a NEW source
             * with THIS id at this path") cannot honor a path that already belongs to another
             * source, so reject the conflict here. (The F2-05 CLI adapter pre-checks/skips to
             * preserve the CLI's silent-dedupe UX.) */
            if (tp_project_atlas_has_source_path(a, op->u.source_add.key)) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "key",
                                     "a source with path '%s' already exists in the atlas", op->u.source_add.key);
            }
            return TP_STATUS_OK;
        case TP_OP_SOURCE_REMOVE:
            return find_source(a, op->u.source_ref.source_id)
                       ? TP_STATUS_OK
                       : tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
        case TP_OP_SOURCE_REPLACE:
            if (!find_source(a, op->u.source_ref.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.source_ref.key || op->u.source_ref.key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "key", "source path must be non-empty");
            }
            return TP_STATUS_OK;

        case TP_OP_SPRITE_OVERRIDE_SET:
            /* A NIL source_id is a PENDING (name-keyed) override -- an override added by
             * export-key before any source scan (tp_project_sprite's documented pending
             * state, decision 0010 §2). The CLI `sprite set` adds overrides this way on a
             * source-less atlas; apply keys by the export-key bridge and leaves the record
             * pending. A NON-nil source_id must still resolve to a real source. */
            if (!tp_id128_is_nil(op->u.sprite_set.source_id) && !find_source(a, op->u.sprite_set.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_set.src_key || op->u.sprite_set.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return validate_sprite_set(&op->u.sprite_set, rej);
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            /* NIL source_id = pending (name-keyed) override (see .SET above). */
            if (!tp_id128_is_nil(op->u.sprite_clear.source_id) && !find_source(a, op->u.sprite_clear.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_clear.src_key || op->u.sprite_clear.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            if (op->u.sprite_clear.mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "fields", "sprite.override.clear names no field");
            }
            return TP_STATUS_OK;
        case TP_OP_SPRITE_NAME_SET:
            /* NIL source_id = pending (name-keyed) override (see .SET above). */
            if (!tp_id128_is_nil(op->u.sprite_name.source_id) && !find_source(a, op->u.sprite_name.source_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "source_id", "no source with that id in the atlas");
            }
            if (!op->u.sprite_name.src_key || op->u.sprite_name.src_key[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "src_key", "sprite key must be non-empty");
            }
            return TP_STATUS_OK; /* name == NULL clears the rename (valid) */

        case TP_OP_ANIMATION_CREATE: {
            const tp_op_anim_create *c = &op->u.anim_create;
            tp_status st = TP_STATUS_OK;
            if (tp_id128_is_nil(c->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "anim_id", "animation.create needs a real anim id");
            }
            if (find_anim(a, c->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "anim_id", "an animation with that id already exists");
            }
            if (!c->name || c->name[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "animation name must be non-empty");
            }
            if ((st = validate_anim_knobs(true, c->fps, true, c->playback, rej))) {
                return st;
            }
            return validate_frames(c->frames, c->frame_count, rej);
        }
        case TP_OP_ANIMATION_REMOVE:
            return find_anim(a, op->u.anim_ref.anim_id)
                       ? TP_STATUS_OK
                       : tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
        case TP_OP_ANIMATION_RENAME: {
            const tp_op_anim_rename *r = &op->u.anim_rename;
            const tp_project_anim *an = find_anim(a, r->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (!r->name || r->name[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name", "animation name must be non-empty");
            }
            /* Reject a collision with ANOTHER animation in the atlas; renaming to the same name
             * (self) is allowed (a no-op) -- mirrors atlas.rename's uniqueness check. This is the
             * policy the pre-H/P1-2 GUI enforced client-side (decision 0015); it now lives here so
             * every frontend reuses the one structured reject (invalid_argument + field). */
            for (int i = 0; i < a->animation_count; i++) {
                const tp_project_anim *other = &a->animations[i];
                if (other != an && other->name && strcmp(other->name, r->name) == 0) {
                    return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "name",
                                         "an animation named '%s' already exists", r->name);
                }
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_SETTINGS_SET: {
            const tp_op_anim_settings *s = &op->u.anim_settings;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "animation.settings.set names no field");
            }
            return validate_anim_knobs((s->mask & TP_ANF_FPS) != 0, s->fps, (s->mask & TP_ANF_PLAYBACK) != 0,
                                       s->playback, rej);
        }
        case TP_OP_ANIMATION_FRAMES_SET: {
            const tp_op_anim_frames_set *s = &op->u.anim_frames_set;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            return validate_frames(s->frames, s->frame_count, rej);
        }
        case TP_OP_ANIMATION_FRAME_ADD: {
            const tp_op_anim_frame_add *s = &op->u.anim_frame_add;
            if (!find_anim(a, s->anim_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (!s->frame || s->frame[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "frame", "frame reference must be non-empty");
            }
            if (s->index < -1) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_RANGE, "index", "index must be >= -1 (-1 = append)");
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAME_REMOVE: {
            const tp_op_anim_frame_rm *s = &op->u.anim_frame_rm;
            const tp_project_anim *an = find_anim(a, s->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->index < 0 || s->index >= an->frame_count) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_BOUNDS, "index", "frame index %d out of [0..%d)", s->index,
                                     an->frame_count);
            }
            return TP_STATUS_OK;
        }
        case TP_OP_ANIMATION_FRAME_MOVE: {
            const tp_op_anim_frame_move *s = &op->u.anim_frame_move;
            const tp_project_anim *an = find_anim(a, s->anim_id);
            if (!an) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "anim_id", "no animation with that id in the atlas");
            }
            if (s->from_index < 0 || s->from_index >= an->frame_count) {
                return tp_op__reject(rej, TP_STATUS_OUT_OF_BOUNDS, "from_index", "from_index %d out of [0..%d)",
                                     s->from_index, an->frame_count);
            }
            /* to_index is intentionally UNBOUNDED: the CLI `anim move-frame` clamps a large
             * (or negative) destination to the last/first slot; apply clamps identically. */
            return TP_STATUS_OK;
        }

        case TP_OP_TARGET_CREATE: {
            const tp_op_target_create *t = &op->u.target_create;
            if (tp_id128_is_nil(t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "target_id", "target.create needs a real target id");
            }
            if (find_target(a, t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "target_id", "a target with that id already exists");
            }
            if (!t->exporter_id || !tp_exporter_find(t->exporter_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "exporter_id", "unknown exporter id '%s'",
                                     t->exporter_id ? t->exporter_id : "");
            }
            if (!t->out_path || t->out_path[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "out_path", "out_path must be non-empty");
            }
            return TP_STATUS_OK;
        }
        case TP_OP_TARGET_REMOVE:
            return find_target(a, op->u.target_ref.target_id)
                       ? TP_STATUS_OK
                       : tp_op__reject(rej, TP_STATUS_NOT_FOUND, "target_id", "no target with that id in the atlas");
        case TP_OP_TARGET_SET: {
            const tp_op_target_set *t = &op->u.target_set;
            if (!find_target(a, t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "target_id", "no target with that id in the atlas");
            }
            if (!t->exporter_id || !tp_exporter_find(t->exporter_id)) {
                return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "exporter_id", "unknown exporter id '%s'",
                                     t->exporter_id ? t->exporter_id : "");
            }
            if (!t->out_path || t->out_path[0] == '\0') {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "out_path", "out_path must be non-empty");
            }
            return TP_STATUS_OK;
        }

        case TP_OP_INVALID:
        case TP_OP_ATLAS_CREATE: /* handled above */
        case TP_OP_KIND_COUNT: break;
    }
    return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not applicable", (int)op->kind);
}
