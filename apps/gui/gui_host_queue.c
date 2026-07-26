#include "gui_host_queue.h"

#include <limits.h>
#include <string.h>

#include "core/nt_assert.h"

#ifdef TP_ENABLE_TEST_SEAMS
static bool s_test_fail_next_poll;
static bool s_test_fail_next_take;
#endif

static void clear_staged(gui_host_queue *queue) {
    NT_ASSERT(queue != NULL);
    if (queue->staged.present) {
        tp_session_job_result_destroy(
            &queue->staged.result);
    }
    queue->staged = (gui_host_completion){0};
    queue->staged_classification =
        GUI_HOST_STAGED_NONE;
}

static void advance_ready_after_staged_clear(
    gui_host_queue *queue) {
    NT_ASSERT(queue != NULL);
    if (!queue->active &&
        !queue->staged.present &&
        queue->lifecycle ==
            GUI_HOST_DRAINING) {
        queue->lifecycle =
            GUI_HOST_READY_TO_CUTOVER;
    }
}

static tp_status copy_text(
    char *out, size_t capacity, const char *text,
    const char *label, tp_error *err) {
    if (!out || capacity == 0U || !text) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "%s is required", label);
    }
    const size_t length = strlen(text);
    if (length >= capacity) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "%s exceeds the supported length", label);
    }
    memcpy(out, text, length + 1U);
    return TP_STATUS_OK;
}

static bool observed_state_matches(
    const tp_session_job_observed_state *state,
    gui_host_job_envelope envelope) {
    return state && state->present &&
           state->session_instance_generation ==
               envelope.session_instance_generation &&
           state->request_id == envelope.request_id &&
           state->kind == envelope.kind;
}

static bool observed_result_matches(
    const tp_session_job_observed_result *result,
    gui_host_job_envelope envelope) {
    return result && result->present &&
           result->session_instance_generation ==
               envelope.session_instance_generation &&
           result->request_id == envelope.request_id &&
           result->kind == envelope.kind &&
           result->result != NULL;
}

static void pop_command(gui_host_queue *queue) {
    NT_ASSERT(queue != NULL);
    NT_ASSERT(queue->command_count > 0U);
    if (queue->command_count > 1U) {
        memmove(
            &queue->commands[0], &queue->commands[1],
            (queue->command_count - 1U) *
                sizeof queue->commands[0]);
    }
    queue->command_count--;
    memset(
        &queue->commands[queue->command_count], 0,
        sizeof queue->commands[0]);
}

static tp_status reserve_start(
    gui_host_queue *queue,
    gui_host_command_kind command_kind,
    tp_session_job_kind job_kind,
    gui_host_command **out, tp_error *err) {
    if (!queue || !out) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host start admission requires queue and output");
    }
    *out = NULL;
    if (queue->lifecycle != GUI_HOST_OPEN) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host is not open for job admission");
    }
    if (gui_host_queue_busy(queue)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host already owns a pending or active job");
    }
    if (queue->command_count >=
        GUI_HOST_COMMAND_CAPACITY) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "host command queue is full");
    }
    if (queue->next_request_id == UINT64_MAX) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host job request id space is exhausted");
    }
    gui_host_command *command =
        &queue->commands[queue->command_count];
    memset(command, 0, sizeof *command);
    command->kind = command_kind;
    command->envelope =
        (gui_host_job_envelope){
            .session_instance_generation =
                queue->session_instance_generation,
            .request_id =
                queue->next_request_id + 1U,
            .kind = job_kind,
        };
    *out = command;
    return TP_STATUS_OK;
}

static void commit_start(gui_host_queue *queue) {
    NT_ASSERT(queue != NULL);
    NT_ASSERT(queue->command_count <
              GUI_HOST_COMMAND_CAPACITY);
    queue->next_request_id++;
    queue->command_count++;
}

static void stage_host_failure(
    gui_host_queue *queue,
    gui_host_job_envelope envelope,
    tp_status status, const tp_error *error) {
    NT_ASSERT(queue != NULL);
    NT_ASSERT(!queue->staged.present);
    queue->staged =
        (gui_host_completion){
            .present = true,
            .publish_result = false,
            .envelope = envelope,
            .state = TP_SESSION_JOB_FAILED,
            .status = status,
            .error = error ? *error : (tp_error){0},
        };
    queue->staged_classification =
        GUI_HOST_STAGED_READY;
}

static void stage_terminal(
    gui_host_queue *queue,
    tp_session_job_result *result) {
    NT_ASSERT(queue != NULL);
    NT_ASSERT(result != NULL);
    NT_ASSERT(!queue->staged.present);
    queue->staged =
        (gui_host_completion){
            .present = true,
            .publish_result = false,
            .envelope = queue->active_envelope,
            .state = result->state,
            .status = result->status,
            .error = result->error,
            .result = *result,
        };
    memset(result, 0, sizeof *result);
    queue->staged_classification =
        GUI_HOST_STAGED_PENDING_OBSERVATION;
}

void gui_host_queue_init(gui_host_queue *queue) {
    if (!queue) {
        return;
    }
    memset(queue, 0, sizeof *queue);
}

tp_status gui_host_queue_open(
    gui_host_queue *queue,
    uint64_t session_instance_generation,
    tp_error *err) {
    if (!queue) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host open requires queue");
    }
    if (queue->lifecycle != GUI_HOST_CLOSED &&
        queue->lifecycle !=
            GUI_HOST_READY_TO_CUTOVER) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host may open only from closed or ready-to-cutover");
    }
    if (session_instance_generation == 0U) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host session instance generation must be non-zero");
    }
    if (queue->active || queue->command_count > 0U) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host cannot open while old work remains");
    }
    clear_staged(queue);
    queue->session_instance_generation =
        session_instance_generation;
    queue->cancellation_requested = false;
    queue->cancel_queued = false;
    queue->drain_cancel_pending = false;
    queue->lifecycle = GUI_HOST_OPEN;
    return TP_STATUS_OK;
}

bool gui_host_queue_can_replace(
    const gui_host_queue *queue) {
    return queue &&
           (queue->lifecycle == GUI_HOST_CLOSED ||
            (queue->lifecycle == GUI_HOST_OPEN &&
             !gui_host_queue_busy(queue)));
}

tp_status gui_host_queue_begin_drain(
    gui_host_queue *queue, tp_error *err) {
    if (!queue ||
        queue->lifecycle != GUI_HOST_OPEN) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host drain requires an open host");
    }
    queue->lifecycle = GUI_HOST_DRAINING;
    queue->drain_cancel_pending =
        queue->active &&
        !queue->cancellation_requested;
    if (queue->command_count > 0U) {
        gui_host_job_envelope rejected = {0};
        bool have_rejected_start = false;
        for (size_t index = 0U;
             index < queue->command_count; ++index) {
            if (queue->commands[index].kind !=
                GUI_HOST_COMMAND_CANCEL) {
                rejected =
                    queue->commands[index].envelope;
                have_rejected_start = true;
                break;
            }
        }
        memset(
            queue->commands, 0,
            sizeof queue->commands);
        queue->command_count = 0U;
        queue->cancel_queued = false;
        if (have_rejected_start &&
            !queue->staged.present) {
            tp_error rejection = {{0}};
            const tp_status status = tp_error_set(
                &rejection,
                TP_STATUS_INVALID_ARGUMENT,
                "host stopped before queued job admission");
            stage_host_failure(
                queue, rejected, status,
                &rejection);
        }
    }
    if (!queue->active &&
        queue->staged_classification !=
            GUI_HOST_STAGED_PENDING_OBSERVATION) {
        queue->lifecycle =
            GUI_HOST_READY_TO_CUTOVER;
    }
    return TP_STATUS_OK;
}

tp_status gui_host_queue_close(
    gui_host_queue *queue, tp_error *err) {
    if (!queue ||
        (queue->lifecycle !=
             GUI_HOST_READY_TO_CUTOVER &&
         queue->lifecycle != GUI_HOST_CLOSED)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host close requires ready-to-cutover");
    }
    if (queue->active ||
        queue->command_count > 0U) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host close requires all admitted work to be detached");
    }
    clear_staged(queue);
    queue->session_instance_generation = 0U;
    queue->cancel_queued = false;
    queue->lifecycle = GUI_HOST_CLOSED;
    return TP_STATUS_OK;
}

tp_status gui_host_queue_enqueue_pack(
    gui_host_queue *queue, tp_id128 atlas_id,
    const char *work_dir,
    const char *preview_exporter_id,
    tp_error *err) {
    gui_host_command *command = NULL;
    tp_status status = reserve_start(
        queue, GUI_HOST_COMMAND_PACK,
        TP_SESSION_JOB_PACK, &command, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = copy_text(
        command->work_dir,
        sizeof command->work_dir, work_dir,
        "Pack work directory", err);
    if (status != TP_STATUS_OK) {
        memset(command, 0, sizeof *command);
        return status;
    }
    status = copy_text(
        command->preview_exporter_id,
        sizeof command->preview_exporter_id,
        preview_exporter_id
            ? preview_exporter_id
            : "",
        "Preview exporter id", err);
    if (status != TP_STATUS_OK) {
        memset(command, 0, sizeof *command);
        return status;
    }
    command->atlas_id = atlas_id;
    commit_start(queue);
    return TP_STATUS_OK;
}

tp_status gui_host_queue_enqueue_export(
    gui_host_queue *queue, tp_id128 atlas_id,
    const char *work_dir, tp_error *err) {
    gui_host_command *command = NULL;
    tp_status status = reserve_start(
        queue, GUI_HOST_COMMAND_EXPORT,
        TP_SESSION_JOB_EXPORT, &command, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = copy_text(
        command->work_dir,
        sizeof command->work_dir, work_dir,
        "Export work directory", err);
    if (status != TP_STATUS_OK) {
        memset(command, 0, sizeof *command);
        return status;
    }
    command->atlas_id = atlas_id;
    commit_start(queue);
    return TP_STATUS_OK;
}

tp_status gui_host_queue_enqueue_cancel(
    gui_host_queue *queue, tp_error *err) {
    if (!queue ||
        (queue->lifecycle != GUI_HOST_OPEN &&
         queue->lifecycle != GUI_HOST_DRAINING)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host cancellation requires open or draining host");
    }
    if (queue->cancellation_requested) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host cancellation was already requested");
    }
    if (queue->cancel_queued) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host cancellation is already queued");
    }
    if (!queue->active &&
        queue->command_count == 0U) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "host has no pending or active job");
    }
    if (queue->command_count >=
        GUI_HOST_COMMAND_CAPACITY) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "host command queue is full");
    }
    gui_host_command *command =
        &queue->commands[queue->command_count++];
    memset(command, 0, sizeof *command);
    command->kind = GUI_HOST_COMMAND_CANCEL;
    command->envelope =
        queue->active
            ? queue->active_envelope
            : queue->commands[0].envelope;
    queue->cancel_queued = true;
    return TP_STATUS_OK;
}

static tp_status admit_start(
    gui_host_queue *queue, tp_session *session,
    const gui_host_command *command,
    tp_error *err) {
    tp_error error = {{0}};
    tp_status status = TP_STATUS_INVALID_ARGUMENT;
    switch (command->kind) {
    case GUI_HOST_COMMAND_PACK: {
        const tp_pack_job_request request = {
            .atlas_id = command->atlas_id,
            .work_dir = command->work_dir,
            .session_instance_generation =
                command->envelope
                    .session_instance_generation,
            .request_id =
                command->envelope.request_id,
            .preview_exporter_id =
                command->preview_exporter_id[0]
                    ? command->preview_exporter_id
                    : NULL,
        };
        status = tp_session_pack_job_start(
            session, &request, &error);
        break;
    }
    case GUI_HOST_COMMAND_EXPORT: {
        const tp_export_command_request request = {
            .work_dir = command->work_dir,
            .session_instance_generation =
                command->envelope
                    .session_instance_generation,
            .request_id =
                command->envelope.request_id,
            .atlas_id = command->atlas_id,
        };
        status = tp_session_export_start(
            session, &request, &error);
        break;
    }
    case GUI_HOST_COMMAND_NONE:
    case GUI_HOST_COMMAND_CANCEL:
    default:
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host received an invalid start command");
    }
    if (status != TP_STATUS_OK) {
        stage_host_failure(
            queue, command->envelope,
            status, &error);
        return TP_STATUS_OK;
    }
    queue->active = true;
    queue->active_envelope =
        command->envelope;
    queue->cancellation_requested = false;
    return TP_STATUS_OK;
}

static tp_status admit_cancel(
    gui_host_queue *queue, tp_session *session,
    tp_error *err) {
    if (!queue->active) {
        return tp_error_set(
            err, TP_STATUS_NOT_FOUND,
            "host has no admitted job to cancel");
    }
    const tp_status status =
        tp_session_job_cancel(session, err);
    if (status == TP_STATUS_OK) {
        queue->cancellation_requested = true;
    }
    return status;
}

tp_status gui_host_queue_drain(
    gui_host_queue *queue, tp_session *session,
    tp_error *err) {
    if (!queue || !session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host drain requires queue and live session");
    }
    if (queue->lifecycle == GUI_HOST_CLOSED ||
        queue->lifecycle ==
            GUI_HOST_READY_TO_CUTOVER) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "host drain requires open or draining host");
    }

    if (queue->drain_cancel_pending) {
        const tp_status cancel_status =
            admit_cancel(queue, session, err);
        queue->drain_cancel_pending = false;
        if (cancel_status != TP_STATUS_OK) {
            return cancel_status;
        }
    }
    while (queue->command_count > 0U) {
        const gui_host_command command =
            queue->commands[0];
        pop_command(queue);
        if (command.kind ==
            GUI_HOST_COMMAND_CANCEL) {
            queue->cancel_queued = false;
            const tp_status cancel_status =
                admit_cancel(queue, session, err);
            if (cancel_status != TP_STATUS_OK) {
                return cancel_status;
            }
        } else if (queue->lifecycle !=
                       GUI_HOST_OPEN ||
                   queue->active ||
                   queue->staged.present ||
                   command.envelope
                           .session_instance_generation !=
                       queue->session_instance_generation) {
            tp_error rejection = {{0}};
            const tp_status status = tp_error_set(
                &rejection,
                TP_STATUS_INVALID_ARGUMENT,
                "host rejected stale or ineligible job start");
            if (!queue->staged.present) {
                stage_host_failure(
                    queue, command.envelope,
                    status, &rejection);
            }
        } else {
            const tp_status start_status =
                admit_start(
                    queue, session, &command,
                    err);
            if (start_status != TP_STATUS_OK) {
                return start_status;
            }
        }
    }

    if (!queue->active) {
        if (queue->lifecycle ==
                GUI_HOST_DRAINING &&
            queue->staged_classification !=
                GUI_HOST_STAGED_PENDING_OBSERVATION) {
            queue->lifecycle =
                GUI_HOST_READY_TO_CUTOVER;
        }
        return TP_STATUS_OK;
    }

    tp_session_job_progress progress = {0};
    tp_error poll_error = {{0}};
#ifdef TP_ENABLE_TEST_SEAMS
    if (s_test_fail_next_poll) {
        s_test_fail_next_poll = false;
        return tp_error_set(
            err, TP_STATUS_OOM,
            "host poll test failure");
    }
#endif
    const tp_status poll_status =
        tp_session_job_poll(
            session, &progress, &poll_error);
    if (poll_status != TP_STATUS_OK) {
        if (err) {
            *err = poll_error;
        }
        return poll_status;
    } else if (progress.state !=
               TP_SESSION_JOB_RUNNING) {
        tp_session_job_result result = {0};
        tp_error take_error = {{0}};
#ifdef TP_ENABLE_TEST_SEAMS
        if (s_test_fail_next_take) {
            s_test_fail_next_take = false;
            return tp_error_set(
                err, TP_STATUS_OOM,
                "host take-result test failure");
        }
#endif
        const tp_status take_status =
            tp_session_job_take_result(
                session, &result, &take_error);
        if (take_status == TP_STATUS_OK) {
            queue->active = false;
            stage_terminal(queue, &result);
        } else {
            if (err) {
                *err = take_error;
            }
            return take_status;
        }
    }

    if (!queue->active &&
        queue->lifecycle ==
            GUI_HOST_DRAINING &&
        queue->staged_classification !=
            GUI_HOST_STAGED_PENDING_OBSERVATION) {
        queue->lifecycle =
            GUI_HOST_READY_TO_CUTOVER;
    }
    return TP_STATUS_OK;
}

void gui_host_queue_reduce_observation(
    gui_host_queue *queue,
    const tp_session_observation *observation,
    uint64_t session_instance_generation) {
    if (!queue || !observation ||
        !queue->staged.present ||
        queue->staged_classification !=
            GUI_HOST_STAGED_PENDING_OBSERVATION) {
        return;
    }
    if (session_instance_generation !=
            queue->staged.envelope
                .session_instance_generation ||
        session_instance_generation !=
            queue->session_instance_generation) {
        clear_staged(queue);
        advance_ready_after_staged_clear(
            queue);
        return;
    }

    const tp_session_job_observed_state state =
        tp_session_observation_job_state(
            observation);
    if (!observed_state_matches(
            &state, queue->staged.envelope)) {
        clear_staged(queue);
        advance_ready_after_staged_clear(
            queue);
        return;
    }
    if (!state.terminal) {
        return;
    }

    queue->staged.state = state.state;
    queue->staged.rejection = state.rejection;
    queue->staged.status =
        state.terminal_status;
    queue->staged.error =
        state.terminal_error;
    if (state.result_accepted) {
        const tp_session_job_observed_result *result =
            tp_session_observation_job_result(
                observation);
        if (!observed_result_matches(
                result, queue->staged.envelope)) {
            clear_staged(queue);
            advance_ready_after_staged_clear(
                queue);
            return;
        }
        queue->staged.publish_result = true;
    } else {
        queue->staged.publish_result = false;
    }
    queue->staged_classification =
        GUI_HOST_STAGED_READY;
    if (!queue->active &&
        queue->lifecycle ==
            GUI_HOST_DRAINING) {
        queue->lifecycle =
            GUI_HOST_READY_TO_CUTOVER;
    }
}

bool gui_host_queue_take_completion(
    gui_host_queue *queue,
    gui_host_completion *out) {
    if (!queue || !out ||
        !queue->staged.present ||
        queue->staged_classification !=
            GUI_HOST_STAGED_READY) {
        return false;
    }
    *out = queue->staged;
    queue->staged =
        (gui_host_completion){0};
    queue->staged_classification =
        GUI_HOST_STAGED_NONE;
    queue->cancellation_requested = false;
    return true;
}

void gui_host_completion_destroy(
    gui_host_completion *completion) {
    if (!completion) {
        return;
    }
    tp_session_job_result_destroy(
        &completion->result);
    memset(completion, 0, sizeof *completion);
}

bool gui_host_queue_busy(
    const gui_host_queue *queue) {
    return queue &&
           (queue->command_count > 0U ||
            queue->active ||
            queue->staged.present);
}

bool gui_host_queue_cancelling(
    const gui_host_queue *queue) {
    return queue &&
           queue->cancellation_requested &&
           gui_host_queue_busy(queue);
}

tp_session_job_kind gui_host_queue_active_kind(
    const gui_host_queue *queue) {
    if (!queue) {
        return TP_SESSION_JOB_NONE;
    }
    if (queue->active) {
        return queue->active_envelope.kind;
    }
    if (queue->command_count > 0U &&
        queue->commands[0].kind !=
            GUI_HOST_COMMAND_CANCEL) {
        return queue->commands[0]
            .envelope.kind;
    }
    if (queue->staged.present) {
        return queue->staged.envelope.kind;
    }
    return TP_SESSION_JOB_NONE;
}

gui_host_lifecycle_state
gui_host_queue_lifecycle(
    const gui_host_queue *queue) {
    return queue ? queue->lifecycle
                 : GUI_HOST_CLOSED;
}

#ifdef TP_ENABLE_TEST_SEAMS
bool gui_host_queue__test_peek_start(
    const gui_host_queue *queue,
    gui_host_command *out) {
    if (!queue || !out ||
        queue->command_count == 0U ||
        queue->commands[0].kind ==
            GUI_HOST_COMMAND_CANCEL) {
        return false;
    }
    *out = queue->commands[0];
    return true;
}

bool gui_host_queue__test_has_staged(
    const gui_host_queue *queue) {
    return queue && queue->staged.present;
}

void gui_host_queue__test_retag_staged_request(
    gui_host_queue *queue, uint64_t request_id) {
    if (queue && queue->staged.present) {
        queue->staged.envelope.request_id =
            request_id;
    }
}

void gui_host_queue__test_fail_next_poll(void) {
    s_test_fail_next_poll = true;
}

void gui_host_queue__test_fail_next_take(void) {
    s_test_fail_next_take = true;
}

bool gui_host_queue__test_active(
    const gui_host_queue *queue) {
    return queue && queue->active;
}
#endif
