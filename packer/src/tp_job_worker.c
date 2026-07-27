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
};

#ifdef TP_ENABLE_TEST_SEAMS
static int s_test_timeout_ms;
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

static void set_terminal_failure(tp_job_worker_process *process,
                                 tp_status status, const char *message) {
    if (process->response_ready) {
        return;
    }
    memset(&process->response, 0, sizeof process->response);
    process->response.kind = process->kind;
    process->response.session_instance_generation =
        process->session_instance_generation;
    process->response.request_id = process->request_id;
    process->response.state =
        process->cancel_requested ? TP_SESSION_JOB_CANCELLED
                                  : TP_SESSION_JOB_FAILED;
    process->response.status =
        process->cancel_requested ? TP_STATUS_CANCELLED : status;
    (void)tp_error_set(
        &process->response.error, process->response.status, "%s",
        message ? message : "job worker failed");
    if (process->kind == TP_SESSION_JOB_EXPORT) {
        process->response.export_result.publication_uncertain = true;
        process->response.export_result.partial_publication =
            process->cancel_requested;
    }
    process->response_ready = true;
    if (process->proc && !process->process_finished &&
        !process->killed) {
        tp_proc_kill(process->proc);
        process->killed = true;
    }
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
    process->started_ms = process_now_ms();
    process->cancel_byte = TP_PROC_CANCEL_BYTE;
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
                process, TP_STATUS_BUILDER_FAILED,
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
            process->progress = message.progress;
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
            (double)TP_JOB_WORKER_CANCEL_GRACE_MS;
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

void tp_job_worker_process_destroy(tp_job_worker_process *process) {
    if (!process) {
        return;
    }
    tp_proc_destroy(process->proc);
    tp_job_worker_proto_response_free(&process->response);
    tp_job_worker_proto_stream_destroy(&process->stream);
    free(process->request);
    free(process);
}

#ifdef TP_ENABLE_TEST_SEAMS
void tp_job_worker__test_set_timeout_ms(int timeout_ms) {
    s_test_timeout_ms = timeout_ms;
}

bool tp_job_worker__test_request_backpressured(
    const tp_job_worker_process *process) {
    return process && process->request_backpressured;
}

void tp_job_worker__test_reset(void) {
    s_test_timeout_ms = 0;
}
#endif
