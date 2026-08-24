#include "tp_format_binding_proto_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_export_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_format_package_internal.h"
#include "tp_utf8_internal.h"

enum {
    TP_BINDING_FRAME_HEADER_BYTES = 12,
    TP_BINDING_FRAME_MAGIC = 0x50424654U, /* "TFBP" little-endian */
};

_Static_assert(TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES <= UINT32_MAX,
               "binding frame length must fit u32");
_Static_assert(TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES >=
                   TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES,
               "binding frame derivation must cover package bytes");
_Static_assert(TP_FORMAT_BINDING_PROTO_LUA_BINDINGS_FOR_PACKAGE_MAX <=
                   TP_FORMAT_BINDING_PROTO_MAX_BINDINGS,
               "binding cap must be able to carry the package-byte maximum");

typedef struct binding_writer {
    uint8_t *bytes;
    size_t offset;
} binding_writer;

typedef struct binding_reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} binding_reader;

typedef struct diagnostic_shape {
    uint32_t format_id_length;
    uint32_t package_path_length;
    uint32_t message_length;
    size_t variable_bytes;
    size_t owned_storage_bytes;
    size_t encoded_bytes;
} diagnostic_shape;

static void wr_u16(binding_writer *writer, uint16_t value) {
    writer->bytes[writer->offset++] = (uint8_t)value;
    writer->bytes[writer->offset++] = (uint8_t)(value >> 8U);
}

static void wr_u32(binding_writer *writer, uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        writer->bytes[writer->offset++] = (uint8_t)(value >> (i * 8U));
    }
}

static void wr_u64(binding_writer *writer, uint64_t value) {
    for (unsigned int i = 0U; i < 8U; ++i) {
        writer->bytes[writer->offset++] = (uint8_t)(value >> (i * 8U));
    }
}

static void wr_bytes(binding_writer *writer, const void *bytes, size_t length) {
    if (length > 0U) {
        memcpy(writer->bytes + writer->offset, bytes, length);
        writer->offset += length;
    }
}

static bool rd_u16(binding_reader *reader, uint16_t *out) {
    if (reader->offset > reader->length ||
        reader->length - reader->offset < 2U) {
        return false;
    }
    const uint8_t *p = reader->bytes + reader->offset;
    *out = (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8U));
    reader->offset += 2U;
    return true;
}

static bool rd_u32(binding_reader *reader, uint32_t *out) {
    if (reader->offset > reader->length ||
        reader->length - reader->offset < 4U) {
        return false;
    }
    const uint8_t *p = reader->bytes + reader->offset;
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
    reader->offset += 4U;
    return true;
}

static bool rd_u64(binding_reader *reader, uint64_t *out) {
    if (reader->offset > reader->length ||
        reader->length - reader->offset < 8U) {
        return false;
    }
    uint64_t value = 0U;
    for (unsigned int i = 0U; i < 8U; ++i) {
        value |= (uint64_t)reader->bytes[reader->offset + i] << (i * 8U);
    }
    reader->offset += 8U;
    *out = value;
    return true;
}

static bool rd_ref(binding_reader *reader, size_t length,
                   const uint8_t **out) {
    if (reader->offset > reader->length ||
        length > reader->length - reader->offset) {
        return false;
    }
    *out = reader->bytes + reader->offset;
    reader->offset += length;
    return true;
}

static bool add_size(size_t *value, size_t addition) {
    if (*value > (size_t)TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES ||
        addition > (size_t)TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES - *value) {
        return false;
    }
    *value += addition;
    return true;
}

static bool bounded_cstr(const char *text, size_t cap, uint32_t *out_length) {
    if (!text) {
        *out_length = UINT32_MAX;
        return true;
    }
    for (size_t i = 0U; i <= cap; ++i) {
        if (text[i] == '\0') {
            *out_length = (uint32_t)i;
            return true;
        }
    }
    return false;
}

static tp_status validate_optional_text(const char *text, uint32_t length,
                                        const char *label, tp_error *error) {
    if (length == UINT32_MAX) {
        return TP_STATUS_OK;
    }
    return tp_utf8_validate_text_field(text, length, label, error);
}

static tp_status diagnostic_measure(const tp_format_diagnostic *diagnostic,
                                    diagnostic_shape *out,
                                    tp_error *error) {
    memset(out, 0, sizeof *out);
    if (!tp_format_diagnostic_semantics_valid_internal(diagnostic) ||
        (diagnostic->code ==
             TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED &&
         !tp_format_diagnostic_truncation_marker_canonical_internal(
             diagnostic)) ||
        diagnostic->frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX ||
        (diagnostic->frame_count > 0U && !diagnostic->frames) ||
        !bounded_cstr(diagnostic->format_id, TP_FORMAT_ID_MAX_BYTES,
                      &out->format_id_length) ||
        !bounded_cstr(diagnostic->package_path,
                      TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
                      &out->package_path_length) ||
        !bounded_cstr(diagnostic->message,
                      TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES,
                      &out->message_length)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid diagnostic");
    }
    if (diagnostic->format_id &&
        !tp_format_id_is_runtime_token(diagnostic->format_id)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid diagnostic format id");
    }
    tp_status status = validate_optional_text(
        diagnostic->format_id, out->format_id_length, "diagnostic format id",
        error);
    if (status == TP_STATUS_OK) {
        status = validate_optional_text(diagnostic->package_path,
                                        out->package_path_length,
                                        "diagnostic package path", error);
    }
    if (status == TP_STATUS_OK) {
        status = validate_optional_text(diagnostic->message,
                                        out->message_length,
                                        "diagnostic message", error);
    }
    if (status != TP_STATUS_OK) {
        return status;
    }

    size_t variable = 0U;
    const uint32_t lengths[3] = {out->format_id_length,
                                 out->package_path_length,
                                 out->message_length};
    for (size_t i = 0U; i < 3U; ++i) {
        if (lengths[i] != UINT32_MAX && !add_size(&variable, lengths[i])) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_format_binding_proto: diagnostic bytes overflow");
        }
    }
    size_t encoded = TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FIXED_BYTES;
    for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
        const tp_format_diagnostic_frame *frame = &diagnostic->frames[i];
        uint32_t length = 0U;
        if (frame->line == 0U || !frame->text ||
            !bounded_cstr(frame->text, TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES,
                          &length) ||
            length == 0U) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "tp_format_binding_proto: invalid diagnostic frame");
        }
        status = tp_utf8_validate_text_field(frame->text, length,
                                             "diagnostic frame", error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (!add_size(&variable, length) ||
            !add_size(&encoded,
                      TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FRAME_FIXED_BYTES)) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_format_binding_proto: diagnostic frame bytes overflow");
        }
    }
    if (!add_size(&encoded, variable)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: diagnostic bytes overflow");
    }
    out->variable_bytes = variable;
    out->owned_storage_bytes = variable;
    if (out->format_id_length != UINT32_MAX) {
        out->owned_storage_bytes++;
    }
    if (out->package_path_length != UINT32_MAX) {
        out->owned_storage_bytes++;
    }
    if (out->message_length != UINT32_MAX) {
        out->owned_storage_bytes++;
    }
    if (!add_size(&out->owned_storage_bytes, diagnostic->frame_count)) {
        return tp_error_set(
            error, TP_STATUS_OUT_OF_BOUNDS,
            "tp_format_binding_proto: diagnostic owned bytes overflow");
    }
    if (diagnostic->frame_count >
        (SIZE_MAX - out->owned_storage_bytes) /
            sizeof(tp_format_diagnostic_frame)) {
        return tp_error_set(
            error, TP_STATUS_OUT_OF_BOUNDS,
            "tp_format_binding_proto: diagnostic owned bytes overflow");
    }
    out->owned_storage_bytes +=
        diagnostic->frame_count * sizeof(tp_format_diagnostic_frame);
    out->encoded_bytes = encoded;
    return TP_STATUS_OK;
}

static tp_status source_validate(const unsigned char *bytes, size_t length,
                                 tp_error *error) {
    if ((length > 0U && !bytes) || length > TP_FORMAT_SOURCE_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid Lua source bytes");
    }
    char message[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
    if (tp_format_package_v1_source_admission_internal(
            bytes, length, message, sizeof message) != 0) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: %s", message);
    }
    return TP_STATUS_OK;
}

static tp_status lua_binding_validate(
    const tp_format_binding_proto_binding *binding,
    tp_format_owned_descriptor **out_owned, tp_error *error) {
    *out_owned = NULL;
    if (binding->api_version != TP_FORMAT_API_VERSION ||
        binding->descriptor_byte_count == 0U ||
        binding->descriptor_byte_count > TP_FORMAT_DESCRIPTOR_MAX_BYTES ||
        !binding->descriptor_bytes ||
        binding->source_byte_count > TP_FORMAT_SOURCE_MAX_BYTES ||
        (binding->source_byte_count > 0U && !binding->source_bytes)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid Lua binding bounds");
    }
    tp_format_descriptor_parse_result parsed = {0};
    tp_status status = tp_format_descriptor_v1_parse(
        binding->descriptor_bytes, binding->descriptor_byte_count, &parsed,
        error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (parsed.outcome != TP_FORMAT_DESCRIPTOR_ADMITTED ||
        !parsed.owned_descriptor) {
        tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: Lua descriptor was not admitted");
    }
    status = source_validate(binding->source_bytes, binding->source_byte_count,
                             error);
    if (status != TP_STATUS_OK) {
        tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
        return status;
    }
    char expected[33];
    tp_format_package_fingerprint_internal(
        binding->api_version, binding->descriptor_bytes,
        binding->descriptor_byte_count, binding->source_bytes,
        binding->source_byte_count, expected);
    if (memcmp(expected, binding->fingerprint, sizeof expected) != 0) {
        tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: Lua fingerprint mismatch");
    }
    *out_owned = parsed.owned_descriptor;
    return TP_STATUS_OK;
}

static bool bounded_string_equal(const char *left, const char *right,
                                 size_t cap) {
    uint32_t left_length = 0U;
    uint32_t right_length = 0U;
    return left && right && bounded_cstr(left, cap, &left_length) &&
           bounded_cstr(right, cap, &right_length) &&
           left_length != UINT32_MAX && right_length != UINT32_MAX &&
           left_length == right_length &&
           memcmp(left, right, left_length) == 0;
}

static bool caps_equal(const tp_export_caps *left,
                       const tp_export_caps *right) {
    return left->transform_mask == right->transform_mask &&
           left->polygons == right->polygons && left->pivot == right->pivot &&
           left->slice9 == right->slice9 &&
           left->multipage == right->multipage &&
           left->aliases == right->aliases &&
           left->animations == right->animations;
}

/* A caller-side descriptor is only a convenience view of the exact bytes. It
 * must nevertheless agree field-for-field so a pre-encode consumer cannot
 * observe one capability/document contract while the worker decodes another.
 * Every caller string is bounded before comparison; no strcmp/strlen touches a
 * hostile or unterminated descriptor field. */
static bool descriptor_semantically_equal(
    const tp_format_descriptor *caller,
    const tp_format_descriptor *reparsed) {
    if (!caller || !reparsed ||
        caller->api_version != reparsed->api_version ||
        !bounded_string_equal(caller->id, reparsed->id,
                              TP_FORMAT_ID_MAX_BYTES) ||
        !bounded_string_equal(caller->display_name, reparsed->display_name,
                              TP_FORMAT_DISPLAY_NAME_MAX_BYTES) ||
        !caps_equal(&caller->caps, &reparsed->caps) ||
        caller->artifact_count != reparsed->artifact_count ||
        caller->host_fact_count != reparsed->host_fact_count ||
        caller->artifact_count < 0 ||
        caller->artifact_count > (int)TP_FORMAT_OUTPUT_MAX ||
        caller->host_fact_count < 0 ||
        caller->host_fact_count > (int)TP_FORMAT_HOST_FACT_MAX ||
        (caller->artifact_count > 0 && !caller->artifacts) ||
        (caller->host_fact_count > 0 && !caller->host_facts)) {
        return false;
    }
    for (int i = 0; i < caller->artifact_count; ++i) {
        if (!bounded_string_equal(caller->artifacts[i].id,
                                  reparsed->artifacts[i].id,
                                  TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
            !bounded_string_equal(caller->artifacts[i].suffix,
                                  reparsed->artifacts[i].suffix,
                                  TP_FORMAT_SUFFIX_MAX_BYTES)) {
            return false;
        }
    }
    for (int i = 0; i < caller->host_fact_count; ++i) {
        const tp_format_host_fact_decl *left = &caller->host_facts[i];
        const tp_format_host_fact_decl *right = &reparsed->host_facts[i];
        if (!bounded_string_equal(left->id, right->id,
                                  TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
            left->kind != right->kind ||
            !bounded_string_equal(left->output_id, right->output_id,
                                  TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
            !bounded_string_equal(left->root_marker, right->root_marker,
                                  TP_FORMAT_LOGICAL_ID_MAX_BYTES) ||
            left->missing != right->missing) {
            return false;
        }
    }
    return true;
}

static tp_status binding_validate(
    const tp_format_binding_proto_binding *binding,
    tp_format_owned_descriptor **out_owned, tp_error *error) {
    *out_owned = NULL;
    if (!binding || !binding->descriptor || !binding->descriptor->id) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: binding descriptor is required");
    }
    if (binding->implementation == TP_FORMAT_IMPLEMENTATION_NATIVE) {
        uint32_t id_length = 0U;
        if (!bounded_cstr(binding->descriptor->id, TP_FORMAT_ID_MAX_BYTES,
                          &id_length) ||
            id_length == 0U || id_length == UINT32_MAX ||
            !tp_format_id_is_runtime_token(binding->descriptor->id)) {
            return tp_error_set(
                error, TP_STATUS_INVALID_ARGUMENT,
                "tp_format_binding_proto: invalid native binding id");
        }
        const tp_exporter *native =
            tp_native_exporter_find(binding->descriptor->id);
        if (!native || native->format != binding->descriptor ||
            binding->api_version != 0U || binding->fingerprint[0] != '\0' ||
            binding->package_path ||
            binding->descriptor_bytes || binding->descriptor_byte_count != 0U ||
            binding->source_bytes || binding->source_byte_count != 0U) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "tp_format_binding_proto: native binding is not compiled in");
        }
        return TP_STATUS_OK;
    }
    if (binding->implementation != TP_FORMAT_IMPLEMENTATION_LUA) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid binding kind");
    }
    uint32_t package_path_length = 0U;
    if (!bounded_cstr(binding->package_path,
                      TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
                      &package_path_length) ||
        package_path_length == 0U || package_path_length == UINT32_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid Lua package path");
    }
    tp_status status = lua_binding_validate(binding, out_owned, error);
    if (status == TP_STATUS_OK) {
        const tp_format_descriptor *parsed =
            tp_format_owned_descriptor_view(*out_owned);
        if (!descriptor_semantically_equal(binding->descriptor, parsed)) {
            tp_format_owned_descriptor_destroy(*out_owned);
            *out_owned = NULL;
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "tp_format_binding_proto: Lua descriptor identity mismatch");
        }
    }
    return status;
}

static tp_status resolution_validate(
    const tp_format_binding_proto_resolution *resolution,
    size_t binding_count, size_t diagnostic_count, tp_error *error) {
    if (!resolution) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: missing resolution");
    }
    switch (resolution->kind) {
        case TP_FORMAT_BINDING_RESOLUTION_ABSENT:
            if (resolution->binding_index != 0U ||
                resolution->diagnostic_offset != 0U ||
                resolution->diagnostic_count != 0U) {
                break;
            }
            return TP_STATUS_OK;
        case TP_FORMAT_BINDING_RESOLUTION_BINDING:
            if ((size_t)resolution->binding_index < binding_count &&
                resolution->diagnostic_offset == 0U &&
                resolution->diagnostic_count == 0U) {
                return TP_STATUS_OK;
            }
            break;
        case TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE: {
            const size_t offset = resolution->diagnostic_offset;
            const size_t count = resolution->diagnostic_count;
            if (resolution->binding_index == UINT32_MAX && count > 0U &&
                offset <= diagnostic_count && count <= diagnostic_count - offset) {
                return TP_STATUS_OK;
            }
            break;
        }
    }
    return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                        "tp_format_binding_proto: invalid resolution");
}

static tp_status unavailable_slice_validate(
    const tp_format_binding_proto_resolution *resolution,
    const tp_format_diagnostic *diagnostics, tp_error *error) {
    if (resolution->kind != TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE) {
        return TP_STATUS_OK;
    }
    const size_t offset = resolution->diagnostic_offset;
    const size_t count = resolution->diagnostic_count;
    tp_format_diagnostic_report *report = NULL;
    const tp_status status =
        tp_format_diagnostic_report_materialize_internal(
            diagnostics + offset, count, &report, error);
    tp_format_diagnostic_report_destroy(report);
    return status;
}

static tp_status unavailable_slices_validate(
    const tp_format_binding_proto_value *value, tp_error *error) {
    bool has_unavailable =
        value->preview.kind == TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE;
    for (size_t i = 0U; !has_unavailable && i < value->target_count; ++i) {
        has_unavailable = value->targets[i].resolution.kind ==
                          TP_FORMAT_BINDING_RESOLUTION_UNAVAILABLE;
    }
    if (!has_unavailable) {
        return TP_STATUS_OK;
    }

    tp_status status = TP_STATUS_OK;
    for (size_t i = 0U; i < value->diagnostic_count; ++i) {
        diagnostic_shape shape;
        status = diagnostic_measure(&value->diagnostics[i], &shape, error);
        if (status != TP_STATUS_OK) {
            break;
        }
    }
    if (status == TP_STATUS_OK) {
        status = unavailable_slice_validate(
            &value->preview, value->diagnostics, error);
    }
    for (size_t i = 0U; status == TP_STATUS_OK && i < value->target_count;
         ++i) {
        status = unavailable_slice_validate(
            &value->targets[i].resolution, value->diagnostics, error);
    }
    return status;
}

static void write_resolution(binding_writer *writer,
                             const tp_format_binding_proto_resolution *value) {
    wr_u32(writer, (uint32_t)value->kind);
    wr_u32(writer, value->binding_index);
    wr_u32(writer, value->diagnostic_offset);
    wr_u32(writer, value->diagnostic_count);
}

static bool read_resolution(binding_reader *reader,
                            tp_format_binding_proto_resolution *out) {
    uint32_t kind = 0U;
    return rd_u32(reader, &kind) &&
           rd_u32(reader, &out->binding_index) &&
           rd_u32(reader, &out->diagnostic_offset) &&
           rd_u32(reader, &out->diagnostic_count) &&
           (out->kind = (tp_format_binding_resolution_kind)kind, true);
}

static tp_status validate_duplicates(
    const tp_format_binding_proto_binding *bindings, size_t count,
    tp_error *error) {
    for (size_t i = 0U; i < count; ++i) {
        for (size_t j = 0U; j < i; ++j) {
            if (bindings[i].implementation != bindings[j].implementation ||
                strcmp(bindings[i].descriptor->id,
                       bindings[j].descriptor->id) != 0) {
                continue;
            }
            if (bindings[i].implementation == TP_FORMAT_IMPLEMENTATION_NATIVE ||
                strcmp(bindings[i].fingerprint,
                       bindings[j].fingerprint) == 0) {
                return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                    "tp_format_binding_proto: duplicate binding identity");
            }
        }
    }
    return TP_STATUS_OK;
}

typedef struct target_identity_pair {
    tp_id128 atlas_id;
    tp_id128 target_id;
} target_identity_pair;

static int target_identity_compare(const void *left, const void *right) {
    const target_identity_pair *a = (const target_identity_pair *)left;
    const target_identity_pair *b = (const target_identity_pair *)right;
    int order = memcmp(a->atlas_id.bytes, b->atlas_id.bytes,
                       sizeof a->atlas_id.bytes);
    return order != 0 ? order
                      : memcmp(a->target_id.bytes, b->target_id.bytes,
                               sizeof a->target_id.bytes);
}

static tp_status validate_target_identity_uniqueness(
    const tp_format_binding_proto_target_ref *targets, size_t count,
    tp_error *error) {
    if (count < 2U) {
        return TP_STATUS_OK;
    }
    if (count > SIZE_MAX / sizeof(target_identity_pair)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: target identity table overflow");
    }
    target_identity_pair *pairs =
        (target_identity_pair *)malloc(count * sizeof *pairs);
    if (!pairs) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "tp_format_binding_proto: target identity allocation failed");
    }
    for (size_t i = 0U; i < count; ++i) {
        pairs[i].atlas_id = targets[i].atlas_id;
        pairs[i].target_id = targets[i].target_id;
    }
    qsort(pairs, count, sizeof *pairs, target_identity_compare);
    tp_status status = TP_STATUS_OK;
    for (size_t i = 1U; i < count; ++i) {
        if (target_identity_compare(&pairs[i - 1U], &pairs[i]) == 0) {
            status = tp_error_set(
                error, TP_STATUS_INVALID_ARGUMENT,
                "tp_format_binding_proto: duplicate atlas/target reference");
            break;
        }
    }
    free(pairs);
    return status;
}

static void write_diagnostic(binding_writer *writer,
                             const tp_format_diagnostic *diagnostic,
                             const diagnostic_shape *shape) {
    wr_u32(writer, (uint32_t)diagnostic->severity);
    wr_u32(writer, (uint32_t)diagnostic->code);
    wr_u32(writer, (uint32_t)diagnostic->phase);
    wr_u32(writer, diagnostic->line);
    wr_u32(writer, diagnostic->column);
    wr_u32(writer, (uint32_t)diagnostic->frame_count);
    wr_u32(writer, shape->format_id_length);
    wr_u32(writer, shape->package_path_length);
    wr_u32(writer, shape->message_length);
    wr_u32(writer, 0U);
    if (shape->format_id_length != UINT32_MAX) {
        wr_bytes(writer, diagnostic->format_id, shape->format_id_length);
    }
    if (shape->package_path_length != UINT32_MAX) {
        wr_bytes(writer, diagnostic->package_path, shape->package_path_length);
    }
    if (shape->message_length != UINT32_MAX) {
        wr_bytes(writer, diagnostic->message, shape->message_length);
    }
    for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
        uint32_t length = 0U;
        (void)bounded_cstr(diagnostic->frames[i].text,
                           TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES, &length);
        wr_u32(writer, diagnostic->frames[i].line);
        wr_u32(writer, length);
        wr_bytes(writer, diagnostic->frames[i].text, length);
    }
}

tp_status tp_format_binding_proto_encode(
    const tp_format_binding_proto_value *value, uint8_t **out_bytes,
    size_t *out_length, tp_error *error) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_length) {
        *out_length = 0U;
    }
    if (!value || !out_bytes || !out_length ||
        value->binding_count > TP_FORMAT_BINDING_PROTO_MAX_BINDINGS ||
        value->target_count > TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS ||
        value->diagnostic_count > TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS ||
        (value->binding_count > 0U && !value->bindings) ||
        (value->target_count > 0U && !value->targets) ||
        (value->diagnostic_count > 0U && !value->diagnostics)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid encode value");
    }

    size_t payload = TP_FORMAT_BINDING_PROTO_FIXED_PAYLOAD_BYTES;
    size_t package_bytes = 0U;
    for (size_t i = 0U; i < value->binding_count; ++i) {
        tp_format_owned_descriptor *owned = NULL;
        tp_status status = binding_validate(&value->bindings[i], &owned, error);
        tp_format_owned_descriptor_destroy(owned);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (value->bindings[i].implementation == TP_FORMAT_IMPLEMENTATION_NATIVE) {
            const size_t id_length = strlen(value->bindings[i].descriptor->id);
            if (!add_size(&payload, 8U) || !add_size(&payload, id_length)) {
                return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                    "tp_format_binding_proto: native binding bytes exceed cap");
            }
        } else {
            const size_t package_path_length = strlen(
                value->bindings[i].package_path);
            if (!add_size(&package_bytes,
                          value->bindings[i].descriptor_byte_count) ||
                !add_size(&package_bytes, value->bindings[i].source_byte_count) ||
                !add_size(&payload,
                          TP_FORMAT_BINDING_PROTO_LUA_BINDING_FIXED_BYTES) ||
                !add_size(&payload,
                          value->bindings[i].descriptor_byte_count) ||
                !add_size(&payload, value->bindings[i].source_byte_count) ||
                !add_size(&payload, package_path_length)) {
                return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                    "tp_format_binding_proto: Lua binding bytes exceed cap");
            }
        }
    }
    if (package_bytes > TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: package bytes exceed cap");
    }
    tp_status status = validate_duplicates(value->bindings,
                                           value->binding_count, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = resolution_validate(&value->preview, value->binding_count,
                                 value->diagnostic_count, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    for (size_t i = 0U; i < value->target_count; ++i) {
        if (tp_id128_is_nil(value->targets[i].atlas_id) ||
            tp_id128_is_nil(value->targets[i].target_id)) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "tp_format_binding_proto: target reference has nil id");
        }
        status = resolution_validate(&value->targets[i].resolution,
                                     value->binding_count,
                                     value->diagnostic_count, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
        if (!add_size(&payload, TP_FORMAT_BINDING_PROTO_TARGET_REF_BYTES)) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "tp_format_binding_proto: target references exceed cap");
        }
    }
    status = validate_target_identity_uniqueness(value->targets,
                                                 value->target_count, error);
    if (status != TP_STATUS_OK) {
        return status;
    }

    diagnostic_shape *shapes = NULL;
    if (value->diagnostic_count > 0U) {
        shapes = (diagnostic_shape *)calloc(value->diagnostic_count,
                                            sizeof *shapes);
        if (!shapes) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "tp_format_binding_proto: diagnostic shape allocation failed");
        }
    }
    size_t diagnostic_bytes = 0U;
    for (size_t i = 0U; i < value->diagnostic_count; ++i) {
        status = diagnostic_measure(&value->diagnostics[i], &shapes[i], error);
        if (status != TP_STATUS_OK ||
            !add_size(&diagnostic_bytes, shapes[i].variable_bytes) ||
            !add_size(&payload, shapes[i].encoded_bytes)) {
            free(shapes);
            return status != TP_STATUS_OK
                       ? status
                       : tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                      "tp_format_binding_proto: diagnostics exceed cap");
        }
    }
    status = unavailable_slices_validate(value, error);
    if (status != TP_STATUS_OK) {
        free(shapes);
        return status;
    }
    if (diagnostic_bytes > TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES ||
        payload > TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES ||
        payload > UINT32_MAX || payload > SIZE_MAX - TP_BINDING_FRAME_HEADER_BYTES) {
        free(shapes);
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: frame exceeds cap");
    }

    const size_t total = TP_BINDING_FRAME_HEADER_BYTES + payload;
    uint8_t *bytes = (uint8_t *)malloc(total);
    if (!bytes) {
        free(shapes);
        return tp_error_set(error, TP_STATUS_OOM,
                            "tp_format_binding_proto: frame allocation failed");
    }
    binding_writer writer = {bytes, 0U};
    wr_u32(&writer, TP_BINDING_FRAME_MAGIC);
    wr_u16(&writer, TP_FORMAT_BINDING_PROTO_VERSION);
    wr_u16(&writer, 0U);
    wr_u32(&writer, (uint32_t)payload);
    wr_u32(&writer, (uint32_t)value->binding_count);
    wr_u32(&writer, (uint32_t)value->target_count);
    wr_u32(&writer, (uint32_t)value->diagnostic_count);
    wr_u32(&writer, (uint32_t)diagnostic_bytes);
    wr_u64(&writer, (uint64_t)package_bytes);
    write_resolution(&writer, &value->preview);
    for (size_t i = 0U; i < value->binding_count; ++i) {
        const tp_format_binding_proto_binding *binding = &value->bindings[i];
        wr_u32(&writer, (uint32_t)binding->implementation);
        if (binding->implementation == TP_FORMAT_IMPLEMENTATION_NATIVE) {
            const uint32_t length = (uint32_t)strlen(binding->descriptor->id);
            wr_u32(&writer, length);
            wr_bytes(&writer, binding->descriptor->id, length);
        } else {
            const uint32_t package_path_length =
                (uint32_t)strlen(binding->package_path);
            wr_u32(&writer, binding->api_version);
            wr_bytes(&writer, binding->fingerprint, 32U);
            wr_u64(&writer, (uint64_t)binding->descriptor_byte_count);
            wr_u64(&writer, (uint64_t)binding->source_byte_count);
            wr_u32(&writer, package_path_length);
            wr_bytes(&writer, binding->package_path, package_path_length);
            wr_bytes(&writer, binding->descriptor_bytes,
                     binding->descriptor_byte_count);
            wr_bytes(&writer, binding->source_bytes, binding->source_byte_count);
        }
    }
    for (size_t i = 0U; i < value->target_count; ++i) {
        wr_bytes(&writer, value->targets[i].atlas_id.bytes, 16U);
        wr_bytes(&writer, value->targets[i].target_id.bytes, 16U);
        write_resolution(&writer, &value->targets[i].resolution);
    }
    for (size_t i = 0U; i < value->diagnostic_count; ++i) {
        write_diagnostic(&writer, &value->diagnostics[i], &shapes[i]);
    }
    free(shapes);
    NT_ASSERT(writer.offset == total);
    *out_bytes = bytes;
    *out_length = total;
    return TP_STATUS_OK;
}

void tp_format_binding_proto_value_free(tp_format_binding_proto_value *value) {
    if (!value) {
        return;
    }
    for (size_t i = 0U; i < value->binding_count; ++i) {
        tp_format_owned_descriptor_destroy(
            (tp_format_owned_descriptor *)value->bindings[i].owned_descriptor);
        free((void *)value->bindings[i].descriptor_bytes);
        free((void *)value->bindings[i].source_bytes);
        free((void *)value->bindings[i].package_path);
    }
    if (value->owned_diagnostic_blocks) {
        for (size_t i = 0U; i < value->diagnostic_count; ++i) {
            free(value->owned_diagnostic_blocks[i]);
        }
    }
    free(value->owned_diagnostic_blocks);
    free(value->diagnostics);
    free(value->targets);
    free(value->bindings);
    memset(value, 0, sizeof *value);
}

static tp_status decode_frame(const uint8_t *bytes, size_t length,
                              binding_reader *out, tp_error *error) {
    if (!bytes || length < TP_BINDING_FRAME_HEADER_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: truncated frame header");
    }
    binding_reader reader = {bytes, length, 0U};
    uint32_t magic = 0U;
    uint16_t version = 0U;
    uint16_t reserved = 0U;
    uint32_t payload_length = 0U;
    (void)rd_u32(&reader, &magic);
    (void)rd_u16(&reader, &version);
    (void)rd_u16(&reader, &reserved);
    (void)rd_u32(&reader, &payload_length);
    if (magic != TP_BINDING_FRAME_MAGIC) {
        return tp_error_set(error, TP_STATUS_BAD_MAGIC,
                            "tp_format_binding_proto: bad frame magic");
    }
    if (version != TP_FORMAT_BINDING_PROTO_VERSION) {
        return tp_error_set(error, TP_STATUS_BAD_VERSION,
                            "tp_format_binding_proto: unsupported version");
    }
    if (reserved != 0U ||
        payload_length > TP_FORMAT_BINDING_PROTO_MAX_FRAME_BYTES ||
        (size_t)payload_length != length - TP_BINDING_FRAME_HEADER_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: invalid declared frame length");
    }
    out->bytes = bytes + TP_BINDING_FRAME_HEADER_BYTES;
    out->length = payload_length;
    out->offset = 0U;
    return TP_STATUS_OK;
}

static tp_status decode_text(binding_reader *reader, uint32_t length,
                             size_t cap, bool nullable, bool allow_empty,
                             const char *label, char **out,
                             size_t *owned_bytes, tp_error *error) {
    *out = NULL;
    if (length == UINT32_MAX) {
        return nullable ? TP_STATUS_OK
                        : tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                       "tp_format_binding_proto: null %s", label);
    }
    if ((size_t)length > cap || (!allow_empty && length == 0U)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid %s length", label);
    }
    const uint8_t *source = NULL;
    if (!rd_ref(reader, length, &source)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: truncated %s", label);
    }
    tp_status status = tp_utf8_validate_text_field(source, length, label, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char *copy = (char *)malloc((size_t)length + 1U);
    if (!copy) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "tp_format_binding_proto: text allocation failed");
    }
    memcpy(copy, source, length);
    copy[length] = '\0';
    *out = copy;
    *owned_bytes += length;
    return TP_STATUS_OK;
}

static tp_status decode_diagnostic(binding_reader *reader,
                                   tp_format_diagnostic *out,
                                   void **out_block, size_t *out_dynamic,
                                   tp_error *error) {
    uint32_t severity = 0U, code = 0U, phase = 0U, frame_count = 0U;
    uint32_t format_length = 0U, path_length = 0U, message_length = 0U;
    uint32_t reserved = 0U;
    if (!rd_u32(reader, &severity) || !rd_u32(reader, &code) ||
        !rd_u32(reader, &phase) || !rd_u32(reader, &out->line) ||
        !rd_u32(reader, &out->column) || !rd_u32(reader, &frame_count) ||
        !rd_u32(reader, &format_length) || !rd_u32(reader, &path_length) ||
        !rd_u32(reader, &message_length) || !rd_u32(reader, &reserved)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: truncated diagnostic");
    }
    out->severity = (tp_format_diagnostic_severity)severity;
    out->code = (tp_format_diagnostic_code)code;
    out->phase = (tp_format_diagnostic_phase)phase;
    if (!tp_format_diagnostic_semantics_valid_internal(out) ||
        reserved != 0U ||
        frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: invalid diagnostic semantics");
    }

    /* Decode into temporary independently allocated strings, then coalesce the
     * complete diagnostic into one owner block for deterministic destruction. */
    char *format = NULL, *path = NULL, *message = NULL;
    size_t dynamic = 0U;
    tp_status status = decode_text(reader, format_length,
                                   TP_FORMAT_ID_MAX_BYTES, true, true,
                                   "diagnostic format id", &format, &dynamic,
                                   error);
    if (status == TP_STATUS_OK) {
        status = decode_text(reader, path_length,
                             TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES, true, true,
                             "diagnostic package path", &path, &dynamic, error);
    }
    if (status == TP_STATUS_OK) {
        status = decode_text(reader, message_length,
                             TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES, true, true,
                             "diagnostic message", &message, &dynamic, error);
    }
    tp_format_diagnostic_frame frames[TP_FORMAT_DIAGNOSTIC_FRAME_MAX] = {{0}};
    char *frame_text[TP_FORMAT_DIAGNOSTIC_FRAME_MAX] = {0};
    for (uint32_t i = 0U; status == TP_STATUS_OK && i < frame_count; ++i) {
        uint32_t length = 0U;
        if (!rd_u32(reader, &frames[i].line) || !rd_u32(reader, &length) ||
            frames[i].line == 0U) {
            status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                  "tp_format_binding_proto: invalid diagnostic frame fields");
            break;
        }
        status = decode_text(reader, length,
                             TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES, false, false,
                             "diagnostic frame", &frame_text[i], &dynamic,
                             error);
        frames[i].text = frame_text[i];
    }
    if (status == TP_STATUS_OK && format &&
        !tp_format_id_is_runtime_token(format)) {
        status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                              "tp_format_binding_proto: invalid diagnostic format id");
    }
    if (status == TP_STATUS_OK &&
        out->code == TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
        tp_format_diagnostic complete = *out;
        complete.format_id = format;
        complete.package_path = path;
        complete.message = message;
        complete.frames = frame_count > 0U ? frames : NULL;
        complete.frame_count = frame_count;
        if (!tp_format_diagnostic_truncation_marker_canonical_internal(
                &complete)) {
            status = tp_error_set(
                error, TP_STATUS_INVALID_ARGUMENT,
                "tp_format_binding_proto: invalid truncation marker");
        }
    }
    if (status != TP_STATUS_OK) {
        free(format);
        free(path);
        free(message);
        for (uint32_t i = 0U; i < frame_count; ++i) {
            free(frame_text[i]);
        }
        return status;
    }

    size_t block_size = (size_t)frame_count * sizeof(tp_format_diagnostic_frame);
    size_t string_storage = 0U;
    const char *strings[3] = {format, path, message};
    for (size_t i = 0U; i < 3U; ++i) {
        if (strings[i]) {
            string_storage += strlen(strings[i]) + 1U;
        }
    }
    for (uint32_t i = 0U; i < frame_count; ++i) {
        string_storage += strlen(frame_text[i]) + 1U;
    }
    if (block_size > SIZE_MAX - string_storage) {
        status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                              "tp_format_binding_proto: diagnostic allocation overflow");
    } else {
        const size_t allocation_size =
            block_size + string_storage > 0U ? block_size + string_storage : 1U;
        uint8_t *block = (uint8_t *)malloc(allocation_size);
        if (!block) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "tp_format_binding_proto: diagnostic allocation failed");
        } else {
            tp_format_diagnostic_frame *owned_frames =
                (tp_format_diagnostic_frame *)block;
            char *cursor = (char *)(block + block_size);
#define COPY_DIAGNOSTIC_STRING(field, source)                              \
    do {                                                                   \
        if (source) {                                                      \
            const size_t n = strlen(source) + 1U;                          \
            memcpy(cursor, source, n);                                     \
            out->field = cursor;                                           \
            cursor += n;                                                   \
        }                                                                  \
    } while (0)
            COPY_DIAGNOSTIC_STRING(format_id, format);
            COPY_DIAGNOSTIC_STRING(package_path, path);
            COPY_DIAGNOSTIC_STRING(message, message);
            for (uint32_t i = 0U; i < frame_count; ++i) {
                owned_frames[i].line = frames[i].line;
                const size_t n = strlen(frame_text[i]) + 1U;
                memcpy(cursor, frame_text[i], n);
                owned_frames[i].text = cursor;
                cursor += n;
            }
#undef COPY_DIAGNOSTIC_STRING
            out->frames = frame_count > 0U ? owned_frames : NULL;
            out->frame_count = frame_count;
            *out_block = block;
            *out_dynamic = dynamic;
        }
    }
    free(format);
    free(path);
    free(message);
    for (uint32_t i = 0U; i < frame_count; ++i) {
        free(frame_text[i]);
    }
    return status;
}

tp_status tp_format_binding_proto_decode(
    const uint8_t *bytes, size_t length,
    tp_format_binding_proto_value *out_value, tp_error *error) {
    if (!out_value) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "tp_format_binding_proto: decode output is required");
    }
    memset(out_value, 0, sizeof *out_value);
    binding_reader reader = {0};
    tp_status status = decode_frame(bytes, length, &reader, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    uint32_t binding_count = 0U, target_count = 0U, diagnostic_count = 0U;
    uint32_t declared_diagnostic_bytes = 0U;
    uint64_t declared_package_bytes = 0U;
    if (!rd_u32(&reader, &binding_count) || !rd_u32(&reader, &target_count) ||
        !rd_u32(&reader, &diagnostic_count) ||
        !rd_u32(&reader, &declared_diagnostic_bytes) ||
        !rd_u64(&reader, &declared_package_bytes) ||
        !read_resolution(&reader, &out_value->preview)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: truncated envelope");
    }
    if (binding_count > TP_FORMAT_BINDING_PROTO_MAX_BINDINGS ||
        target_count > TP_FORMAT_BINDING_PROTO_MAX_TARGET_REFS ||
        diagnostic_count > TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTICS ||
        declared_diagnostic_bytes >
            TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES ||
        declared_package_bytes > TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: declared envelope limits exceeded");
    }
    size_t minimum_remaining = 0U;
    if (!add_size(&minimum_remaining, (size_t)binding_count * 8U) ||
        !add_size(&minimum_remaining,
                  (size_t)target_count *
                      TP_FORMAT_BINDING_PROTO_TARGET_REF_BYTES) ||
        !add_size(&minimum_remaining,
                  (size_t)diagnostic_count *
                      TP_FORMAT_BINDING_PROTO_DIAGNOSTIC_FIXED_BYTES) ||
        minimum_remaining > reader.length - reader.offset) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "tp_format_binding_proto: counts exceed remaining frame");
    }

    if (binding_count > 0U) {
        out_value->bindings = (tp_format_binding_proto_binding *)calloc(
            binding_count, sizeof *out_value->bindings);
        if (!out_value->bindings) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "tp_format_binding_proto: binding allocation failed");
            goto fail;
        }
    }
    out_value->binding_count = binding_count;
    size_t package_bytes = 0U;
    for (size_t i = 0U; i < binding_count; ++i) {
        uint32_t implementation = 0U;
        if (!rd_u32(&reader, &implementation)) {
            status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                  "tp_format_binding_proto: truncated binding");
            goto fail;
        }
        tp_format_binding_proto_binding *binding = &out_value->bindings[i];
        binding->implementation =
            (tp_format_implementation_kind)implementation;
        if (binding->implementation == TP_FORMAT_IMPLEMENTATION_NATIVE) {
            uint32_t id_length = 0U;
            const uint8_t *id_bytes = NULL;
            if (!rd_u32(&reader, &id_length) || id_length == 0U ||
                id_length > TP_FORMAT_ID_MAX_BYTES ||
                !rd_ref(&reader, id_length, &id_bytes) ||
                tp_utf8_validate_text_field(id_bytes, id_length,
                                            "native format id", error) !=
                    TP_STATUS_OK) {
                status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                      "tp_format_binding_proto: invalid native id");
                goto fail;
            }
            char id[TP_FORMAT_ID_MAX_BYTES + 1U];
            memcpy(id, id_bytes, id_length);
            id[id_length] = '\0';
            const tp_exporter *native = tp_native_exporter_find(id);
            if (!native || !native->format) {
                status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                      "tp_format_binding_proto: native id is not compiled in");
                goto fail;
            }
            binding->descriptor = native->format;
        } else if (binding->implementation == TP_FORMAT_IMPLEMENTATION_LUA) {
            uint64_t descriptor_size = 0U, source_size = 0U;
            uint32_t package_path_size = 0U;
            const uint8_t *fingerprint = NULL;
            if (!rd_u32(&reader, &binding->api_version) ||
                !rd_ref(&reader, 32U, &fingerprint) ||
                !rd_u64(&reader, &descriptor_size) ||
                !rd_u64(&reader, &source_size) ||
                !rd_u32(&reader, &package_path_size) ||
                descriptor_size == 0U ||
                descriptor_size > TP_FORMAT_DESCRIPTOR_MAX_BYTES ||
                source_size > TP_FORMAT_SOURCE_MAX_BYTES ||
                descriptor_size > SIZE_MAX || source_size > SIZE_MAX ||
                package_path_size == 0U ||
                package_path_size > TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES ||
                package_bytes > TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES -
                                    (size_t)descriptor_size ||
                package_bytes + (size_t)descriptor_size >
                    TP_FORMAT_BINDING_PROTO_MAX_PACKAGE_BYTES -
                        (size_t)source_size) {
                status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                      "tp_format_binding_proto: invalid Lua binding lengths");
                goto fail;
            }
            memcpy(binding->fingerprint, fingerprint, 32U);
            binding->fingerprint[32] = '\0';
            size_t package_path_owned_bytes = 0U;
            status = decode_text(&reader, package_path_size,
                                 TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
                                 false, false, "Lua package path",
                                 (char **)&binding->package_path,
                                 &package_path_owned_bytes, error);
            if (status != TP_STATUS_OK) {
                goto fail;
            }
            const uint8_t *descriptor_ref = NULL, *source_ref = NULL;
            if (!rd_ref(&reader, (size_t)descriptor_size, &descriptor_ref) ||
                !rd_ref(&reader, (size_t)source_size, &source_ref)) {
                status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                      "tp_format_binding_proto: truncated Lua package");
                goto fail;
            }
            unsigned char *descriptor_copy =
                (unsigned char *)malloc((size_t)descriptor_size);
            unsigned char *source_copy = source_size > 0U
                                             ? (unsigned char *)malloc(
                                                   (size_t)source_size)
                                             : NULL;
            if (!descriptor_copy || (source_size > 0U && !source_copy)) {
                free(descriptor_copy);
                free(source_copy);
                status = tp_error_set(error, TP_STATUS_OOM,
                                      "tp_format_binding_proto: Lua package allocation failed");
                goto fail;
            }
            memcpy(descriptor_copy, descriptor_ref, (size_t)descriptor_size);
            if (source_size > 0U) {
                memcpy(source_copy, source_ref, (size_t)source_size);
            }
            binding->descriptor_bytes = descriptor_copy;
            binding->descriptor_byte_count = (size_t)descriptor_size;
            binding->source_bytes = source_copy;
            binding->source_byte_count = (size_t)source_size;
            tp_format_owned_descriptor *owned = NULL;
            status = lua_binding_validate(binding, &owned, error);
            if (status != TP_STATUS_OK) {
                goto fail;
            }
            binding->owned_descriptor = owned;
            binding->descriptor = tp_format_owned_descriptor_view(owned);
            package_bytes += (size_t)descriptor_size + (size_t)source_size;
        } else {
            status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                  "tp_format_binding_proto: invalid binding tag");
            goto fail;
        }
    }
    if (package_bytes != (size_t)declared_package_bytes) {
        status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                              "tp_format_binding_proto: package byte total mismatch");
        goto fail;
    }
    status = validate_duplicates(out_value->bindings, binding_count, error);
    if (status != TP_STATUS_OK) {
        goto fail;
    }

    if (target_count > 0U) {
        out_value->targets = (tp_format_binding_proto_target_ref *)calloc(
            target_count, sizeof *out_value->targets);
        if (!out_value->targets) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "tp_format_binding_proto: target allocation failed");
            goto fail;
        }
    }
    out_value->target_count = target_count;
    for (size_t i = 0U; i < target_count; ++i) {
        const uint8_t *atlas = NULL, *target = NULL;
        if (!rd_ref(&reader, 16U, &atlas) || !rd_ref(&reader, 16U, &target) ||
            !read_resolution(&reader, &out_value->targets[i].resolution)) {
            status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                  "tp_format_binding_proto: truncated target reference");
            goto fail;
        }
        memcpy(out_value->targets[i].atlas_id.bytes, atlas, 16U);
        memcpy(out_value->targets[i].target_id.bytes, target, 16U);
        if (tp_id128_is_nil(out_value->targets[i].atlas_id) ||
            tp_id128_is_nil(out_value->targets[i].target_id)) {
            status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                  "tp_format_binding_proto: target reference has nil id");
            goto fail;
        }
    }
    status = validate_target_identity_uniqueness(out_value->targets,
                                                 target_count, error);
    if (status != TP_STATUS_OK) {
        goto fail;
    }

    if (diagnostic_count > 0U) {
        out_value->diagnostics = (tp_format_diagnostic *)calloc(
            diagnostic_count, sizeof *out_value->diagnostics);
        out_value->owned_diagnostic_blocks =
            (void **)calloc(diagnostic_count,
                            sizeof *out_value->owned_diagnostic_blocks);
        if (!out_value->diagnostics || !out_value->owned_diagnostic_blocks) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "tp_format_binding_proto: diagnostic allocation failed");
            goto fail;
        }
    }
    out_value->diagnostic_count = diagnostic_count;
    size_t diagnostic_bytes = 0U;
    for (size_t i = 0U; i < diagnostic_count; ++i) {
        size_t row_bytes = 0U;
        status = decode_diagnostic(&reader, &out_value->diagnostics[i],
                                   &out_value->owned_diagnostic_blocks[i],
                                   &row_bytes, error);
        if (status != TP_STATUS_OK ||
            diagnostic_bytes > TP_FORMAT_BINDING_PROTO_MAX_DIAGNOSTIC_BYTES -
                                   row_bytes) {
            if (status == TP_STATUS_OK) {
                status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                      "tp_format_binding_proto: diagnostic bytes exceed cap");
            }
            goto fail;
        }
        diagnostic_bytes += row_bytes;
    }
    if (diagnostic_bytes != declared_diagnostic_bytes ||
        reader.offset != reader.length) {
        status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                              "tp_format_binding_proto: aggregate/trailing byte mismatch");
        goto fail;
    }
    out_value->diagnostic_dynamic_bytes = diagnostic_bytes;
    status = resolution_validate(&out_value->preview, binding_count,
                                 diagnostic_count, error);
    if (status != TP_STATUS_OK) {
        goto fail;
    }
    for (size_t i = 0U; i < target_count; ++i) {
        status = resolution_validate(&out_value->targets[i].resolution,
                                     binding_count, diagnostic_count, error);
        if (status != TP_STATUS_OK) {
            goto fail;
        }
    }
    status = unavailable_slices_validate(out_value, error);
    if (status != TP_STATUS_OK) {
        goto fail;
    }
    return TP_STATUS_OK;

fail:
    tp_format_binding_proto_value_free(out_value);
    return status;
}
