#ifndef TP_CORE_SRC_TP_PROJECT_IDENTITY_INTERNAL_H
#define TP_CORE_SRC_TP_PROJECT_IDENTITY_INTERNAL_H

#include "tp_core/tp_project.h"

tp_status tp_project_assign_missing_ids(tp_project *project, const tp_rng *rng,
                                        tp_error *error);
tp_status tp_project_validate_canonical(const tp_project *project,
                                        tp_error *error);
tp_status tp_project_validate_schema_shape(const tp_project *project,
                                           tp_error *error);

/* Structural IDs share one project-wide namespace across every entity kind and
 * atlas. Create-operation validation uses this before mutating a transaction
 * candidate so cross-kind/cross-atlas duplicates are rejected at the offending
 * operation rather than leaking into the live model until Save. */
bool tp_project_has_structural_id(const tp_project *project, tp_id128 id);

/* Exact saved-model domain for fields that are narrowed into the builder's
 * uint8 sprite descriptor.  Used both by canonical-project validation and at
 * the conversion boundary so no alternate in-memory ingress can wrap/coerce. */
tp_status tp_project_validate_sprite_pack_overrides(
    const tp_project_sprite *sprite, tp_error *error);

#endif
