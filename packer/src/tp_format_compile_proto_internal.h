#ifndef TP_CORE_SRC_TP_FORMAT_COMPILE_PROTO_INTERNAL_H
#define TP_CORE_SRC_TP_FORMAT_COMPILE_PROTO_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"

#define TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC 0x51435450U /* "PTCQ" */
#define TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC 0x52435450U /* "PTCR" */
#define TP_FORMAT_COMPILE_PROTO_VERSION 1U

#define TP_FORMAT_COMPILE_PROTO_MAX_FRAMES 1024U
#define TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES 83886080U
#define TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES 16777216U
#define TP_FORMAT_COMPILE_PROTO_MAX_PROCESSES 65U

#define TP_FORMAT_COMPILE_PROTO_HEADER_BYTES 12U
#define TP_FORMAT_COMPILE_PROTO_REQUEST_FIXED_BYTES 20U
#define TP_FORMAT_COMPILE_PROTO_RESULT_FIXED_BYTES 16U
#define TP_FORMAT_COMPILE_PROTO_DIAGNOSTIC_FIXED_BYTES 36U
#define TP_FORMAT_COMPILE_PROTO_FRAME_FIXED_BYTES 8U
#define TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_PAYLOAD_BYTES                     \
    (TP_FORMAT_COMPILE_PROTO_REQUEST_FIXED_BYTES + TP_FORMAT_ID_MAX_BYTES +  \
     TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + TP_FORMAT_DESCRIPTOR_MAX_BYTES +  \
     TP_FORMAT_SOURCE_MAX_BYTES)
#define TP_FORMAT_COMPILE_PROTO_MAX_DIAGNOSTIC_BYTES                         \
    (TP_FORMAT_COMPILE_PROTO_DIAGNOSTIC_FIXED_BYTES +                        \
     TP_FORMAT_ID_MAX_BYTES + TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES +          \
     TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES +                                \
     TP_FORMAT_DIAGNOSTIC_FRAME_MAX *                                        \
         (TP_FORMAT_COMPILE_PROTO_FRAME_FIXED_BYTES +                        \
          TP_FORMAT_DIAGNOSTIC_FRAME_MAX_BYTES))
#define TP_FORMAT_COMPILE_PROTO_MAX_RESULT_PAYLOAD_BYTES                     \
    (TP_FORMAT_COMPILE_PROTO_RESULT_FIXED_BYTES +                            \
     TP_FORMAT_DIAGNOSTIC_MAX * TP_FORMAT_COMPILE_PROTO_MAX_DIAGNOSTIC_BYTES)
#define TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_PAYLOAD_BYTES                   \
    TP_FORMAT_COMPILE_PROTO_MAX_RESULT_PAYLOAD_BYTES

typedef enum tp_format_compile_proto_kind {
    TP_FORMAT_COMPILE_PROTO_REQUEST = 1,
    TP_FORMAT_COMPILE_PROTO_END = 2,
    TP_FORMAT_COMPILE_PROTO_ANNOUNCE = 3,
    TP_FORMAT_COMPILE_PROTO_RESULT = 4,
    TP_FORMAT_COMPILE_PROTO_COMPLETE = 5,
} tp_format_compile_proto_kind;

typedef struct tp_format_compile_proto_request {
    uint32_t candidate_index;
    const char *format_id;
    const char *package_path;
    const unsigned char *descriptor_bytes;
    size_t descriptor_byte_count;
    const unsigned char *source_bytes;
    size_t source_byte_count;
} tp_format_compile_proto_request;

typedef struct tp_format_compile_proto_result {
    uint32_t candidate_index;
    tp_status status;
    bool available;
    const tp_format_diagnostic_report *diagnostics;
} tp_format_compile_proto_result;

typedef struct tp_format_compile_proto_message {
    tp_format_compile_proto_kind kind;
    union {
        tp_format_compile_proto_request request;
        uint32_t candidate_index;
        tp_format_compile_proto_result result;
    };
} tp_format_compile_proto_message;

tp_status tp_format_compile_proto_encode_request(
    const tp_format_compile_proto_request *request, uint8_t **out_bytes,
    size_t *out_length, tp_error *error);
tp_status tp_format_compile_proto_encode_end(
    uint8_t **out_bytes, size_t *out_length, tp_error *error);
tp_status tp_format_compile_proto_encode_announce(
    uint32_t candidate_index, uint8_t **out_bytes, size_t *out_length,
    tp_error *error);
tp_status tp_format_compile_proto_encode_result(
    const tp_format_compile_proto_result *result, uint8_t **out_bytes,
    size_t *out_length, tp_error *error);
tp_status tp_format_compile_proto_encode_complete(
    uint8_t **out_bytes, size_t *out_length, tp_error *error);

/* Decode exactly one complete frame. Request-side decode accepts REQUEST/END;
 * response-side decode accepts ANNOUNCE/RESULT/COMPLETE. Successful REQUEST
 * and RESULT decodes own their dynamic values until message_free. */
tp_status tp_format_compile_proto_decode_request_message(
    const uint8_t *bytes, size_t length,
    tp_format_compile_proto_message *out_message, tp_error *error);
tp_status tp_format_compile_proto_decode_response_message(
    const uint8_t *bytes, size_t length,
    tp_format_compile_proto_message *out_message, tp_error *error);
void tp_format_compile_proto_message_free(
    tp_format_compile_proto_message *message);

/* Validate a 12-byte header before frame-sized allocation. */
tp_status tp_format_compile_proto_frame_size(
    const uint8_t header[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES],
    bool request_stream, size_t *out_size, tp_error *error);

#endif /* TP_CORE_SRC_TP_FORMAT_COMPILE_PROTO_INTERNAL_H */
