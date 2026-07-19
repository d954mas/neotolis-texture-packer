#ifndef TP_CORE_SRC_TP_MODEL_SEAM_H
#define TP_CORE_SRC_TP_MODEL_SEAM_H

#include "tp_core/tp_project.h"
#include "tp_core/tp_transaction.h"

typedef struct tp_project_generation tp_project_generation;

/* The live project (immutable borrowed view; valid until the next model replace
 * or destruction). Core-only: clients read owned session snapshots. */
const tp_project *tp_model_project(const tp_model *model);

/* Lazily installs a shared owner for the current immutable project generation
 * and returns one retained reference. The model remains unchanged on OOM. */
tp_status tp_model__retain_project_generation(
    tp_model *model, tp_project_generation **out, tp_error *error);

/* Model-owned project replacement after a separately staged project persisted.
 * Takes ownership of `project`; session stays the persistence/orchestration caller. */
void tp_model__adopt_project(tp_model *model, tp_project *project);

bool tp_model__recovery_degraded(const tp_model *model);
tp_status tp_model__recovery_status(const tp_model *model);

#endif /* TP_CORE_SRC_TP_MODEL_SEAM_H */
