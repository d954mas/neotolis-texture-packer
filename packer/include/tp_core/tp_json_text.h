#ifndef TP_CORE_TP_JSON_TEXT_H
#define TP_CORE_TP_JSON_TEXT_H

#include <stddef.h>
#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shared raw Unicode/NUL admission before a JSON parser exposes C strings.
 * This does not parse JSON syntax or impose a document-specific schema. */
tp_status tp_json_reject_c_string_ambiguity(
    const char *json, size_t json_len, tp_status rejection_status,
    const char *context, tp_error *error);

#ifdef __cplusplus
}
#endif

#endif
