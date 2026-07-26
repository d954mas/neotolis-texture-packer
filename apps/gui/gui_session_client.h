#ifndef NTPACKER_GUI_SESSION_CLIENT_H
#define NTPACKER_GUI_SESSION_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUI_SESSION_CLIENT_MAX_REDUCERS 16U

typedef void (*gui_session_client_reducer_fn)(
    void *context, const tp_session_observation *observation,
    uint64_t session_instance_generation);

typedef struct gui_session_client_reducer {
    gui_session_client_reducer_fn reduce;
    void *context;
} gui_session_client_reducer;

typedef struct gui_session_client {
    tp_session *session;
    tp_session_observation *latest;
    tp_session_observation *snapshot_owner;
    tp_session_observation_token observed;
    uint64_t session_instance_generation;
    uint64_t snapshot_lifetime_generation;
    gui_session_client_reducer
        reducers[GUI_SESSION_CLIENT_MAX_REDUCERS];
    size_t reducer_count;
    bool frame_pinned;
    bool observe_requested;
} gui_session_client;

void gui_session_client_init(gui_session_client *client);
tp_status gui_session_client_register_reducer(
    gui_session_client *client, gui_session_client_reducer_fn reduce,
    void *context, tp_error *err);

/* Initial observation is prepared before the old binding is released. On
 * failure the client, token, generation, and frame observation are unchanged. */
tp_status gui_session_client_attach(
    gui_session_client *client, tp_session *session, tp_error *err);
tp_status gui_session_client_observe(
    gui_session_client *client, tp_error *err);
tp_status gui_session_client_resync(
    gui_session_client *client, tp_error *err);

/* A frame begin observes once and pins both the latest typed observation and
 * the most recent immutable model snapshot until frame_end. */
tp_status gui_session_client_frame_begin(
    gui_session_client *client, tp_error *err);
void gui_session_client_frame_end(gui_session_client *client);

/* Legacy mutation invalidation may request an earlier atomic observation.
 * While a frame is pinned the request is deferred; it never supplies state. */
tp_status gui_session_client_request_observe(
    gui_session_client *client, tp_error *err);
void gui_session_client_detach(gui_session_client *client);

const tp_session_snapshot *gui_session_client_snapshot(
    const gui_session_client *client);
const tp_session_observation *gui_session_client_observation(
    const gui_session_client *client);
tp_session_observation_token gui_session_client_observed_token(
    const gui_session_client *client);
uint64_t gui_session_client_instance_generation(
    const gui_session_client *client);
uint64_t gui_session_client_snapshot_lifetime_generation(
    const gui_session_client *client);
uint64_t gui_session_client_source_runtime_generation(
    const gui_session_client *client);
tp_session_job_observed_state gui_session_client_job_state(
    const gui_session_client *client);
bool gui_session_client_frame_is_pinned(
    const gui_session_client *client);

#ifdef TP_ENABLE_TEST_SEAMS
void gui_session_client__test_fail_next_observe(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SESSION_CLIENT_H */
