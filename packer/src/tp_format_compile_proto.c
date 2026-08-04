#include "tp_format_compile_proto_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tp_format_descriptor_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_utf8_internal.h"

typedef struct compile_writer {
    uint8_t *bytes;
    size_t length;
    size_t offset;
} compile_writer;

typedef struct compile_reader {
    const uint8_t *bytes;
    size_t length;
    size_t offset;
} compile_reader;

typedef struct diagnostic_shape {
    uint32_t format_id_length;
    uint32_t package_path_length;
    uint32_t message_length;
    size_t bytes;
} diagnostic_shape;

_Static_assert(TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_PAYLOAD_BYTES < UINT32_MAX,
               "compile request payload must fit u32");
_Static_assert(TP_FORMAT_COMPILE_PROTO_MAX_RESULT_PAYLOAD_BYTES < UINT32_MAX,
               "compile result payload must fit u32");

static void wr_u16(compile_writer *writer, uint16_t value) {
    writer->bytes[writer->offset++] = (uint8_t)value;
    writer->bytes[writer->offset++] = (uint8_t)(value >> 8U);
}

static void wr_u32(compile_writer *writer, uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        writer->bytes[writer->offset++] = (uint8_t)(value >> (i * 8U));
    }
}

static void wr_bytes(compile_writer *writer, const void *bytes, size_t length) {
    if (length > 0U) {
        memcpy(writer->bytes + writer->offset, bytes, length);
        writer->offset += length;
    }
}

static bool rd_u32(compile_reader *reader, uint32_t *out) {
    if (reader->offset > reader->length ||
        reader->length - reader->offset < 4U) {
        return false;
    }
    const uint8_t *bytes = reader->bytes + reader->offset;
    *out = (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
    reader->offset += 4U;
    return true;
}

static bool rd_ref(compile_reader *reader, size_t length,
                   const uint8_t **out) {
    if (reader->offset > reader->length ||
        length > reader->length - reader->offset) {
        return false;
    }
    *out = reader->bytes + reader->offset;
    reader->offset += length;
    return true;
}

static bool add_size(size_t *value, size_t increment) {
    if (*value > SIZE_MAX - increment) {
        return false;
    }
    *value += increment;
    return true;
}

static bool bounded_length(const char *text, size_t maximum,
                           size_t *out_length) {
    if (!text) {
        return false;
    }
    size_t length = 0U;
    while (length <= maximum && text[length] != '\0') {
        ++length;
    }
    if (length > maximum) {
        return false;
    }
    *out_length = length;
    return true;
}

static tp_status validate_text(const char *text, size_t length,
                               const char *label, tp_error *error) {
    return tp_utf8_validate_text_field(text, length, label, error);
}

static tp_status allocate_frame(tp_format_compile_proto_kind kind,
                                uint32_t magic, size_t payload_length,
                                uint8_t **out_bytes, size_t *out_length,
                                tp_error *error) {
    if (!out_bytes || !out_length) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile protocol output is required");
    }
    *out_bytes = NULL;
    *out_length = 0U;
    if (payload_length > UINT32_MAX ||
        payload_length > SIZE_MAX - TP_FORMAT_COMPILE_PROTO_HEADER_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol frame size overflow");
    }
    const size_t total = TP_FORMAT_COMPILE_PROTO_HEADER_BYTES + payload_length;
    uint8_t *bytes = (uint8_t *)malloc(total);
    if (!bytes) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "compile protocol frame allocation failed");
    }
    compile_writer writer = {bytes, total, 0U};
    wr_u32(&writer, magic);
    wr_u16(&writer, (uint16_t)TP_FORMAT_COMPILE_PROTO_VERSION);
    wr_u16(&writer, (uint16_t)kind);
    wr_u32(&writer, (uint32_t)payload_length);
    *out_bytes = bytes;
    *out_length = total;
    return TP_STATUS_OK;
}

static tp_status encode_empty(tp_format_compile_proto_kind kind,
                              uint32_t magic, uint8_t **out_bytes,
                              size_t *out_length, tp_error *error) {
    return allocate_frame(kind, magic, 0U, out_bytes, out_length, error);
}

tp_status tp_format_compile_proto_encode_request(
    const tp_format_compile_proto_request *request, uint8_t **out_bytes,
    size_t *out_length, tp_error *error) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_length) {
        *out_length = 0U;
    }
    if (!request || !out_bytes || !out_length || !request->format_id ||
        !request->package_path || !request->descriptor_bytes ||
        (request->source_byte_count > 0U && !request->source_bytes)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile request fields are required");
    }
    size_t id_length = 0U;
    size_t path_length = 0U;
    if (!bounded_length(request->format_id, TP_FORMAT_ID_MAX_BYTES,
                        &id_length) ||
        !tp_format_id_is_runtime_token(request->format_id) ||
        !bounded_length(request->package_path,
                        TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES, &path_length) ||
        request->descriptor_byte_count == 0U ||
        request->descriptor_byte_count > TP_FORMAT_DESCRIPTOR_MAX_BYTES ||
        request->source_byte_count > TP_FORMAT_SOURCE_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile request fields exceed their contract");
    }
    tp_status status = validate_text(request->format_id, id_length,
                                     "compile format id", error);
    if (status == TP_STATUS_OK) {
        status = validate_text(request->package_path, path_length,
                               "compile package path", error);
    }
    if (status != TP_STATUS_OK) {
        return status;
    }
    size_t payload = TP_FORMAT_COMPILE_PROTO_REQUEST_FIXED_BYTES;
    if (!add_size(&payload, id_length) || !add_size(&payload, path_length) ||
        !add_size(&payload, request->descriptor_byte_count) ||
        !add_size(&payload, request->source_byte_count) ||
        payload > TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_PAYLOAD_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile request frame exceeds its derived cap");
    }
    status = allocate_frame(TP_FORMAT_COMPILE_PROTO_REQUEST,
                            TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC, payload,
                            out_bytes, out_length, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    compile_writer writer = {*out_bytes, *out_length,
                             TP_FORMAT_COMPILE_PROTO_HEADER_BYTES};
    wr_u32(&writer, request->candidate_index);
    wr_u32(&writer, (uint32_t)id_length);
    wr_u32(&writer, (uint32_t)path_length);
    wr_u32(&writer, (uint32_t)request->descriptor_byte_count);
    wr_u32(&writer, (uint32_t)request->source_byte_count);
    wr_bytes(&writer, request->format_id, id_length);
    wr_bytes(&writer, request->package_path, path_length);
    wr_bytes(&writer, request->descriptor_bytes,
             request->descriptor_byte_count);
    wr_bytes(&writer, request->source_bytes, request->source_byte_count);
    return TP_STATUS_OK;
}

tp_status tp_format_compile_proto_encode_end(
    uint8_t **out_bytes, size_t *out_length, tp_error *error) {
    return encode_empty(TP_FORMAT_COMPILE_PROTO_END,
                        TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC, out_bytes,
                        out_length, error);
}

tp_status tp_format_compile_proto_encode_announce(
    uint32_t candidate_index, uint8_t **out_bytes, size_t *out_length,
    tp_error *error) {
    tp_status status = allocate_frame(
        TP_FORMAT_COMPILE_PROTO_ANNOUNCE,
        TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC, 4U, out_bytes, out_length,
        error);
    if (status == TP_STATUS_OK) {
        compile_writer writer = {*out_bytes, *out_length,
                                 TP_FORMAT_COMPILE_PROTO_HEADER_BYTES};
        wr_u32(&writer, candidate_index);
    }
    return status;
}

static bool compile_diagnostic_code_valid(
    tp_format_diagnostic_code code) {
    switch (code) {
        case TP_FORMAT_DIAGNOSTIC_SOURCE_INVALID_UTF8:
        case TP_FORMAT_DIAGNOSTIC_SOURCE_BINARY:
        case TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR:
        case TP_FORMAT_DIAGNOSTIC_MEMORY_LIMIT:
        case TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED: return true;
        default: return false;
    }
}

static bool optional_shape(const char *text, size_t maximum,
                           uint32_t *out_length) {
    if (!text) {
        *out_length = UINT32_MAX;
        return true;
    }
    size_t length = 0U;
    if (!bounded_length(text, maximum, &length)) {
        return false;
    }
    *out_length = (uint32_t)length;
    return true;
}

static tp_status measure_diagnostic(const tp_format_diagnostic *diagnostic,
                                    diagnostic_shape *out,
                                    tp_error *error) {
    memset(out, 0, sizeof *out);
    if (!tp_format_diagnostic_semantics_valid_internal(diagnostic) ||
        !compile_diagnostic_code_valid(diagnostic->code) ||
        (diagnostic->code ==
             TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED &&
         !tp_format_diagnostic_truncation_marker_canonical_internal(
             diagnostic)) ||
        diagnostic->frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX ||
        (diagnostic->frame_count > 0U && !diagnostic->frames) ||
        !optional_shape(diagnostic->format_id, TP_FORMAT_ID_MAX_BYTES,
                        &out->format_id_length) ||
        !optional_shape(diagnostic->package_path,
                        TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
                        &out->package_path_length) ||
        !optional_shape(diagnostic->message,
                        TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES,
                        &out->message_length)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile result contains an invalid diagnostic");
    }
    if (diagnostic->format_id &&
        !tp_format_id_is_runtime_token(diagnostic->format_id)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile diagnostic format id is invalid");
    }
    const char *texts[3] = {diagnostic->format_id, diagnostic->package_path,
                            diagnostic->message};
    const uint32_t lengths[3] = {out->format_id_length,
                                 out->package_path_length,
                                 out->message_length};
    const char *labels[3] = {"compile diagnostic format id",
                             "compile diagnostic package path",
                             "compile diagnostic message"};
    out->bytes = TP_FORMAT_COMPILE_PROTO_DIAGNOSTIC_FIXED_BYTES;
    for (size_t i = 0U; i < 3U; ++i) {
        if (lengths[i] != UINT32_MAX) {
            tp_status status = validate_text(texts[i], lengths[i], labels[i],
                                             error);
            if (status != TP_STATUS_OK) {
                return status;
            }
            if (!add_size(&out->bytes, lengths[i])) {
                return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                    "compile diagnostic size overflow");
            }
        }
    }
    for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
        const tp_format_diagnostic_frame *frame = &diagnostic->frames[i];
        size_t length = 0U;
        if (frame->line == 0U ||
            !bounded_length(frame->text,
                            TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES, &length) ||
            length == 0U) {
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "compile diagnostic frame is invalid");
        }
        tp_status status = validate_text(frame->text, length,
                                         "compile diagnostic frame", error);
        if (status != TP_STATUS_OK ||
            !add_size(&out->bytes,
                      TP_FORMAT_COMPILE_PROTO_FRAME_FIXED_BYTES + length)) {
            return status != TP_STATUS_OK
                       ? status
                       : tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                      "compile diagnostic frame size overflow");
        }
    }
    return TP_STATUS_OK;
}

static void write_optional(compile_writer *writer, const char *text,
                           uint32_t length) {
    if (length != UINT32_MAX) {
        wr_bytes(writer, text, length);
    }
}

static void write_diagnostic(compile_writer *writer,
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
    write_optional(writer, diagnostic->format_id, shape->format_id_length);
    write_optional(writer, diagnostic->package_path,
                   shape->package_path_length);
    write_optional(writer, diagnostic->message, shape->message_length);
    for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
        size_t length = 0U;
        (void)bounded_length(diagnostic->frames[i].text,
                             TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES, &length);
        wr_u32(writer, diagnostic->frames[i].line);
        wr_u32(writer, (uint32_t)length);
        wr_bytes(writer, diagnostic->frames[i].text, length);
    }
}

tp_status tp_format_compile_proto_encode_result(
    const tp_format_compile_proto_result *result, uint8_t **out_bytes,
    size_t *out_length, tp_error *error) {
    if (out_bytes) {
        *out_bytes = NULL;
    }
    if (out_length) {
        *out_length = 0U;
    }
    if (!result || !out_bytes || !out_length ||
        result->status < TP_STATUS_OK ||
        result->status > TP_STATUS_EXPORT_BUSY ||
        (result->status == TP_STATUS_OK &&
         (!result->available || result->diagnostics)) ||
        (result->status == TP_STATUS_INVALID_ARGUMENT &&
         (result->available || !result->diagnostics)) ||
        (result->status != TP_STATUS_OK &&
         result->status != TP_STATUS_INVALID_ARGUMENT &&
         (result->available || result->diagnostics))) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile result state is inconsistent");
    }
    const size_t diagnostic_count = result->diagnostics
                                        ? tp_format_diagnostic_report_count(
                                              result->diagnostics)
                                        : 0U;
    if (diagnostic_count > TP_FORMAT_DIAGNOSTIC_MAX ||
        (result->status == TP_STATUS_INVALID_ARGUMENT &&
         diagnostic_count == 0U)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile result diagnostic count is invalid");
    }
    diagnostic_shape *shapes = diagnostic_count > 0U
                                   ? (diagnostic_shape *)calloc(
                                         diagnostic_count, sizeof *shapes)
                                   : NULL;
    if (diagnostic_count > 0U && !shapes) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "compile result shape allocation failed");
    }
    size_t payload = TP_FORMAT_COMPILE_PROTO_RESULT_FIXED_BYTES;
    tp_status status = TP_STATUS_OK;
    for (size_t i = 0U; i < diagnostic_count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(result->diagnostics, i);
        if (diagnostic &&
            diagnostic->code ==
                TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED &&
            i + 1U != diagnostic_count) {
            free(shapes);
            return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                "compile truncation marker must be final");
        }
        status = measure_diagnostic(diagnostic, &shapes[i], error);
        if (status != TP_STATUS_OK || !add_size(&payload, shapes[i].bytes) ||
            payload > TP_FORMAT_COMPILE_PROTO_MAX_RESULT_PAYLOAD_BYTES) {
            if (status == TP_STATUS_OK) {
                status = tp_error_set(
                    error, TP_STATUS_OUT_OF_BOUNDS,
                    "compile result frame exceeds its derived cap");
            }
            free(shapes);
            return status;
        }
    }
    status = allocate_frame(TP_FORMAT_COMPILE_PROTO_RESULT,
                            TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC, payload,
                            out_bytes, out_length, error);
    if (status != TP_STATUS_OK) {
        free(shapes);
        return status;
    }
    compile_writer writer = {*out_bytes, *out_length,
                             TP_FORMAT_COMPILE_PROTO_HEADER_BYTES};
    wr_u32(&writer, result->candidate_index);
    wr_u32(&writer, (uint32_t)result->status);
    wr_u32(&writer, result->available ? 1U : 0U);
    wr_u32(&writer, (uint32_t)diagnostic_count);
    for (size_t i = 0U; i < diagnostic_count; ++i) {
        write_diagnostic(&writer,
                         tp_format_diagnostic_report_at(result->diagnostics, i),
                         &shapes[i]);
    }
    free(shapes);
    return TP_STATUS_OK;
}

tp_status tp_format_compile_proto_encode_complete(
    uint8_t **out_bytes, size_t *out_length, tp_error *error) {
    return encode_empty(TP_FORMAT_COMPILE_PROTO_COMPLETE,
                        TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC, out_bytes,
                        out_length, error);
}

static uint16_t header_u16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t header_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

tp_status tp_format_compile_proto_frame_size(
    const uint8_t header[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES],
    bool request_stream, size_t *out_size, tp_error *error) {
    if (!header || !out_size) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile protocol header and output are required");
    }
    *out_size = 0U;
    const uint32_t expected_magic = request_stream
                                        ? TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC
                                        : TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC;
    const uint32_t magic = header_u32(header);
    const uint16_t version = header_u16(header + 4U);
    const uint16_t kind = header_u16(header + 6U);
    const uint32_t payload = header_u32(header + 8U);
    if (magic != expected_magic) {
        return tp_error_set(error, TP_STATUS_BAD_MAGIC,
                            "compile protocol frame magic is invalid");
    }
    if (version != TP_FORMAT_COMPILE_PROTO_VERSION) {
        return tp_error_set(error, TP_STATUS_BAD_VERSION,
                            "compile protocol frame version is invalid");
    }
    size_t maximum = 0U;
    switch ((tp_format_compile_proto_kind)kind) {
        case TP_FORMAT_COMPILE_PROTO_REQUEST:
            if (!request_stream) {
                goto invalid_kind;
            }
            maximum = TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_PAYLOAD_BYTES;
            break;
        case TP_FORMAT_COMPILE_PROTO_END:
            if (!request_stream || payload != 0U) {
                goto invalid_kind;
            }
            maximum = 0U;
            break;
        case TP_FORMAT_COMPILE_PROTO_ANNOUNCE:
            if (request_stream || payload != 4U) {
                goto invalid_kind;
            }
            maximum = 4U;
            break;
        case TP_FORMAT_COMPILE_PROTO_RESULT:
            if (request_stream) {
                goto invalid_kind;
            }
            maximum = TP_FORMAT_COMPILE_PROTO_MAX_RESULT_PAYLOAD_BYTES;
            break;
        case TP_FORMAT_COMPILE_PROTO_COMPLETE:
            if (request_stream || payload != 0U) {
                goto invalid_kind;
            }
            maximum = 0U;
            break;
        default: goto invalid_kind;
    }
    if ((size_t)payload > maximum) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol payload exceeds its frame cap");
    }
    *out_size = TP_FORMAT_COMPILE_PROTO_HEADER_BYTES + (size_t)payload;
    return TP_STATUS_OK;

invalid_kind:
    return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                        "compile protocol frame kind or size is invalid");
}

static tp_status decode_header(const uint8_t *bytes, size_t length,
                               bool request_stream,
                               tp_format_compile_proto_kind *out_kind,
                               compile_reader *out_reader, tp_error *error) {
    if (!bytes || length < TP_FORMAT_COMPILE_PROTO_HEADER_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol frame is truncated");
    }
    size_t expected = 0U;
    tp_status status = tp_format_compile_proto_frame_size(
        bytes, request_stream, &expected, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (expected != length) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol frame length is inconsistent");
    }
    *out_kind = (tp_format_compile_proto_kind)header_u16(bytes + 6U);
    out_reader->bytes = bytes;
    out_reader->length = length;
    out_reader->offset = TP_FORMAT_COMPILE_PROTO_HEADER_BYTES;
    return TP_STATUS_OK;
}

static char *dup_text(const uint8_t *bytes, uint32_t length) {
    char *copy = (char *)malloc((size_t)length + 1U);
    if (!copy) {
        return NULL;
    }
    if (length > 0U) {
        memcpy(copy, bytes, length);
    }
    copy[length] = '\0';
    return copy;
}

static unsigned char *dup_bytes(const uint8_t *bytes, uint32_t length) {
    unsigned char *copy = (unsigned char *)malloc(length > 0U ? length : 1U);
    if (!copy) {
        return NULL;
    }
    if (length > 0U) {
        memcpy(copy, bytes, length);
    }
    return copy;
}

static tp_status decode_request_payload(
    compile_reader *reader, tp_format_compile_proto_request *out,
    tp_error *error) {
    uint32_t id_length = 0U;
    uint32_t path_length = 0U;
    uint32_t descriptor_length = 0U;
    uint32_t source_length = 0U;
    if (!rd_u32(reader, &out->candidate_index) ||
        !rd_u32(reader, &id_length) || !rd_u32(reader, &path_length) ||
        !rd_u32(reader, &descriptor_length) ||
        !rd_u32(reader, &source_length) ||
        id_length == 0U || id_length > TP_FORMAT_ID_MAX_BYTES ||
        path_length > TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES ||
        descriptor_length == 0U ||
        descriptor_length > TP_FORMAT_DESCRIPTOR_MAX_BYTES ||
        source_length > TP_FORMAT_SOURCE_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile request payload fields are invalid");
    }
    const uint8_t *id = NULL;
    const uint8_t *path = NULL;
    const uint8_t *descriptor = NULL;
    const uint8_t *source = NULL;
    if (!rd_ref(reader, id_length, &id) ||
        !rd_ref(reader, path_length, &path) ||
        !rd_ref(reader, descriptor_length, &descriptor) ||
        !rd_ref(reader, source_length, &source) ||
        reader->offset != reader->length) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile request payload is truncated or trailing");
    }
    tp_status status = tp_utf8_validate_text_field(
        id, id_length, "compile format id", error);
    if (status == TP_STATUS_OK) {
        status = tp_utf8_validate_text_field(
            path, path_length, "compile package path", error);
    }
    if (status != TP_STATUS_OK) {
        return status;
    }
    out->format_id = dup_text(id, id_length);
    out->package_path = dup_text(path, path_length);
    out->descriptor_bytes = dup_bytes(descriptor, descriptor_length);
    out->source_bytes = dup_bytes(source, source_length);
    if (!out->format_id || !out->package_path || !out->descriptor_bytes ||
        !out->source_bytes) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "compile request decode allocation failed");
    }
    if (!tp_format_id_is_runtime_token(out->format_id)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile request format id is invalid");
    }
    out->descriptor_byte_count = descriptor_length;
    out->source_byte_count = source_length;
    return TP_STATUS_OK;
}

tp_status tp_format_compile_proto_decode_request_message(
    const uint8_t *bytes, size_t length,
    tp_format_compile_proto_message *out_message, tp_error *error) {
    if (out_message) {
        memset(out_message, 0, sizeof *out_message);
    }
    if (!out_message) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile request message output is required");
    }
    compile_reader reader = {0};
    tp_status status = decode_header(bytes, length, true, &out_message->kind,
                                     &reader, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (out_message->kind == TP_FORMAT_COMPILE_PROTO_END) {
        return TP_STATUS_OK;
    }
    status = decode_request_payload(&reader, &out_message->request, error);
    if (status != TP_STATUS_OK) {
        tp_format_compile_proto_message_free(out_message);
    }
    return status;
}

static tp_status read_optional_text(compile_reader *reader, uint32_t length,
                                    size_t maximum, const char *label,
                                    char **out, tp_error *error) {
    *out = NULL;
    if (length == UINT32_MAX) {
        return TP_STATUS_OK;
    }
    if (length > maximum) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile diagnostic text exceeds its cap");
    }
    const uint8_t *bytes = NULL;
    if (!rd_ref(reader, length, &bytes)) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile diagnostic text is truncated");
    }
    tp_status status = tp_utf8_validate_text_field(bytes, length, label, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    *out = dup_text(bytes, length);
    return *out ? TP_STATUS_OK
                : tp_error_set(error, TP_STATUS_OOM,
                               "compile diagnostic text allocation failed");
}

static tp_status decode_diagnostic(compile_reader *reader,
                                   tp_format_diagnostic *out,
                                   tp_error *error) {
    uint32_t severity = 0U;
    uint32_t code = 0U;
    uint32_t phase = 0U;
    uint32_t frame_count = 0U;
    uint32_t id_length = 0U;
    uint32_t path_length = 0U;
    uint32_t message_length = 0U;
    if (!rd_u32(reader, &severity) || !rd_u32(reader, &code) ||
        !rd_u32(reader, &phase) || !rd_u32(reader, &out->line) ||
        !rd_u32(reader, &out->column) || !rd_u32(reader, &frame_count) ||
        !rd_u32(reader, &id_length) || !rd_u32(reader, &path_length) ||
        !rd_u32(reader, &message_length) ||
        frame_count > TP_FORMAT_DIAGNOSTIC_FRAME_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile diagnostic fixed fields are invalid");
    }
    out->severity = (tp_format_diagnostic_severity)severity;
    out->code = (tp_format_diagnostic_code)code;
    out->phase = (tp_format_diagnostic_phase)phase;
    if (!tp_format_diagnostic_semantics_valid_internal(out) ||
        !compile_diagnostic_code_valid(out->code)) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile diagnostic semantics are invalid");
    }
    char *format_id = NULL;
    char *package_path = NULL;
    char *message = NULL;
    tp_format_diagnostic_frame *frames = NULL;
    tp_status status = read_optional_text(
        reader, id_length, TP_FORMAT_ID_MAX_BYTES,
        "compile diagnostic format id", &format_id, error);
    if (status == TP_STATUS_OK) {
        status = read_optional_text(
            reader, path_length, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES,
            "compile diagnostic package path", &package_path, error);
    }
    if (status == TP_STATUS_OK) {
        status = read_optional_text(
            reader, message_length, TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES,
            "compile diagnostic message", &message, error);
    }
    if (status == TP_STATUS_OK && format_id &&
        !tp_format_id_is_runtime_token(format_id)) {
        status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                              "compile diagnostic format id is invalid");
    }
    if (status == TP_STATUS_OK && frame_count > 0U) {
        frames = (tp_format_diagnostic_frame *)calloc(frame_count,
                                                       sizeof *frames);
        if (!frames) {
            status = tp_error_set(error, TP_STATUS_OOM,
                                  "compile diagnostic frames allocation failed");
        }
    }
    for (uint32_t i = 0U; status == TP_STATUS_OK && i < frame_count; ++i) {
        uint32_t text_length = 0U;
        if (!rd_u32(reader, &frames[i].line) ||
            !rd_u32(reader, &text_length) ||
            frames[i].line == 0U || text_length == 0U ||
            text_length > TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES) {
            status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                  "compile diagnostic frame is invalid");
            break;
        }
        char *text = NULL;
        status = read_optional_text(reader, text_length,
                                    TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES,
                                    "compile diagnostic frame", &text, error);
        frames[i].text = text;
    }
    if (status == TP_STATUS_OK) {
        tp_format_diagnostic complete = *out;
        complete.format_id = format_id;
        complete.package_path = package_path;
        complete.message = message;
        complete.frames = frames;
        complete.frame_count = frame_count;
        if (complete.code != TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED ||
            tp_format_diagnostic_truncation_marker_canonical_internal(
                &complete)) {
            *out = complete;
            return TP_STATUS_OK;
        }
        status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                              "compile truncation marker is invalid");
    }
    free(format_id);
    free(package_path);
    free(message);
    if (frames) {
        for (uint32_t i = 0U; i < frame_count; ++i) {
            free((void *)frames[i].text);
        }
    }
    free(frames);
    return status;
}

static void free_temporary_diagnostic(tp_format_diagnostic *diagnostic) {
    free((void *)diagnostic->format_id);
    free((void *)diagnostic->package_path);
    free((void *)diagnostic->message);
    if (diagnostic->frames) {
        for (size_t i = 0U; i < diagnostic->frame_count; ++i) {
            free((void *)diagnostic->frames[i].text);
        }
    }
    free((void *)diagnostic->frames);
    memset(diagnostic, 0, sizeof *diagnostic);
}

static tp_status decode_result_payload(
    compile_reader *reader, tp_format_compile_proto_result *out,
    tp_error *error) {
    uint32_t status_value = 0U;
    uint32_t available = 0U;
    uint32_t diagnostic_count = 0U;
    if (!rd_u32(reader, &out->candidate_index) ||
        !rd_u32(reader, &status_value) || !rd_u32(reader, &available) ||
        !rd_u32(reader, &diagnostic_count) || available > 1U ||
        status_value > TP_STATUS_EXPORT_BUSY ||
        diagnostic_count > TP_FORMAT_DIAGNOSTIC_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile result fixed fields are invalid");
    }
    out->status = (tp_status)status_value;
    out->available = available != 0U;
    if ((out->status == TP_STATUS_OK &&
         (!out->available || diagnostic_count != 0U)) ||
        (out->status == TP_STATUS_INVALID_ARGUMENT &&
         (out->available || diagnostic_count == 0U)) ||
        (out->status != TP_STATUS_OK &&
         out->status != TP_STATUS_INVALID_ARGUMENT &&
         (out->available || diagnostic_count != 0U))) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile result state is inconsistent");
    }
    tp_format_diagnostic_report *report = NULL;
    tp_status status = TP_STATUS_OK;
    if (diagnostic_count > 0U) {
        status = tp_format_diagnostic_report_create_internal(&report, error);
    }
    bool marker_seen = false;
    for (uint32_t i = 0U; status == TP_STATUS_OK && i < diagnostic_count; ++i) {
        tp_format_diagnostic diagnostic = {0};
        status = decode_diagnostic(reader, &diagnostic, error);
        if (status == TP_STATUS_OK && marker_seen) {
            status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                  "compile result has diagnostics after truncation marker");
        } else if (status == TP_STATUS_OK &&
                   diagnostic.code ==
                       TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
            if (i + 1U != diagnostic_count ||
                !tp_format_diagnostic_truncation_marker_canonical_internal(
                    &diagnostic)) {
                status = tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                                      "compile truncation marker is invalid");
            } else {
                status = tp_format_diagnostic_report_mark_truncated_internal(
                    report, diagnostic.phase, error);
                marker_seen = status == TP_STATUS_OK;
            }
        } else if (status == TP_STATUS_OK) {
            status = tp_format_diagnostic_report_append_internal(
                report, &diagnostic, error);
            if (status == TP_STATUS_OK &&
                tp_format_diagnostic_report_truncated(report)) {
                status = tp_error_set(
                    error, TP_STATUS_OUT_OF_BOUNDS,
                    "compile result diagnostics exceed their owned-report cap");
            }
        }
        free_temporary_diagnostic(&diagnostic);
    }
    if (status == TP_STATUS_OK && reader->offset != reader->length) {
        status = tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                              "compile result payload has trailing bytes");
    }
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(report);
        return status;
    }
    out->diagnostics = report;
    return TP_STATUS_OK;
}

tp_status tp_format_compile_proto_decode_response_message(
    const uint8_t *bytes, size_t length,
    tp_format_compile_proto_message *out_message, tp_error *error) {
    if (out_message) {
        memset(out_message, 0, sizeof *out_message);
    }
    if (!out_message) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile response message output is required");
    }
    compile_reader reader = {0};
    tp_status status = decode_header(bytes, length, false, &out_message->kind,
                                     &reader, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (out_message->kind == TP_FORMAT_COMPILE_PROTO_COMPLETE) {
        return TP_STATUS_OK;
    }
    if (out_message->kind == TP_FORMAT_COMPILE_PROTO_ANNOUNCE) {
        if (!rd_u32(&reader, &out_message->candidate_index) ||
            reader.offset != reader.length) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "compile announce payload is invalid");
        }
        return TP_STATUS_OK;
    }
    status = decode_result_payload(&reader, &out_message->result, error);
    if (status != TP_STATUS_OK) {
        tp_format_compile_proto_message_free(out_message);
    }
    return status;
}

void tp_format_compile_proto_message_free(
    tp_format_compile_proto_message *message) {
    if (!message) {
        return;
    }
    if (message->kind == TP_FORMAT_COMPILE_PROTO_REQUEST) {
        free((void *)message->request.format_id);
        free((void *)message->request.package_path);
        free((void *)message->request.descriptor_bytes);
        free((void *)message->request.source_bytes);
    } else if (message->kind == TP_FORMAT_COMPILE_PROTO_RESULT) {
        tp_format_diagnostic_report_destroy(
            (tp_format_diagnostic_report *)message->result.diagnostics);
    }
    memset(message, 0, sizeof *message);
}
