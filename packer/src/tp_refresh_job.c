#include "tp_core/tp_job.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "tinycthread.h"

#include "tp_job_owner_internal.h"
#include "tp_source_runtime_internal.h"

typedef struct tp_refresh_job {
    tp_session_owned_job owner;
    thrd_t thread;
    bool thread_started;
    _Atomic bool cancel_requested;
    _Atomic int state;
    tp_session_snapshot *snapshot;
    tp_source_runtime_projection *previous;
    tp_session_job_result terminal;
} tp_refresh_job;

static void refresh_cancel(tp_session_owned_job *owned) {
    tp_refresh_job *job = (tp_refresh_job *)owned;
    atomic_store_explicit(
        &job->cancel_requested, true, memory_order_release);
}

static void refresh_release_payload(tp_session_owned_job *owned) {
    tp_refresh_job *job = (tp_refresh_job *)owned;
    tp_source_runtime_destroy(job->terminal.refresh.projection);
    job->terminal.refresh.projection = NULL;
    if (job->terminal.state == TP_SESSION_JOB_SUCCEEDED) {
        const bool cancelled = atomic_load_explicit(
            &job->cancel_requested, memory_order_acquire);
        job->terminal.state = cancelled
                                  ? TP_SESSION_JOB_CANCELLED
                                  : TP_SESSION_JOB_FAILED;
        job->terminal.status = tp_error_set(
            &job->terminal.error,
            cancelled ? TP_STATUS_CANCELLED : TP_STATUS_NOT_FOUND,
            cancelled ? "Refresh cancelled"
                      : "Refresh result was discarded");
        atomic_store_explicit(
            &job->state, job->terminal.state,
            memory_order_release);
    }
}

static void refresh_destroy(tp_session_owned_job *owned) {
    tp_refresh_job *job = (tp_refresh_job *)owned;
    refresh_cancel(owned);
    if (job->thread_started) {
        (void)thrd_join(job->thread, NULL);
    }
    tp_source_runtime_destroy(job->terminal.refresh.projection);
    tp_source_runtime_destroy(job->previous);
    tp_session_snapshot_destroy(job->snapshot);
    free(job);
}

static bool refresh_observe(tp_session_owned_job *owned,
                            tp_session_job_sample *out) {
    tp_refresh_job *job = (tp_refresh_job *)owned;
    memset(out, 0, sizeof *out);
    out->state = (tp_session_job_state)atomic_load_explicit(
        &job->state, memory_order_acquire);
    out->current =
        out->state == TP_SESSION_JOB_RUNNING ? 0 : 1;
    out->total = 1;
    out->cancellation_requested = atomic_load_explicit(
        &job->cancel_requested, memory_order_acquire);
    if (out->state != TP_SESSION_JOB_RUNNING) {
        out->terminal_status = job->terminal.status;
        out->terminal_error = job->terminal.error;
        out->terminal_result = &job->terminal;
    }
    return true;
}

static int refresh_thread(void *context) {
    tp_refresh_job *job = context;
    tp_source_runtime_projection *projection = NULL;
    tp_error error = {{0}};
    tp_status status = tp_source_runtime_build(
        job->snapshot, &projection, &error);
    const bool cancelled = atomic_load_explicit(
        &job->cancel_requested, memory_order_acquire);
    memset(&job->terminal, 0, sizeof job->terminal);
    job->terminal.kind = TP_SESSION_JOB_REFRESH;
    if (cancelled) {
        tp_source_runtime_destroy(projection);
        job->terminal.state = TP_SESSION_JOB_CANCELLED;
        job->terminal.status = tp_error_set(
            &job->terminal.error, TP_STATUS_CANCELLED,
            "Refresh cancelled");
    } else if (status != TP_STATUS_OK) {
        tp_source_runtime_destroy(projection);
        job->terminal.state = TP_SESSION_JOB_FAILED;
        job->terminal.status = status;
        job->terminal.error = error;
    } else {
        job->terminal.state = TP_SESSION_JOB_SUCCEEDED;
        job->terminal.status = TP_STATUS_OK;
        job->terminal.refresh.projection = projection;
        tp_source_runtime_diff(
            job->previous, projection,
            &job->terminal.refresh.added,
            &job->terminal.refresh.removed,
            &job->terminal.refresh.changed,
            &job->terminal.refresh.unavailable);
    }
    atomic_store_explicit(
        &job->state, job->terminal.state,
        memory_order_release);
    return 0;
}

static tp_status refresh_start_thread(void *context, tp_error *err) {
    tp_refresh_job *job = context;
    if (thrd_create(&job->thread, refresh_thread, job) !=
        thrd_success) {
        return tp_error_set(
            err, TP_STATUS_BUILDER_FAILED,
            "Refresh worker thread could not start");
    }
    job->thread_started = true;
    return TP_STATUS_OK;
}

tp_status tp_session_refresh_start(
    tp_session *session, const tp_refresh_job_request *request,
    tp_error *err) {
    if (!session || !request) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "Refresh start requires session and request");
    }
    tp_refresh_job *job = calloc(1U, sizeof *job);
    if (!job) {
        return tp_error_set(
            err, TP_STATUS_OOM,
            "Refresh job allocation failed");
    }
    tp_session_owned_job_init(
        &job->owner, refresh_cancel, refresh_destroy);
    job->owner.release_payload = refresh_release_payload;
    atomic_init(&job->cancel_requested, false);
    atomic_init(&job->state, TP_SESSION_JOB_RUNNING);
    tp_status status = tp_session_snapshot_create(
        session, &job->snapshot, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
        return status;
    }
    const struct tp_session_view *view =
        tp_session_view(session);
    if (view && view->sources) {
        job->previous =
            tp_source_runtime_clone(view->sources);
        if (!job->previous) {
            tp_session_job_release_internal(&job->owner);
            return tp_error_set(
                err, TP_STATUS_OOM,
                "Refresh baseline allocation failed");
        }
    }
    const tp_session_job_descriptor descriptor = {
        .session_instance_generation =
            request->session_instance_generation,
        .request_id = request->request_id,
        .kind = TP_SESSION_JOB_REFRESH,
        .base_input_token =
            tp_session_snapshot_input_token(job->snapshot),
    };
    tp_session_owned_job_configure_observation(
        &job->owner, &descriptor, refresh_observe);
    status = tp_session_job_start_internal(
        session, &job->owner, refresh_start_thread, job, err);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(&job->owner);
    }
    return status;
}
