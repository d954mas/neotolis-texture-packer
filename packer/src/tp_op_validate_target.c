#include "tp_op_validate_family_internal.h"

#include "tp_core/tp_export.h"
#include "tp_op_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_mutation_internal.h"

static const tp_project_target *find_target(const tp_project_atlas *a, tp_id128 id) {
    return tp_project_atlas_find_target_by_id((tp_project_atlas *)a, id);
}

static tp_status validate_exporter_id(const char *value,
                                      tp_op_reject *reject) {
    tp_error error = {0};
    const tp_status status = tp_exporter_id_validate(value, &error);
    return status == TP_STATUS_OK
               ? TP_STATUS_OK
               : tp_op__reject(reject, status, "exporter_id", "%s",
                               error.msg);
}

/* Validate every string that the active operation can persist before path
 * canonicalization, mutation, or integer/string encoding. Unmasked payload
 * storage is deliberately ignored: field-presence means it is not data. */

tp_status tp_op_validate_target_family(
    const tp_project *p, const tp_project_atlas *a,
    const tp_operation *op, tp_op_reject *rej) {
    switch (op->kind) {
        case TP_OP_TARGET_CREATE: {
            const tp_op_target_create *t = &op->u.target_create;
            if (tp_id128_is_nil(t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_ID_MALFORMED, "target_id", "target.create needs a real target id");
            }
            if (tp_project_has_structural_id(p, t->target_id)) {
                return tp_op__reject(rej, TP_STATUS_DUPLICATE_ID, "target_id",
                                     "that structural id already belongs to a project entity");
            }
            tp_status exporter_status =
                validate_exporter_id(t->exporter_id, rej);
            if (exporter_status != TP_STATUS_OK) {
                return exporter_status;
            }
            if (!tp_exporter_find(t->exporter_id)) {
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
            /* Masked set (mirrors atlas.settings.set): validate a field ONLY when its mask bit
             * is set. An all-zero mask names no field -> reject (matches atlas/anim settings). */
            if (t->mask == 0) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "", "target.set names no field");
            }
            if ((t->mask & ~(uint32_t)TP_TF_ALL) != 0U) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "",
                                     "target.set contains unknown field bits");
            }
            if (t->mask & TP_TF_EXPORTER) {
                tp_status exporter_status =
                    validate_exporter_id(t->exporter_id, rej);
                if (exporter_status != TP_STATUS_OK) {
                    return exporter_status;
                }
                if (!tp_exporter_find(t->exporter_id)) {
                    return tp_op__reject(
                        rej, TP_STATUS_NOT_FOUND, "exporter_id",
                        "unknown exporter id '%s'", t->exporter_id);
                }
            }
            if ((t->mask & TP_TF_OUT_PATH) && (!t->out_path || t->out_path[0] == '\0')) {
                return tp_op__reject(rej, TP_STATUS_INVALID_ARGUMENT, "out_path", "out_path must be non-empty");
            }
            return TP_STATUS_OK;
        }
        default:
            return tp_op__reject(rej, TP_STATUS_UNKNOWN_OP, "op",
                                 "operation is not a target family member");
    }
}
