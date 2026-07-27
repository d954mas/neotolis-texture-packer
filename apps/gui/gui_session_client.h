#ifndef NTPACKER_GUI_SESSION_CLIENT_H
#define NTPACKER_GUI_SESSION_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_session.h"
#include "tp_core/tp_transaction.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GUI_SESSION_CLIENT_MAX_REDUCERS 16U
#define GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS 64U
#define GUI_SESSION_CLIENT_MAX_ID_GENERATION_ATTEMPTS 8U

typedef enum gui_session_submit_echo_state {
    GUI_SESSION_SUBMIT_ECHO_NOT_APPLICABLE = 0,
    GUI_SESSION_SUBMIT_ECHO_PENDING,
    GUI_SESSION_SUBMIT_ECHO_OBSERVED,
} gui_session_submit_echo_state;

typedef struct gui_session_submit_identity {
    tp_id128 origin_view_id;
    tp_id128 draft_instance_id;
} gui_session_submit_identity;

typedef struct gui_session_submit_request {
    tp_operation *operations;
    int operation_count;
    int64_t expected_revision;
    const char *semantic_label;
    gui_session_submit_identity identity;
    /* NULL generates a fresh production ID. A canonical 32-lowercase-hex
     * value retries the exact retained transaction without retaining ops. */
    const char *retained_transaction_id;
} gui_session_submit_request;

typedef struct gui_session_submit_terminal {
    char transaction_id[33];
    gui_session_submit_identity identity;
    tp_status status;
    bool committed;
    bool no_change;
    int64_t revision;
    gui_session_submit_echo_state echo_state;
} gui_session_submit_terminal;

typedef struct gui_session_submit_result {
    tp_txn_result transaction;
    gui_session_submit_terminal terminal;
    tp_status terminal_status;
    tp_status observation_status;
    bool observation_pending;
} gui_session_submit_result;

typedef struct gui_session_pending_submit {
    bool result_available;
    tp_status observation_status;
    gui_session_submit_terminal terminal;
    tp_txn_result result;
} gui_session_pending_submit;

typedef void (*gui_session_client_reducer_fn)(
    void *context, const tp_session_observation *observation,
    uint64_t session_instance_generation);

typedef struct gui_session_client_reducer {
    gui_session_client_reducer_fn reduce;
    void *context;
} gui_session_client_reducer;

typedef struct gui_session_client_prepared {
    tp_session *session;
    tp_session_observation *initial;
    uint64_t next_instance_generation;
} gui_session_client_prepared;

typedef struct gui_session_client {
    tp_session *session;
    tp_session_observation *latest;
    tp_session_observation *snapshot_owner;
    tp_session_observation_token observed;
    uint64_t session_instance_generation;
    uint64_t snapshot_lifetime_generation;
    gui_session_client_reducer
        reducers[GUI_SESSION_CLIENT_MAX_REDUCERS];
    gui_session_pending_submit
        pending[GUI_SESSION_CLIENT_MAX_PENDING_SUBMITS];
    tp_rng transaction_rng;
    size_t reducer_count;
    bool frame_pinned;
    bool admission_open;
} gui_session_client;

void gui_session_client_init(gui_session_client *client);
tp_status gui_session_client_register_reducer(
    gui_session_client *client, gui_session_client_reducer_fn reduce,
    void *context, tp_error *err);

/* Initial observation is prepared before the old binding is released. On
 * failure the client, token, generation, and frame observation are unchanged. */
tp_status gui_session_client_attach(
    gui_session_client *client, tp_session *session, tp_error *err);
tp_status gui_session_client_prepare(
    gui_session_client *client, tp_session *session,
    gui_session_client_prepared *prepared, tp_error *err);
void gui_session_client_cancel_prepared(
    gui_session_client_prepared *prepared);
tp_session *gui_session_client_commit_prepared(
    gui_session_client *client,
    gui_session_client_prepared *prepared);
void gui_session_client_close_admission(
    gui_session_client *client);
tp_status gui_session_client_observe(
    gui_session_client *client, tp_error *err);
tp_status gui_session_client_resync(
    gui_session_client *client, tp_error *err);
tp_status gui_session_client_submit(
    gui_session_client *client,
    const gui_session_submit_request *request,
    gui_session_submit_result *out, tp_error *err);
void gui_session_submit_result_destroy(
    gui_session_submit_result *result);
bool gui_session_client_pending_submit_query(
    const gui_session_client *client,
    const char transaction_id[33],
    gui_session_submit_identity identity,
    gui_session_submit_terminal *out);
/* A frame begin observes once and pins both the latest typed observation and
 * the most recent immutable model snapshot until frame_end. */
tp_status gui_session_client_frame_begin(
    gui_session_client *client, tp_error *err);
void gui_session_client_frame_end(gui_session_client *client);

/* Source-runtime and lifecycle owners may request an earlier atomic
 * observation. While a frame is pinned the request is deferred; it never
 * supplies state. */
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
tp_session *gui_session_client_attached_session(
    const gui_session_client *client);

#ifdef TP_ENABLE_TEST_SEAMS
void gui_session_client__test_fail_next_observe(void);
void gui_session_client__test_fail_observes(unsigned int count);
void gui_session_client__test_fail_next_result_copy(void);
void gui_session_client__test_set_transaction_rng(
    gui_session_client *client, tp_rng rng);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_SESSION_CLIENT_H */
