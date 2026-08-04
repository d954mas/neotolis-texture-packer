#include "tp_format_compile_worker_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_compile_proto_internal.h"
#include "tp_format_descriptor_internal.h"
#include "tp_format_diagnostic_internal.h"
#include "tp_lua_host_internal.h"
#include "tp_proc_internal.h"

#define TP_FORMAT_COMPILE_TIMEOUT_MS (5 * 60 * 1000)
#define TP_FORMAT_COMPILE_POLL_MS 20
#define TP_FORMAT_COMPILE_IO_CHUNK 65536U

typedef struct compile_stream {
    uint8_t *bytes;
    size_t length;
    size_t capacity;
} compile_stream;

typedef struct compile_attempt {
    size_t request_bytes;
    size_t response_bytes;
    size_t source_bytes;
    uint32_t frame_count;
    uint32_t process_count;
} compile_attempt;

typedef struct compile_process {
    tp_proc *proc;
    compile_stream stream;
    tp_proc_result result;
    tp_status failure_status;
    double started_ms;
    bool finished;
    bool stdout_eof;
} compile_process;

typedef enum wait_outcome {
    TP_COMPILE_WAIT_MESSAGE = 0,
    TP_COMPILE_WAIT_TIMEOUT,
    TP_COMPILE_WAIT_ABNORMAL,
    TP_COMPILE_WAIT_GLOBAL_OOM,
    TP_COMPILE_WAIT_CLEAN_EXIT,
    TP_COMPILE_WAIT_PROTOCOL,
    TP_COMPILE_WAIT_IO,
} wait_outcome;

static double compile_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * 1000.0 /
           (double)frequency.QuadPart;
#else
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (double)value.tv_sec * 1000.0 +
           (double)value.tv_nsec / 1000000.0;
#endif
}

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

static bool read_request_frame(uint8_t **out_bytes, size_t *out_length,
                               bool *out_clean_eof) {
    *out_bytes = NULL;
    *out_length = 0U;
    uint8_t header[TP_FORMAT_COMPILE_PROTO_HEADER_BYTES];
    if (!read_exact_file(header, sizeof header, out_clean_eof)) {
        return false;
    }
    size_t frame_size = 0U;
    if (tp_format_compile_proto_frame_size(
            header, true, &frame_size, NULL) != TP_STATUS_OK) {
        return false;
    }
    uint8_t *frame = (uint8_t *)malloc(frame_size);
    if (!frame) {
        return false;
    }
    memcpy(frame, header, sizeof header);
    bool ignored_eof = false;
    if (!read_exact_file(frame + sizeof header, frame_size - sizeof header,
                         &ignored_eof)) {
        free(frame);
        return false;
    }
    *out_bytes = frame;
    *out_length = frame_size;
    return true;
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
        bool clean_eof = false;
        if (!read_request_frame(&owned_frame, &frame_length, &clean_eof)) {
            free(owned_frame);
            return clean_eof ? 13 : 14;
        }
        frame = owned_frame;
    }
}

static void stream_destroy(compile_stream *stream) {
    free(stream->bytes);
    memset(stream, 0, sizeof *stream);
}

static bool stream_append(compile_stream *stream, const uint8_t *bytes,
                          size_t length) {
    if (length > SIZE_MAX - stream->length) {
        return false;
    }
    const size_t needed = stream->length + length;
    if (needed > stream->capacity) {
        size_t capacity = stream->capacity > 0U ? stream->capacity : 4096U;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2U) {
                capacity = needed;
                break;
            }
            capacity *= 2U;
        }
        uint8_t *grown = (uint8_t *)realloc(stream->bytes, capacity);
        if (!grown) {
            return false;
        }
        stream->bytes = grown;
        stream->capacity = capacity;
    }
    memcpy(stream->bytes + stream->length, bytes, length);
    stream->length = needed;
    return true;
}

static tp_status stream_next(compile_stream *stream, compile_attempt *attempt,
                             tp_format_compile_proto_message *out,
                             bool *out_ready, tp_error *error) {
    *out_ready = false;
    if (stream->length < TP_FORMAT_COMPILE_PROTO_HEADER_BYTES) {
        return TP_STATUS_OK;
    }
    size_t frame_size = 0U;
    tp_status status = tp_format_compile_proto_frame_size(
        stream->bytes, false, &frame_size, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (stream->length < frame_size) {
        return TP_STATUS_OK;
    }
    if (attempt->frame_count >= TP_FORMAT_COMPILE_PROTO_MAX_FRAMES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol frame budget exhausted");
    }
    status = tp_format_compile_proto_decode_response_message(
        stream->bytes, frame_size, out, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    attempt->frame_count++;
    memmove(stream->bytes, stream->bytes + frame_size,
            stream->length - frame_size);
    stream->length -= frame_size;
    *out_ready = true;
    return TP_STATUS_OK;
}

static bool reserve_request(compile_attempt *attempt, size_t frame_bytes,
                            size_t source_bytes, tp_error *error) {
    if (attempt->frame_count >= TP_FORMAT_COMPILE_PROTO_MAX_FRAMES ||
        attempt->request_bytes >
            TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES ||
        frame_bytes > TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES -
                          attempt->request_bytes ||
        attempt->source_bytes > TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX ||
        source_bytes > TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX -
                           attempt->source_bytes) {
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile validation request/work budget exhausted");
        return false;
    }
    attempt->frame_count++;
    attempt->request_bytes += frame_bytes;
    attempt->source_bytes += source_bytes;
    return true;
}

static bool charge_response_bytes(compile_attempt *attempt,
                                  size_t byte_count, tp_error *error) {
    if (attempt->response_bytes >
            TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES ||
        byte_count > TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES -
                         attempt->response_bytes) {
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile validation response budget exhausted");
        return false;
    }
    attempt->response_bytes += byte_count;
    return true;
}

#ifdef TP_ENABLE_TEST_SEAMS
static compile_attempt test_budget_to_attempt(
    const tp_format_compile_worker_test_budget *budget) {
    return (compile_attempt){
        .frame_count = budget->frame_count > UINT32_MAX
                           ? UINT32_MAX
                           : (uint32_t)budget->frame_count,
        .request_bytes = budget->request_bytes,
        .response_bytes = budget->response_bytes,
        .source_bytes = budget->source_bytes,
    };
}

static void test_budget_from_attempt(
    tp_format_compile_worker_test_budget *budget,
    const compile_attempt *attempt) {
    budget->frame_count = attempt->frame_count;
    budget->request_bytes = attempt->request_bytes;
    budget->response_bytes = attempt->response_bytes;
    budget->source_bytes = attempt->source_bytes;
}

tp_status tp_format_compile_worker__test_reserve_request(
    tp_format_compile_worker_test_budget *budget, size_t frame_bytes,
    size_t source_bytes, tp_error *error) {
    if (!budget) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile test budget is required");
    }
    compile_attempt attempt = test_budget_to_attempt(budget);
    if (!reserve_request(&attempt, frame_bytes, source_bytes, error)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    test_budget_from_attempt(budget, &attempt);
    return TP_STATUS_OK;
}

tp_status tp_format_compile_worker__test_reserve_response_frame(
    tp_format_compile_worker_test_budget *budget, tp_error *error) {
    if (!budget || budget->frame_count >= TP_FORMAT_COMPILE_PROTO_MAX_FRAMES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile protocol frame budget exhausted");
    }
    budget->frame_count++;
    return TP_STATUS_OK;
}

tp_status tp_format_compile_worker__test_charge_response_bytes(
    tp_format_compile_worker_test_budget *budget, size_t byte_count,
    tp_error *error) {
    if (!budget) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile test budget is required");
    }
    compile_attempt attempt = test_budget_to_attempt(budget);
    if (!charge_response_bytes(&attempt, byte_count, error)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    test_budget_from_attempt(budget, &attempt);
    return TP_STATUS_OK;
}
#endif

static bool pump_stdout(compile_process *process, compile_attempt *attempt,
                        tp_error *error) {
    uint8_t chunk[TP_FORMAT_COMPILE_IO_CHUNK];
    if (attempt->response_bytes >
        TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES) {
        process->failure_status = TP_STATUS_OUT_OF_BOUNDS;
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile validation response budget exhausted");
        return false;
    }
    size_t allowance = TP_FORMAT_COMPILE_PROTO_MAX_RESPONSE_STREAM_BYTES -
                       attempt->response_bytes;
    size_t capacity = sizeof chunk;
    if (allowance < capacity) {
        capacity = allowance + (allowance < sizeof chunk ? 1U : 0U);
    }
    if (capacity == 0U) {
        capacity = 1U;
    }
    size_t read_count = 0U;
    bool eof = false;
    if (!tp_proc_try_read_stdout(process->proc, chunk, capacity, &read_count,
                                 &eof)) {
        process->failure_status = TP_STATUS_BUILDER_FAILED;
        (void)tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                           "compile worker response channel read failed");
        return false;
    }
    if (!charge_response_bytes(attempt, read_count, error)) {
        process->failure_status = TP_STATUS_OUT_OF_BOUNDS;
        return false;
    }
    if (read_count > 0U &&
        !stream_append(&process->stream, chunk, read_count)) {
        process->failure_status = TP_STATUS_OOM;
        (void)tp_error_set(error, TP_STATUS_OOM,
                           "compile response stream allocation failed");
        return false;
    }
    process->stdout_eof = process->stdout_eof || eof;
    return true;
}

static wait_outcome wait_message(
    compile_process *process, compile_attempt *attempt,
    tp_format_compile_proto_kind expected_kind, uint32_t expected_index,
    int timeout_ms, tp_format_compile_proto_message *out, tp_error *error) {
    for (;;) {
        if (!pump_stdout(process, attempt, error)) {
            return process->failure_status == TP_STATUS_OOM
                       ? TP_COMPILE_WAIT_IO
                       : TP_COMPILE_WAIT_PROTOCOL;
        }
        bool ready = false;
        tp_status status = stream_next(&process->stream, attempt, out, &ready,
                                       error);
        if (status != TP_STATUS_OK) {
            process->failure_status = status;
            return TP_COMPILE_WAIT_PROTOCOL;
        }
        if (ready) {
            const bool index_matches =
                (out->kind == TP_FORMAT_COMPILE_PROTO_ANNOUNCE &&
                 out->candidate_index == expected_index) ||
                (out->kind == TP_FORMAT_COMPILE_PROTO_RESULT &&
                 out->result.candidate_index == expected_index) ||
                out->kind == TP_FORMAT_COMPILE_PROTO_COMPLETE;
            if (out->kind != expected_kind || !index_matches) {
                tp_format_compile_proto_message_free(out);
                (void)tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                                   "compile worker response violates the FSM");
                return TP_COMPILE_WAIT_PROTOCOL;
            }
            return TP_COMPILE_WAIT_MESSAGE;
        }
        if (process->stdout_eof && process->stream.length > 0U) {
            (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                               "compile worker response ended in a partial frame");
            return TP_COMPILE_WAIT_PROTOCOL;
        }
        if (!process->finished) {
            bool finished = false;
            if (!tp_proc_wait_slice(process->proc, TP_FORMAT_COMPILE_POLL_MS,
                                    &process->result, &finished)) {
                (void)tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                   "compile worker wait failed");
                return TP_COMPILE_WAIT_IO;
            }
            process->finished = finished;
        }
        if (process->finished && process->stdout_eof) {
            if (process->result.how == TP_PROC_END_EXITED &&
                process->result.code == TP_FORMAT_COMPILE_WORKER_EXIT_OOM) {
                return TP_COMPILE_WAIT_GLOBAL_OOM;
            }
            return process->result.how == TP_PROC_END_ABNORMAL ||
                           process->result.code != 0
                       ? TP_COMPILE_WAIT_ABNORMAL
                       : TP_COMPILE_WAIT_CLEAN_EXIT;
        }
        if (compile_now_ms() - process->started_ms >= (double)timeout_ms) {
            tp_proc_kill(process->proc);
            return process->stream.length == 0U
                       ? TP_COMPILE_WAIT_TIMEOUT
                       : TP_COMPILE_WAIT_PROTOCOL;
        }
    }
}

static void process_destroy(compile_process *process) {
    tp_proc_destroy(process->proc);
    stream_destroy(&process->stream);
    memset(process, 0, sizeof *process);
}

static bool process_spawn(compile_process *process, compile_attempt *attempt,
                          const char *exe, tp_error *error) {
    if (attempt->process_count >= TP_FORMAT_COMPILE_PROTO_MAX_PROCESSES) {
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile worker restart budget exhausted");
        return false;
    }
    memset(process, 0, sizeof *process);
    process->proc =
        tp_proc_spawn_owned_tree(exe, TP_BUILD_WORKER_ARGV1, NULL);
    if (!process->proc) {
        (void)tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                           "compile worker process could not be spawned");
        return false;
    }
    attempt->process_count++;
    process->started_ms = compile_now_ms();
    return true;
}

static tp_status send_request_frame(compile_process *process,
                                    compile_attempt *attempt,
                                    const uint8_t *bytes, size_t length,
                                    int timeout_ms, tp_error *error) {
    if (process->stream.length != 0U) {
        return tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                            "compile worker emitted an unattributed response");
    }
    size_t offset = 0U;
    while (offset < length) {
        size_t consumed = 0U;
        bool would_block = false;
        if (!tp_proc_try_write_stdin(process->proc, bytes + offset,
                                     length - offset, &consumed,
                                     &would_block)) {
            return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                "compile worker request channel write failed");
        }
        offset += consumed;
        if (offset == length) {
            return TP_STATUS_OK;
        }
        if (!would_block && consumed == 0U) {
            return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                "compile worker request channel made no progress");
        }
        if (!pump_stdout(process, attempt, error)) {
            return process->failure_status != TP_STATUS_OK
                       ? process->failure_status
                       : TP_STATUS_BUILDER_FAILED;
        }
        if (process->stream.length != 0U) {
            return tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                                "compile worker responded before a request completed");
        }
        if (!process->finished) {
            bool finished = false;
            if (!tp_proc_wait_slice(process->proc, TP_FORMAT_COMPILE_POLL_MS,
                                    &process->result, &finished)) {
                return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                    "compile worker request wait failed");
            }
            process->finished = finished;
        }
        if (process->finished) {
            return tp_error_set(
                error,
                process->result.how == TP_PROC_END_ABNORMAL ||
                        process->result.code != 0
                    ? TP_STATUS_BUILDER_CRASHED
                    : TP_STATUS_BUILDER_FAILED,
                "compile worker exited while receiving a request");
        }
        if (compile_now_ms() - process->started_ms >= (double)timeout_ms) {
            tp_proc_kill(process->proc);
            return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                "compile worker timed out while receiving a request");
        }
    }
    return TP_STATUS_OK;
}

static tp_status make_worker_failed_result(
    const tp_format_compile_candidate *candidate,
    tp_format_compile_row_result *out, const char *message, tp_error *error) {
    tp_format_diagnostic_report *report = NULL;
    tp_status status =
        tp_format_diagnostic_report_create_internal(&report, error);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const tp_format_diagnostic diagnostic = {
        .severity = TP_FORMAT_DIAGNOSTIC_ERROR,
        .code = TP_FORMAT_DIAGNOSTIC_COMPILE_WORKER_FAILED,
        .phase = TP_FORMAT_PHASE_COMPILE,
        .format_id = candidate->descriptor->id,
        .package_path = candidate->package_path,
        .message = message,
    };
    status = tp_format_diagnostic_report_append_internal(
        report, &diagnostic, error);
    if (status != TP_STATUS_OK) {
        tp_format_diagnostic_report_destroy(report);
        return status;
    }
    out->candidate_index = candidate->candidate_index;
    out->available = false;
    out->diagnostics = report;
    return TP_STATUS_OK;
}

static void destroy_results(tp_format_compile_row_result *results,
                            size_t count) {
    if (!results) {
        return;
    }
    for (size_t i = 0U; i < count; ++i) {
        tp_format_diagnostic_report_destroy(results[i].diagnostics);
    }
    free(results);
}

static tp_status fail_global(tp_format_catalog_scan *scan,
                             tp_format_compile_row_result *results,
                             size_t result_count, compile_process *process,
                             tp_status status, tp_error *error,
                             const char *fallback) {
    if (process && process->proc) {
        process_destroy(process);
    }
    destroy_results(results, result_count);
    tp_format_catalog_scan_invalidate_compile_internal(scan);
    if (status == TP_STATUS_OK) {
        status = TP_STATUS_BUILDER_FAILED;
    }
    if (error && error->msg[0] == '\0') {
        (void)tp_error_set(error, status, "%s", fallback);
    }
    return status;
}

static tp_status verify_complete_exit(compile_process *process,
                                      compile_attempt *attempt,
                                      int timeout_ms, tp_error *error) {
    for (;;) {
        if (!pump_stdout(process, attempt, error)) {
            return TP_STATUS_BUILDER_FAILED;
        }
        if (process->stream.length != 0U) {
            return tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                                "compile worker emitted trailing response bytes");
        }
        if (!process->finished) {
            bool finished = false;
            if (!tp_proc_wait_slice(process->proc, TP_FORMAT_COMPILE_POLL_MS,
                                    &process->result, &finished)) {
                return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                    "compile worker final wait failed");
            }
            process->finished = finished;
        }
        if (process->finished && process->stdout_eof) {
            if (process->result.how != TP_PROC_END_EXITED ||
                process->result.code != 0) {
                return tp_error_set(error, TP_STATUS_BUILDER_FAILED,
                                    "compile worker did not exit cleanly after COMPLETE");
            }
            return TP_STATUS_OK;
        }
        if (compile_now_ms() - process->started_ms >= (double)timeout_ms) {
            tp_proc_kill(process->proc);
            return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                "compile worker timed out after COMPLETE");
        }
    }
}

tp_status tp_format_compile_worker_run(
    tp_format_catalog_scan *scan,
    const tp_format_compile_worker_options *options, tp_error *error) {
    if (error) {
        memset(error, 0, sizeof *error);
    }
    if (!scan || tp_format_catalog_scan_compile_state_internal(scan) !=
                     TP_FORMAT_COMPILE_BATCH_PENDING) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile worker requires a pending catalog scan");
    }
    const size_t candidate_count = tp_format_catalog_scan_compile_count(scan);
    if (candidate_count == 0U || candidate_count > TP_FORMAT_PACKAGE_MAX) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile worker candidate count is invalid");
    }
    char self_path[4096];
    const char *exe = options ? options->worker_exe : NULL;
    if (!exe) {
        if (!tp_proc_self_path(self_path, sizeof self_path)) {
            tp_format_catalog_scan_invalidate_compile_internal(scan);
            return tp_error_set(error, TP_STATUS_BUILDER_CRASHED,
                                "compile worker executable could not be resolved");
        }
        exe = self_path;
    }
    const int timeout_ms = options && options->timeout_ms > 0
                               ? options->timeout_ms
                               : TP_FORMAT_COMPILE_TIMEOUT_MS;
    tp_format_compile_row_result *results =
        (tp_format_compile_row_result *)calloc(candidate_count,
                                                sizeof *results);
    if (!results) {
        tp_format_catalog_scan_invalidate_compile_internal(scan);
        return tp_error_set(error, TP_STATUS_OOM,
                            "compile result table allocation failed");
    }
    compile_attempt attempt = {0};
    compile_process process = {0};
    if (!process_spawn(&process, &attempt, exe, error)) {
        return fail_global(scan, results, 0U, &process,
                           error && strstr(error->msg, "budget")
                               ? TP_STATUS_OUT_OF_BOUNDS
                               : TP_STATUS_BUILDER_CRASHED,
                           error, "compile worker spawn failed");
    }

    size_t result_count = 0U;
    for (size_t index = 0U; index < candidate_count; ++index) {
        tp_format_compile_candidate candidate = {0};
        if (!tp_format_catalog_scan_compile_at(scan, index, &candidate)) {
            return fail_global(scan, results, result_count, &process,
                               TP_STATUS_INVALID_ARGUMENT, error,
                               "compile candidate snapshot is missing");
        }
        tp_format_compile_proto_request request = {
            .candidate_index = candidate.candidate_index,
            .format_id = candidate.descriptor->id,
            .package_path = candidate.package_path,
            .descriptor_bytes = candidate.descriptor_bytes,
            .descriptor_byte_count = candidate.descriptor_byte_count,
            .source_bytes = candidate.source_bytes,
            .source_byte_count = candidate.source_byte_count,
        };
        uint8_t *encoded = NULL;
        size_t encoded_length = 0U;
        tp_status status = tp_format_compile_proto_encode_request(
            &request, &encoded, &encoded_length, error);
        if (status != TP_STATUS_OK ||
            !reserve_request(&attempt, encoded_length,
                             candidate.source_byte_count, error)) {
            free(encoded);
            return fail_global(
                scan, results, result_count, &process,
                status != TP_STATUS_OK ? status : TP_STATUS_OUT_OF_BOUNDS,
                error, "compile request budget failed");
        }
        status = send_request_frame(&process, &attempt, encoded,
                                    encoded_length, timeout_ms, error);
        if (status != TP_STATUS_OK) {
            free(encoded);
            return fail_global(scan, results, result_count, &process,
                               status, error,
                               "compile worker request write failed before ANNOUNCE");
        }
        free(encoded);

        tp_format_compile_proto_message message = {0};
        wait_outcome outcome = wait_message(
            &process, &attempt, TP_FORMAT_COMPILE_PROTO_ANNOUNCE,
            candidate.candidate_index, timeout_ms, &message, error);
        if (outcome != TP_COMPILE_WAIT_MESSAGE) {
            return fail_global(
                scan, results, result_count, &process,
                outcome == TP_COMPILE_WAIT_GLOBAL_OOM
                    ? TP_STATUS_OOM
                    : outcome == TP_COMPILE_WAIT_TIMEOUT ||
                        outcome == TP_COMPILE_WAIT_ABNORMAL
                    ? TP_STATUS_BUILDER_CRASHED
                    : process.failure_status != TP_STATUS_OK
                          ? process.failure_status
                          : TP_STATUS_BUILDER_FAILED,
                error, "compile worker failed before ANNOUNCE");
        }
        tp_format_compile_proto_message_free(&message);

        outcome = wait_message(
            &process, &attempt, TP_FORMAT_COMPILE_PROTO_RESULT,
            candidate.candidate_index, timeout_ms, &message, error);
        if (outcome == TP_COMPILE_WAIT_GLOBAL_OOM) {
            return fail_global(scan, results, result_count, &process,
                               TP_STATUS_OOM, error,
                               "compile worker allocation failed");
        }
        if (outcome == TP_COMPILE_WAIT_TIMEOUT ||
            outcome == TP_COMPILE_WAIT_ABNORMAL) {
            const char *failure = outcome == TP_COMPILE_WAIT_TIMEOUT
                                      ? "compile worker timed out after ANNOUNCE"
                                      : "compile worker crashed after ANNOUNCE";
            process_destroy(&process);
            status = make_worker_failed_result(
                &candidate, &results[result_count], failure, error);
            if (status != TP_STATUS_OK) {
                return fail_global(scan, results, result_count, &process,
                                   status, error,
                                   "compile worker failure diagnostic allocation failed");
            }
            result_count++;
            const bool restart_budget_exhausted =
                attempt.process_count >=
                TP_FORMAT_COMPILE_PROTO_MAX_PROCESSES;
            if (!process_spawn(&process, &attempt, exe, error)) {
                return fail_global(scan, results, result_count, &process,
                                   restart_budget_exhausted
                                       ? TP_STATUS_OUT_OF_BOUNDS
                                       : TP_STATUS_BUILDER_CRASHED,
                                   error, restart_budget_exhausted
                                              ? "compile worker restart budget exhausted"
                                              : "compile worker restart failed");
            }
            continue;
        }
        if (outcome != TP_COMPILE_WAIT_MESSAGE) {
            return fail_global(scan, results, result_count, &process,
                               process.failure_status != TP_STATUS_OK
                                   ? process.failure_status
                                   : TP_STATUS_BUILDER_FAILED,
                               error,
                               "compile worker did not produce a valid RESULT");
        }
        if (message.result.status != TP_STATUS_OK &&
            message.result.status != TP_STATUS_INVALID_ARGUMENT) {
            status = message.result.status;
            tp_format_compile_proto_message_free(&message);
            return fail_global(scan, results, result_count, &process, status,
                               error, "compile worker reported a global failure");
        }
        results[result_count].candidate_index = candidate.candidate_index;
        results[result_count].available = message.result.available;
        results[result_count].diagnostics =
            (tp_format_diagnostic_report *)message.result.diagnostics;
        message.result.diagnostics = NULL;
        tp_format_compile_proto_message_free(&message);
        result_count++;
    }

    uint8_t *end = NULL;
    size_t end_length = 0U;
    tp_status status =
        tp_format_compile_proto_encode_end(&end, &end_length, error);
    if (status != TP_STATUS_OK ||
        !reserve_request(&attempt, end_length, 0U, error)) {
        free(end);
        return fail_global(scan, results, result_count, &process,
                           status != TP_STATUS_OK ? status
                                                  : TP_STATUS_OUT_OF_BOUNDS,
                           error, "compile END budget failed");
    }
    const bool end_written = tp_proc_write_stdin(process.proc, end, end_length);
    free(end);
    if (!end_written) {
        return fail_global(scan, results, result_count, &process,
                           TP_STATUS_BUILDER_FAILED, error,
                           "compile END write failed");
    }
    tp_format_compile_proto_message complete = {0};
    const wait_outcome complete_outcome = wait_message(
        &process, &attempt, TP_FORMAT_COMPILE_PROTO_COMPLETE, 0U, timeout_ms,
        &complete, error);
    if (complete_outcome != TP_COMPILE_WAIT_MESSAGE) {
        return fail_global(scan, results, result_count, &process,
                           TP_STATUS_BUILDER_FAILED, error,
                           "compile worker did not produce COMPLETE");
    }
    tp_format_compile_proto_message_free(&complete);
    status = verify_complete_exit(&process, &attempt, timeout_ms, error);
    process_destroy(&process);
    if (status != TP_STATUS_OK) {
        return fail_global(scan, results, result_count, NULL, status, error,
                           "compile worker completion was not clean");
    }
    status = tp_format_catalog_scan_complete_compile_internal(
        scan, results, result_count, error);
    destroy_results(results, result_count);
    return status;
}
