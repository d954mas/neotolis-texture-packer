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
gui_host_lifecycle_state
gui_host_binding_lifecycle(
    const gui_host_binding *binding);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_HOST_BINDING_H */
