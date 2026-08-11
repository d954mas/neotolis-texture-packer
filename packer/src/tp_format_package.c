#include "tp_format_package_internal.h"

#include <stdio.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_core/tp_id.h"
#include "tp_hex.h"
#include "tp_utf8_internal.h"

static const char *const g_transform_tokens[] = {
    "identity",       "flip_h",        "flip_v",       "rotate_180",
    "transpose",      "rotate_90_cw",  "rotate_90_ccw", "anti_transpose",
};

static void write_u32le(unsigned char out[4], uint32_t value) {
    out[0] = (unsigned char)value;
    out[1] = (unsigned char)(value >> 8U);
    out[2] = (unsigned char)(value >> 16U);
    out[3] = (unsigned char)(value >> 24U);
}

static void write_u64le(unsigned char out[8], uint64_t value) {
    for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        out[shift / 8U] = (unsigned char)(value >> shift);
    }
}

tp_format_diagnostic_code tp_format_package_v1_source_admission_internal(
    const unsigned char *bytes, size_t byte_count, char *message,
    size_t message_capacity) {
    NT_ASSERT(bytes || byte_count == 0U);
    NT_ASSERT(message && message_capacity > 0U);
    if (!bytes) {
        bytes = (const unsigned char *)"";
    }
    if (byte_count >= 3U && bytes[0] == 0xefU && bytes[1] == 0xbbU &&
        bytes[2] == 0xbfU) {
        (void)snprintf(message, message_capacity,
                       "export.lua must not contain a UTF-8 BOM");
        return TP_FORMAT_DIAGNOSTIC_SOURCE_INVALID_UTF8;
    }
    if ((byte_count > 0U && bytes[0] == 0x1bU) ||
        memchr(bytes, 0, byte_count)) {
        (void)snprintf(message, message_capacity,
                       "export.lua must be text source without binary chunks or NUL");
        return TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY;
    }
    tp_error validation = {{0}};
    if (tp_utf8_validate_bytes((const char *)bytes, byte_count,
                               TP_STATUS_INVALID_UTF8, "export.lua",
                               &validation) != TP_STATUS_OK) {
        (void)snprintf(message, message_capacity, "%s",
                       validation.msg[0] ? validation.msg
                                         : "export.lua is not strict UTF-8");
        tp_error_trim_partial_utf8(message);
        return TP_FORMAT_DIAGNOSTIC_SOURCE_INVALID_UTF8;
    }
    message[0] = '\0';
    return (tp_format_diagnostic_code)0;
}

void tp_format_package_fingerprint_internal(
    uint32_t api_version, const unsigned char *descriptor_bytes,
    size_t descriptor_byte_count, const unsigned char *source_bytes,
    size_t source_byte_count, char out[33]) {
    static const unsigned char tag[] = "ntpacker-format-package-v1";
    NT_ASSERT(descriptor_bytes || descriptor_byte_count == 0U);
    NT_ASSERT(source_bytes || source_byte_count == 0U);
    NT_ASSERT(out);
    unsigned char api[4];
    unsigned char descriptor_size[8];
    unsigned char source_size[8];
    write_u32le(api, api_version);
    write_u64le(descriptor_size, (uint64_t)descriptor_byte_count);
    write_u64le(source_size, (uint64_t)source_byte_count);
    tp_hasher hasher = tp_hasher_init();
    tp_hasher_update(&hasher, tag, sizeof tag);
    tp_hasher_update(&hasher, api, sizeof api);
    tp_hasher_update(&hasher, descriptor_size, sizeof descriptor_size);
    tp_hasher_update(&hasher, descriptor_bytes, descriptor_byte_count);
    tp_hasher_update(&hasher, source_size, sizeof source_size);
    tp_hasher_update(&hasher, source_bytes, source_byte_count);
    const tp_id128 value = tp_hasher_final(hasher);
    tp_hex_encode_lower(value.bytes, sizeof value.bytes, out);
}

int tp_format_transform_token_value_internal(const char *token) {
    if (!token) {
        return -1;
    }
    for (int i = 0; i < (int)(sizeof g_transform_tokens /
                              sizeof g_transform_tokens[0]);
         ++i) {
        if (strcmp(token, g_transform_tokens[i]) == 0) {
            return i;
        }
    }
    return -1;
}

const char *tp_format_transform_token_internal(uint8_t value) {
    NT_ASSERT(value < sizeof g_transform_tokens / sizeof g_transform_tokens[0]);
    return g_transform_tokens[value];
}
