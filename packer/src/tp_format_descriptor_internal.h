#ifndef TP_CORE_SRC_TP_FORMAT_DESCRIPTOR_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_DESCRIPTOR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"

/* Deep-owned API-v1 descriptor produced by the hostile-byte parser. */
typedef struct tp_format_owned_descriptor tp_format_owned_descriptor;

typedef enum tp_format_descriptor_outcome {
    TP_FORMAT_DESCRIPTOR_ADMITTED = 1,
    TP_FORMAT_DESCRIPTOR_REJECTED = 2,
} tp_format_descriptor_outcome;

typedef struct tp_format_descriptor_parse_result {
    tp_format_descriptor_outcome outcome;
    tp_format_owned_descriptor *owned_descriptor;
    tp_format_diagnostic_code rejection_code;
    uint32_t line;
    uint32_t column;
    char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
} tp_format_descriptor_parse_result;

/* Malformed package input is a normal REJECTED result.  Only invalid caller
 * arguments, allocation failure, or an internal failure return non-OK. */
tp_status tp_format_descriptor_v1_parse(
    const unsigned char *bytes, size_t byte_count,
    tp_format_descriptor_parse_result *out, tp_error *error);

const tp_format_descriptor *tp_format_owned_descriptor_view(
    const tp_format_owned_descriptor *descriptor);
void tp_format_owned_descriptor_destroy(
    tp_format_owned_descriptor *descriptor);

bool tp_format_id_is_runtime_token(const char *text);
bool tp_format_logical_id_is_token(const char *text);
bool tp_format_package_name_is_portable(const char *text);

#endif /* TP_CORE_SRC_TP_FORMAT_DESCRIPTOR_INTERNAL_H */
