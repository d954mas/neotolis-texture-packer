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

/* Forced terminalize + close. The ordinary shutdown is a bounded, non-blocking
 * negotiation; when the host exhausts its retry budget it still has to LEAVE,
 * and it must leave in the CLOSED state the rest of the GUI teardown asserts.
 * The order matters: the queue drops its leases first (they name a job in a
 * session that is about to be destroyed), then the prepared candidate of a
 * replacement that will never cut over is released, then the active session is
 * retired through the same owner teardown as a clean close -- destroying the
 * session is what terminates the worker process. */
void gui_host_binding_force_close(
    gui_host_binding *binding) {
    if (!binding) {
        return;
    }
    gui_host_queue_force_close(&binding->queue);
    if (binding->transition ==
        GUI_HOST_TRANSITION_REPLACE) {
        gui_session_client_cancel_prepared(
            &binding->prepared);
    }
    binding->prepared =
        (gui_session_client_prepared){0};
    tp_session *retired =
        gui_session_client_attached_session(
            &binding->client);
    gui_session_client_detach(&binding->client);
    retire_session(
        retired,
        binding->discard_retired_session);
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
    /* ONE drain call site for both pump shapes. They differ only in WHICH queue
     * lifecycle is drainable and in how a missing session is treated: an idle
     * binding drains an OPEN queue and tolerates "no session attached yet",
     * while a binding mid-transition drains a DRAINING queue and asserts the
     * session is still attached (the transition owns it until cutover). The
     * lifecycle is re-read after the drain -- draining is what advances it to
     * READY_TO_CUTOVER. */
    const bool idle = binding->transition ==
                      GUI_HOST_TRANSITION_NONE;
    const gui_host_lifecycle_state drainable =
        idle ? GUI_HOST_OPEN : GUI_HOST_DRAINING;
    if (gui_host_queue_lifecycle(&binding->queue) ==
        drainable) {
        tp_session *active =
            gui_session_client_attached_session(
                &binding->client);
        if (idle && !active) {
            return TP_STATUS_OK;
        }
        NT_ASSERT(active != NULL);
        const tp_status drain_status =
            gui_host_queue_drain(
                &binding->queue, active, err);
        if (drain_status != TP_STATUS_OK) {
            return drain_status;
        }
    }
    if (idle ||
        gui_host_queue_lifecycle(&binding->queue) !=
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

/* Ingress and commands are refused while the owner is closed or draining: an
 * uninitialized binding is all-zero, so CLOSED covers "no owner yet" too. */
static bool binding_is_open(
    const gui_host_binding *binding) {
    return gui_host_binding_lifecycle(binding) ==
           GUI_HOST_OPEN;
}

static tp_status ingress_closed(tp_error *err) {
    return tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT,
        "GUI session ingress is closed during lifecycle transition");
}

tp_status gui_host_binding_enqueue_pack(
    gui_host_binding *binding, tp_id128 atlas_id,
    const char *work_dir,
    const char *preview_exporter_id,
    tp_error *err) {
    if (!binding_is_open(binding)) {
        return ingress_closed(err);
    }
    return gui_host_queue_enqueue_pack(
        &binding->queue, atlas_id, work_dir,
        preview_exporter_id, err);
}

tp_status gui_host_binding_enqueue_export(
    gui_host_binding *binding, tp_id128 atlas_id,
    const char *work_dir, tp_error *err) {
    if (!binding_is_open(binding)) {
        return ingress_closed(err);
    }
    return gui_host_queue_enqueue_export(
        &binding->queue, atlas_id, work_dir, err);
}

/* Cancel is deliberately admitted while draining: a lifecycle transition is
 * exactly when the running job has to be asked to stop. */
tp_status gui_host_binding_enqueue_cancel(
    gui_host_binding *binding, tp_error *err) {
    if (!binding) {
        return ingress_closed(err);
    }
    return gui_host_queue_enqueue_cancel(
        &binding->queue, err);
}

bool gui_host_binding_take_completion(
    gui_host_binding *binding,
    gui_host_completion *out) {
    return binding && out &&
           gui_host_queue_take_completion(
               &binding->queue, out);
}

bool gui_host_binding_job_busy(
    const gui_host_binding *binding) {
    return binding &&
           gui_host_queue_busy(&binding->queue);
}

tp_session_job_kind
gui_host_binding_job_active_kind(
    const gui_host_binding *binding) {
    return binding ? gui_host_queue_active_kind(
                         &binding->queue)
                   : TP_SESSION_JOB_NONE;
}

/* The host owner holds the sole active-session pointer. Commands take it from
 * that ownership instead of a global borrow. */
static tp_session *command_session(
    gui_host_binding *binding) {
    if (!binding_is_open(binding)) {
        return NULL;
    }
    return gui_session_client_attached_session(
        &binding->client);
}

static tp_session *query_session(
    const gui_host_binding *binding) {
    return binding ? gui_session_client_attached_session(
                         &binding->client)
                   : NULL;
}

static tp_status command_closed(tp_error *err) {
    return tp_error_set(
        err, TP_STATUS_INVALID_ARGUMENT,
        "GUI host command requires an open session owner");
}

bool gui_host_binding_can_undo(
    const gui_host_binding *binding) {
    return tp_session_can_undo(
        query_session(binding));
}

bool gui_host_binding_can_redo(
    const gui_host_binding *binding) {
    return tp_session_can_redo(
        query_session(binding));
}

int gui_host_binding_undo_depth(
    const gui_host_binding *binding) {
    return tp_session_undo_depth(
        query_session(binding));
}

int gui_host_binding_redo_depth(
    const gui_host_binding *binding) {
    return tp_session_redo_depth(
        query_session(binding));
}

tp_status gui_host_binding_undo(
    gui_host_binding *binding, tp_error *err) {
    tp_session *session = command_session(binding);
    return session ? tp_session_undo(session, err)
                   : command_closed(err);
}

tp_status gui_host_binding_redo(
    gui_host_binding *binding, tp_error *err) {
    tp_session *session = command_session(binding);
    return session ? tp_session_redo(session, err)
                   : command_closed(err);
}

tp_status gui_host_binding_invalidate_sources(
    gui_host_binding *binding, tp_error *err) {
    tp_session *session = command_session(binding);
    return session
               ? tp_session_invalidate_sources(
                     session, err)
               : command_closed(err);
}

tp_status gui_host_binding_save(
    gui_host_binding *binding,
    tp_session_save_result *out, tp_error *err) {
    tp_session *session = command_session(binding);
    return session
               ? tp_session_save(session, out, err)
               : command_closed(err);
}

tp_status gui_host_binding_save_as(
    gui_host_binding *binding,
    const char *canonical_path,
    tp_session_save_result *out, tp_error *err) {
    tp_session *session = command_session(binding);
    return session
               ? tp_session_save_as(
                     session, canonical_path, out,
                     err)
               : command_closed(err);
}

#ifdef TP_ENABLE_TEST_SEAMS
bool gui_host_binding__test_has_staged(
    const gui_host_binding *binding) {
    return binding &&
           gui_host_queue__test_has_staged(
               &binding->queue);
}
#endif
