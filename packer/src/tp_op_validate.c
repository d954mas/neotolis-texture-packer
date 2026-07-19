/*
 * Operation payload + reference validation moved INTO core.
 * The range / name / exporter-id / reference rules that were duplicated in
 * apps/cli/cli_mutate.c (CLI-specific) and the apps/gui wrappers now live here,
 * once, producing a STRUCTURED status id + offending field + context (not prose).
 * Pure: never mutates `p`. Bounds-checked, UB-clean on arbitrary payloads.
 */

#include "tp_core/tp_operation.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_op_internal.h"
#include "tp_op_validate_family_internal.h"
#include "tp_source_path_text_internal.h"
#include "tp_source_plan_internal.h"
#include "tp_utf8_internal.h"

static const tp_project_atlas *find_atlas(const tp_project *p, tp_id128 id) {
    return tp_project_atlas_by_id(p, id);
}
const tp_project_source *tp_op_validate_find_source(const tp_project_atlas *a,
                                                    tp_id128 id) {
    return tp_project_atlas_source_by_id(a, id);
}
static tp_status validate_persisted_utf8(const char *value, const char *field,
                                         tp_op_reject *reject) {
    if (!value) {
        return TP_STATUS_OK; /* Required/nullable semantics stay with each op. */
    }
    tp_error error = {{0}};
    const tp_status status = tp_utf8_validate_c_string(
        value, TP_STATUS_INVALID_UTF8, field, &error);
    return status == TP_STATUS_OK
               ? TP_STATUS_OK
               : tp_op__reject(reject, TP_STATUS_INVALID_UTF8, field, "%s",
                               error.msg);
}

static tp_status validate_source_path_text(const char *value,
                                           tp_op_reject *reject) {
    if (!value || value[0] == '\0') {
        return TP_STATUS_OK; /* Required semantics stay with the op family. */
    }
    const tp_status status = tp_source_path_text_admit(value);
    if (status == TP_STATUS_OK) {
        return TP_STATUS_OK;
    }
    if (status == TP_STATUS_INVALID_UTF8) {
        return validate_persisted_utf8(value, "key", reject);
    }
    return tp_op__reject(reject, status, "key",
                         "source path exceeds the supported limit");
}

static tp_status validate_operation_utf8(const tp_operation *operation,
                                         tp_op_reject *reject) {
    tp_status status = TP_STATUS_OK;
#define CHECK_UTF8(value, field)                                                \
    do {                                                                        \
        status = validate_persisted_utf8((value), (field), reject);             \
        if (status != TP_STATUS_OK) {                                            \
            return status;                                                      \
        }                                                                       \
    } while (0)
    switch (operation->kind) {
        case TP_OP_ATLAS_CREATE:
            CHECK_UTF8(operation->u.atlas_create.name, "name");
            break;
        case TP_OP_ATLAS_RENAME:
            CHECK_UTF8(operation->u.atlas_rename.name, "name");
            break;
        case TP_OP_SOURCE_ADD:
            status = validate_source_path_text(operation->u.source_add.key,
                                               reject);
            if (status != TP_STATUS_OK) {
                return status;
            }
            break;
        case TP_OP_SOURCE_REPLACE:
            status = validate_source_path_text(operation->u.source_ref.key,
                                               reject);
            if (status != TP_STATUS_OK) {
                return status;
            }
            break;
        case TP_OP_SPRITE_OVERRIDE_SET:
            CHECK_UTF8(operation->u.sprite_set.src_key, "src_key");
            break;
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
            CHECK_UTF8(operation->u.sprite_clear.src_key, "src_key");
            break;
        case TP_OP_SPRITE_NAME_SET:
            CHECK_UTF8(operation->u.sprite_name.src_key, "src_key");
            CHECK_UTF8(operation->u.sprite_name.name, "name");
            break;
        case TP_OP_ANIMATION_CREATE:
            CHECK_UTF8(operation->u.anim_create.name, "name");
            if (operation->u.anim_create.frames &&
                operation->u.anim_create.frame_count > 0) {
                for (int i = 0; i < operation->u.anim_create.frame_count; i++) {
                    CHECK_UTF8(operation->u.anim_create.frames[i].src_key,
                               "frames");
                }
            }
            break;
        case TP_OP_ANIMATION_RENAME:
            CHECK_UTF8(operation->u.anim_rename.name, "name");
            break;
        case TP_OP_ANIMATION_FRAMES_SET:
            if (operation->u.anim_frames_set.frames &&
                operation->u.anim_frames_set.frame_count > 0) {
                for (int i = 0;
                     i < operation->u.anim_frames_set.frame_count; i++) {
                    CHECK_UTF8(
                        operation->u.anim_frames_set.frames[i].src_key,
                        "frames");
                }
            }
            break;
        case TP_OP_ANIMATION_FRAME_ADD:
            CHECK_UTF8(operation->u.anim_frame_add.frame.src_key, "frame");
            break;
        case TP_OP_TARGET_CREATE:
            CHECK_UTF8(operation->u.target_create.exporter_id, "exporter_id");
            CHECK_UTF8(operation->u.target_create.out_path, "out_path");
            break;
        case TP_OP_TARGET_SET:
            if (operation->u.target_set.mask & TP_TF_EXPORTER) {
                CHECK_UTF8(operation->u.target_set.exporter_id, "exporter_id");
            }
            if (operation->u.target_set.mask & TP_TF_OUT_PATH) {
                CHECK_UTF8(operation->u.target_set.out_path, "out_path");
            }
            break;
        case TP_OP_INVALID:
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_SETTINGS_SET:
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_ANIMATION_REMOVE:
        case TP_OP_ANIMATION_SETTINGS_SET:
        case TP_OP_ANIMATION_FRAME_REMOVE:
        case TP_OP_ANIMATION_FRAME_MOVE:
        case TP_OP_TARGET_REMOVE:
        case TP_OP_KIND_COUNT:
            break;
    }
#undef CHECK_UTF8
    return TP_STATUS_OK;
}

tp_status tp_op__canonical_view(const tp_project *project,
                                const tp_operation *operation,
                                tp_operation *view, char *path_buf,
                                size_t path_buf_size) {
    if (!project || !operation || !view || !path_buf || path_buf_size == 0U) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    *view = *operation;
    char **key = NULL;
    if (operation->kind == TP_OP_SOURCE_ADD) {
        key = &view->u.source_add.key;
    } else if (operation->kind == TP_OP_SOURCE_REPLACE) {
        key = &view->u.source_ref.key;
    }
    if (!key || !*key || (*key)[0] == '\0' || !project->project_dir) {
        return TP_STATUS_OK;
    }
    tp_status status = tp_project_resolve_path(project, *key, path_buf,
                                               path_buf_size);
    if (status == TP_STATUS_OK) {
        *key = path_buf;
    }
    return status;
}

tp_status tp_operation_validate(const tp_project *p, const tp_operation *op, tp_op_reject *rej) {
    tp_op__reject_ok(rej);
    if (!p || !op) {
        return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "null project or operation");
    }
    if (tp_op_info_by_kind(op->kind) == NULL) {
        return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not in the catalog", (int)op->kind);
    }
    tp_status utf8_status = validate_operation_utf8(op, rej);
    if (utf8_status != TP_STATUS_OK) {
        return utf8_status;
    }
    tp_operation canonical;
    char canonical_path[TP_IDENTITY_PATH_MAX];
    tp_status canonical_status = tp_op__canonical_view(
        p, op, &canonical, canonical_path, sizeof canonical_path);
    if (canonical_status != TP_STATUS_OK) {
        return tp_op__reject(rej, canonical_status, "key",
                             "source path cannot be resolved against the project");
    }
    op = &canonical;
    /* atlas.create is the one op that must NOT already resolve its atlas_id. */
    if (op->kind == TP_OP_ATLAS_CREATE) {
        return tp_op_validate_atlas_family(p, NULL, op, rej);
    }

    /* Every other op addresses an existing parent atlas. */
    const tp_project_atlas *a = find_atlas(p, op->atlas_id);
    if (!a) {
        return tp_op__reject(rej, TP_STATUS_NOT_FOUND, "atlas_id", "no atlas with that id");
    }

    switch (op->kind) {
        case TP_OP_ATLAS_REMOVE:
        case TP_OP_ATLAS_RENAME:
        case TP_OP_ATLAS_SETTINGS_SET:
            return tp_op_validate_atlas_family(p, a, op, rej);

        case TP_OP_SOURCE_ADD:
        case TP_OP_SOURCE_REMOVE:
        case TP_OP_SOURCE_REPLACE:
        case TP_OP_SPRITE_OVERRIDE_SET:
        case TP_OP_SPRITE_OVERRIDE_CLEAR:
        case TP_OP_SPRITE_NAME_SET:
            return tp_op_validate_source_sprite_family(p, a, op, rej);

        case TP_OP_ANIMATION_CREATE:
        case TP_OP_ANIMATION_REMOVE:
        case TP_OP_ANIMATION_RENAME:
        case TP_OP_ANIMATION_SETTINGS_SET:
        case TP_OP_ANIMATION_FRAMES_SET:
        case TP_OP_ANIMATION_FRAME_ADD:
        case TP_OP_ANIMATION_FRAME_REMOVE:
        case TP_OP_ANIMATION_FRAME_MOVE:
            return tp_op_validate_animation_family(p, a, op, rej);

        case TP_OP_TARGET_CREATE:
        case TP_OP_TARGET_REMOVE:
        case TP_OP_TARGET_SET:
            return tp_op_validate_target_family(p, a, op, rej);

        case TP_OP_INVALID:
        case TP_OP_ATLAS_CREATE: /* handled above */
        case TP_OP_KIND_COUNT: break;
    }
    return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op", "operation kind %d is not applicable", (int)op->kind);
}
