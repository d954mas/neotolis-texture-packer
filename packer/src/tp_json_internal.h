#ifndef TP_JSON_INTERNAL_H
#define TP_JSON_INTERNAL_H

#include <stddef.h>

#include "cJSON.h"
#include "tp_core/tp_error.h"

/* Reject byte sequences and JSON spellings that cannot be represented
 * faithfully by the core's UTF-8 C-string model. This is a strict, raw,
 * length-aware UTF-8/NUL admission check and must run before values are
 * consumed through cJSON's NUL-terminated strings. */
tp_status tp_json_reject_c_string_ambiguity(
    const char *json, size_t json_len, tp_status rejection_status,
    const char *context, tp_error *error);

/* cJSON accepts duplicate object members. Mutation/project contracts do not:
 * every decoded object must have one unambiguous value per key. */
tp_status tp_json_reject_duplicate_keys(
    const cJSON *root, tp_status rejection_status, const char *context,
    tp_error *error);

#endif /* TP_JSON_INTERNAL_H */
