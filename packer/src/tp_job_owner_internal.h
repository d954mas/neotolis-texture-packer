#ifndef TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H
#define TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H

#include <stdatomic.h>

#include "tp_core/tp_session.h"
#include "tp_session_job_observation_internal.h"

typedef struct tp_session_owned_job tp_session_owned_job;
struct tp_session_owned_job {
    _Atomic unsigned refs;
    void (*cancel)(tp_session_owned_job *job);
    /* Bounded host-side transport progress. Never called under the session
     * gate; it may perform non-blocking process I/O but must not call back into
     * the session. */
    void (*pump)(tp_session_owned_job *job);
    void (*destroy)(tp_session_owned_job *job);
    tp_session_job_descriptor observation_descriptor;
    tp_session_job_observe_fn observe;
};

void tp_session_owned_job_init(tp_session_owned_job *job,
                               void (*cancel)(tp_session_owned_job *job),
                               void (*destroy)(tp_session_owned_job *job));
void tp_session_job_retain_internal(tp_session_owned_job *job);

typedef tp_status (*tp_session_job_start_fn)(void *context, tp_error *err);
/* Invoked while the session gate is held. The callback is bounded and may only
 * create the worker; it must not call the session or wait for worker progress. */
tp_status tp_session_job_start_internal(
    tp_session *session, tp_session_owned_job *job,
    tp_session_job_start_fn start, void *start_context,
    tp_error *err);
tp_status tp_session_job_attach_internal(tp_session *session,
                                         tp_session_owned_job *job,
                                         tp_error *err);
tp_session_owned_job *tp_session_job_acquire_internal(
    const tp_session *session);
void tp_session_job_release_internal(tp_session_owned_job *job);
tp_status tp_session_job_detach_internal(tp_session *session,
                                         tp_session_owned_job *expected,
                                         tp_error *err);

#endif
