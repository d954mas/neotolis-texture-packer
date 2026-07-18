#ifndef TP_PROJECT_PARSE_INTERNAL_H
#define TP_PROJECT_PARSE_INTERNAL_H

#include "tp_core/tp_error.h"
#include "tp_core/tp_project.h"
#include "tp_project_internal.h"

extern const tp_project_json_limits TP_PROJECT_JSON_LIMITS;

tp_status tp_project_json_admit(const char *text, size_t len,
                                const tp_project_json_limits *limits,
                                tp_error *err);

#endif /* TP_PROJECT_PARSE_INTERNAL_H */
