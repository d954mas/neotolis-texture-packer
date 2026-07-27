#include "tp_session_job_observation_internal.h"

#include <stdint.h>
#include <string.h>

#include "core/nt_assert.h"
#include "tp_core/tp_project.h"
#include "tp_job_owner_internal.h"
#include "tp_model_seam.h"
#include "tp_session_internal.h"
#include "tp_session_layout.h"

static void bump_generation(uint64_t *generation) {
    NT_ASSERT(generation != NULL);
    if (*generation < UINT64_MAX) {
        (*generation)++;
    }
}

static bool descriptor_is_valid(
    const tp_session_job_descriptor *descriptor) {
    return descriptor &&
           descriptor->kind != TP_SESSION_JOB_NONE &&
           (descriptor->target_count == 0U || descriptor->targets);
}

void tp_session_owned_job_configure_observation(
    tp_session_owned_job *job,
    const tp_session_job_descriptor *descriptor,
    tp_session_job_observe_fn observe) {
    NT_ASSERT(job != NULL);
    NT_ASSERT(descriptor_is_valid(descriptor));
    NT_ASSERT(observe != NULL);
    job->observation_descriptor = *descriptor;
    job->observe = observe;
}

static bool targets_still_exist(
    const tp_session *session,
    const tp_session_job_descriptor *descriptor) {
    const tp_project *project = tp_model_project(session->model);
    for (size_t i = 0U; i < descriptor->target_count; ++i) {
        const tp_session_job_target *target = &descriptor->targets[i];
        const tp_project_atlas *atlas =
            tp_project_atlas_by_id(project, target->atlas_id);
        if (!atlas) {
            return false;
        }
        if (target->kind == TP_SESSION_JOB_TARGET_EXPORT_TARGET) {
            if (tp_id128_is_nil(target->id) ||
                !tp_project_atlas_target_by_id(atlas, target->id)) {
                return false;
            }
        } else if (target->kind != TP_SESSION_JOB_TARGET_ATLAS) {
            return false;
        }
    }
    return true;
}

static bool running_state_changed(
    const tp_session_job_observed_state *state,
    const tp_session_job_sample *sample) {
    return state->state != sample->state ||
           state->current != sample->current ||
           state->total != sample->total ||
           state->cancellation_requested !=
               sample->cancellation_requested;
}

static tp_session_job_admission admit_locked(
    tp_session *session, tp_session_owned_job *job,
    tp_session_owned_job **out_retired_result) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(job != NULL);
    NT_ASSERT(out_retired_result != NULL);
    *out_retired_result = NULL;

    if (session->discarded) {
        return TP_SESSION_JOB_ADMISSION_SESSION_CLOSED;
    }
    const tp_session_job_descriptor *current =
        session->observed_job_owner
            ? &session->observed_job_owner->observation_descriptor
            : NULL;
    const tp_session_job_descriptor *incoming =
        &job->observation_descriptor;
    if (!current ||
        incoming->session_instance_generation !=
            current->session_instance_generation) {
        return TP_SESSION_JOB_ADMISSION_OLD_INSTANCE;
    }
    if (incoming->request_id != current->request_id) {
        return TP_SESSION_JOB_ADMISSION_SUPERSEDED;
    }
    if (session->observed_job_owner != job ||
        session->observed_job_state.terminal) {
        return TP_SESSION_JOB_ADMISSION_DUPLICATE;
    }

    tp_session_job_sample sample = {0};
    if (!job->observe || !job->observe(job, &sample)) {
        return TP_SESSION_JOB_ADMISSION_UNCHANGED;
    }
    if (sample.state == TP_SESSION_JOB_RUNNING) {
        if (!running_state_changed(
                &session->observed_job_state, &sample)) {
            return TP_SESSION_JOB_ADMISSION_UNCHANGED;
        }
        session->observed_job_state.state = sample.state;
        session->observed_job_state.current = sample.current;
        session->observed_job_state.total = sample.total;
        session->observed_job_state.cancellation_requested =
            sample.cancellation_requested;
        bump_generation(&session->job_state_generation);
        return TP_SESSION_JOB_ADMISSION_PROGRESS;
    }

    session->observed_job_state.state = sample.state;
    session->observed_job_state.current = sample.current;
    session->observed_job_state.total = sample.total;
    session->observed_job_state.cancellation_requested =
        sample.cancellation_requested;
    session->observed_job_state.terminal = true;
    session->observed_job_state.terminal_status =
        sample.terminal_status;
    session->observed_job_state.terminal_error =
        sample.terminal_error;
    session->observed_job_state.result_accepted = false;
    session->observed_job_state.rejection =
        TP_SESSION_JOB_REJECTION_NONE;

    tp_session_job_admission admission =
        TP_SESSION_JOB_ADMISSION_TERMINAL;
    bool result_rejected = false;
    if (sample.cancellation_requested &&
        sample.state != TP_SESSION_JOB_CANCELLED) {
        session->observed_job_state.rejection =
            TP_SESSION_JOB_REJECTION_CANCELLED;
        admission = TP_SESSION_JOB_ADMISSION_CANCELLED;
        result_rejected = true;
    } else if (!targets_still_exist(session, incoming)) {
        session->observed_job_state.rejection =
            TP_SESSION_JOB_REJECTION_TARGET_DELETED;
        admission = TP_SESSION_JOB_ADMISSION_TARGET_DELETED;
        result_rejected = true;
    } else if (sample.state == TP_SESSION_JOB_CANCELLED) {
        session->observed_job_state.rejection =
            TP_SESSION_JOB_REJECTION_CANCELLED;
        admission = TP_SESSION_JOB_ADMISSION_CANCELLED;
        result_rejected = true;
    } else if (sample.terminal_result) {
        tp_session_job_retain_internal(job);
        *out_retired_result = session->observed_result_owner;
        session->observed_result_owner = job;
        session->observed_job_result =
            (tp_session_job_observed_result){
                .present = true,
                .session_instance_generation =
                    incoming->session_instance_generation,
                .request_id = incoming->request_id,
                .kind = incoming->kind,
                .base_input_token = incoming->base_input_token,
                .targets = incoming->targets,
                .target_count = incoming->target_count,
                .result = sample.terminal_result,
            };
        session->observed_job_state.result_accepted = true;
        bump_generation(&session->result_generation);
    }
    /* The rejected terminal result is dropped here, but the job stays pinned by
     * observed_job_owner until the host consumes the terminal state -- and a
     * cancelled big Pack would keep every page pixel resident for that whole
     * window. Tell the job to free its payload now; its descriptor/targets (which
     * observed_job_state borrows) stay valid. */
    if (result_rejected && sample.terminal_result &&
        job->release_payload) {
        job->release_payload(job);
    }
    bump_generation(&session->job_state_generation);
    return admission;
}

/* The single site that turns "no request id yet" into an admitted request id.
 * Both host entry points route through it -- tp_session_job_start_internal
 * reserves before it spawns (so the exact admitted identity is encoded into the
 * child request) and __begin_locked reserves for the attach path -- so the
 * assign-if-zero rule and the exhaustion rejection cannot drift apart. Calling
 * it twice for the same job is a no-op: a non-zero id is already reserved. */
tp_status tp_session_job_observation__reserve_request_id_locked(
    tp_session *session, tp_session_owned_job *job, tp_error *err) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(job != NULL);
    if (job->observation_descriptor.request_id != 0U) {
        return TP_STATUS_OK;
    }
    if (session->next_job_request_id == UINT64_MAX) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "session job request id space is exhausted");
    }
    job->observation_descriptor.request_id =
        ++session->next_job_request_id;
    return TP_STATUS_OK;
}

#ifdef TP_ENABLE_TEST_SEAMS
/* Positions the monotonic request-id counter so the exhaustion rejection above is
 * reachable without reserving 2^64 ids. Compiled out of the shipped library. */
void tp_session_job_observation__test_set_next_request_id(
    tp_session *session, uint64_t next_request_id) {
    NT_ASSERT(session != NULL);
    session->next_job_request_id = next_request_id;
}
#endif

tp_status tp_session_job_observation__begin_locked(
    tp_session *session, tp_session_owned_job *job,
    tp_session_owned_job **out_retired, tp_error *err) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(job != NULL);
    NT_ASSERT(out_retired != NULL);
    *out_retired = NULL;
    const tp_status shape_status =
        tp_session_job_observation__validate_begin_locked(
            session, job, err);
    if (shape_status != TP_STATUS_OK) {
        return shape_status;
    }
    const tp_status reservation_status =
        tp_session_job_observation__reserve_request_id_locked(
            session, job, err);
    if (reservation_status != TP_STATUS_OK) {
        return reservation_status;
    }

    *out_retired = session->observed_job_owner;
    tp_session_job_retain_internal(job);
    session->observed_job_owner = job;
    session->observed_job_state =
        (tp_session_job_observed_state){
            .present = true,
            .session_instance_generation =
                job->observation_descriptor
                    .session_instance_generation,
            .request_id = job->observation_descriptor.request_id,
            .kind = job->observation_descriptor.kind,
            .state = TP_SESSION_JOB_RUNNING,
            .base_input_token =
                job->observation_descriptor.base_input_token,
            .targets = job->observation_descriptor.targets,
            .target_count = job->observation_descriptor.target_count,
        };
    if (job->observe) {
        tp_session_job_sample sample = {0};
        if (job->observe(job, &sample) &&
            sample.state == TP_SESSION_JOB_RUNNING) {
            session->observed_job_state.current = sample.current;
            session->observed_job_state.total = sample.total;
            session->observed_job_state.cancellation_requested =
                sample.cancellation_requested;
        }
    }
    bump_generation(&session->job_state_generation);
    return TP_STATUS_OK;
}

/* Sole owner of the "publishable observed job" shape rule. __begin_locked calls
 * it on the way in, and the start path calls it alone as a pre-flight: there the
 * request id is reserved explicitly through __reserve_request_id_locked, which
 * owns that separate rejection. One definition, so the two paths cannot drift. */
tp_status tp_session_job_observation__validate_begin_locked(
    const tp_session *session, const tp_session_owned_job *job,
    tp_error *err) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(job != NULL);
    if (!job->observe ||
        !descriptor_is_valid(&job->observation_descriptor)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "job observation requires descriptor and observe callback");
    }
    return TP_STATUS_OK;
}

#ifdef TP_ENABLE_TEST_SEAMS
/* See tp_session_job_observation_internal.h: test-only publication of an
 * observed job without the active-job lease, no shipping caller. */
tp_status tp_session_job_observation_begin_internal(
    tp_session *session, tp_session_owned_job *job, tp_error *err) {
    if (!session || !job) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "job observation begin requires session and owner");
    }
    gate_lock(session);
    if (session->discarded) {
        gate_unlock(session);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "session was discarded");
    }
    tp_session_owned_job *retired = NULL;
    const tp_status status =
        tp_session_job_observation__begin_locked(
            session, job, &retired, err);
    gate_unlock(session);
    tp_session_job_release_internal(retired);
    return status;
}
#endif /* TP_ENABLE_TEST_SEAMS */

tp_session_job_admission tp_session_job_observation_admit_internal(
    tp_session *session, tp_session_owned_job *job) {
    if (!session || !job) {
        return TP_SESSION_JOB_ADMISSION_SUPERSEDED;
    }
    gate_lock(session);
    tp_session_owned_job *retired = NULL;
    const tp_session_job_admission admission =
        admit_locked(session, job, &retired);
    gate_unlock(session);
    tp_session_job_release_internal(retired);
    return admission;
}

void tp_session_job_observation__copy_locked(
    tp_session *session, tp_session_job_observed_state *out_state,
    tp_session_job_observed_result *out_result,
    tp_session_owned_job **out_job_pin,
    tp_session_owned_job **out_result_pin) {
    NT_ASSERT(session != NULL);
    NT_ASSERT(out_state != NULL);
    NT_ASSERT(out_result != NULL);
    NT_ASSERT(out_job_pin != NULL);
    NT_ASSERT(out_result_pin != NULL);
    *out_state = session->observed_job_state;
    *out_result = session->observed_job_result;
    *out_job_pin = session->observed_job_owner;
    *out_result_pin = session->observed_result_owner;
    tp_session_job_retain_internal(*out_job_pin);
    tp_session_job_retain_internal(*out_result_pin);
}

tp_session_owned_job *tp_session_job_observation__detach_locked(
    tp_session *session, tp_session_owned_job *expected) {
    NT_ASSERT(session != NULL);
    if (session->observed_job_owner != expected ||
        session->observed_job_state.terminal) {
        return NULL;
    }
    tp_session_owned_job *retired = session->observed_job_owner;
    session->observed_job_owner = NULL;
    memset(&session->observed_job_state, 0,
           sizeof session->observed_job_state);
    bump_generation(&session->job_state_generation);
    return retired;
}
