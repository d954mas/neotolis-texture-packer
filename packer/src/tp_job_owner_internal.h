#ifndef TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H
#define TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H

#include <stdatomic.h>

#include "tp_core/tp_session.h"

typedef struct tp_session_owned_job tp_session_owned_job;
typedef bool (*tp_session_job_request_cancel_fn)(
    tp_session_owned_job *job);
typedef void (*tp_session_job_compact_fn)(
    tp_session_owned_job *job);

typedef struct tp_session_job_descriptor {
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
    tp_session_input_token base_input_token;
    const tp_session_job_target *targets;
    size_t target_count;
    /* Concrete-owner operations used after generic session admission. Cancel
     * returns whether this request won that owner's terminal boundary; compact
     * releases only request/worker state and must be idempotent. */
    tp_session_job_request_cancel_fn request_cancel;
    tp_session_job_compact_fn compact;
} tp_session_job_descriptor;

typedef struct tp_session_job_sample {
    tp_session_job_state state;
    int current;
    int total;
    bool cancellation_requested;
    tp_status terminal_status;
    tp_error terminal_error;
    tp_session_job_result *terminal_result;
} tp_session_job_sample;

typedef bool (*tp_session_job_observe_fn)(
    tp_session_owned_job *job,
    tp_session_job_sample *out);
struct tp_session_owned_job {
    _Atomic unsigned refs;
    void (*cancel)(tp_session_owned_job *job);
    /* Bounded host-side transport progress. It may perform non-blocking process
     * I/O but must not call back into the session. */
    void (*pump)(tp_session_owned_job *job);
    void (*destroy)(tp_session_owned_job *job);
    /* Optional (NULL = nothing to release). Called on the session owner thread
     * the moment the session REJECTS this job's terminal result (cancelled, or its
     * targets were deleted): the result will never be adopted, so the job frees
     * the heavy payload it is still holding -- for a Pack that is the whole page
     * arena. The descriptor and its `targets` array MUST stay valid: the observed
     * job state borrows them until the owner pin drops. */
    void (*release_payload)(tp_session_owned_job *job);
    tp_session_job_descriptor observation_descriptor;
    tp_session_job_observe_fn observe;
};

void tp_session_owned_job_configure_observation(
    tp_session_owned_job *job,
    const tp_session_job_descriptor *descriptor,
    tp_session_job_observe_fn observe);

void tp_session_owned_job_init(tp_session_owned_job *job,
                               void (*cancel)(tp_session_owned_job *job),
                               void (*destroy)(tp_session_owned_job *job));
void tp_session_job_retain_internal(tp_session_owned_job *job);

typedef tp_status (*tp_session_job_start_fn)(void *context, tp_error *err);
/* Invoked on the session owner thread inside admission. The callback is bounded
 * and may only create the worker; it must not call the session or wait for
 * worker progress. */
tp_status tp_session_job_start_internal(
    tp_session *session, tp_session_owned_job *job,
    tp_session_job_start_fn start, void *start_context,
    tp_error *err);
#ifdef TP_ENABLE_TEST_SEAMS
/* TEST SEAM, compiled out of every shipping build. Takes the active-job lease
 * for an already-created job, i.e. without the fail-atomic spawn callback that
 * tp_session_job_start_internal owns. Nothing in production may adopt a job it
 * did not start; the seam exists so the owner refcount/detach table stays
 * testable against a synthetic job. A production TU naming it is a boundary
 * violation (JOB_OBSERVATION_TEST_SEAM in
 * cmake/check_architecture_boundaries.cmake). */
tp_status tp_session_job_attach_internal(tp_session *session,
                                         tp_session_owned_job *job,
                                         tp_error *err);
#endif
tp_session_owned_job *tp_session_job_acquire_internal(
    const tp_session *session);
void tp_session_job_release_internal(tp_session_owned_job *job);
tp_status tp_session_job_detach_internal(tp_session *session,
                                         tp_session_owned_job *expected,
                                         tp_error *err);

#endif
