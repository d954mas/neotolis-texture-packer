#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_job_worker_process_internal.h"
#include "tp_proc_internal.h"
#include "unity.h"

#define TEST_WORKER_MODE_ENV "TP_TEST_JOB_WORKER_MODE"

void setUp(void) {}
void tearDown(void) {
    tp_job_worker__test_reset();
#if defined(_WIN32)
    (void)_putenv_s(TEST_WORKER_MODE_ENV, "");
#else
    (void)unsetenv(TEST_WORKER_MODE_ENV);
#endif
}

static void sleep_ms(unsigned milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    const struct timespec delay = {
        .tv_sec = (time_t)(milliseconds / 1000U),
        .tv_nsec = (long)(milliseconds % 1000U) * 1000000L,
    };
    (void)nanosleep(&delay, NULL);
#endif
}

static bool read_exact_stdin(void *out, size_t size) {
    uint8_t *bytes = out;
    size_t offset = 0U;
    while (offset < size) {
#if defined(_WIN32)
        const unsigned chunk =
            size - offset > (size_t)INT_MAX
                ? (unsigned)INT_MAX
                : (unsigned)(size - offset);
        const int count =
            _read(_fileno(stdin), bytes + offset, chunk);
        if (count <= 0) {
            return false;
        }
        offset += (size_t)count;
#else
        const ssize_t count =
            read(STDIN_FILENO, bytes + offset, size - offset);
        if (count <= 0) {
            return false;
        }
        offset += (size_t)count;
#endif
    }
    return true;
}

static uint32_t read_le_u32(const uint8_t *bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint8_t *read_request(size_t *out_length) {
    uint8_t header[12];
    if (!read_exact_stdin(header, sizeof header)) {
        return NULL;
    }
    const size_t payload = (size_t)read_le_u32(header + 8U);
    if (payload > TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES) {
        return NULL;
    }
    uint8_t *bytes = malloc(sizeof header + payload);
    if (!bytes) {
        return NULL;
    }
    memcpy(bytes, header, sizeof header);
    if (!read_exact_stdin(bytes + sizeof header, payload)) {
        free(bytes);
        return NULL;
    }
    *out_length = sizeof header + payload;
    return bytes;
}

static int write_response(
    const tp_job_worker_proto_request *request,
    tp_job_worker_proto_response *response, bool mismatch,
    bool trailing, bool nonzero_exit) {
    response->kind = request->kind;
    response->session_instance_generation =
        request->session_instance_generation;
    response->request_id =
        mismatch ? request->request_id + 1U : request->request_id;
    uint8_t *bytes = NULL;
    size_t length = 0U;
    if (tp_job_worker_proto_encode_response(
            response, &bytes, &length, NULL) != TP_STATUS_OK) {
        return 3;
    }
    const bool wrote =
        fwrite(bytes, 1U, length, stdout) == length &&
        (!trailing || fwrite("x", 1U, 1U, stdout) == 1U) &&
        fflush(stdout) == 0;
    free(bytes);
    return wrote ? (nonzero_exit ? 7 : 0) : 4;
}

static int write_terminal(const tp_job_worker_proto_request *request,
                          bool mismatch, bool trailing,
                          bool nonzero_exit) {
    tp_job_worker_proto_response response = {0};
    response.state = TP_SESSION_JOB_SUCCEEDED;
    response.status = TP_STATUS_OK;
    response.elapsed_ms = 1.0;
    response.export_result.atlases_ok = 1;
    return write_response(
        request, &response, mismatch, trailing, nonzero_exit);
}

static bool write_progress(const tp_job_worker_proto_request *request,
                           int current,
                           tp_job_worker_progress_phase phase) {
    const tp_job_worker_proto_progress progress = {
        .request_id = request->request_id,
        .current = current,
        .total = 32,
        .phase = phase,
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    const bool encoded =
        tp_job_worker_proto_encode_progress(
            &progress, &bytes, &length, NULL) == TP_STATUS_OK;
    const bool wrote =
        encoded && fwrite(bytes, 1U, length, stdout) == length;
    free(bytes);
    return wrote;
}

static tp_job_worker_progress_phase blocked_phase(const char *mode) {
    if (strcmp(mode, "block-traversal") == 0) {
        return TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL;
    }
    if (strcmp(mode, "block-read") == 0) {
        return TP_JOB_WORKER_PHASE_SOURCE_READ;
    }
    if (strcmp(mode, "block-decode") == 0) {
        return TP_JOB_WORKER_PHASE_IMAGE_DECODE;
    }
    if (strcmp(mode, "block-build") == 0) {
        return TP_JOB_WORKER_PHASE_BUILD;
    }
    if (strcmp(mode, "block-write") == 0) {
        return TP_JOB_WORKER_PHASE_EXPORT_WRITE;
    }
    return 0;
}

static int wait_for_cancel(
    const tp_job_worker_proto_request *request, bool partial) {
    for (;;) {
        const tp_proc_stdin_event event = tp_proc_child_poll_stdin();
        if (event == TP_PROC_STDIN_EVENT_NONE) {
            sleep_ms(1U);
            continue;
        }
        if (event != TP_PROC_STDIN_EVENT_CANCEL) {
            return 5;
        }
        if (!partial) {
            return write_terminal(request, false, false, false);
        }
        tp_job_worker_proto_response response = {0};
        response.state = TP_SESSION_JOB_SUCCEEDED;
        response.status = TP_STATUS_OK;
        response.elapsed_ms = 3.0;
        response.export_result.atlases_ok = 1;
        response.export_result.atlases_failed = 1;
        response.export_result.targets = 2;
        response.export_result.files = 5;
        response.export_result.partial_publication = true;
        response.export_result.publication_uncertain = true;
        (void)snprintf(
            response.export_result.first_error,
            sizeof response.export_result.first_error,
            "atlas: writer failed");
        return write_response(
            request, &response, false, false, false);
    }
}

static int run_fake_worker(const char *mode) {
#if defined(_WIN32)
    (void)_setmode(_fileno(stdin), _O_BINARY);
    (void)_setmode(_fileno(stdout), _O_BINARY);
#endif
    if (strcmp(mode, "blocked-input") == 0) {
        sleep_ms(5000U);
        return 0;
    }
    size_t request_length = 0U;
    uint8_t *request_bytes = read_request(&request_length);
    if (!request_bytes) {
        return 2;
    }
    tp_job_worker_proto_request request = {0};
    const tp_status decode_status =
        tp_job_worker_proto_decode_request(
            request_bytes, request_length, &request, NULL);
    free(request_bytes);
    if (decode_status != TP_STATUS_OK) {
        return 2;
    }
    int result = 0;
    if (strcmp(mode, "success") == 0) {
        result = write_terminal(&request, false, false, false);
    } else if (strcmp(mode, "progress-burst") == 0) {
        bool wrote = true;
        for (int current = 1; current <= 32; ++current) {
            wrote = wrote &&
                    write_progress(
                        &request, current,
                        TP_JOB_WORKER_PHASE_SOURCE_READ);
        }
        result =
            wrote ? write_terminal(&request, false, false, false) : 4;
    } else if (strcmp(mode, "mismatch") == 0) {
        result = write_terminal(&request, true, false, false);
    } else if (strcmp(mode, "trailing") == 0) {
        result = write_terminal(&request, false, true, false);
    } else if (strcmp(mode, "nonzero") == 0) {
        result = write_terminal(&request, false, false, true);
    } else if (strcmp(mode, "malformed") == 0) {
        result = fwrite("not-a-frame", 1U, 11U, stdout) == 11U ? 0 : 4;
    } else if (strcmp(mode, "oversized") == 0) {
        uint8_t header[12] = {
            'P', 'T', 'J', 'R', 1U, 0U, 0U, 0U,
            1U, 0U, 0U, 0U,
        };
        const uint32_t oversized =
            (uint32_t)TP_JOB_WORKER_PROTO_MAX_FRAME_BYTES + 1U;
        header[8] = (uint8_t)oversized;
        header[9] = (uint8_t)(oversized >> 8U);
        header[10] = (uint8_t)(oversized >> 16U);
        header[11] = (uint8_t)(oversized >> 24U);
        result = fwrite(header, 1U, sizeof header, stdout) ==
                         sizeof header
                     ? 0
                     : 4;
    } else if (strcmp(mode, "cancel-race") == 0) {
        result = wait_for_cancel(&request, false);
    } else if (blocked_phase(mode) != 0) {
        result =
            write_progress(&request, 1, blocked_phase(mode)) &&
                    fflush(stdout) == 0
                ? (sleep_ms(5000U), 0)
                : 4;
    } else if (strcmp(mode, "partial-cancel") == 0) {
        result = wait_for_cancel(&request, true);
    } else if (strcmp(mode, "timeout") == 0) {
        sleep_ms(5000U);
    } else {
        result = 6;
    }
    tp_job_worker_proto_request_free(&request);
    return result;
}

static void set_worker_mode(const char *mode) {
#if defined(_WIN32)
    TEST_ASSERT_EQUAL_INT(0, _putenv_s(TEST_WORKER_MODE_ENV, mode));
#else
    TEST_ASSERT_EQUAL_INT(0, setenv(TEST_WORKER_MODE_ENV, mode, 1));
#endif
}

static tp_job_worker_proto_request sample_request(void) {
    static const uint8_t project[] = "{}";
    tp_job_worker_proto_request request = {0};
    request.kind = TP_SESSION_JOB_EXPORT;
    request.session_instance_generation = 9U;
    request.request_id = 77U;
    request.project_json = project;
    request.project_json_len = sizeof project - 1U;
    request.project_dir = "C:/project";
    request.work_dir = "C:/work";
    request.preview_exporter_id = "";
    return request;
}

static tp_job_worker_process *start_process(const char *mode) {
    set_worker_mode(mode);
    const tp_job_worker_proto_request request = sample_request();
    tp_job_worker_process *process = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_job_worker_process_start(&request, &process, &error));
    TEST_ASSERT_NOT_NULL(process);
    return process;
}

static tp_job_worker_process *start_process_with_request(
    const char *mode, const tp_job_worker_proto_request *request) {
    set_worker_mode(mode);
    tp_job_worker_process *process = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_job_worker_process_start(request, &process, &error));
    TEST_ASSERT_NOT_NULL(process);
    return process;
}

static const tp_job_worker_proto_response *pump_to_terminal(
    tp_job_worker_process *process, int limit_ms) {
    for (int elapsed = 0;
         elapsed < limit_ms &&
         !tp_job_worker_process_terminal(process);
         ++elapsed) {
        tp_job_worker_process_pump(process);
        sleep_ms(1U);
    }
    TEST_ASSERT_TRUE(tp_job_worker_process_terminal(process));
    const tp_job_worker_proto_response *response =
        tp_job_worker_process_response(process);
    TEST_ASSERT_NOT_NULL(response);
    return response;
}

void test_clean_matching_terminal_is_admitted(void) {
    tp_job_worker_process *process = start_process("success");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, response->status);
    tp_job_worker_process_destroy(process);
}

void test_cancel_admitted_before_terminal_owns_outcome(void) {
    tp_job_worker_process *process = start_process("cancel-race");
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, response->status);
    tp_job_worker_process_destroy(process);
}

void test_more_than_one_pump_budget_of_progress_reaches_terminal(void) {
    tp_job_worker_process *process = start_process("progress-burst");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, response->state);
    const tp_job_worker_proto_progress progress =
        tp_job_worker_process_progress(process);
    TEST_ASSERT_EQUAL_INT(32, progress.current);
    TEST_ASSERT_EQUAL_INT(TP_JOB_WORKER_PHASE_SOURCE_READ, progress.phase);
    tp_job_worker_process_destroy(process);
}

static void assert_blocked_phase_is_cancellable(
    const char *mode, tp_job_worker_progress_phase expected_phase) {
    tp_job_worker_process *process = start_process(mode);
    tp_job_worker_proto_progress progress = {0};
    for (int elapsed = 0; elapsed < 2000 && progress.phase == 0;
         ++elapsed) {
        tp_job_worker_process_pump(process);
        progress = tp_job_worker_process_progress(process);
        sleep_ms(1U);
    }
    TEST_ASSERT_EQUAL_INT(expected_phase, progress.phase);
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, response->status);
    tp_job_worker_process_destroy(process);
}

void test_each_blocking_operation_class_is_cancellable(void) {
    assert_blocked_phase_is_cancellable(
        "block-traversal", TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL);
    assert_blocked_phase_is_cancellable(
        "block-read", TP_JOB_WORKER_PHASE_SOURCE_READ);
    assert_blocked_phase_is_cancellable(
        "block-decode", TP_JOB_WORKER_PHASE_IMAGE_DECODE);
    assert_blocked_phase_is_cancellable(
        "block-build", TP_JOB_WORKER_PHASE_BUILD);
    assert_blocked_phase_is_cancellable(
        "block-write", TP_JOB_WORKER_PHASE_EXPORT_WRITE);
}

void test_cancel_preserves_partial_export_failure_metadata(void) {
    tp_job_worker_process *process = start_process("partial-cancel");
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    TEST_ASSERT_EQUAL_INT(1, response->export_result.atlases_failed);
    TEST_ASSERT_EQUAL_INT(2, response->export_result.targets);
    TEST_ASSERT_EQUAL_INT(5, response->export_result.files);
    TEST_ASSERT_TRUE(response->export_result.partial_publication);
    TEST_ASSERT_TRUE(response->export_result.publication_uncertain);
    TEST_ASSERT_EQUAL_STRING(
        "atlas: writer failed", response->export_result.first_error);
    tp_job_worker_process_destroy(process);
}

void test_cancel_during_request_backpressure_is_bounded(void) {
    const size_t payload_size = 1024U * 1024U;
    uint8_t *payload = malloc(payload_size);
    TEST_ASSERT_NOT_NULL(payload);
    memset(payload, ' ', payload_size);
    tp_job_worker_proto_request request = sample_request();
    request.project_json = payload;
    request.project_json_len = payload_size;
    tp_job_worker_process *process =
        start_process_with_request("blocked-input", &request);
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    tp_job_worker_process_destroy(process);
    free(payload);
}

static void assert_worker_failure(const char *mode,
                                  tp_status expected_status) {
    tp_job_worker_process *process = start_process(mode);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(expected_status, response->status);
    TEST_ASSERT_NOT_EQUAL(0, response->error.msg[0]);
    tp_job_worker_process_destroy(process);
}

void test_malformed_and_oversized_output_fail_closed(void) {
    assert_worker_failure("malformed", TP_STATUS_BUILDER_FAILED);
    assert_worker_failure("oversized", TP_STATUS_BUILDER_FAILED);
}

void test_mismatched_terminal_and_trailing_bytes_fail_closed(void) {
    assert_worker_failure("mismatch", TP_STATUS_BUILDER_FAILED);
    assert_worker_failure("trailing", TP_STATUS_BUILDER_FAILED);
}

void test_nonzero_exit_after_terminal_is_not_admitted(void) {
    assert_worker_failure("nonzero", TP_STATUS_BUILDER_CRASHED);
}

void test_timeout_force_terminates_owned_process(void) {
    tp_job_worker__test_set_timeout_ms(20);
    tp_job_worker_process *process = start_process("timeout");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED, response->status);
    tp_job_worker_process_destroy(process);
}

void test_destroy_live_process_returns_without_waiting_for_child(void) {
    tp_proc__test_reset_destroy_trace();
    const size_t payload_size = 1024U * 1024U;
    uint8_t *payload = malloc(payload_size);
    TEST_ASSERT_NOT_NULL(payload);
    memset(payload, ' ', payload_size);
    tp_job_worker_proto_request request = sample_request();
    request.project_json = payload;
    request.project_json_len = payload_size;
    tp_job_worker_process *process =
        start_process_with_request("blocked-input", &request);
    for (int pump = 0;
         pump < 2000 &&
         !tp_job_worker__test_request_backpressured(process);
         ++pump) {
        tp_job_worker_process_pump(process);
        sleep_ms(1U);
    }
    TEST_ASSERT_TRUE(tp_job_worker__test_request_backpressured(process));
    tp_job_worker_process_destroy(process);
    TEST_ASSERT_GREATER_THAN_UINT(0U,
        tp_proc__test_destroy_kill_calls());
    TEST_ASSERT_EQUAL_UINT(
        0U, tp_proc__test_destroy_blocking_wait_calls());
    free(payload);
}

int main(int argc, char **argv) {
    if (argc >= 2 && argv && argv[1] &&
        strcmp(argv[1], TP_BUILD_WORKER_ARGV1) == 0) {
        const char *mode = getenv(TEST_WORKER_MODE_ENV);
        return mode ? run_fake_worker(mode) : 8;
    }
    UNITY_BEGIN();
    RUN_TEST(test_clean_matching_terminal_is_admitted);
    RUN_TEST(test_cancel_admitted_before_terminal_owns_outcome);
    RUN_TEST(test_more_than_one_pump_budget_of_progress_reaches_terminal);
    RUN_TEST(test_each_blocking_operation_class_is_cancellable);
    RUN_TEST(test_cancel_preserves_partial_export_failure_metadata);
    RUN_TEST(test_cancel_during_request_backpressure_is_bounded);
    RUN_TEST(test_malformed_and_oversized_output_fail_closed);
    RUN_TEST(test_mismatched_terminal_and_trailing_bytes_fail_closed);
    RUN_TEST(test_nonzero_exit_after_terminal_is_not_admitted);
    RUN_TEST(test_timeout_force_terminates_owned_process);
    RUN_TEST(test_destroy_live_process_returns_without_waiting_for_child);
    return UNITY_END();
}
