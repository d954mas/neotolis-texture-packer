#include "tp_format_compile_worker_internal.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_format_catalog_internal.h"
#include "tp_format_compile_proto_internal.h"
#include "tp_format_diagnostic_internal.h"
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
                            tp_error *error) {
    if (attempt->frame_count >= TP_FORMAT_COMPILE_PROTO_MAX_FRAMES ||
        attempt->request_bytes >
            TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES ||
        frame_bytes > TP_FORMAT_COMPILE_PROTO_MAX_REQUEST_STREAM_BYTES -
                          attempt->request_bytes) {
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile validation request budget exhausted");
        return false;
    }
    attempt->frame_count++;
    attempt->request_bytes += frame_bytes;
    return true;
}

static bool charge_source_bytes(compile_attempt *attempt,
                                size_t source_bytes, tp_error *error) {
    if (attempt->source_bytes > TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX ||
        source_bytes > TP_FORMAT_CATALOG_PACKAGE_BYTES_MAX -
                           attempt->source_bytes) {
        (void)tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                           "compile validation work budget exhausted");
        return false;
    }
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
    tp_error *error) {
    if (!budget) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile test budget is required");
    }
    compile_attempt attempt = test_budget_to_attempt(budget);
    if (!reserve_request(&attempt, frame_bytes, error)) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    test_budget_from_attempt(budget, &attempt);
    return TP_STATUS_OK;
}

tp_status tp_format_compile_worker__test_charge_source_bytes(
    tp_format_compile_worker_test_budget *budget, size_t source_bytes,
    tp_error *error) {
    if (!budget) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "compile test budget is required");
    }
    compile_attempt attempt = test_budget_to_attempt(budget);
    if (!charge_source_bytes(&attempt, source_bytes, error)) {
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

static tp_status classify_request_channel_failure(
    compile_process *process, int timeout_ms, const char *message,
    tp_error *error) {
    /* A worker can consume the frame header, fail its frame allocation, and
     * exit while the parent still has a backpressured payload write in flight.
     * Reap that terminal state before deciding whether this is an ordinary
     * unavailable worker or the process-wide OOM sentinel. */
    const double grace_deadline =
        compile_now_ms() + (timeout_ms < 1000 ? timeout_ms : 1000);
    while (!process->finished && compile_now_ms() < grace_deadline) {
        bool finished = false;
        if (!tp_proc_wait_slice(process->proc, TP_FORMAT_COMPILE_POLL_MS,
                                &process->result, &finished)) {
            break;
        }
        process->finished = finished;
    }
    if (process->finished &&
        process->result.how == TP_PROC_END_EXITED &&
        process->result.code == TP_FORMAT_COMPILE_WORKER_EXIT_OOM) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "compile worker host allocation failed");
    }
    return tp_error_set(
        error,
        process->finished && process->result.how == TP_PROC_END_EXITED &&
                process->result.code == 0
            ? TP_STATUS_BUILDER_FAILED
            : TP_STATUS_BUILDER_CRASHED,
        "%s", message);
}

static tp_status send_request_frame(compile_process *process,
                                    compile_attempt *attempt,
                                    const uint8_t *bytes, size_t length,
                                    int timeout_ms,
                                    tp_error *error) {
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
            return classify_request_channel_failure(
                process, timeout_ms,
                "compile worker request channel write failed", error);
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
            size_t completed = 0U;
            if (!tp_proc_poll_pending_stdin_write(
                    process->proc, &completed, NULL)) {
                return classify_request_channel_failure(
                    process, timeout_ms,
                    "compile worker request completion poll failed", error);
            }
            if (completed > length - offset) {
                return tp_error_set(
                    error, TP_STATUS_BUILDER_FAILED,
                    "compile worker request completion is out of bounds");
            }
            offset += completed;
            if (offset == length) {
                return TP_STATUS_OK;
            }
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
            return classify_request_channel_failure(
                process, timeout_ms,
                "compile worker exited while receiving a request", error);
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
    const char *source_path, tp_format_compile_row_result *out,
    const char *message, tp_error *error) {
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
        .package_path = source_path,
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

static tp_status logical_source_path(
    const char *package_path,
    char out[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U], tp_error *error) {
    const int written = snprintf(
        out, TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U, "%s/export.lua",
        package_path ? package_path : "");
    if (!package_path || written < 0 ||
        (size_t)written > TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES) {
        return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                            "compile logical source path exceeds its bound");
    }
    return TP_STATUS_OK;
}

static bool diagnostics_match_candidate(
    const tp_format_diagnostic_report *report, const char *format_id,
    const char *source_path) {
    const size_t count = tp_format_diagnostic_report_count(report);
    bool saw_attributed_diagnostic = false;
    for (size_t i = 0U; i < count; ++i) {
        const tp_format_diagnostic *diagnostic =
            tp_format_diagnostic_report_at(report, i);
        if (!diagnostic) {
            return false;
        }
        if (diagnostic->code ==
            TP_FORMAT_DIAGNOSTIC_DIAGNOSTICS_TRUNCATED) {
            if (!tp_format_diagnostic_truncation_marker_canonical_internal(
                    diagnostic)) {
                return false;
            }
            continue;
        }
        if (!diagnostic->format_id || !diagnostic->package_path ||
            strcmp(diagnostic->format_id, format_id) != 0 ||
            strcmp(diagnostic->package_path, source_path) != 0) {
            return false;
        }
        saw_attributed_diagnostic = true;
    }
    return saw_attributed_diagnostic;
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

static tp_status complete_scan_with_results(
    tp_format_catalog_scan *scan, tp_format_compile_row_result *results,
    size_t result_count, tp_error *error) {
    const tp_status status =
        tp_format_catalog_scan_complete_compile_internal(
            scan, results, result_count, error);
    destroy_results(results, result_count);
    return status;
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
    int timeout_ms = TP_FORMAT_COMPILE_TIMEOUT_MS;
#ifdef TP_ENABLE_TEST_SEAMS
    if (options && options->timeout_ms > 0) {
        timeout_ms = options->timeout_ms;
    }
#endif
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
    for (size_t index = 0U; index < candidate_count;) {
        tp_format_compile_candidate candidate = {0};
        if (!tp_format_catalog_scan_compile_at(scan, index, &candidate)) {
            return fail_global(scan, results, result_count, &process,
                               TP_STATUS_INVALID_ARGUMENT, error,
                               "compile candidate snapshot is missing");
        }
        char source_path[TP_FORMAT_DIAGNOSTIC_PATH_MAX_BYTES + 1U];
        tp_status status =
            logical_source_path(candidate.package_path, source_path, error);
        if (status != TP_STATUS_OK) {
            return fail_global(scan, results, result_count, &process, status,
                               error,
                               "compile logical source path is invalid");
        }
        tp_format_compile_proto_request request = {
            .candidate_index = candidate.candidate_index,
            .format_id = candidate.descriptor->id,
            .package_path = source_path,
            .descriptor_bytes = candidate.descriptor_bytes,
            .descriptor_byte_count = candidate.descriptor_byte_count,
            .source_bytes = candidate.source_bytes,
            .source_byte_count = candidate.source_byte_count,
        };
        uint8_t *encoded = NULL;
        size_t encoded_length = 0U;
        status = tp_format_compile_proto_encode_request(
            &request, &encoded, &encoded_length, error);
        if (status != TP_STATUS_OK ||
            !reserve_request(&attempt, encoded_length, error)) {
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
        if (!charge_source_bytes(&attempt, candidate.source_byte_count,
                                 error)) {
            return fail_global(scan, results, result_count, &process,
                               TP_STATUS_OUT_OF_BOUNDS, error,
                               "compile source work budget exhausted");
        }

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
                &candidate, source_path, &results[result_count], failure,
                error);
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
            index++;
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
        if (message.result.diagnostics &&
            !diagnostics_match_candidate(message.result.diagnostics,
                                         candidate.descriptor->id,
                                         source_path)) {
            tp_format_compile_proto_message_free(&message);
            status = tp_error_set(
                error, TP_STATUS_BUILDER_FAILED,
                "compile worker diagnostic attribution is invalid");
            return fail_global(scan, results, result_count, &process, status,
                               error,
                               "compile worker diagnostic attribution failed");
        }
        results[result_count].candidate_index = candidate.candidate_index;
        results[result_count].available = message.result.available;
        results[result_count].diagnostics =
            (tp_format_diagnostic_report *)message.result.diagnostics;
        message.result.diagnostics = NULL;
        tp_format_compile_proto_message_free(&message);
        result_count++;
        index++;
    }

    uint8_t *end = NULL;
    size_t end_length = 0U;
    tp_status status =
        tp_format_compile_proto_encode_end(&end, &end_length, error);
    if (status != TP_STATUS_OK ||
        !reserve_request(&attempt, end_length, error)) {
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
    return complete_scan_with_results(scan, results, result_count, error);
}
