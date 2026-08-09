#ifndef TP_CORE_SRC_TP_FORMAT_PACKAGE_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_PACKAGE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"

/* Exact API-v1 package byte contract shared by discovery, binding validation,
 * and the isolated Lua host. Callers own status/diagnostic translation. */
tp_format_diagnostic_code tp_format_package_v1_source_admission_internal(
    const unsigned char *bytes, size_t byte_count, char *message,
    size_t message_capacity);

void tp_format_package_fingerprint_internal(
    uint32_t api_version, const unsigned char *descriptor_bytes,
    size_t descriptor_byte_count, const unsigned char *source_bytes,
    size_t source_byte_count, char out[33]);

/* Frozen D4 vocabulary shared by descriptor admission and Lua projection. */
int tp_format_transform_token_value_internal(const char *token);
const char *tp_format_transform_token_internal(uint8_t value);

#endif /* TP_CORE_SRC_TP_FORMAT_PACKAGE_INTERNAL_H */
