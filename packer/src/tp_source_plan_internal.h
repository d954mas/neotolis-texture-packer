#ifndef TP_SOURCE_PLAN_INTERNAL_H
#define TP_SOURCE_PLAN_INTERNAL_H

#include <stdbool.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"

typedef struct tp_source_path_identity {
    char absolute[TP_IDENTITY_PATH_MAX];
    char canonical[TP_IDENTITY_PATH_MAX];
    bool has_canonical;
} tp_source_path_identity;

tp_status tp_source_path_identity_from_input(const char *input,
                                             tp_source_path_identity *out,
                                             tp_error *err);
tp_status tp_source_path_identity_from_stored(const tp_project *project,
                                              const char *path,
                                              bool resolve_canonical,
                                              tp_source_path_identity *out,
                                              tp_error *err);
bool tp_source_path_identity_equal_text(const char *left_absolute,
                                        const char *left_canonical,
                                        const char *right_absolute,
                                        const char *right_canonical);

#endif /* TP_SOURCE_PLAN_INTERNAL_H */
