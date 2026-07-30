#ifndef TP_CORE_SRC_TP_PROJECT_GENERATION_INTERNAL_H
#define TP_CORE_SRC_TP_PROJECT_GENERATION_INTERNAL_H

#include "tp_core/tp_project.h"

typedef struct tp_project_generation tp_project_generation;

/* Takes ownership of project on success. The caller retains ownership on OOM. */
tp_project_generation *tp_project_generation_create_owned(tp_project *project);
void tp_project_generation_retain(tp_project_generation *generation);
void tp_project_generation_release(tp_project_generation *generation);
const tp_project *tp_project_generation_project(
    const tp_project_generation *generation);

#ifdef TP_ENABLE_TEST_SEAMS
/* One-shot, thread-local allocation fault seam used by session lifetime tests.
 * Compiled out of every build that does not define TP_ENABLE_TEST_SEAMS, so a
 * consumer must recompile tp_project_generation.c with the define. */
void tp_project_generation__test_fail_next_allocation(void);
#endif

#endif /* TP_CORE_SRC_TP_PROJECT_GENERATION_INTERNAL_H */
