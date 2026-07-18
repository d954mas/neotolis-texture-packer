#ifndef TP_PROJECT_MODEL_INTERNAL_H
#define TP_PROJECT_MODEL_INTERNAL_H

#include "tp_core/tp_project.h"

/* Direct materialization primitives shared only by the model and loader. */
tp_project *tp_project_alloc_empty(void);
tp_status atlas_push_source(tp_project_atlas *atlas, const char *path,
                            tp_source_kind kind, tp_id128 id);
tp_project_sprite *sprite_push_default(tp_project_atlas *atlas);

#endif /* TP_PROJECT_MODEL_INTERNAL_H */
