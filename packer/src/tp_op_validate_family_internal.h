#ifndef TP_OP_VALIDATE_FAMILY_INTERNAL_H
#define TP_OP_VALIDATE_FAMILY_INTERNAL_H

#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"

const tp_project_source *tp_op_validate_find_source(
    const tp_project_atlas *atlas, tp_id128 source_id);

tp_status tp_op_validate_atlas_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject);
tp_status tp_op_validate_source_sprite_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject);
tp_status tp_op_validate_animation_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject);
tp_status tp_op_validate_target_family(
    const tp_project *project, const tp_project_atlas *atlas,
    const tp_operation *operation, tp_op_reject *reject);

#endif /* TP_OP_VALIDATE_FAMILY_INTERNAL_H */
