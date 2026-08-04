#ifndef TP_CORE_SRC_TP_FORMAT_BINDING_PROTO_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_BINDING_PROTO_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"
#include "tp_core/tp_id.h"

#define TP_FORMAT_BINDING_PROTO_VERSION 1U
#define TP_FORMAT_BINDING_PROTO_MAX_BINDINGS 256U
#define TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES 67108864U
#define TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS 262144U
#define TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS 4096U
#define TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES 4194304U

/* The derivation deliberately uses only frozen public component maxima.  It is
 * a payload cap; the 12-byte frame header is additional.  The diagnostic term
 * covers fixed diagnostic/frame fields as well as the separately charged
 * aggregate string bytes. */
#define TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES 40ULL
#define TP_FORMAT_BINDING_PROTO_NATIVE_BINDING_MAX_BYTES \
    (8ULL + TP_FORMAT_ID_MAX_BYTES)
#define TP_FORMAT_BINDING_PROTO_LUA_BINDING_FIXED_BYTES 56ULL
#define TP_FORMAT_BINDING_PROTO_LUA_PACKAGE_PER_BINDING_MAX_BYTES \
    (TP_FORMAT_DESCRIPTOR_MAX_BYTES + TP_FORMAT_SOURCE_MAX_BYTES)
#define TP_FORMAT_BINDING_PROTO_LUA_BINDINGS_FOR_PACKAGE_MAX              \
    ((TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES +                         \
      TP_FORMAT_BINDING_PROTO_LUA_PACKAGE_PER_BINDING_MAX_BYTES - 1ULL) / \
     TP_FORMAT_BINDING_PROTO_LUA_PACKAGE_PER_BINDING_MAX_BYTES)
#define TP_FORMAT_BINDING_PROTO_BINDING_TABLE_MAX_BYTES                    \
    (TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES +                           \
     TP_FORMAT_BINDING_PROTO_LUA_BINDINGS_FOR_PACKAGE_MAX *                \
         TP_FORMAT_BINDING_PROTO_LUA_BINDING_FIXED_BYTES +                 \
     (TP_FORMAT_BINDING_PROTO_MAX_BINDINGS -                               \
      TP_FORMAT_BINDING_PROTO_LUA_BINDINGS_FOR_PACKAGE_MAX) *              \
         TP_FORMAT_BINDING_PROTO_NATIVE_BINDING_MAX_BYTES)
#define TP_FORMAT_BINDING_PROTO_TARGET_REF_BYTES 48ULL
#define TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FIXED_BYTES 40ULL
#define TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FRAME_FIXED_BYTES 8ULL
#define TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES                              \
    (TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES +                          \
     TP_FORMAT_BINDING_PROTO_BINDING_TABLE_MAX_BYTES +                      \
     TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS *                              \
         TP_FORMAT_BINDING_PROTO_TARGET_REF_BYTES +                         \
     TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS *                              \
         TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FIXED_BYTES +                   \
     TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS *                              \
         TP_FORMAT_DIAGNOSTIC_FRAME_MAX *                                   \
         TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FRAME_FIXED_BYTES +             \
     TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES)

typedef enum tp_format_binding_resolution_kind {
    TP_FORMAT_BINDING_RESOLUTION_ABSENT = 0,
    TP_FORMAT_BINDING_RESOLUTION_BINDING = 1,
    TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE = 2,
} tp_format_binding_resolution_kind;

typedef struct tp_format_binding_proto_binding {
    tp_format_implementation_kind implementation;
    const tp_format_descriptor *descriptor;

    /* Native: descriptor is the compiled-in descriptor and all fields below
     * are zero. Lua: descriptor is the reparsed exact descriptor and the
     * remaining fields are exact admitted package bytes/identity. */
    uint32_t api_version;
    char fingerprint[33];
    const unsigned char *descriptor_bytes;
    size_t descriptor_byte_count;
    const unsigned char *source_bytes;
    size_t source_byte_count;

    /* Decoder-private ownership token. Callers must not inspect it. */
    void *owned_descriptor;
} tp_format_binding_proto_binding;

typedef struct tp_format_binding_proto_resolution {
    tp_format_binding_resolution_kind kind;
    uint32_t binding_index;
    uint32_t diagnostic_offset;
    uint32_t diagnostic_count;
} tp_format_binding_proto_resolution;

typedef struct tp_format_binding_proto_target_ref {
    tp_id128 atlas_id;
    tp_id128 target_id;
    tp_format_binding_proto_resolution resolution;
} tp_format_binding_proto_target_ref;

typedef struct tp_format_binding_proto_value {
    tp_format_binding_proto_binding *bindings;
    size_t binding_count;
    tp_format_binding_proto_resolution preview;
    tp_format_binding_proto_target_ref *targets;
    size_t target_count;

    /* Unavailable-row diagnostics are one flattened owned table. Resolutions
     * refer to validated shared slices in this array. */
    tp_format_diagnostic *diagnostics;
    size_t diagnostic_count;
    size_t diagnostic_dynamic_bytes;

    /* Decoder-private blocks, one per flattened diagnostic. */
    void **owned_diagnostic_blocks;
} tp_format_binding_proto_value;

tp_status tp_format_binding_proto_encode(
    const tp_format_binding_proto_value *value, uint8_t **out_bytes,
    size_t *out_length, tp_error *error);

tp_status tp_format_binding_proto_decode(
    const uint8_t *bytes, size_t length,
    tp_format_binding_proto_value *out_value, tp_error *error);

void tp_format_binding_proto_value_free(tp_format_binding_proto_value *value);

#endif /* TP_CORE_SRC_TP_FORMAT_BINDING_PROTO_INTERNAL_H */
