#include "gui_host_binding.h"

#include <string.h>

#include "core/nt_assert.h"

static void reduce_queue(
    void *context,
    const tp_session_observation *observation,
    uint64_t session_instance_generation) {
    gui_host_binding *binding = context;
    NT_ASSERT(binding != NULL);
    gui_host_queue_reduce_observation(
        &binding->queue, observation,
        session_instance_generation);
}

void gui_host_binding_init(
    gui_host_binding *binding) {
    if (!binding) {
        return;
    }
    memset(binding, 0, sizeof *binding);
    gui_session_client_init(&binding->client);
    gui_host_queue_init(&binding->queue);
    tp_error error = {{0}};
    const tp_status status =
        gui_session_client_register_reducer(
            &binding->client, reduce_queue,
            binding, &error);
    NT_ASSERT(status == TP_STATUS_OK);
}

tp_status gui_host_binding_attach_initial(
    gui_host_binding *binding,
    tp_session *session, tp_error *err) {
    if (!binding || !session) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host initial attach requires binding and session");
    }
    if (binding->transition !=
            GUI_HOST_TRANSITION_NONE ||
        gui_session_client_attached_session(
            &binding->client) ||
        gui_host_queue_lifecycle(
            &binding->queue) !=
            GUI_HOST_CLOSED) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host initial attach requires an empty binding");
    }
    const tp_status attach_status =
        gui_session_client_attach(
            &binding->client, session, err);
    if (attach_status != TP_STATUS_OK) {
        return attach_status;
    }
    const tp_status open_status =
        gui_host_queue_open(
            &binding->queue,
            gui_session_client_instance_generation(
                &binding->client),
            err);
    NT_ASSERT(open_status == TP_STATUS_OK);
    return TP_STATUS_OK;
}

static bool binding_can_begin(
    const gui_host_binding *binding) {
    return binding &&
           binding->transition ==
               GUI_HOST_TRANSITION_NONE &&
           gui_session_client_attached_session(
               &binding->client) &&
           gui_host_queue_lifecycle(
               &binding->queue) ==
               GUI_HOST_OPEN;
}

tp_status gui_host_binding_begin_replace(
    gui_host_binding *binding,
    tp_session *candidate,
    bool discard_retired_session,
    tp_error *err) {
    if (!candidate) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host replacement requires a candidate");
    }
    if (binding &&
        candidate ==
            gui_session_client_attached_session(
                &binding->client)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host replacement candidate aliases the active session");
    }
    if (!binding_can_begin(binding)) {
        tp_session_destroy(candidate);
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host replacement requires an open idle lifecycle owner");
    }
    gui_session_client_prepared prepared = {0};
    const tp_status prepare_status =
        gui_session_client_prepare(
            &binding->client, candidate,
            &prepared, err);
    if (prepare_status != TP_STATUS_OK) {
        tp_session_destroy(candidate);
        return prepare_status;
    }
    const tp_status drain_status =
        gui_host_queue_begin_drain(
            &binding->queue, err);
    if (drain_status != TP_STATUS_OK) {
        gui_session_client_cancel_prepared(
            &prepared);
        tp_session_destroy(candidate);
        return drain_status;
    }
    binding->prepared = prepared;
    gui_session_client_close_admission(
        &binding->client);
    binding->transition =
        GUI_HOST_TRANSITION_REPLACE;
    binding->discard_retired_session =
        discard_retired_session;
    return TP_STATUS_OK;
}

tp_status gui_host_binding_begin_shutdown(
    gui_host_binding *binding,
    bool discard_retired_session,
    tp_error *err) {
    if (!binding_can_begin(binding)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host shutdown requires an open idle lifecycle owner");
    }
    const tp_status drain_status =
        gui_host_queue_begin_drain(
            &binding->queue, err);
    if (drain_status != TP_STATUS_OK) {
        return drain_status;
    }
    gui_session_client_close_admission(
        &binding->client);
    binding->transition =
        GUI_HOST_TRANSITION_SHUTDOWN;
    binding->discard_retired_session =
        discard_retired_session;
    return TP_STATUS_OK;
}

static void retire_session(
    tp_session *session, bool discard) {
    if (!session) {
        return;
    }
    if (discard) {
        (void)tp_session_discard(
            session, NULL);
    }
    tp_session_destroy(session);
}

static void complete_replace(
    gui_host_binding *binding,
    gui_host_transition_kind *completed) {
    NT_ASSERT(binding != NULL);
    NT_ASSERT(
        binding->transition ==
        GUI_HOST_TRANSITION_REPLACE);
    const uint64_t generation =
        binding->prepared.next_instance_generation;
    NT_ASSERT(generation != 0U);
    gui_host_queue_commit_cutover(
        &binding->queue, generation);
    tp_session *retired =
        gui_session_client_commit_prepared(
            &binding->client,
            &binding->prepared);
    NT_ASSERT(
        gui_session_client_instance_generation(
            &binding->client) ==
        generation);
    retire_session(
        retired,
        binding->discard_retired_session);
    *completed = GUI_HOST_TRANSITION_REPLACE;
    binding->transition =
        GUI_HOST_TRANSITION_NONE;
    binding->discard_retired_session = false;
}

static void complete_shutdown(
    gui_host_binding *binding,
    gui_host_transition_kind *completed) {
    NT_ASSERT(binding != NULL);
    NT_ASSERT(
        binding->transition ==
        GUI_HOST_TRANSITION_SHUTDOWN);
    gui_host_queue_commit_close(
        &binding->queue);
    tp_session *retired =
        gui_session_client_attached_session(
            &binding->client);
    gui_session_client_detach(
        &binding->client);
    retire_session(
        retired,
        binding->discard_retired_session);
    *completed = GUI_HOST_TRANSITION_SHUTDOWN;
    binding->transition =
        GUI_HOST_TRANSITION_NONE;
    binding->discard_retired_session = false;
}

tp_status gui_host_binding_pump(
    gui_host_binding *binding,
    gui_host_transition_kind *completed,
    tp_error *err) {
    if (!binding || !completed) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "GUI host pump requires binding and completion output");
    }
    *completed = GUI_HOST_TRANSITION_NONE;
    if (binding->transition ==
        GUI_HOST_TRANSITION_NONE) {
        tp_session *active =
            gui_session_client_attached_session(
                &binding->client);
        if (!active ||
            gui_host_queue_lifecycle(
                &binding->queue) !=
                GUI_HOST_OPEN) {
            return TP_STATUS_OK;
        }
        const tp_status drain_status =
            gui_host_queue_drain(
                &binding->queue, active, err);
        if (drain_status != TP_STATUS_OK) {
            return drain_status;
        }
        return TP_STATUS_OK;
    }
    if (gui_host_queue_lifecycle(
            &binding->queue) ==
        GUI_HOST_DRAINING) {
        tp_session *active =
            gui_session_client_attached_session(
                &binding->client);
        NT_ASSERT(active != NULL);
        const tp_status drain_status =
            gui_host_queue_drain(
                &binding->queue, active, err);
        if (drain_status != TP_STATUS_OK) {
            return drain_status;
        }
    }
    if (gui_host_queue_lifecycle(
            &binding->queue) !=
        GUI_HOST_READY_TO_CUTOVER) {
        return TP_STATUS_OK;
    }
    if (binding->transition ==
        GUI_HOST_TRANSITION_REPLACE) {
        complete_replace(binding, completed);
    } else {
        complete_shutdown(binding, completed);
    }
    return TP_STATUS_OK;
}

gui_host_lifecycle_state
gui_host_binding_lifecycle(
    const gui_host_binding *binding) {
    return binding
               ? gui_host_queue_lifecycle(
                     &binding->queue)
               : GUI_HOST_CLOSED;
}
