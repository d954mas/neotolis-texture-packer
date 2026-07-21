#ifndef TP_CORE_TP_SESSION_H
#define TP_CORE_TP_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_identity.h"
#include "tp_core/tp_selector.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_session tp_session;
typedef struct tp_session_snapshot tp_session_snapshot;
typedef struct tp_txn_request tp_txn_request;
typedef struct tp_txn_result tp_txn_result;
typedef struct tp_project tp_project;
typedef struct tp_journal tp_journal;

/* Immutable identity of the inputs consumed by a derived job. Both zeroes are
 * valid generations; freshness is exact value equality, never a sentinel test. */
typedef struct tp_session_input_token {
    uint64_t model_generation;
    uint64_t source_generation;
} tp_session_input_token;
bool tp_session_input_token_equal(tp_session_input_token left,
                                  tp_session_input_token right);

/* Stable state-notice vocabulary for recovery durability. `notice_id` in the
 * DTO below always points at this program-lifetime string; consumers surface it
 * only while `degraded` is true. */
#define TP_SESSION_NOTICE_RECOVERY_DEGRADED "recovery_degraded"

typedef struct tp_session_recovery_health {
    const char *notice_id;
    bool available;
    bool degraded;
    tp_status first_cause;
    bool has_last_durable_revision;
    int64_t last_durable_revision;
    /* Unix seconds when known. RESERVED: no producer sets this true today --
     * the synchronous recovery writer has no trustworthy injected clock, so
     * tp_session.c hard-codes has_last_durable_time to false. It stays reserved
     * until a clock is injected; readers need not hunt for a producer. */
    bool has_last_durable_time;
    int64_t last_durable_time;
    /* Monotonic for this session lifetime. A changed value tells a polling
     * client to refresh the state notice; wrap is saturated at UINT64_MAX. */
    uint64_t generation;
} tp_session_recovery_health;

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

/* Immutable value DTOs owned by a tp_session_snapshot. Strings remain valid until
 * the snapshot is destroyed. No project/model pointer crosses this boundary. */
typedef struct tp_snapshot_atlas {
    tp_id128 id;
    const char *name;
    int max_size;
    int padding;
    int margin;
    int extrude;
    int alpha_threshold;
    int max_vertices;
    int shape;
    bool allow_transform;
    bool power_of_two;
    float pixels_per_unit;
    int source_count;
    int sprite_count;
    int animation_count;
    int target_count;
} tp_snapshot_atlas;

typedef enum tp_snapshot_source_kind {
    TP_SNAPSHOT_SOURCE_FOLDER = 0,
    TP_SNAPSHOT_SOURCE_FILE = 1
} tp_snapshot_source_kind;

typedef struct tp_snapshot_source {
    tp_id128 id;
    tp_snapshot_source_kind kind;
    const char *path;
} tp_snapshot_source;

typedef struct tp_snapshot_sprite {
    tp_id128 id;
    tp_id128 source_id;
    const char *source_key;
    const char *name;
    float origin_x;
    float origin_y;
    uint16_t slice9_lrtb[4];
    const char *rename;
    int16_t override_shape;
    int16_t override_allow_rotate;
    int16_t override_max_vertices;
    int16_t override_margin;
    int16_t override_extrude;
} tp_snapshot_sprite;

typedef struct tp_snapshot_frame {
    tp_id128 sprite_id;
    tp_id128 source_id;
    const char *source_key;
    const char *name;
} tp_snapshot_frame;

typedef struct tp_snapshot_animation {
    tp_id128 id;
    const char *name;
    float fps;
    int playback;
    bool flip_h;
    bool flip_v;
    int frame_count;
} tp_snapshot_animation;

typedef struct tp_snapshot_target {
    tp_id128 id;
    const char *exporter_id;
    const char *out_path;
    bool enabled;
} tp_snapshot_target;

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
} tp_session_event;

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
int64_t tp_session_snapshot_revision(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_model_generation(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_admission_sequence(const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_source_generation(const tp_session_snapshot *snapshot);
tp_session_input_token tp_session_snapshot_input_token(
    const tp_session_snapshot *snapshot);
uint64_t tp_session_snapshot_event_sequence(const tp_session_snapshot *snapshot);
bool tp_session_snapshot_dirty(const tp_session_snapshot *snapshot);
bool tp_session_snapshot_recovery_available(const tp_session_snapshot *snapshot);
tp_session_recovery_health tp_session_snapshot_recovery_health_query(
    const tp_session_snapshot *snapshot);
tp_session_identity tp_session_snapshot_identity(const tp_session_snapshot *snapshot);
/* Borrowed immutable path owned by `snapshot`; empty for an unsaved identity. */
const char *tp_session_snapshot_canonical_path(const tp_session_snapshot *snapshot);
int tp_session_snapshot_project_schema_version(const tp_session_snapshot *snapshot);
const char *tp_session_snapshot_project_dir(const tp_session_snapshot *snapshot);
/* Returns the exact bytes fingerprint captured by Open/last successful Save.
 * The value is session-owned authority copied into this immutable snapshot;
 * callers must not re-fingerprint the path to reconstruct the baseline. */
bool tp_session_snapshot_saved_file_fingerprint(
    const tp_session_snapshot *snapshot, tp_id128 *out_fingerprint);
int tp_session_snapshot_atlas_count(const tp_session_snapshot *snapshot);
const tp_snapshot_atlas *tp_session_snapshot_atlas_at(const tp_session_snapshot *snapshot,
                                                      int index);
const tp_snapshot_atlas *tp_session_snapshot_atlas_by_id(const tp_session_snapshot *snapshot,
                                                         tp_id128 id);
const tp_snapshot_source *tp_session_snapshot_source_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
/* Direct atlas/source-index query for linear snapshot consumers. Returns the
 * immutable source DTO and resolves its path without rescanning atlas/source
 * IDs. For valid indices, *out_source is returned even when path resolution
 * fails, allowing callers to report the unresolved source without a second
 * lookup. */
tp_status tp_session_snapshot_source_resolved_at(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    const tp_snapshot_source **out_source, char *out_path, size_t capacity,
    tp_error *err);
const tp_snapshot_source *tp_session_snapshot_source_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 source_id);
const tp_snapshot_sprite *tp_session_snapshot_sprite_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
/* Direct index query for consumers already iterating one atlas DTO. */
const tp_snapshot_sprite *tp_session_snapshot_sprite_at_index(
    const tp_session_snapshot *snapshot, int atlas_index, int sprite_index);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_key(const tp_session_snapshot *snapshot,
                                                            tp_id128 atlas_id, tp_id128 source_id,
                                                            const char *source_key);
const tp_snapshot_sprite *tp_session_snapshot_sprite_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 sprite_id);
const tp_snapshot_animation *tp_session_snapshot_animation_at(const tp_session_snapshot *snapshot,
                                                              tp_id128 atlas_id, int index);
const tp_snapshot_animation *tp_session_snapshot_animation_by_id(const tp_session_snapshot *snapshot,
                                                                 tp_id128 atlas_id, tp_id128 animation_id);
const tp_snapshot_frame *tp_session_snapshot_animation_frame_at(const tp_session_snapshot *snapshot,
                                                                tp_id128 atlas_id, tp_id128 animation_id,
                                                                int index);
/* Returns the snapshot-owned contiguous frame range for one stable animation.
 * `out_count` is set to zero when the animation is absent. */
const tp_snapshot_frame *tp_session_snapshot_animation_frames(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, int *out_count);
const tp_snapshot_target *tp_session_snapshot_target_at(const tp_session_snapshot *snapshot,
                                                        tp_id128 atlas_id, int index);
const tp_snapshot_target *tp_session_snapshot_target_by_id(const tp_session_snapshot *snapshot,
                                                           tp_id128 atlas_id, tp_id128 target_id);
tp_status tp_session_snapshot_resolve_path(const tp_session_snapshot *snapshot,
                                           tp_id128 atlas_id, tp_id128 source_id,
                                           char *out, size_t capacity, tp_error *err);
/* Resolves one structural selector through the canonical selector owner without
 * exposing the snapshot's project. A non-nil atlas_scope limits child-entity
 * matching to that atlas before ambiguity is decided. */
tp_status tp_session_snapshot_resolve_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_scope,
    tp_selector_kind want, const char *selector, tp_selector_result *out,
    tp_selector_candidates *candidates, tp_error *err);
/* Resolve a runtime sprite selector within one atlas to its canonical persistent
 * address. The selector grammar is owned by tp_selector: sprite_<id>, sprite:<key>,
 * or source_<id>:<key>. A bare/export key is accepted as sprite:<key>, but multiple
 * matching sources return AMBIGUOUS_SELECTOR; the first match is never selected.
 * On success `out_source_key` receives the normalized, extension-preserving key. */
tp_status tp_session_snapshot_resolve_sprite_selector(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, tp_selector_result *out, tp_id128 *out_source_id,
    char *out_source_key, size_t source_key_capacity,
    tp_selector_candidates *candidates, tp_error *err);
bool tp_session_snapshot_target_out_path_shared(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 target_id, const char *out_path);
tp_status tp_session_snapshot_next_atlas_defaults(
    const tp_session_snapshot *snapshot, char *name, size_t name_cap,
    char *out_path, size_t out_path_cap, const char **exporter_id,
    bool *target_enabled, tp_error *err);
tp_status tp_session_snapshot_next_animation_name(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id, const char *base,
    char *name, size_t name_cap, tp_error *err);
tp_status tp_session_snapshot_target_defaults(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char **exporter_id, char *out_path, size_t out_path_cap,
    bool *enabled, tp_error *err);
tp_status tp_session_snapshot_resolve_frame(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_id128 animation_id, const char *selector, int *out_index,
    tp_error *err);
tp_status tp_session_snapshot_resolve_target(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *selector, const tp_snapshot_target **out, tp_error *err);
tp_status tp_session_snapshot_serialize(const tp_session_snapshot *snapshot,
                                        char **out, size_t *out_len,
                                        tp_error *err);
tp_id128 tp_session_snapshot_semantic_identity(
    const tp_session_snapshot *snapshot);
#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_SESSION_H */
