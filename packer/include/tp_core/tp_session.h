#ifndef TP_CORE_TP_SESSION_H
#define TP_CORE_TP_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_selector.h"
#include "tp_core/tp_session_snapshot_query.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_session tp_session;
typedef struct tp_session_observation tp_session_observation;
typedef struct tp_session_job_result tp_session_job_result;
typedef struct tp_txn_request tp_txn_request;
typedef struct tp_txn_result tp_txn_result;
typedef struct tp_project tp_project;
typedef struct tp_journal tp_journal;

/* ---- shared visible History (F3, master spec §8-§9.5) -------------------- *
 * One session-owned enumerable History surface shared by every view (GUI/MCP/Dev
 * API). It is a pull model: a client counts rows and copies each out; there are no
 * push callbacks, so the F3-01 "subscriber disconnect / callback reentrancy" faults
 * are N/A here (see the event API note below).
 *
 * DESIGN. The model's in-memory undo/redo stack (tp_diff.h) stays the SINGLE
 * authority for edit rows and the cursor -- the session keeps NO second copy.
 * Undo/Redo MOVE THE CURSOR; they do not add rows (a row appears once, when its
 * transaction commits). `undoable`/`undone` on each edit row project the cursor:
 * rows below it are undoable, rows in the redo branch read `undone`. This is the
 * §8/§9 semantics -- a new edit after Undo discards the redo branch, so those rows
 * simply disappear from the enumeration. The UNDO/REDO kinds are reserved (kept for
 * the append-only enum); the cursor model never materializes them as rows.
 *
 * Save checkpoints (§9.2) and runtime-source refreshes (§9.3) are NON-undoable
 * markers the session interleaves into the spine. They never change revision, the
 * cursor, or the edit-record budget. Markers are session-side bounded state: a
 * fixed FIFO cap, plus eviction tied to the edit window -- a marker whose anchoring
 * edit record is FIFO-evicted or redo-discarded is dropped with it, so markers can
 * never grow unbounded. A failed Save records no checkpoint. Normal reopen starts a
 * fresh (empty) History. */
typedef enum tp_session_history_kind {
    TP_SESSION_HISTORY_EDIT = 1,
    TP_SESSION_HISTORY_UNDO = 2,             /* reserved: cursor model, not emitted */
    TP_SESSION_HISTORY_REDO = 3,             /* reserved: cursor model, not emitted */
    TP_SESSION_HISTORY_SAVE_CHECKPOINT = 4,
    TP_SESSION_HISTORY_RUNTIME_REFRESH = 5
} tp_session_history_kind;

/* Copy-out display bounds for the variable-length transaction strings. Longer
 * values are truncated for the visible panel; the author id used for A6 badging
 * fits comfortably ("human", "agent(<id>)"). */
#define TP_SESSION_HISTORY_LABEL_MAX 256
#define TP_SESSION_HISTORY_AUTHOR_MAX 128

/* Immutable value DTO copied out by tp_session_history_at (no borrowed pointers
 * cross the boundary, following tp_session_recovery_health). */
typedef struct tp_session_history_entry {
    tp_session_history_kind kind;
    /* EDIT: the revision the transaction produced. Marker: the model revision at
     * the marker (a marker never advances revision, so it equals the row below). */
    int64_t revision;
    /* EDIT rows only; markers leave these empty. `author` is A6-ready -- it passes
     * through exactly what the transaction carried (a future MCP writes its own). */
    char label[TP_SESSION_HISTORY_LABEL_MAX];
    char author[TP_SESSION_HISTORY_AUTHOR_MAX];
    char transaction_id[33];
    /* SAVE_CHECKPOINT (§9.2): the saved state identity + canonical path. Nil id /
     * empty path on every other kind. */
    tp_id128 state_identity;
    char path[TP_IDENTITY_PATH_MAX];
    /* Cursor projection. An EDIT below the cursor is `undoable`; an EDIT in the redo
     * branch is `undone`. A marker is never undoable and reads `undone` only while it
     * sits above the cursor (its saved/refreshed state is ahead of the current one). */
    bool undoable;
    bool undone;
} tp_session_history_entry;

/* Session calls are synchronous and serialized. They are intentionally
 * non-reentrant: side-effect/journal callbacks invoked during admission must not
 * call back into this same session. There is no actor thread or hidden mailbox. */

typedef enum tp_session_event_kind {
    TP_SESSION_EVENT_MODEL_COMMITTED = 1,
    TP_SESSION_EVENT_UNDONE = 2,
    TP_SESSION_EVENT_REDONE = 3,
    TP_SESSION_EVENT_SAVED = 4,
    TP_SESSION_EVENT_DISCARDED = 5,
    TP_SESSION_EVENT_SOURCE_RUNTIME_CHANGED = 6
} tp_session_event_kind;

typedef struct tp_session_event {
    uint64_t sequence;
    tp_session_event_kind kind;
    int64_t revision_before;
    int64_t revision_after;
    uint64_t admission_sequence;
    uint64_t model_generation;
    uint64_t source_generation;
    char transaction_id[33];
    char label[TP_SESSION_HISTORY_LABEL_MAX];
    char author[TP_SESSION_HISTORY_AUTHOR_MAX];
} tp_session_event;

typedef enum tp_session_job_kind {
    TP_SESSION_JOB_NONE = 0,
    TP_SESSION_JOB_PACK,
    TP_SESSION_JOB_EXPORT
} tp_session_job_kind;

typedef enum tp_session_job_state {
    TP_SESSION_JOB_RUNNING = 1,
    TP_SESSION_JOB_SUCCEEDED,
    TP_SESSION_JOB_FAILED,
    TP_SESSION_JOB_CANCELLED
} tp_session_job_state;

typedef enum tp_session_job_rejection {
    TP_SESSION_JOB_REJECTION_NONE = 0,
    TP_SESSION_JOB_REJECTION_SUPERSEDED,
    TP_SESSION_JOB_REJECTION_CANCELLED,
    TP_SESSION_JOB_REJECTION_TARGET_DELETED,
    TP_SESSION_JOB_REJECTION_OLD_INSTANCE,
    TP_SESSION_JOB_REJECTION_DUPLICATE,
    TP_SESSION_JOB_REJECTION_SESSION_CLOSED
} tp_session_job_rejection;

typedef enum tp_session_job_target_kind {
    TP_SESSION_JOB_TARGET_ATLAS = 1,
    TP_SESSION_JOB_TARGET_EXPORT_TARGET
} tp_session_job_target_kind;

typedef struct tp_session_job_target {
    tp_session_job_target_kind kind;
    tp_id128 atlas_id;
    tp_id128 id;
} tp_session_job_target;

/* Immutable latest-value projection. `target_ids` is borrowed from the owning
 * observation and remains valid for that observation lifetime. Running elapsed
 * time is presentation-derived; it does not create a per-frame generation. */
typedef struct tp_session_job_observed_state {
    bool present;
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
    tp_session_job_state state;
    int current;
    int total;
    bool cancellation_requested;
    bool terminal;
    bool result_accepted;
    tp_session_job_rejection rejection;
    tp_session_input_token base_input_token;
    const tp_session_job_target *targets;
    size_t target_count;
    tp_status terminal_status;
    tp_error terminal_error;
} tp_session_job_observed_state;

/* Immutable accepted-result slot. Its envelope identity is independent from
 * the currently running job, so an observer can always match a retained
 * payload to the request that produced it. `targets` and `result` are borrowed
 * from the owning observation and remain valid for that observation lifetime. */
typedef struct tp_session_job_observed_result {
    bool present;
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
    tp_session_input_token base_input_token;
    const tp_session_job_target *targets;
    size_t target_count;
    const tp_session_job_result *result;
} tp_session_job_observed_result;

/* One linearizable session cut across event/model/source/recovery and the
 * coalesced job/result owners. A NULL `after` requests an initial complete
 * resync. */
typedef struct tp_session_observation_token {
    uint64_t event_sequence;
    uint64_t source_runtime_generation;
    uint64_t recovery_health_generation;
    uint64_t recovery_owner_generation;
    uint64_t job_state_generation;
    uint64_t result_generation;
} tp_session_observation_token;

#define TP_SESSION_OBSERVATION_EVENT_CAPACITY 64U

bool tp_session_observation_token_equal(
    tp_session_observation_token left,
    tp_session_observation_token right);
tp_status tp_session_observe(
    const tp_session *session,
    const tp_session_observation_token *after,
    tp_session_observation **out,
    tp_error *err);
void tp_session_observation_destroy(tp_session_observation *observation);
tp_session_observation_token tp_session_observation_token_query(
    const tp_session_observation *observation);
bool tp_session_observation_resync_required(
    const tp_session_observation *observation);
size_t tp_session_observation_event_count(
    const tp_session_observation *observation);
const tp_session_event *tp_session_observation_event_at(
    const tp_session_observation *observation, size_t index);
tp_session_recovery_health tp_session_observation_recovery_health(
    const tp_session_observation *observation);
tp_session_job_observed_state tp_session_observation_job_state(
    const tp_session_observation *observation);
/* Borrowed accepted-result envelope; valid only while `observation` is alive. */
const tp_session_job_observed_result *tp_session_observation_job_result(
    const tp_session_observation *observation);
/* Borrowed and possibly NULL for a source/runtime/recovery-only delta. */
const tp_session_snapshot *tp_session_observation_snapshot(
    const tp_session_observation *observation);

typedef struct tp_session_save_result {
    bool saved;
    /* The file was atomically published and session identity/fingerprint were
     * advanced, but the containing-directory durability barrier failed. This
     * is a successful Save with a structured warning, not a retryable failure. */
    bool file_durability_degraded;
    tp_status file_durability_status;
    /* Save attempts a fresh recovery checkpoint after project publication.
     * Success clears prior model degradation; failure leaves Save successful,
     * preserves the older evidence, and reports the sticky first cause here. */
    bool recovery_degraded;
    tp_status recovery_status;
    /* A cross-identity Save As retired the prior live recovery slot. The
     * frontend should attach a fresh slot for target_path; Save itself remains
     * successful and recovery stays outside the publication commit point. */
    bool recovery_rebind_required;
    char target_path[TP_IDENTITY_PATH_MAX];
    tp_id128 file_fingerprint;
    tp_id128 recovery_token;
    bool has_recovery_token;
} tp_session_save_result;

/* Creates a synchronous single-writer session over a new default project. The
 * session is the sole owner of its model. `rng` is used for session/structural IDs. */
tp_status tp_session_create(const tp_rng *rng, tp_session **out, tp_error *err);
tp_status tp_session_create_default_project(const tp_rng *rng,
                                            tp_session **out,
                                            tp_error *err);
tp_status tp_session_open(const char *path, const tp_rng *rng,
                          tp_session **out, tp_error *err);

void tp_session_destroy(tp_session *session);

tp_status tp_session_apply(tp_session *session, const tp_txn_request *request,
                           tp_txn_result *result, tp_error *err);
tp_status tp_session_undo(tp_session *session, tp_error *err);
tp_status tp_session_redo(tp_session *session, tp_error *err);
tp_status tp_session_save(tp_session *session, tp_session_save_result *result,
                          tp_error *err);
tp_status tp_session_save_as(tp_session *session, const char *path,
                             tp_session_save_result *result, tp_error *err);
/* Save As with create-only publication for commands such as `ntpacker new`. */
tp_status tp_session_save_new(tp_session *session, const char *path,
                              tp_session_save_result *result, tp_error *err);
tp_status tp_session_discard(tp_session *session, tp_error *err);
tp_status tp_session_invalidate_sources(tp_session *session, tp_error *err);
/* Declares recovery expected for this live host so availability/reporting can
 * expose missing or degraded recovery. Model commands remain available when
 * recovery is unavailable. */
tp_status tp_session_require_recovery(tp_session *session, tp_error *err);
int64_t tp_session_revision(const tp_session *session);
bool tp_session_recovery_available(const tp_session *session);
tp_session_recovery_health tp_session_recovery_health_query(
    const tp_session *session);
bool tp_session_can_undo(const tp_session *session);
bool tp_session_can_redo(const tp_session *session);
int tp_session_undo_depth(const tp_session *session);
int tp_session_redo_depth(const tp_session *session);

/* Shared visible-History enumeration (see the DTO block above). `count` is the
 * total rows (edit records + interleaved markers); `at` copies the index-th row
 * (0-based, oldest first) into *out. Out-of-range index -> TP_STATUS_OUT_OF_BOUNDS.
 * The row set is a snapshot of the moment of the call; the next mutation may change
 * it, so re-count before re-enumerating. */
int tp_session_history_count(const tp_session *session);
tp_status tp_session_history_at(const tp_session *session, int index,
                                tp_session_history_entry *out, tp_error *err);

uint64_t tp_session_event_sequence(const tp_session *session);

/* Returns committed events after `after_sequence`. If the bounded event window no
 * longer contains the requested sequence, returns zero events and asks the caller
 * to resync from an owned snapshot.
 *
 * PULL MODEL (F3-01 W4): events are pulled here (events_after) with snapshot resync
 * on window overflow -- there is no client push callback to register. So that
 * packet's "event subscriber disconnect" and "callback reentrancy rejection" faults
 * are N/A by design: nothing to disconnect, and no re-entrant callback can occur. */
tp_status tp_session_events_after(const tp_session *session, uint64_t after_sequence,
                                  tp_session_event *out, size_t capacity,
                                  size_t *out_count, bool *out_resync_required,
                                  tp_error *err);

tp_status tp_session_snapshot_create(const tp_session *session,
                                     tp_session_snapshot **out, tp_error *err);
/* Loads one immutable file-oriented snapshot without acquiring the exclusive
 * writer lease. Exact schema-v5 bytes are parsed read-only; the snapshot never
 * rewrites or repairs the source file. This is the boundary for one-shot
 * inspect/validate/pack clients. Relative project paths use the same canonical
 * identity owner as tp_session_open(). */
tp_status tp_session_snapshot_load(const char *path,
                                   tp_session_snapshot **out, tp_error *err);
/* Applies one transaction to an isolated clone of `snapshot` and returns the
 * same structured result as the live session path. The snapshot and any live
 * session remain unchanged; no writer lease, journal, event, Save, or other
 * external side effect is created. This is the mutation-preview boundary for
 * one-shot clients. */
tp_status tp_session_snapshot_apply_preview(
    const tp_session_snapshot *snapshot, const tp_txn_request *request,
    tp_txn_result *result, tp_error *err);
void tp_session_snapshot_destroy(tp_session_snapshot *snapshot);
#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SESSION_H */
