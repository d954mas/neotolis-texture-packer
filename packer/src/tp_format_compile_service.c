#include "tp_format_compile_worker_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_format_compile_proto_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_lua_host_internal.h"

static bool write_frame_bytes(const uint8_t *bytes, size_t length) {
    return fwrite(bytes, 1U, length, stdout) == length &&
           fflush(stdout) == 0;
}
static bool write_encoded(tp_status status, uint8_t *bytes, size_t length) {
    const bool wrote = status == TP_STATUS_OK &&
                       write_frame_bytes(bytes, length);
    free(bytes);
    return wrote;
}

static void stack_wr_u16(uint8_t **cursor, uint16_t value) {
    *(*cursor)++ = (uint8_t)value;
    *(*cursor)++ = (uint8_t)(value >> 8U);
}

static void stack_wr_u32(uint8_t **cursor, uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i) {
        *(*cursor)++ = (uint8_t)(value >> (i * 8U));
    }
}

/* Global statuses must remain reportable even when the ordinary result encoder
 * cannot allocate. The fixed RESULT has no dynamic fields. */
static bool write_global_result(uint32_t candidate_index, tp_status status) {
    uint8_t frame[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES +
                  TP_FORMAT_COMPILE_PROTO_RESULT_FIXED_BYTES];
    uint8_t *cursor = frame;
    stack_wr_u32(&cursor, TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_VERSION);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_RESULT);
    stack_wr_u32(&cursor, TP_FORMAT_COMPILE_PROTO_RESULT_FIXED_BYTES);
    stack_wr_u32(&cursor, candidate_index);
    stack_wr_u32(&cursor, (uint32_t)status);
    stack_wr_u32(&cursor, 0U);
    stack_wr_u32(&cursor, 0U);
    return write_frame_bytes(frame, sizeof frame);
}

static bool write_fixed_announce(uint32_t candidate_index) {
    uint8_t frame[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES + 4U];
    uint8_t *cursor = frame;
    stack_wr_u32(&cursor, TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_VERSION);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_ANNOUNCE);
    stack_wr_u32(&cursor, 4U);
    stack_wr_u32(&cursor, candidate_index);
    return write_frame_bytes(frame, sizeof frame);
}

static bool write_fixed_complete(void) {
    uint8_t frame[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES];
    uint8_t *cursor = frame;
    stack_wr_u32(&cursor, TP_FORMAT_COMPILE_PROTO_RESPONSE_MAGIC);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_VERSION);
    stack_wr_u16(&cursor, (uint16_t)TP_FORMAT_COMPILE_PROTO_COMPLETE);
    stack_wr_u32(&cursor, 0U);
    return write_frame_bytes(frame, sizeof frame);
}

static uint32_t fixed_rd_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) | ((uint32_t)bytes[3] << 24U);
}

#ifdef TP_ENABLE_TEST_SEAMS
static bool test_action_for(const char *action, uint32_t candidate_index);
#endif

typedef enum request_frame_read {
    TP_REQUEST_FRAME_OK = 0,
    TP_REQUEST_FRAME_CLEAN_EOF,
    TP_REQUEST_FRAME_ERROR,
    TP_REQUEST_FRAME_OOM,
} request_frame_read;

static bool read_exact_file(uint8_t *bytes, size_t length,
                            bool *out_clean_eof) {
    *out_clean_eof = false;
    size_t offset = 0U;
    while (offset < length) {
        const size_t count = fread(bytes + offset, 1U, length - offset, stdin);
        if (count == 0U) {
            *out_clean_eof = feof(stdin) != 0 && offset == 0U;
            return false;
        }
        offset += count;
    }
    return true;
}

static request_frame_read read_request_frame(uint8_t **out_bytes,
                                             size_t *out_length) {
    *out_bytes = NULL;
    *out_length = 0U;
    uint8_t header[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES];
    bool clean_eof = false;
    if (!read_exact_file(header, sizeof header, &clean_eof)) {
        return clean_eof ? TP_REQUEST_FRAME_CLEAN_EOF
                         : TP_REQUEST_FRAME_ERROR;
    }
    size_t frame_size = 0U;
    if (tp_format_compile_proto_frame_size(
            header, true, &frame_size, NULL) != TP_STATUS_OK) {
        return TP_REQUEST_FRAME_ERROR;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    if (test_action_for("oom_read_request_frame", 0U)) {
        return TP_REQUEST_FRAME_OOM;
    }
#endif
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    if (!frame) {
        return TP_REQUEST_FRAME_OOM;
    }
    memcpy(frame, header, sizeof header);
    bool ignored_eof = false;
    if (!read_exact_file(frame + sizeof header, frame_size - sizeof header,
                         &ignored_eof)) {
        free(frame);
        return TP_REQUEST_FRAME_ERROR;
    }
    *out_bytes = frame;
    *out_length = frame_size;
    return TP_REQUEST_FRAME_OK;
}

#ifdef TP_ENABLE_TEST_SEAMS
static bool test_action_for(const char *action, uint32_t candidate_index) {
    const char *selected = getenv("TP_TEST_FORMAT_COMPILE_ACTION");
    if (!selected || strcmp(selected, action) != 0) {
        return false;
    }
    const char *index_text = getenv("TP_TEST_FORMAT_COMPILE_INDEX");
    if (!index_text || index_text[0] == '\0') {
        return true;
    }
    char *end = NULL;
    const unsigned long parsed = strtoul(index_text, &end, 10);
    return end && *end == '\0' && parsed == candidate_index;
}

static void test_hang(void) {
    volatile unsigned int value = 0U;
    for (;;) {
        value++;
    }
}
#endif

static tp_status validate_snapshot(
    const tp_format_compile_proto_request *request,
    tp_format_diagnostic_report **out_report, tp_error *error) {
    tp_format_descriptor_parse_result parsed = {0};
    tp_status status = tp_format_descriptor_v1_parse(
        request->descriptor_bytes, request->descriptor_byte_count, &parsed,
        error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const tp_format_descriptor *descriptor =
        parsed.outcome == TP_FORMAT_DESCRIPTOR_ADMITTED
            ? tp_format_owned_descriptor_view(parsed.owned_descriptor)
            : NULL;
    if (!descriptor || strcmp(descriptor->id, request->format_id) != 0) {
        tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
        return tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                            "compile worker descriptor snapshot is inconsistent");
    }
    tp_format_owned_descriptor_destroy(parsed.owned_descriptor);
    return tp_lua_compile_validate(
        request->source_bytes, request->source_byte_count, request->format_id,
        request->package_path, out_report, error);
}

#ifdef TP_ENABLE_TEST_SEAMS
static tp_status replace_with_hostile_diagnostic(
    const tp_format_compile_proto_request *request,
    tp_format_diagnostic_report **report, bool wrong_id, tp_error *error) {
    tp_format_diagnostic_report *hostile = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&hostile, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_ERROR,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .format_id = wrong_id ? "wrong-diagnostic-id" : request->format_id,
        .package_path = wrong_id ? request->package_path
                                 : "formats/wrong-package/export.lua",
        .message = "hostile compile diagnostic attribution",
    };
    status = tp_format_diagnostic_report_append_internal(
        hostile, &diagnostic, error);
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(hostile);
        return status;
    }
    tp_format_diagnostic_report_destroy(*report);
    *report = hostile;
    return TP_STATUS_OK;
}

static tp_status replace_with_marker_only_diagnostic(
    tp_format_diagnostic_report **report, tp_error *error) {
    tp_format_diagnostic_report *marker_only = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&marker_only, error);
    if (status == TP_STATUS_OK) {
        status = tp_format_diagnostic_report_mark_truncated_internal(
            marker_only, TP_FORMAT_PHASE_COMPILE, error);
    }
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(marker_only);
        return status;
    }
    tp_format_diagnostic_report_destroy(*report);
    *report = marker_only;
    return TP_STATUS_OK;
}
#endif

static int service_request(const tp_format_compile_proto_request *request) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (test_action_for("global_oom_before_announce",
                        request->candidate_index)) {
        return TP_FORMAT_COMPILE_WORKER_EXIT_OOM;
    }
    if (test_action_for("crash_before_announce", request->candidate_index)) {
        abort();
    }
    if (test_action_for("wrong_announce", request->candidate_index)) {
        (void)write_fixed_announce(request->candidate_index + 1U);
        return 2;
    }
#endif
    if (!write_fixed_announce(request->candidate_index)) {
        return 2;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    if (test_action_for("crash_after_announce", request->candidate_index)) {
        abort();
    }
    if (test_action_for("clean_exit_after_announce",
                        request->candidate_index)) {
        exit(0);
    }
    if (test_action_for("hang_after_announce", request->candidate_index)) {
        test_hang();
    }
    if (test_action_for("malformed_after_announce",
                        request->candidate_index)) {
        const uint8_t malformed[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES] = {0};
        (void)write_frame_bytes(malformed, sizeof malformed);
        return 4;
    }
    if (test_action_for("partial_after_announce", request->candidate_index)) {
        const uint8_t partial[5] = {0x50U, 0x54U, 0x43U, 0x52U, 0x01U};
        (void)write_frame_bytes(partial, sizeof partial);
        return 4;
    }
    if (test_action_for("duplicate_announce", request->candidate_index)) {
        (void)write_fixed_announce(request->candidate_index);
    }
#endif
    tp_error error = {{0}};
    tp_format_diagnostic_report *report = NULL;
    const tp_status status = validate_snapshot(request, &report, &error);
#ifdef TP_ENABLE_TEST_SEAMS
    if (report &&
        (test_action_for("wrong_diagnostic_id", request->candidate_index) ||
         test_action_for("wrong_diagnostic_path", request->candidate_index))) {
        const bool wrong_id =
            test_action_for("wrong_diagnostic_id", request->candidate_index);
        const tp_status hostile_status = replace_with_hostile_diagnostic(
            request, &report, wrong_id, &error);
        if (hostile_status != TP_STATUS_OK) {
            tp_format_diagnostic_report_destroy(report);
            return write_global_result(request->candidate_index,
                                       hostile_status)
                       ? 0
                       : 5;
        }
    }
    if (report &&
        test_action_for("marker_only_diagnostic", request->candidate_index)) {
        const tp_status hostile_status =
            replace_with_marker_only_diagnostic(&report, &error);
        if (hostile_status != TP_STATUS_OK) {
            tp_format_diagnostic_report_destroy(report);
            return write_global_result(request->candidate_index,
                                       hostile_status)
                       ? 0
                       : 5;
        }
    }
#endif
    const bool row_rejection = status == TP_STATUS_INVALID_ARGUMENT && report;
    const bool available = status == TP_STATUS_OK && !report;
    if (!available && !row_rejection) {
        tp_format_diagnostic_report_destroy(report);
        return write_global_result(request->candidate_index, status) ? 0 : 5;
    }
    tp_format_compile_proto_result result = {
        .candidate_index = request->candidate_index,
        .status = available ? TP_STATUS_OK : TP_STATUS_INVALID_ARGUMENT,
        .available = available,
        .diagnostics = report,
    };
#ifdef TP_ENABLE_TEST_SEAMS
    if (test_action_for("wrong_result", request->candidate_index)) {
        result.candidate_index++;
    }
#endif
    uint8_t *encoded = NULL;
    size_t encoded_length = 0U;
    const tp_status encode_status = tp_format_compile_proto_encode_result(
        &result, &encoded, &encoded_length, NULL);
    bool wrote = false;
    if (encode_status == TP_STATUS_OK) {
        wrote = write_encoded(encode_status, encoded, encoded_length);
    } else {
        free(encoded);
        wrote = write_global_result(request->candidate_index, encode_status);
    }
#ifdef TP_ENABLE_TEST_SEAMS
    if (wrote && test_action_for("duplicate_result",
                                 request->candidate_index)) {
        uint8_t *duplicate = NULL;
        size_t duplicate_length = 0U;
        const tp_status duplicate_status =
            tp_format_compile_proto_encode_result(
                &result, &duplicate, &duplicate_length, NULL);
        (void)write_encoded(duplicate_status, duplicate, duplicate_length);
    }
#endif
    tp_format_diagnostic_report_destroy(report);
#ifdef TP_ENABLE_TEST_SEAMS
    if (wrote && test_action_for("crash_after_result",
                                 request->candidate_index)) {
        abort();
    }
    if (wrote && test_action_for("clean_exit_after_result",
                                 request->candidate_index)) {
        exit(0);
    }
    if (wrote && test_action_for("hang_after_result",
                                 request->candidate_index)) {
        test_hang();
    }
#endif
    return wrote ? 0 : 6;
}

int tp_format_compile_worker_main_request(const uint8_t *bytes,
                                          size_t length) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    uint8_t *owned_frame = NULL;
    const uint8_t *frame = bytes;
    size_t frame_length = length;
    for (;;) {
        tp_format_compile_proto_message message = {0};
        const tp_status decode_status =
            tp_format_compile_proto_decode_request_message(
                frame, frame_length, &message, NULL);
        if (decode_status != TP_STATUS_OK) {
            if (decode_status == TP_STATUS_OOM &&
                frame_length >= TP_FORMAT_COMPILE_PROTO_HEADER_BYTES + 4U &&
                fixed_rd_u32(frame) ==
                    TP_FORMAT_COMPILE_PROTO_REQUEST_MAGIC) {
                const uint32_t candidate_index = fixed_rd_u32(
                    frame + TP_FORMAT_COMPILE_PROTO_HEADER_BYTES);
                (void)write_fixed_announce(candidate_index);
                (void)write_global_result(candidate_index, TP_STATUS_OOM);
            }
            free(owned_frame);
            return decode_status == TP_STATUS_OOM ? 0 : 10;
        }
        free(owned_frame);
        owned_frame = NULL;
        frame = NULL;
        frame_length = 0U;
        if (message.kind == TP_FORMAT_COMPILE_PROTO_END) {
            tp_format_compile_proto_message_free(&message);
            /* COMPLETE certifies the entire request stream. It is emitted only
             * after the parent closed stdin and no trailing byte exists. */
            const int trailing = fgetc(stdin);
            if (trailing != EOF || !feof(stdin)) {
                return 11;
            }
#ifdef TP_ENABLE_TEST_SEAMS
            if (test_action_for("missing_complete", 0U)) {
                return 0;
            }
#endif
            const bool wrote = write_fixed_complete();
#ifdef TP_ENABLE_TEST_SEAMS
            if (wrote && test_action_for("trailing_after_complete", 0U)) {
                const uint8_t extra = 0x7fU;
                (void)write_frame_bytes(&extra, 1U);
            }
#endif
            return wrote ? 0 : 12;
        }
        const int service_status = service_request(&message.request);
        tp_format_compile_proto_message_free(&message);
        if (service_status != 0) {
            return service_status;
        }
        const request_frame_read read_status =
            read_request_frame(&owned_frame, &frame_length);
        if (read_status != TP_REQUEST_FRAME_OK) {
            free(owned_frame);
            if (read_status == TP_REQUEST_FRAME_OOM) {
                return TP_FORMAT_COMPILE_WORKER_EXIT_OOM;
            }
            return read_status == TP_REQUEST_FRAME_CLEAN_EOF ? 13 : 14;
        }
        frame = owned_frame;
    }
}
