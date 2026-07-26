#ifndef TP_CORE_SRC_TP_SESSION_JOB_OBSERVATION_INTERNAL_H
#define TP_CORE_SRC_TP_SESSION_JOB_OBSERVATION_INTERNAL_H

#include "tp_core/tp_session.h"

typedef struct tp_session_owned_job tp_session_owned_job;

typedef struct tp_session_job_descriptor {
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
    tp_session_input_token base_input_token;
    const tp_session_job_target *targets;
    size_t target_count;
} tp_session_job_descriptor;

typedef struct tp_session_job_sample {
    tp_session_job_state state;
    int current;
    int total;
    bool cancellation_requested;
    tp_status terminal_status;
    tp_error terminal_error;
    const tp_session_job_result *terminal_result;
} tp_session_job_sample;

typedef bool (*tp_session_job_observe_fn)(
    tp_session_owned_job *job, tp_session_job_sample *out);

typedef enum tp_session_job_admission {
    TP_SESSION_JOB_ADMISSION_PROGRESS = 1,
    TP_SESSION_JOB_ADMISSION_TERMINAL,
    TP_SESSION_JOB_ADMISSION_UNCHANGED,
    TP_SESSION_JOB_ADMISSION_SUPERSEDED,
    TP_SESSION_JOB_ADMISSION_CANCELLED,
    TP_SESSION_JOB_ADMISSION_TARGET_DELETED,
    TP_SESSION_JOB_ADMISSION_OLD_INSTANCE,
    TP_SESSION_JOB_ADMISSION_DUPLICATE,
    TP_SESSION_JOB_ADMISSION_SESSION_CLOSED
} tp_session_job_admission;

void tp_session_owned_job_configure_observation(
    tp_session_owned_job *job,
    const tp_session_job_descriptor *descriptor,
    tp_session_job_observe_fn observe);

/* Host-admission-thread ports. They acquire the session gate; workers only
 * update their private atomics/immutable terminal payload. */
tp_status tp_session_job_observation_begin_internal(
    tp_session *session, tp_session_owned_job *job, tp_error *err);
tp_session_job_admission tp_session_job_observation_admit_internal(
    tp_session *session, tp_session_owned_job *job);

/* Session-family locked helpers. */
void tp_session_job_observation__copy_locked(
    tp_session *session, tp_session_job_observed_state *out_state,
    tp_session_job_observed_result *out_result,
    tp_session_owned_job **out_job_pin,
    tp_session_owned_job **out_result_pin);
tp_status tp_session_job_observation__validate_begin_locked(
    const tp_session *session, const tp_session_owned_job *job,
    tp_error *err);
tp_session_owned_job *tp_session_job_observation__detach_locked(
    tp_session *session, tp_session_owned_job *expected);
tp_status tp_session_job_observation__begin_locked(
    tp_session *session, tp_session_owned_job *job,
    tp_session_owned_job **out_retired, tp_error *err);

#endif
