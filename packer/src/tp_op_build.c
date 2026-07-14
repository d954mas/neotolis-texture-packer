/*
 * F2-01 task 3: shared selector -> operation builders (the CLI/MCP convenience
 * layer). A human selector + already-typed args -> a typed, ID-only operation, or a
 * structured ambiguity/not-found. The target is resolved through the production
 * tp_selector (F1-03) so every frontend selects the same entity without guessing --
 * an ambiguous selector returns a candidate list, never a silent first-match.
 *
 * This is a representative, cohesive set (create / rename / remove / set across
 * structural kinds). The remaining per-verb builders are the same mechanical shape:
 * resolve the target id(s), then fill the typed payload from validated args. Kept
 * thin on purpose -- the resolution seam is the reusable part.
 */

#include "tp_core/tp_operation.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_selector.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_strutil.h" /* shared tp_strdup (one core definition, fix [9]) */

/* Resolve `selector` to one entity of `want` kind that OWNED BY atlas `atlas_index`. A
 * project-wide match in a DIFFERENT atlas is treated as not-found -- the atlas scopes the
 * search, so a builder never pairs atlas A with a sub-entity that actually lives in B
 * (which validate/apply would then reject as NOT_FOUND). */
static tp_status resolve_in_atlas(const tp_project *p, int atlas_index, tp_selector_kind want, const char *selector,
                                  tp_selector_result *out, tp_selector_candidates *cand, tp_error *err) {
    tp_status st = tp_op_resolve_target(p, NULL, -1, want, selector, out, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    if (out->atlas_index != atlas_index) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "selector '%s' resolves to a %s in a different atlas", selector,
                            tp_selector_kind_token(want));
    }
    return TP_STATUS_OK;
}

tp_status tp_op_resolve_target(const tp_project *p, const struct tp_sprite_index *sprites, int sprite_atlas_index,
                               tp_selector_kind want, const char *selector, tp_selector_result *out,
                               tp_selector_candidates *cand, tp_error *err) {
    if (!p || !selector || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null project/selector/out");
    }
    tp_selector_result res;
    tp_status st = tp_selector_resolve(p, selector, sprites, sprite_atlas_index, &res, cand, err);
    if (st != TP_STATUS_OK) {
        return st; /* NOT_FOUND / AMBIGUOUS_SELECTOR / INVALID_ARGUMENT already set */
    }
    if (res.kind != want) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "selector '%s' resolved to a %s, expected a %s", selector,
                            tp_selector_kind_token(res.kind), tp_selector_kind_token(want));
    }
    *out = res;
    return TP_STATUS_OK;
}

tp_status tp_op_build_atlas_create(tp_id128 new_id, const char *name, tp_operation *out) {
    if (!out || !name) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof *out);
    out->kind = TP_OP_ATLAS_CREATE;
    out->atlas_id = new_id;
    out->u.atlas_create.name = tp_strdup(name);
    return out->u.atlas_create.name ? TP_STATUS_OK : TP_STATUS_OOM;
}

tp_status tp_op_build_atlas_rename(const tp_project *p, const char *atlas_sel, const char *new_name, tp_operation *out,
                                   tp_selector_candidates *cand, tp_error *err) {
    if (!out || !new_name) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null out/new_name");
    }
    tp_selector_result res;
    tp_status st = tp_op_resolve_target(p, NULL, -1, TP_SEL_ATLAS, atlas_sel, &res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    out->kind = TP_OP_ATLAS_RENAME;
    out->atlas_id = res.id;
    out->u.atlas_rename.name = tp_strdup(new_name);
    return out->u.atlas_rename.name ? TP_STATUS_OK : TP_STATUS_OOM;
}

tp_status tp_op_build_atlas_remove(const tp_project *p, const char *atlas_sel, tp_operation *out,
                                   tp_selector_candidates *cand, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null out");
    }
    tp_selector_result res;
    tp_status st = tp_op_resolve_target(p, NULL, -1, TP_SEL_ATLAS, atlas_sel, &res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    out->kind = TP_OP_ATLAS_REMOVE;
    out->atlas_id = res.id;
    return TP_STATUS_OK;
}

tp_status tp_op_build_target_set(const tp_project *p, const char *atlas_sel, const char *target_sel,
                                 const char *exporter_id, const char *out_path, bool enabled, tp_operation *out,
                                 tp_selector_candidates *cand, tp_error *err) {
    if (!out || !exporter_id || !out_path) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null out/exporter_id/out_path");
    }
    tp_selector_result atlas_res;
    tp_status st = tp_op_resolve_target(p, NULL, -1, TP_SEL_ATLAS, atlas_sel, &atlas_res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_selector_result tgt_res; /* scope the target to the resolved atlas (fix [5]) */
    st = resolve_in_atlas(p, atlas_res.atlas_index, TP_SEL_TARGET, target_sel, &tgt_res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    out->kind = TP_OP_TARGET_SET;
    out->atlas_id = atlas_res.id;
    out->u.target_set.target_id = tgt_res.id;
    out->u.target_set.mask = TP_TF_ALL; /* selector builder = full replace (all three fields) */
    out->u.target_set.enabled = enabled;
    out->u.target_set.exporter_id = tp_strdup(exporter_id);
    out->u.target_set.out_path = tp_strdup(out_path);
    if (!out->u.target_set.exporter_id || !out->u.target_set.out_path) {
        tp_operation_free(out);
        return TP_STATUS_OOM;
    }
    return TP_STATUS_OK;
}

tp_status tp_op_build_anim_remove(const tp_project *p, const char *atlas_sel, const char *anim_sel, tp_operation *out,
                                  tp_selector_candidates *cand, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null out");
    }
    tp_selector_result atlas_res;
    tp_status st = tp_op_resolve_target(p, NULL, -1, TP_SEL_ATLAS, atlas_sel, &atlas_res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    tp_selector_result anim_res; /* scope the animation to the resolved atlas (fix [5]) */
    st = resolve_in_atlas(p, atlas_res.atlas_index, TP_SEL_ANIM, anim_sel, &anim_res, cand, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    memset(out, 0, sizeof *out);
    out->kind = TP_OP_ANIMATION_REMOVE;
    out->atlas_id = atlas_res.id;
    out->u.anim_ref.anim_id = anim_res.id;
    return TP_STATUS_OK;
}
