#ifndef TP_CORE_SRC_TP_MODEL_SEAM_H
#define TP_CORE_SRC_TP_MODEL_SEAM_H

#include "tp_core/tp_project.h"
#include "tp_core/tp_transaction.h"

/* Model-owned project replacement after a separately staged project persisted.
 * Takes ownership of `project`; session stays the persistence/orchestration caller. */
void tp_model__adopt_project(tp_model *model, tp_project *project);
tp_status tp_model__migrate_sprite_refs(tp_model *model, tp_error *error);

#endif /* TP_CORE_SRC_TP_MODEL_SEAM_H */
