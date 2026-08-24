#include "tp_job_worker_process_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "tp_core/tp_build_worker.h"
#include "tp_proc_internal.h"

#define TP_JOB_WORKER_IO_BUDGET (64U * 1024U)
#define TP_JOB_WORKER_TIMEOUT_MS (5 * 60 * 1000)
/* A cancelled worker must still finish the phase it is in before it can write a
 * terminal frame: the longest such phase is a full source decode, which a large
 * atlas can hold for most of a second. 250 ms made the host kill an obedient
 * worker mid-shutdown and report a grace-period failure instead of its real
 * cancelled terminal. */
#define TP_JOB_WORKER_CANCEL_GRACE_MS 1000

struct tp_job_worker_process {
    tp_proc *proc;
    uint8_t *request;
    size_t request_size;
    size_t request_offset;
    tp_job_worker_proto_stream stream;
    tp_job_worker_proto_progress progress;
    tp_job_worker_proto_response response;
    tp_export_command_report *export_report;
    tp_proc_result process_result;
    tp_session_job_kind kind;
    uint64_t session_instance_generation;
    uint64_t request_id;
    double started_ms;
    double cancel_started_ms;
    bool cancel_requested;
    bool cancel_sent;
    uint8_t cancel_byte;
    bool request_backpressured;
    bool killed;
    bool process_finished;
    bool stdout_eof;
    bool stream_incomplete;
    bool response_ready;
    bool terminal;
    bool export_dry_run;
    tp_job_worker_progress_phase last_export_phase;
    bool export_target_outcome_pending;
    bool export_atlas_complete;
    bool export_publication_pending;
    bool lua_panic_marked;
};

#ifdef TP_ENABLE_TEST_SEAMS
static int s_test_timeout_ms;
static int s_test_cancel_grace_ms;
#endif

static double process_now_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) ||
        frequency.QuadPart == 0) {
        return 0.0;
    }
    return (double)counter.QuadPart * 1000.0 /
           (double)frequency.QuadPart;
#else
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return 0.0;
    }
    return (double)value.tv_sec * 1000.0 +
           (double)value.tv_nsec / 1000000.0;
#endif
}

static int process_timeout_ms(void) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_test_timeout_ms > 0) {
        return s_test_timeout_ms;
    }
#endif
    return TP_JOB_WORKER_TIMEOUT_MS;
}

static int process_cancel_grace_ms(void) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_test_cancel_grace_ms > 0) {
        return s_test_cancel_grace_ms;
    }
#endif
    return TP_JOB_WORKER_CANCEL_GRACE_MS;
}

static void set_terminal_failure(tp_job_worker_process *process,
                                 tp_status status, const char *message) {
    if (process->response_ready) {
        return;
    }
    memset(&process->response, 0, sizeof process->response);
    process->response.kind = process->kind;
    /* A synthesized terminal has no worker-measured duration, and memset left it
     * at 0 -- so every cancelled, killed or crashed job reported "0 ms" to the
     * UI. The host owns the only clock that saw the whole request, so it measures
     * the elapsed time itself. */
    const double elapsed = process_now_ms() - process->started_ms;
    process->response.elapsed_ms = elapsed > 0.0 ? elapsed : 0.0;
    process->response.session_instance_generation =
        process->session_instance_generation;
    process->response.request_id = process->request_id;
    process->response.state =
        process->cancel_requested ? TP_SESSION_JOB_CANCELLED
                                  : TP_SESSION_JOB_FAILED;
    process->response.status =
        process->cancel_requested ? TP_STATUS_CANCELLED : status;
    if (!process->cancel_requested && process->lua_panic_marked) {
        message = "Lua handler panicked in the job worker";
        tp_error report_error = {{0}};
        const tp_status report_status =
            tp_export_command_report_mark_lua_panic(
                process->export_report, &report_error);
        if (report_status != TP_STATUS_OK) {
            process->response.status = report_status;
            message = report_error.msg[0]
                          ? report_error.msg
                          : "Lua panic target attribution failed";
        }
    }
    (void)tp_error_set(
        &process->response.error, process->response.status, "%s",
        message ? message : "job worker failed");
    process->response_ready = true;
    if (process->proc && !process->process_finished &&
        !process->killed) {
        tp_proc_kill(process->proc);
        process->killed = true;
    }
}

static bool export_phase_is_pack_work(
    tp_job_worker_progress_phase phase) {
    switch (phase) {
        case TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL:
        case TP_JOB_WORKER_PHASE_SOURCE_READ:
        case TP_JOB_WORKER_PHASE_IMAGE_DECODE:
        case TP_JOB_WORKER_PHASE_PACK_INPUT:
        case TP_JOB_WORKER_PHASE_BUILD:
            return true;
        default:
            return false;
    }
}

static bool admit_export_phase(tp_job_worker_process *process,
                               tp_job_worker_progress_phase next) {
    const tp_job_worker_progress_phase previous =
        process->last_export_phase;
    if (process->export_atlas_complete &&
        next != TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL &&
        next != TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY) {
        return false;
    }
    bool valid = false;
    switch (next) {
        case TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL:
            valid = previous == 0 ||
                    process->export_atlas_complete;
            break;
        case TP_JOB_WORKER_PHASE_SOURCE_READ:
        case TP_JOB_WORKER_PHASE_IMAGE_DECODE:
        case TP_JOB_WORKER_PHASE_PACK_INPUT:
        case TP_JOB_WORKER_PHASE_BUILD:
            valid = previous == 0 ||
                    export_phase_is_pack_work(previous);
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_WRITE:
            valid = previous == 0 ||
                    export_phase_is_pack_work(previous);
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE:
        case TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE:
            valid = previous == TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_WRITE ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE;
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_READY:
            valid = previous == TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE;
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN:
            valid = previous == TP_JOB_WORKER_PHASE_EXPORT_READY;
            if (valid && !process->export_dry_run) {
                process->export_publication_pending = true;
            }
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE:
            valid = previous == TP_JOB_WORKER_PHASE_EXPORT_WRITE ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_SERIALIZE ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_READY ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_PUBLICATION_BEGIN ||
                    /* A later admitted target can fail before serializer
                     * entry, so two owned completions may be consecutive. */
                    previous == TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE;
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_HANDLER_PANIC:
            valid = previous ==
                    TP_JOB_WORKER_PHASE_EXPORT_LUA_SERIALIZE;
            if (valid) {
                process->lua_panic_marked = true;
            }
            break;
        case TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY:
            valid = process->export_atlas_complete ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE ||
                    previous == TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL ||
                    previous == TP_JOB_WORKER_PHASE_SOURCE_READ ||
                    previous == TP_JOB_WORKER_PHASE_IMAGE_DECODE ||
                    previous == TP_JOB_WORKER_PHASE_PACK_INPUT ||
                    previous == TP_JOB_WORKER_PHASE_BUILD ||
                    previous == TP_JOB_WORKER_PHASE_EXPORT_WRITE;
            break;
        default:
            valid = false;
            break;
    }
    if (valid) {
        process->last_export_phase = next;
        if (next == TP_JOB_WORKER_PHASE_SOURCE_TRAVERSAL) {
            process->export_atlas_complete = false;
        }
        if (next == TP_JOB_WORKER_PHASE_EXPORT_TARGET_COMPLETE) {
            process->export_target_outcome_pending = true;
        } else if (next != TP_JOB_WORKER_PHASE_EXPORT_TERMINAL_BOUNDARY) {
            process->export_target_outcome_pending = false;
        }
    }
    return valid;
}

static void replace_terminal_failure(tp_job_worker_process *process,
                                     tp_status status,
                                     const char *message) {
    tp_job_worker_proto_response_free(&process->response);
    process->response_ready = false;
    set_terminal_failure(process, status, message);
}

static void admit_terminal(tp_job_worker_process *process,
                           tp_job_worker_proto_response *terminal) {
    if (terminal->kind != process->kind ||
        terminal->session_instance_generation !=
            process->session_instance_generation ||
        terminal->request_id != process->request_id) {
        tp_job_worker_proto_response_free(terminal);
        set_terminal_failure(
            process, TP_STATUS_BUILDER_FAILED,
            "job worker terminal identity does not match the request");
        return;
    }
    /* The terminal frame is admitted verbatim. A worker that observed the cancel
     * byte already folds CANCELLED into its own terminal state, and the host
     * owner (tp_job.c job_publish_response) is the ONE place that decides whether
     * an accepted host cancellation outranks a terminal that raced it. Relabelling
     * here as well produced two competing cancel decisions over one outcome. */
    process->response = *terminal;
    memset(terminal, 0, sizeof *terminal);
    process->response_ready = true;
}

tp_status tp_job_worker_process_start(
    const tp_job_worker_proto_request *request,
    tp_export_command_report *export_report,
    tp_job_worker_process **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!request || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "job worker process requires request and output");
    }
    tp_job_worker_process *process = calloc(1U, sizeof *process);
    if (!process) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "job worker process allocation failed");
    }
    tp_status status = tp_job_worker_proto_encode_request(
        request, &process->request, &process->request_size, err);
    if (status != TP_STATUS_OK) {
        free(process);
        return status;
    }
    char executable[4096];
    if (!tp_proc_self_path(executable, sizeof executable)) {
        free(process->request);
        free(process);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker executable path resolution failed");
    }
    process->proc = tp_proc_spawn_owned_tree(
        executable, TP_BUILD_WORKER_ARGV1, NULL);
    if (!process->proc) {
        free(process->request);
        free(process);
        return tp_error_set(err, TP_STATUS_BUILDER_FAILED,
                            "job worker process spawn failed");
    }
    tp_job_worker_proto_stream_init(&process->stream);
    process->kind = request->kind;
    process->session_instance_generation =
        request->session_instance_generation;
    process->request_id = request->request_id;
    process->export_dry_run = request->dry_run;
    process->started_ms = process_now_ms();
    process->cancel_byte = TP_PROC_CANCEL_BYTE;
    process->export_report = export_report;
    *out = process;
    return TP_STATUS_OK;
}

void tp_job_worker_process_request_cancel(tp_job_worker_process *process) {
    if (!process || process->response_ready ||
        process->terminal || process->cancel_requested) {
        return;
    }
    process->cancel_requested = true;
    process->cancel_started_ms = process_now_ms();
}

static void pump_request(tp_job_worker_process *process) {
    if (process->response_ready) {
        return;
    }
    if (process->request_offset >= process->request_size) {
        if (process->cancel_requested && !process->cancel_sent) {
            size_t consumed = 0U;
            bool would_block = false;
            if (!tp_proc_try_write_stdin(
                    process->proc, &process->cancel_byte, 1U,
                    &consumed, &would_block)) {
                set_terminal_failure(
                    process, TP_STATUS_BUILDER_CRASHED,
                    "job worker cancellation channel failed");
            } else if (consumed == 1U) {
                process->cancel_sent = true;
            }
            (void)would_block;
        }
        return;
    }
    const size_t remaining =
        process->request_size - process->request_offset;
    const size_t budget =
        remaining < TP_JOB_WORKER_IO_BUDGET
            ? remaining
            : TP_JOB_WORKER_IO_BUDGET;
    size_t consumed = 0U;
    bool would_block = false;
    if (!tp_proc_try_write_stdin(
            process->proc,
            process->request + process->request_offset, budget,
            &consumed, &would_block)) {
        set_terminal_failure(process, TP_STATUS_BUILDER_CRASHED,
                             "job worker request channel failed");
        return;
    }
    process->request_offset += consumed;
    process->request_backpressured =
        process->request_backpressured || would_block;
    (void)would_block;
}

static void accept_stream_messages(tp_job_worker_process *process) {
    process->stream_incomplete = false;
    for (int count = 0; count < 16 && !process->response_ready; ++count) {
        tp_job_worker_proto_stream_message message = {0};
        bool ready = false;
        tp_error error = {{0}};
        const tp_status status = tp_job_worker_proto_stream_next(
            &process->stream, &message, &ready, &error);
        if (status != TP_STATUS_OK) {
            set_terminal_failure(
                process, status == TP_STATUS_OOM ? TP_STATUS_OOM
                                                 : TP_STATUS_BUILDER_FAILED,
                error.msg[0] ? error.msg
                             : "job worker response is malformed");
            return;
        }
        if (!ready) {
            process->stream_incomplete = process->stream.length > 0U;
            return;
        }
        if (message.kind == TP_JOB_WORKER_STREAM_PROGRESS) {
            if (message.progress.request_id != process->request_id) {
                set_terminal_failure(
                    process, TP_STATUS_BUILDER_FAILED,
                    "job worker progress identity does not match the request");
                return;
            }
            if (process->kind == TP_SESSION_JOB_EXPORT &&
                !admit_export_phase(process, message.progress.phase)) {
                char transition[128];
                (void)snprintf(
                    transition, sizeof transition,
                    "job worker reported an invalid Export phase transition: %d -> %d",
                    (int)process->last_export_phase,
                    (int)message.progress.phase);
                set_terminal_failure(
                    process, TP_STATUS_BUILDER_FAILED,
                    transition);
                return;
            }
            process->progress = message.progress;
        } else if (message.kind == TP_JOB_WORKER_STREAM_FRAGMENT) {
            if (process->kind != TP_SESSION_JOB_EXPORT ||
                message.fragment.request_id != process->request_id ||
                !process->export_report) {
                set_terminal_failure(
                    process, TP_STATUS_BUILDER_FAILED,
                    "job worker Export fragment identity does not match the request");
                tp_job_worker_proto_stream_message_free(&message);
                return;
            }
            const bool target_outcome =
                message.fragment.outcome.kind ==
                TP_EXPORT_COMMAND_OUTCOME_TARGET;
            if ((target_outcome &&
                 !process->export_target_outcome_pending) ||
                (!target_outcome &&
                 process->export_target_outcome_pending)) {
                set_terminal_failure(
                    process, TP_STATUS_BUILDER_FAILED,
                    "job worker Export fragment does not match its lifecycle phase");
                tp_job_worker_proto_stream_message_free(&message);
                return;
            }
            tp_error adoption_error = {{0}};
            const tp_status adoption_status =
                tp_export_command_report_apply_outcome(
                    process->export_report, &message.fragment.outcome,
                    &adoption_error);
            if (adoption_status != TP_STATUS_OK) {
                set_terminal_failure(
                    process,
                    adoption_status == TP_STATUS_OOM
                        ? TP_STATUS_OOM
                        : TP_STATUS_BUILDER_FAILED,
                    adoption_error.msg[0]
                        ? adoption_error.msg
                        : "job worker Export outcome could not be adopted");
                tp_job_worker_proto_stream_message_free(&message);
                return;
            }
            if (target_outcome) {
                process->export_target_outcome_pending = false;
                process->export_publication_pending = false;
            } else {
                process->export_atlas_complete = true;
            }
        } else {
            admit_terminal(process, &message.terminal);
        }
        tp_job_worker_proto_stream_message_free(&message);
    }
    if (process->response_ready && process->stream.length > 0U) {
        replace_terminal_failure(
            process, TP_STATUS_BUILDER_FAILED,
            "job worker wrote bytes after its terminal frame");
    }
}

static void pump_stdout(tp_job_worker_process *process) {
    uint8_t buffer[TP_JOB_WORKER_IO_BUDGET];
    size_t length = 0U;
    bool eof = false;
    if (!tp_proc_try_read_stdout(
            process->proc, buffer, sizeof buffer, &length, &eof)) {
        set_terminal_failure(process, TP_STATUS_BUILDER_CRASHED,
                             "job worker response channel failed");
        return;
    }
    process->stdout_eof = process->stdout_eof || eof;
    if (length > 0U && process->response_ready) {
        replace_terminal_failure(
            process, TP_STATUS_BUILDER_FAILED,
            "job worker wrote bytes after its terminal frame");
        return;
    }
    if (length > 0U) {
        tp_error error = {{0}};
        if (tp_job_worker_proto_stream_feed(
                &process->stream, buffer, length, &error) !=
            TP_STATUS_OK) {
            set_terminal_failure(
                process, TP_STATUS_BUILDER_FAILED,
                error.msg[0] ? error.msg
                             : "job worker response exceeds protocol cap");
            return;
        }
    }
    if (!process->response_ready && process->stream.length > 0U) {
        accept_stream_messages(process);
    }
}

void tp_job_worker_process_pump(tp_job_worker_process *process) {
    if (!process || process->terminal) {
        return;
    }
    pump_request(process);
    pump_stdout(process);

    const double now = process_now_ms();
    const bool cancel_expired =
        process->cancel_requested &&
        now - process->cancel_started_ms >=
            (double)process_cancel_grace_ms();
    const bool timeout_expired =
        now - process->started_ms >= (double)process_timeout_ms();
    if (cancel_expired || timeout_expired) {
        const tp_status status =
            cancel_expired ? TP_STATUS_CANCELLED
                           : TP_STATUS_BUILDER_CRASHED;
        const char *message =
            cancel_expired
                ? "job worker did not stop within the cancellation grace period"
                : "job worker exceeded its execution timeout";
        if (process->response_ready) {
            replace_terminal_failure(process, status, message);
        } else {
            set_terminal_failure(process, status, message);
        }
        process->terminal = process->response_ready;
        return;
    }

    if (!process->process_finished) {
        bool finished = false;
        if (!tp_proc_wait_slice(
                process->proc, 0, &process->process_result, &finished)) {
            set_terminal_failure(process, TP_STATUS_BUILDER_CRASHED,
                                 "job worker process wait failed");
            return;
        }
        process->process_finished = finished;
    }
    if (process->process_finished && !process->stdout_eof) {
        pump_stdout(process);
    }
    if (process->process_finished && process->stdout_eof) {
        if (!process->response_ready && process->stream.length > 0U &&
            !process->stream_incomplete) {
            return;
        }
        if (!process->response_ready && process->stream_incomplete) {
            set_terminal_failure(
                process, TP_STATUS_BUILDER_FAILED,
                "job worker response ended with an incomplete frame");
        }
        if (process->response_ready && !process->killed &&
            (process->process_result.how != TP_PROC_END_EXITED ||
             process->process_result.code != 0)) {
            replace_terminal_failure(
                process, TP_STATUS_BUILDER_CRASHED,
                "job worker failed after writing its terminal frame");
        }
        if (!process->response_ready) {
            set_terminal_failure(
                process, TP_STATUS_BUILDER_CRASHED,
                process->killed
                    ? "job worker process was forcibly terminated"
                    : process->process_result.how == TP_PROC_END_EXITED &&
                              process->process_result.code == 0
                          ? "job worker exited without a terminal frame"
                          : "job worker process crashed");
        }
        process->terminal = process->response_ready;
    }
}

tp_job_worker_proto_progress tp_job_worker_process_progress(
    const tp_job_worker_process *process) {
    return process ? process->progress
                   : (tp_job_worker_proto_progress){0};
}

bool tp_job_worker_process_terminal(
    const tp_job_worker_process *process) {
    return process && process->terminal;
}

const tp_job_worker_proto_response *tp_job_worker_process_response(
    const tp_job_worker_process *process) {
    return process && process->terminal ? &process->response : NULL;
}

tp_export_command_report *tp_job_worker_process_take_export_report(
    tp_job_worker_process *process, bool *out_publication_pending) {
    if (out_publication_pending) {
        *out_publication_pending = false;
    }
    if (!process || !process->terminal ||
        process->kind != TP_SESSION_JOB_EXPORT) {
        return NULL;
    }
    tp_export_command_report *report = process->export_report;
    process->export_report = NULL;
    if (out_publication_pending) {
        *out_publication_pending = process->export_publication_pending;
    }
    return report;
}

void tp_job_worker_process_destroy(tp_job_worker_process *process) {
    if (!process) {
        return;
    }
    tp_proc_destroy(process->proc);
    tp_job_worker_proto_response_free(&process->response);
    if (process->export_report) {
        tp_export_command_report_destroy(process->export_report);
        free(process->export_report);
    }
    tp_job_worker_proto_stream_destroy(&process->stream);
    free(process->request);
    free(process);
}

#ifdef TP_ENABLE_TEST_SEAMS
void tp_job_worker__test_set_timeout_ms(int timeout_ms) {
    s_test_timeout_ms = timeout_ms;
}

void tp_job_worker__test_set_cancel_grace_ms(int grace_ms) {
    s_test_cancel_grace_ms = grace_ms;
}

bool tp_job_worker__test_request_backpressured(
    const tp_job_worker_process *process) {
    return process && process->request_backpressured;
}

void tp_job_worker__test_reset(void) {
    s_test_timeout_ms = 0;
    s_test_cancel_grace_ms = 0;
}
#endif
