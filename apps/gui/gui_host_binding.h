#ifndef NTPACKER_GUI_HOST_BINDING_H
#define NTPACKER_GUI_HOST_BINDING_H

#include <stdbool.h>
#include <stdint.h>

#include "gui_host_queue.h"
#include "gui_session_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gui_host_transition_kind {
    GUI_HOST_TRANSITION_NONE = 0,
    GUI_HOST_TRANSITION_REPLACE,
    GUI_HOST_TRANSITION_SHUTDOWN
} gui_host_transition_kind;

typedef struct gui_host_binding {
    gui_session_client client;
    gui_host_queue queue;
    gui_session_client_prepared prepared;
    gui_host_transition_kind transition;
    bool discard_retired_session;
} gui_host_binding;

void gui_host_binding_init(gui_host_binding *binding);

/* Initial startup only. On failure the caller retains `session`; on success
 * the binding consumes it as its sole active-session owner. */
tp_status gui_host_binding_attach_initial(
    gui_host_binding *binding, tp_session *session,
    tp_error *err);

/* Consumes `candidate` on every return path. Successful preparation remains
 * invisible until a later pump reaches the non-fallible cutover. */
tp_status gui_host_binding_begin_replace(
    gui_host_binding *binding, tp_session *candidate,
    bool discard_retired_session, tp_error *err);
tp_status gui_host_binding_begin_shutdown(
    gui_host_binding *binding,
    bool discard_retired_session, tp_error *err);

/* Between-frame host-owner pump. It never blocks waiting for a running job;
 * the existing queue/session process owner performs bounded cancellation
 * escalation across repeated calls. */
tp_status gui_host_binding_pump(
    gui_host_binding *binding,
    gui_host_transition_kind *completed,
    tp_error *err);
/* Escape hatch for an exhausted shutdown budget: force the owner terminal and
 * CLOSED in one non-fallible step -- discard the queue's leases and any
 * prepared candidate, then retire the active session through the same teardown
 * a clean close uses (which is what kills the worker process). Safe to call in
 * any lifecycle state, including CLOSED. */
void gui_host_binding_force_close(
    gui_host_binding *binding);
gui_host_lifecycle_state
gui_host_binding_lifecycle(
    const gui_host_binding *binding);

/* Job ingress. The queue is the host owner's private admission structure, so
 * every request and every completion crosses this seam instead of reaching
 * `binding->queue` directly. Request payloads are copied on enqueue; the
 * completion is the caller's to destroy with `gui_host_completion_destroy`. */
tp_status gui_host_binding_enqueue_pack(
    gui_host_binding *binding, tp_id128 atlas_id,
    const char *work_dir,
    const char *preview_exporter_id, tp_error *err);
tp_status gui_host_binding_enqueue_export(
    gui_host_binding *binding, tp_id128 atlas_id,
    const char *work_dir, tp_error *err);
tp_status gui_host_binding_enqueue_cancel(
    gui_host_binding *binding, tp_error *err);
bool gui_host_binding_take_completion(
    gui_host_binding *binding,
    gui_host_completion *out);
bool gui_host_binding_job_busy(
    const gui_host_binding *binding);
tp_session_job_kind
gui_host_binding_job_active_kind(
    const gui_host_binding *binding);

/* Host-owner commands (spec 6.1/9). The binding holds the sole active-session
 * pointer, so history, identity, and source-runtime commands run here and no
 * frontend file borrows the session to issue them. The mutating commands
 * require an open (non-draining) lifecycle owner; the depth/availability
 * queries are pure reads and stay valid across a drain. Commands carry no
 * receipt: their outcome is the next atomic observation. */
bool gui_host_binding_can_undo(
    const gui_host_binding *binding);
bool gui_host_binding_can_redo(
    const gui_host_binding *binding);
int gui_host_binding_undo_depth(
    const gui_host_binding *binding);
int gui_host_binding_redo_depth(
    const gui_host_binding *binding);
tp_status gui_host_binding_undo(
    gui_host_binding *binding, tp_error *err);
tp_status gui_host_binding_redo(
    gui_host_binding *binding, tp_error *err);
tp_status gui_host_binding_invalidate_sources(
    gui_host_binding *binding, tp_error *err);
tp_status gui_host_binding_save(
    gui_host_binding *binding,
    tp_session_save_result *out, tp_error *err);
tp_status gui_host_binding_save_as(
    gui_host_binding *binding,
    const char *canonical_path,
    tp_session_save_result *out, tp_error *err);

#ifdef TP_ENABLE_TEST_SEAMS
bool gui_host_binding__test_has_staged(
    const gui_host_binding *binding);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_HOST_BINDING_H */
