#include "tp_core/tp_session.h"

#include <stdlib.h>

#include "core/nt_assert.h"
#include "tp_model_seam.h"
#include "tp_job_owner_internal.h"
#include "tp_session_internal.h"
#include "tp_session_job_observation_internal.h"
#include "tp_session_layout.h"
#include "tp_session_snapshot_internal.h"

struct tp_session_observation {
    tp_session_observation_token token;
    bool resync_required;
    size_t event_count;
    tp_session_event events[TP_SESSION_OBSERVATION_EVENT_CAPACITY];
    tp_session_recovery_health recovery_health;
    tp_session_job_observed_state job_state;
    tp_session_job_observed_result job_result;
    tp_session_owned_job *job_pin;
    tp_session_owned_job *result_pin;
    tp_session_snapshot *snapshot;
};

#ifdef TP_ENABLE_TEST_SEAMS
static _Thread_local tp_session_test_observe_after_cut_fn
    s_test_after_cut_hook;
static _Thread_local void *s_test_after_cut_context;

void tp_session__test_set_observe_after_cut(
    tp_session_test_observe_after_cut_fn hook, void *context) {
    s_test_after_cut_hook = hook;
    s_test_after_cut_context = context;
}
#endif

bool tp_session_observation_token_equal(
    tp_session_observation_token left,
    tp_session_observation_token right) {
    return left.event_sequence == right.event_sequence &&
           left.source_runtime_generation ==
               right.source_runtime_generation &&
           left.recovery_health_generation ==
               right.recovery_health_generation &&
           left.recovery_owner_generation ==
               right.recovery_owner_generation &&
           left.job_state_generation == right.job_state_generation &&
           left.result_generation == right.result_generation;
}

static tp_session_observation_token current_token_locked(
    const tp_session *session) {
    tp_session_observation_token token = {
        .event_sequence = session->event_sequence,
        .source_runtime_generation = session->source_generation,
        .recovery_health_generation =
            tp_model__recovery_health_generation(session->model),
        .recovery_owner_generation =
            session->recovery_owner_generation,
        .job_state_generation = session->job_state_generation,
        .result_generation = session->result_generation,
    };
    return token;
}

static bool token_is_future(tp_session_observation_token after,
                            tp_session_observation_token current) {
    return after.event_sequence > current.event_sequence ||
           after.source_runtime_generation >
               current.source_runtime_generation ||
           after.recovery_health_generation >
               current.recovery_health_generation ||
           after.recovery_owner_generation >
               current.recovery_owner_generation ||
           after.job_state_generation > current.job_state_generation ||
           after.result_generation > current.result_generation;
}

static bool event_requires_model_snapshot(tp_session_event_kind kind) {
    return kind != TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED;
}

tp_status tp_session_observe(
    const tp_session *session,
    const tp_session_observation_token *after,
    tp_session_observation **out,
    tp_error *err) {
    if (!session || !out) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "session observation requires session and output");
    }
    *out = NULL;

    gate_lock(session);
    const tp_session_observation_token current =
        current_token_locked(session);
    if (after && tp_session_observation_token_equal(*after, current)) {
        gate_unlock(session);
        return TP_STATUS_OK;
    }

    tp_session_observation *observation =
        (tp_session_observation *)calloc(1U, sizeof *observation);
    if (!observation) {
        gate_unlock(session);
        return tp_error_set(
            err, TP_STATUS_OOM, "session observation allocation failed");
    }
    observation->token = current;
    observation->recovery_health =
        tp_session__recovery_health_locked(session);
    tp_session_job_observation__copy_locked(
        (tp_session *)session, &observation->job_state,
        &observation->job_result, &observation->job_pin,
        &observation->result_pin);
    observation->resync_required =
        !after || token_is_future(after ? *after : current, current);

    if (after && !observation->resync_required) {
        const uint64_t oldest =
            session->event_count > 0U
                ? session->events[session->event_start].sequence
                : session->event_sequence + 1U;
        if (after->event_sequence < session->event_sequence &&
            after->event_sequence + 1U < oldest) {
            observation->resync_required = true;
        }
    }

    bool snapshot_required = observation->resync_required;
    if (after && !observation->resync_required) {
        for (size_t i = 0U; i < session->event_count; ++i) {
            const size_t slot =
                (session->event_start + i) % TP_SESSION_EVENT_CAPACITY;
            const tp_session_event *event = &session->events[slot];
            if (event->sequence <= after->event_sequence ||
                event->sequence > current.event_sequence) {
                continue;
            }
            NT_ASSERT(observation->event_count <
                      TP_SESSION_OBSERVATION_EVENT_CAPACITY);
            observation->events[observation->event_count++] = *event;
            snapshot_required =
                snapshot_required ||
                event_requires_model_snapshot(event->kind);
        }
    }

    tp_status status = TP_STATUS_OK;
    if (snapshot_required) {
        status = tp_session_snapshot__capture_locked(
            session, &observation->snapshot, err);
    }
    gate_unlock(session);
    if (status != TP_STATUS_OK) {
        tp_session_job_release_internal(observation->job_pin);
        tp_session_job_release_internal(observation->result_pin);
        free(observation);
        return status;
    }

#ifdef TP_ENABLE_TEST_SEAMS
    tp_session_test_observe_after_cut_fn hook = s_test_after_cut_hook;
    void *hook_context = s_test_after_cut_context;
    s_test_after_cut_hook = NULL;
    s_test_after_cut_context = NULL;
    if (hook) {
        hook(hook_context);
    }
#endif

    if (observation->snapshot) {
        status = tp_session_snapshot__materialize_captured(
            observation->snapshot, err);
        if (status != TP_STATUS_OK) {
            observation->snapshot = NULL;
            tp_session_job_release_internal(observation->job_pin);
            tp_session_job_release_internal(observation->result_pin);
            free(observation);
            return status;
        }
        NT_ASSERT(tp_session_snapshot_event_sequence(
                      observation->snapshot) ==
                  observation->token.event_sequence);
    }
    NT_ASSERT(observation->event_count <=
              TP_SESSION_OBSERVATION_EVENT_CAPACITY);
    NT_ASSERT(observation->recovery_health.generation ==
              observation->token.recovery_health_generation);
    *out = observation;
    return TP_STATUS_OK;
}

void tp_session_observation_destroy(
    tp_session_observation *observation) {
    if (!observation) {
        return;
    }
    tp_session_snapshot_destroy(observation->snapshot);
    tp_session_job_release_internal(observation->job_pin);
    tp_session_job_release_internal(observation->result_pin);
    free(observation);
}

tp_session_observation_token tp_session_observation_token_query(
    const tp_session_observation *observation) {
    return observation ? observation->token
                       : (tp_session_observation_token){0};
}

bool tp_session_observation_resync_required(
    const tp_session_observation *observation) {
    return observation && observation->resync_required;
}

size_t tp_session_observation_event_count(
    const tp_session_observation *observation) {
    return observation ? observation->event_count : 0U;
}

const tp_session_event *tp_session_observation_event_at(
    const tp_session_observation *observation, size_t index) {
    if (!observation || index >= observation->event_count) {
        return NULL;
    }
    return &observation->events[index];
}

tp_session_recovery_health tp_session_observation_recovery_health(
    const tp_session_observation *observation) {
    return observation ? observation->recovery_health
                       : (tp_session_recovery_health){0};
}

tp_session_job_observed_state tp_session_observation_job_state(
    const tp_session_observation *observation) {
    return observation ? observation->job_state
                       : (tp_session_job_observed_state){0};
}

const tp_session_job_observed_result *tp_session_observation_job_result(
    const tp_session_observation *observation) {
    return observation && observation->job_result.present
               ? &observation->job_result
               : NULL;
}

const tp_session_snapshot *tp_session_observation_snapshot(
    const tp_session_observation *observation) {
    return observation ? observation->snapshot : NULL;
}
