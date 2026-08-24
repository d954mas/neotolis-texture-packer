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
#include "tp_export_command_report_internal.h"
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

static bool write_completed_export_fragment(
    const tp_job_worker_proto_request *request) {
    const tp_id128 atlas_id = {{1U}};
    const tp_id128 target_id = {{2U}};
    const tp_job_worker_proto_fragment fragment = {
        .request_id = request->request_id,
        .outcome = {
            .kind = TP_EXPORT_COMMAND_OUTCOME_TARGET,
            .atlas_index = 0,
            .atlas_id = atlas_id,
            .atlas_name = "main",
            .report_present = true,
            .dry_run = request->dry_run,
            .input_outcome = TP_EXPORT_INPUT_READY,
            .target_index = 0,
            .target = {
                .id = target_id,
                .exporter_id = "fixture-json",
                .out_path = "C:/out/main",
                .pack_run = -1,
                .writer_outcome = TP_EXPORT_WRITER_SUCCEEDED,
                .ok = true,
                .completed = true,
            },
        },
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    const bool encoded = tp_job_worker_proto_encode_fragment(
                             &fragment, &bytes, &length, NULL) == TP_STATUS_OK;
    const bool wrote = encoded &&
                       fwrite(bytes, 1U, length, stdout) == length &&
                       fflush(stdout) == 0;
    free(bytes);
    return wrote;
}

static bool write_contradictory_export_fragment(
    const tp_job_worker_proto_request *request) {
    const tp_id128 atlas_id = {{1U}};
    const tp_id128 target_id = {{2U}};
    const tp_job_worker_proto_fragment fragment = {
        .request_id = request->request_id,
        .outcome = {
            .kind = TP_EXPORT_COMMAND_OUTCOME_TARGET,
            .atlas_index = 0,
            .atlas_id = atlas_id,
            .atlas_name = "main",
            .report_present = true,
            .dry_run = request->dry_run,
            .input_outcome = TP_EXPORT_INPUT_READY,
            .target_index = 0,
            .target = {
                .id = target_id,
                .exporter_id = "fixture-json",
                .out_path = "C:/out/main",
                .pack_run = -1,
                .writer_outcome = TP_EXPORT_WRITER_FAILED,
                .ok = true,
                .completed = true,
            },
        },
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    const bool encoded = tp_job_worker_proto_encode_fragment(
                             &fragment, &bytes, &length, NULL) == TP_STATUS_OK;
    const bool wrote = encoded &&
                       fwrite(bytes, 1U, length, stdout) == length &&
                       fflush(stdout) == 0;
    free(bytes);
    return wrote;
}

static bool write_export_atlas_fragment(
    const tp_job_worker_proto_request *request, bool report_present,
    bool dry_run) {
    const tp_id128 atlas_id = {{1U}};
    const tp_job_worker_proto_fragment fragment = {
        .request_id = request->request_id,
        .outcome = {
            .kind = TP_EXPORT_COMMAND_OUTCOME_ATLAS,
            .atlas_index = 0,
            .atlas_id = atlas_id,
            .atlas_name = "main",
            .status = TP_STATUS_OK,
            .report_present = report_present,
            .dry_run = dry_run,
            .input_outcome = report_present ? TP_EXPORT_INPUT_READY
                                            : TP_EXPORT_INPUT_NOT_EVALUATED,
        },
    };
    uint8_t *bytes = NULL;
    size_t length = 0U;
    const bool encoded = tp_job_worker_proto_encode_fragment(
                             &fragment, &bytes, &length, NULL) == TP_STATUS_OK;
    const bool wrote = encoded &&
                       fwrite(bytes, 1U, length, stdout) == length &&
                       fflush(stdout) == 0;
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

static bool write_export_phase_prefix(
    const tp_job_worker_proto_request *request,
    tp_job_worker_progress_phase terminal_phase) {
    const tp_job_worker_progress_phase phases[] = {
        TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL,
        TP_JOB_WORKER_PHASE_EXPORT_WRITE,
        TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE,
        TP_JOB_WORKER_PHASE_EXPORT_READY,
        TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN,
    };
    for (size_t i = 0U; i < sizeof phases / sizeof phases[0]; ++i) {
        if (!write_progress(request, 1, phases[i])) {
            return false;
        }
        if (phases[i] == terminal_phase) {
            return fflush(stdout) == 0;
        }
    }
    return false;
}

static bool write_completed_export_fragment_after_phase(
    const tp_job_worker_proto_request *request) {
    return write_export_phase_prefix(
               request, TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN) &&
           write_progress(
               request, 1, TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE) &&
           write_completed_export_fragment(request) && fflush(stdout) == 0;
}

static int wait_for_cancel(const tp_job_worker_proto_request *request,
                           tp_session_job_state state) {
    for (;;) {
        const tp_proc_stdin_event event = tp_proc_child_poll_stdin();
        if (event == TP_PROC_STDIN_EVENT_NONE) {
            sleep_ms(1U);
            continue;
        }
        if (event != TP_PROC_STDIN_EVENT_CANCEL) {
            return 5;
        }
        tp_job_worker_proto_response response = {0};
        response.kind = request->kind;
        response.state = state;
        response.status = state == TP_SESSION_JOB_CANCELLED
                              ? TP_STATUS_CANCELLED
                              : TP_STATUS_OK;
        response.elapsed_ms = 3.0;
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
    } else if (strcmp(mode, "fragment-crash") == 0) {
        result = write_completed_export_fragment_after_phase(&request) ? 7 : 4;
    } else if (strcmp(mode, "published-fragment-crash") == 0) {
        result = write_completed_export_fragment_after_phase(&request)
                     ? 7
                     : 4;
    } else if (strcmp(mode, "premature-fragment") == 0) {
        result = write_export_phase_prefix(
                     &request, TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN) &&
                         write_completed_export_fragment(&request)
                     ? write_terminal(&request, false, false, false)
                     : 4;
    } else if (strcmp(mode, "target-then-erasing-atlas") == 0) {
        result = write_completed_export_fragment_after_phase(&request) &&
                         write_export_atlas_fragment(
                             &request, false, request.dry_run)
                     ? write_terminal(&request, false, false, false)
                     : 4;
    } else if (strcmp(mode, "atlas-dry-mismatch") == 0) {
        result = write_export_atlas_fragment(
                     &request, true, !request.dry_run)
                     ? write_terminal(&request, false, false, false)
                     : 4;
    } else if (strcmp(mode, "reportless-success") == 0) {
        result = write_export_atlas_fragment(
                     &request, false, request.dry_run)
                     ? write_terminal(&request, false, false, false)
                     : 4;
    } else if (strcmp(mode, "contradictory-target") == 0) {
        result = write_export_phase_prefix(
                     &request, TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN) &&
                         write_progress(
                             &request, 1,
                             TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE) &&
                         write_contradictory_export_fragment(&request)
                     ? write_terminal(&request, false, false, false)
                     : 4;
    } else if (strcmp(mode, "malformed") == 0) {
        result = fwrite("not-a-frame", 1U, 11U, stdout) == 11U ? 0 : 4;
    } else if (strcmp(mode, "oversized") == 0) {
        uint8_t header[12] = {
            'P', 'T', 'J', 'R',
            (uint8_t)TP_JOB_WORKER_PROTO_VERSION, 0U, 0U, 0U,
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
        result = wait_for_cancel(&request, TP_SESSION_JOB_CANCELLED);
    } else if (strcmp(mode, "cancel-race-success") == 0) {
        result = wait_for_cancel(&request, TP_SESSION_JOB_SUCCEEDED);
    } else if (strcmp(mode, "invalid-export-transition") == 0) {
        result = write_progress(
                     &request, 1, TP_JOB_WORKER_PHASE_EXPORT_READY) &&
                         fflush(stdout) == 0
                     ? (sleep_ms(5000U), 0)
                     : 4;
    } else if (strcmp(mode, "block-serialize") == 0) {
        result = write_export_phase_prefix(
                     &request, TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE)
                     ? (sleep_ms(5000U), 0)
                     : 4;
    } else if (strcmp(mode, "lua-panic") == 0) {
        result = write_progress(
                     &request, 1, TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL) &&
                         write_progress(
                             &request, 1,
                             TP_JOB_WORKER_PHASE_EXPORT_WRITE) &&
                         write_progress(
                             &request, 1,
                             TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE) &&
                         write_progress(
                             &request, 1,
                             TP_JOB_WORKER_PHASE_EXPORT_HANDLER_PANIC) &&
                         fflush(stdout) == 0
                     ? 7
                     : 4;
    } else if (strcmp(mode, "generic-panic-marker") == 0) {
        result = write_export_phase_prefix(
                     &request, TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE) &&
                         write_progress(
                             &request, 1,
                             TP_JOB_WORKER_PHASE_EXPORT_HANDLER_PANIC) &&
                         fflush(stdout) == 0
                     ? (sleep_ms(5000U), 0)
                     : 4;
    } else if (strcmp(mode, "block-publication") == 0) {
        result = write_export_phase_prefix(
                     &request, TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN)
                     ? (sleep_ms(5000U), 0)
                     : 4;
    } else if (blocked_phase(mode) != 0) {
        result =
            write_progress(&request, 1, blocked_phase(mode)) &&
                    fflush(stdout) == 0
                ? (sleep_ms(5000U), 0)
                : 4;
    } else if (strcmp(mode, "partial-cancel") == 0) {
        result = write_completed_export_fragment_after_phase(&request)
                     ? wait_for_cancel(&request, TP_SESSION_JOB_CANCELLED)
                     : 4;
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
    request.host_pid = 909U;
    request.project_json = project;
    request.project_json_len = sizeof project - 1U;
    request.project_dir = "C:/project";
    request.work_dir = "C:/work";
    request.preview_exporter_id = "";
    return request;
}

static char *test_strdup(const char *text) {
    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    TEST_ASSERT_NOT_NULL(copy);
    memcpy(copy, text, length);
    return copy;
}

static tp_export_command_report *sample_export_report_with_targets(
    bool dry_run, int target_count) {
    tp_export_command_report *report = calloc(1U, sizeof *report);
    TEST_ASSERT_NOT_NULL(report);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_command_report_allocate(report, 1, dry_run, &error));
    tp_export_command_atlas_report *atlas = &report->atlases[0];
    atlas->id = (tp_id128){{1U}};
    atlas->name = test_strdup("main");
    atlas->status = TP_STATUS_BUILDER_FAILED;
    atlas->note = test_strdup("not_attempted_worker_failed");
    atlas->report_present = true;
    atlas->report.dry_run = dry_run;
    atlas->report.input_outcome = TP_EXPORT_INPUT_NOT_EVALUATED;
    atlas->report.target_count = target_count;
    atlas->report.targets = calloc(
        (size_t)target_count, sizeof *atlas->report.targets);
    TEST_ASSERT_NOT_NULL(atlas->report.targets);
    for (int i = 0; i < target_count; ++i) {
        tp_export_report_target *target = &atlas->report.targets[i];
        target->id.bytes[0] = (uint8_t)(i + 2);
        target->exporter_id = test_strdup("fixture-json");
        target->out_path = test_strdup(
            i == 0 ? "C:/out/main" : "C:/out/secondary");
        target->pack_run = -1;
        target->writer_outcome = TP_EXPORT_WRITER_NOT_ATTEMPTED;
        target->error = test_strdup("not_attempted_worker_failed");
    }
    tp_export_command_report_recount(report);
    return report;
}

static tp_export_command_report *sample_export_report(bool dry_run) {
    return sample_export_report_with_targets(dry_run, 1);
}

void test_terminal_failure_belongs_to_atlas_without_final_outcome(void) {
    tp_export_command_report *report = sample_export_report(false);
    report->atlases[0].report.targets[0].completed = true;
    report->atlases[0].report.targets[0].ok = true;
    const tp_error failure = {
        .msg = "worker crashed after its target fragment",
    };
    tp_export_command_report_apply_terminal_failure(
        report, TP_STATUS_BUILDER_CRASHED, &failure);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED,
                          report->atlases[0].status);
    TEST_ASSERT_EQUAL_STRING("worker crashed after its target fragment",
                             report->atlases[0].error.msg);
    TEST_ASSERT_NULL(report->atlases[0].note);
    tp_export_command_report_destroy(report);
    free(report);
}

void test_report_adopts_decoded_target_outcome_ownership(void) {
    tp_export_command_report *report = sample_export_report(false);
    tp_export_command_outcome outcome = {
        .kind = TP_EXPORT_COMMAND_OUTCOME_TARGET,
        .atlas_index = 0,
        .atlas_id = {{1U}},
        .atlas_name = test_strdup("main"),
        .status = TP_STATUS_OK,
        .report_present = true,
        .input_outcome = TP_EXPORT_INPUT_READY,
        .target_index = 0,
        .target = {
            .id = {{2U}},
            .exporter_id = test_strdup("fixture-json"),
            .out_path = test_strdup("C:/out/main"),
            .pack_run = -1,
            .writer_outcome = TP_EXPORT_WRITER_SUCCEEDED,
            .ok = true,
            .completed = true,
        },
    };
    const char *owned_exporter_id = outcome.target.exporter_id;
    const char *owned_out_path = outcome.target.out_path;
    tp_error error = {{0}};

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_export_command_report_apply_outcome(report, &outcome, &error));
    TEST_ASSERT_EQUAL_PTR(
        owned_exporter_id, report->atlases[0].report.targets[0].exporter_id);
    TEST_ASSERT_EQUAL_PTR(
        owned_out_path, report->atlases[0].report.targets[0].out_path);
    TEST_ASSERT_NULL(outcome.target.exporter_id);
    TEST_ASSERT_NULL(outcome.target.out_path);

    tp_export_command_outcome_destroy(&outcome);
    TEST_ASSERT_EQUAL_STRING(
        "fixture-json", report->atlases[0].report.targets[0].exporter_id);
    tp_export_command_report_destroy(report);
    free(report);
}

static tp_job_worker_process *start_process_with_report(
    const char *mode, tp_export_command_report *report) {
    set_worker_mode(mode);
    const tp_job_worker_proto_request request = sample_request();
    tp_job_worker_process *process = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_job_worker_process_start(&request, report, &process, &error));
    TEST_ASSERT_NOT_NULL(process);
    return process;
}

static tp_job_worker_process *start_process(const char *mode) {
    return start_process_with_report(mode, sample_export_report(false));
}

static tp_job_worker_process *start_process_with_request(
    const char *mode, const tp_job_worker_proto_request *request) {
    set_worker_mode(mode);
    tp_job_worker_process *process = NULL;
    tp_error error = {{0}};
    tp_export_command_report *report =
        sample_export_report(request->dry_run);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_job_worker_process_start(request, report, &process, &error));
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

static void pump_to_phase(tp_job_worker_process *process,
                          tp_job_worker_progress_phase phase,
                          int limit_ms) {
    for (int elapsed = 0;
         elapsed < limit_ms &&
         tp_job_worker_process_progress(process).phase != phase &&
         !tp_job_worker_process_terminal(process);
         ++elapsed) {
        tp_job_worker_process_pump(process);
        sleep_ms(1U);
    }
    TEST_ASSERT_EQUAL_INT(
        phase, tp_job_worker_process_progress(process).phase);
}

static tp_export_command_report *take_finalized_export_report(
    tp_job_worker_process *process,
    const tp_job_worker_proto_response *response) {
    bool publication_pending = false;
    tp_export_command_report *report =
        tp_job_worker_process_take_export_report(
            process, &publication_pending);
    tp_export_command_report_finalize(
        report, response->state, publication_pending);
    return report;
}

void test_clean_matching_terminal_is_admitted(void) {
    tp_job_worker_process *process = start_process("success");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, response->status);
    tp_job_worker_process_destroy(process);
}

/* A worker that observed the cancel byte folds CANCELLED into its OWN terminal
 * frame; the process layer admits it unchanged. */
void test_cancel_admitted_before_terminal_owns_outcome(void) {
    tp_job_worker_process *process = start_process("cancel-race");
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_CANCELLED, response->status);
    tp_job_worker_process_destroy(process);
}

/* The other half of that contract, and the one a relabel here would break: a
 * worker that saw the cancel byte and still SUCCEEDED is admitted VERBATIM at
 * this layer. Exactly one place decides that an accepted host cancellation
 * outranks a terminal that raced it -- tp_job.c job_publish_response (covered
 * end to end by test_job_input_token.c) -- and two competing cancel decisions
 * over one outcome is the bug this asserts against. */
void test_successful_terminal_after_cancel_is_admitted_verbatim(void) {
    tp_job_worker_process *process = start_process("cancel-race-success");
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_SUCCEEDED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, response->status);
    tp_job_worker_process_destroy(process);
}

/* A synthesized terminal (here: the cancellation grace expiring on a worker that
 * never answers) carries no worker-measured duration. The host measures it, so
 * the UI never reports a job that "took 0 ms". */
void test_synthesized_terminal_carries_host_measured_elapsed(void) {
    tp_job_worker__test_set_cancel_grace_ms(10);
    tp_job_worker_process *process = start_process("timeout");
    sleep_ms(30U);
    tp_job_worker_process_request_cancel(process);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_CANCELLED, response->state);
    TEST_ASSERT_TRUE_MESSAGE(
        response->elapsed_ms > 0.0,
        "a synthesized terminal must report the host's measured elapsed time");
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
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL_INT(1, report->targets_ok);
    TEST_ASSERT_TRUE(report->atlases[0].report.targets[0].completed);
    TEST_ASSERT_TRUE(report->partial_publication);
    tp_export_command_report_destroy(report);
    free(report);
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

void test_atlas_fragment_cannot_erase_targets_or_change_dry_run(void) {
    tp_job_worker_process *process =
        start_process("target-then-erasing-atlas");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_FAILED, response->status);
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_TRUE(report->atlases[0].report.targets[0].completed);
    TEST_ASSERT_EQUAL_INT(1, report->targets_ok);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);

    assert_worker_failure("atlas-dry-mismatch", TP_STATUS_BUILDER_FAILED);
    assert_worker_failure("reportless-success", TP_STATUS_BUILDER_FAILED);
    assert_worker_failure("contradictory-target", TP_STATUS_BUILDER_FAILED);
}

void test_invalid_export_phase_transition_fails_closed(void) {
    assert_worker_failure("invalid-export-transition",
                          TP_STATUS_BUILDER_FAILED);
}

void test_target_fragment_requires_target_complete_phase(void) {
    tp_job_worker_process *process = start_process("premature-fragment");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_FAILED, response->status);
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_TRUE(report->atlases[0].report.targets[0].completed);
    TEST_ASSERT_FALSE(report->atlases[0].report.targets[0].ok);
    TEST_ASSERT_TRUE(
        report->atlases[0].report.targets[0].publication_uncertain);
    TEST_ASSERT_TRUE(report->publication_uncertain);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
}

static const tp_job_worker_proto_response *timeout_in_phase(
    const char *mode, tp_job_worker_progress_phase phase,
    tp_job_worker_process **out_process) {
    tp_job_worker__test_set_timeout_ms(2000);
    tp_job_worker_process *process = start_process(mode);
    pump_to_phase(process, phase, 1000);
    tp_job_worker__test_set_timeout_ms(1);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    *out_process = process;
    return response;
}

void test_worker_death_is_attributed_to_publication_window(void) {
    tp_job_worker_process *process = NULL;
    const tp_job_worker_proto_response *response =
        timeout_in_phase("block-serialize",
                         TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE,
                         &process);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_FALSE(report->publication_uncertain);
    TEST_ASSERT_FALSE(report->partial_publication);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);

    response = timeout_in_phase(
        "block-publication",
        TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN,
        &process);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    report = take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_TRUE(report->publication_uncertain);
    TEST_ASSERT_TRUE(report->partial_publication);
    const tp_export_report_target *target =
        &report->atlases[0].report.targets[0];
    TEST_ASSERT_TRUE(target->completed);
    TEST_ASSERT_FALSE(target->ok);
    TEST_ASSERT_TRUE(target->publication_uncertain);
    TEST_ASSERT_EQUAL_INT(TP_EXPORT_WRITER_FAILED,
                          target->writer_outcome);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
}

void test_take_export_report_only_transfers_report_ownership(void) {
    tp_job_worker__test_set_timeout_ms(2000);
    tp_job_worker_process *process = start_process_with_report(
        "block-publication", sample_export_report_with_targets(false, 2));
    pump_to_phase(process,
                  TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN,
                  1000);
    tp_job_worker__test_set_timeout_ms(1);
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);

    bool publication_pending = false;
    tp_export_command_report *report =
        tp_job_worker_process_take_export_report(
            process, &publication_pending);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_TRUE(publication_pending);
    TEST_ASSERT_FALSE(report->publication_uncertain);
    TEST_ASSERT_FALSE(report->atlases[0].report.targets[0].completed);
    TEST_ASSERT_FALSE(report->atlases[0].report.targets[1].completed);

    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
}

void test_private_lua_panic_marker_is_not_a_generic_crash(void) {
    tp_job_worker_process *process = start_process("lua-panic");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED, response->status);
    TEST_ASSERT_NOT_NULL(strstr(response->error.msg, "Lua handler panicked"));
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_FALSE(report->publication_uncertain);
    const tp_export_report_target *target =
        &report->atlases[0].report.targets[0];
    TEST_ASSERT_TRUE(target->completed);
    TEST_ASSERT_FALSE(target->ok);
    TEST_ASSERT_EQUAL_INT(TP_EXPORT_WRITER_FAILED,
                          target->writer_outcome);
    TEST_ASSERT_NOT_NULL(strstr(target->error, "Lua handler panicked"));
    TEST_ASSERT_EQUAL_INT(
        TP_FORMAT_DIAGNOSTIC_HANDLER_PANIC,
        tp_format_diagnostic_report_at(target->format_diagnostics, 0U)->code);
    TEST_ASSERT_EQUAL_INT(1, report->targets_failed);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
}

void test_lua_panic_marker_requires_known_lua_serializer(void) {
    assert_worker_failure("generic-panic-marker",
                          TP_STATUS_BUILDER_FAILED);
}

void test_mismatched_terminal_and_trailing_bytes_fail_closed(void) {
    assert_worker_failure("mismatch", TP_STATUS_BUILDER_FAILED);
    assert_worker_failure("trailing", TP_STATUS_BUILDER_FAILED);
}

void test_nonzero_exit_after_terminal_is_not_admitted(void) {
    assert_worker_failure("nonzero", TP_STATUS_BUILDER_CRASHED);
}

void test_worker_crash_retains_latest_completed_export_fragment(void) {
    tp_job_worker_process *process = start_process("fragment-crash");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED, response->status);
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_EQUAL_STRING(
        "main", report->atlases[0].name);
    TEST_ASSERT_TRUE(report->atlases[0].report.targets[0].completed);
    TEST_ASSERT_EQUAL_INT(1, report->targets_ok);
    TEST_ASSERT_TRUE(report->partial_publication);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
}

void test_host_report_adoption_oom_is_not_mislabeled_as_worker_corruption(void) {
    tp_job_worker_process *process =
        start_process("published-fragment-crash");
    tp_export_command_report__test_fail_next_adoption();
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 2000);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM, response->status);
    TEST_ASSERT_NOT_NULL(strstr(response->error.msg, "adoption allocation"));
    tp_export_command_report *report =
        take_finalized_export_report(process, response);
    TEST_ASSERT_NOT_NULL(report);
    TEST_ASSERT_TRUE(report->publication_uncertain);
    TEST_ASSERT_TRUE(report->partial_publication);
    tp_export_command_report_destroy(report);
    free(report);
    tp_job_worker_process_destroy(process);
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

void test_forced_terminal_does_not_wait_for_kill_completion(void) {
    tp_proc__test_reset_destroy_trace();
    tp_proc__test_fail_next_kill();
    tp_job_worker__test_set_timeout_ms(20);
    tp_job_worker_process *process = start_process("timeout");
    const tp_job_worker_proto_response *response =
        pump_to_terminal(process, 200);
    TEST_ASSERT_EQUAL_INT(TP_SESSION_JOB_FAILED, response->state);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_BUILDER_CRASHED, response->status);
    tp_job_worker_process_destroy(process);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(
        2U, tp_proc__test_destroy_kill_calls());
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
    RUN_TEST(test_report_adopts_decoded_target_outcome_ownership);
    RUN_TEST(test_clean_matching_terminal_is_admitted);
    RUN_TEST(test_cancel_admitted_before_terminal_owns_outcome);
    RUN_TEST(test_successful_terminal_after_cancel_is_admitted_verbatim);
    RUN_TEST(test_synthesized_terminal_carries_host_measured_elapsed);
    RUN_TEST(test_more_than_one_pump_budget_of_progress_reaches_terminal);
    RUN_TEST(test_each_blocking_operation_class_is_cancellable);
    RUN_TEST(test_cancel_preserves_partial_export_failure_metadata);
    RUN_TEST(test_cancel_during_request_backpressure_is_bounded);
    RUN_TEST(test_malformed_and_oversized_output_fail_closed);
    RUN_TEST(test_atlas_fragment_cannot_erase_targets_or_change_dry_run);
    RUN_TEST(test_invalid_export_phase_transition_fails_closed);
    RUN_TEST(test_target_fragment_requires_target_complete_phase);
    RUN_TEST(test_worker_death_is_attributed_to_publication_window);
    RUN_TEST(test_take_export_report_only_transfers_report_ownership);
    RUN_TEST(test_terminal_failure_belongs_to_atlas_without_final_outcome);
    RUN_TEST(test_private_lua_panic_marker_is_not_a_generic_crash);
    RUN_TEST(test_lua_panic_marker_requires_known_lua_serializer);
    RUN_TEST(test_mismatched_terminal_and_trailing_bytes_fail_closed);
    RUN_TEST(test_nonzero_exit_after_terminal_is_not_admitted);
    RUN_TEST(test_worker_crash_retains_latest_completed_export_fragment);
    RUN_TEST(
        test_host_report_adoption_oom_is_not_mislabeled_as_worker_corruption);
    RUN_TEST(test_timeout_force_terminates_owned_process);
    RUN_TEST(test_forced_terminal_does_not_wait_for_kill_completion);
    RUN_TEST(test_destroy_live_process_returns_without_waiting_for_child);
    return UNITY_END();
}
