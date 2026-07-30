#ifndef NTPACKER_GUI_HOST_QUEUE_H
#define NTPACKER_GUI_HOST_QUEUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_export.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum gui_host_lifecycle_state {
    GUI_HOST_CLOSED = 0,
    GUI_HOST_OPEN,
    GUI_HOST_DRAINING,
    GUI_HOST_READY_TO_CUTOVER
} gui_host_lifecycle_state;

typedef enum gui_host_staged_classification {
    GUI_HOST_STAGED_NONE = 0,
    GUI_HOST_STAGED_PENDING_OBSERVATION,
    GUI_HOST_STAGED_READY
} gui_host_staged_classification;

typedef struct gui_host_job_envelope {
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
} gui_host_job_envelope;

/* `rejection` is the authoritative machine class of a completion that publishes
 * no result; `status`/`error` carry the human-readable detail of the same fact
 * and never contradict it. A completion the session observed carries the class
 * the session admitted; a completion the host itself refused before admission
 * carries the host's own class. */
typedef struct gui_host_completion {
    bool publish_result;
    gui_host_job_envelope envelope;
    tp_session_job_state state;
    tp_session_job_rejection rejection;
    tp_status status;
    tp_error error;
    tp_session_job_result result;
} gui_host_completion;

typedef struct gui_host_command {
    gui_host_job_envelope envelope;
    tp_id128 atlas_id;
    char work_dir[TP_IDENTITY_PATH_MAX];
    char preview_exporter_id[TP_EXPORTER_ID_MAX];
} gui_host_command;

typedef struct gui_host_queue {
    gui_host_lifecycle_state lifecycle;
    uint64_t session_instance_generation;
    uint64_t next_request_id;
    gui_host_command pending_start;
    gui_host_job_envelope active_envelope;
    bool cancellation_requested;
    bool cancel_queued;
    gui_host_staged_classification staged_classification;
    gui_host_completion staged;
} gui_host_queue;

void gui_host_queue_init(gui_host_queue *queue);
tp_status gui_host_queue_open(
    gui_host_queue *queue, uint64_t session_instance_generation,
    tp_error *err);
tp_status gui_host_queue_begin_drain(
    gui_host_queue *queue, tp_error *err);
void gui_host_queue_commit_cutover(
    gui_host_queue *queue,
    uint64_t session_instance_generation);
void gui_host_queue_commit_close(
    gui_host_queue *queue);
/* Forced close for the host's exhausted shutdown budget: discards the pending
 * start, the active lease and any staged completion, and reaches CLOSED from
 * any lifecycle state. Unlike commit_close it asserts nothing about drainage --
 * the session behind those leases is destroyed immediately afterwards. */
void gui_host_queue_force_close(
    gui_host_queue *queue);

tp_status gui_host_queue_enqueue_pack(
    gui_host_queue *queue, tp_id128 atlas_id,
    const char *work_dir, const char *preview_exporter_id,
    tp_error *err);
tp_status gui_host_queue_enqueue_export(
    gui_host_queue *queue, tp_id128 atlas_id,
    const char *work_dir, tp_error *err);
tp_status gui_host_queue_enqueue_cancel(
    gui_host_queue *queue, tp_error *err);

/* Host-thread-only admission call. The queue borrows `session` only for this
 * call and never stores it. All request strings were copied on enqueue. */
tp_status gui_host_queue_drain(
    gui_host_queue *queue, tp_session *session, tp_error *err);

/* Reducer called from gui_session_client before observation publication. */
void gui_host_queue_reduce_observation(
    gui_host_queue *queue,
    const tp_session_observation *observation,
    uint64_t session_instance_generation);

bool gui_host_queue_take_completion(
    gui_host_queue *queue, gui_host_completion *out);
void gui_host_completion_destroy(
    gui_host_completion *completion);

bool gui_host_queue_busy(const gui_host_queue *queue);
tp_session_job_kind gui_host_queue_active_kind(
    const gui_host_queue *queue);
gui_host_lifecycle_state gui_host_queue_lifecycle(
    const gui_host_queue *queue);

#ifdef TP_ENABLE_TEST_SEAMS
bool gui_host_queue__test_peek_start(
    const gui_host_queue *queue, gui_host_command *out);
bool gui_host_queue__test_has_staged(
    const gui_host_queue *queue);
void gui_host_queue__test_retag_staged_request(
    gui_host_queue *queue, uint64_t request_id);
/* Ages a queued start out of the live host instance. Every lifecycle transition
 * that changes the instance generation clears the pending start first, so the
 * drain's OLD_INSTANCE guard has no other reachable driver. */
void gui_host_queue__test_retag_pending_start_instance(
    gui_host_queue *queue, uint64_t session_instance_generation);
/* Positions the monotonic request-id counter so the exhaustion branch in start
 * admission is reachable in a test without submitting 2^64 jobs. */
void gui_host_queue__test_set_next_request_id(
    gui_host_queue *queue, uint64_t next_request_id);
void gui_host_queue__test_fail_next_poll(void);
void gui_host_queue__test_fail_next_take(void);
/* Makes the next `count` cancel admissions fail with a TRANSIENT rejection
 * (never NOT_FOUND), so the drain's keep-queued path can be driven. */
void gui_host_queue__test_fail_cancels(unsigned count);
bool gui_host_queue__test_active(
    const gui_host_queue *queue);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_HOST_QUEUE_H */
