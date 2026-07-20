#ifndef TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H
#define TP_CORE_SRC_TP_JOB_OWNER_INTERNAL_H

#include <stdatomic.h>

#include "tp_core/tp_session.h"

typedef struct tp_session_owned_job tp_session_owned_job;
struct tp_session_owned_job {
    _Atomic unsigned refs;
    void (*cancel)(tp_session_owned_job *job);
    void (*destroy)(tp_session_owned_job *job);
};

void tp_session_owned_job_init(tp_session_owned_job *job,
                               void (*cancel)(tp_session_owned_job *job),
                               void (*destroy)(tp_session_owned_job *job));

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
