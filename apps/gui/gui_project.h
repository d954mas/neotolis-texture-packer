#ifndef NTPACKER_GUI_PROJECT_H
#define NTPACKER_GUI_PROJECT_H

/* GUI projection over one core-owned tp_session. The session is the sole mutable
 * project/history/idempotency/recovery owner; GUI code keeps only immutable snapshots,
 * the display path/name, feature-local draft/gesture state, and derived presentation state.
 * The two independently observed state axes are:
 *   - dirty        : session-owned semantic identity vs the last saved baseline, so
 *                    undoing back to the saved state clears it. Save/Open/New rebaseline it.
 *                    Menu-bar dot. (An active draft is NOT yet in the identity; destructive
 *                    gates first ask the feature owner to submit or reject it.)
 *   - preview_stale : model changed since the last successful pack. Since in-process packing is
 *                    blocked (engine #282), nothing clears it this round.
 *
 * Every mutation becomes typed operation intent and commits atomically through tp_session;
 * one accepted transaction captures one semantic diff and one undo step. Undo/Redo also
 * run through tp_session. One gui_session_client atomically observes and frame-pins the
 * immutable state consumed by presentation.
 * Value edits use one view-local draft reducer and build a narrow typed
 * operation only at submit.
 *
 * Refresh (F4) is deliberately NOT a model mutation: rescanning disk sources changes what is
 * DISPLAYED/packed, not the PROJECT MODEL (sources are paths). So Refresh calls
 * gui_project_mark_stale (preview_stale only) and never dirties the project.
 *
 * File operations take explicit paths (the OS dialogs live in the UI layer) so they can be
 * driven headless by the startup self-test. */

#include <stddef.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_recovery.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_session_snapshot_query.h"
#include "gui_project_view.h"
#include "gui_session_client.h"
#include "gui_host_queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an add-source attempt (the GUI surfaces "already added" distinctly). */
typedef enum { GUI_ADD_FAILED = 0, GUI_ADD_ADDED, GUI_ADD_DUPLICATE } gui_add_status;

typedef enum gui_project_lifecycle_kind {
    GUI_PROJECT_LIFECYCLE_NONE = 0,
    GUI_PROJECT_LIFECYCLE_NEW,
    GUI_PROJECT_LIFECYCLE_OPEN,
    GUI_PROJECT_LIFECYCLE_SHUTDOWN
} gui_project_lifecycle_kind;

typedef enum gui_project_lifecycle_state {
    GUI_PROJECT_LIFECYCLE_CLOSED = 0,
    GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
    GUI_PROJECT_LIFECYCLE_DRAINING
} gui_project_lifecycle_state;

typedef bool (*gui_project_controller_attached_fn)(
    void *context);

typedef struct gui_project_controller_status_port {
    gui_project_controller_attached_fn attached;
    void *context;
} gui_project_controller_status_port;

/* Creates the initial fresh in-memory project (one default atlas, no path, clean). Crash recovery is
 * collected and resolved separately through the R6 APIs below; startup never adopts an orphan live. */
void gui_project_init(void);
/* Tears the model down and, when recovery is enabled, deletes the recovery slot (clean-exit reset:
 * a cleanly-exited session leaves NO journal to recover). */
void gui_project_shutdown(void);

/* Require recovery admission for every subsequently created GUI session. The
 * interactive host calls this once before gui_project_init; tests may leave
 * recovery optional. */
void gui_project_require_recovery(void);
/* Configure the app-data recovery directory. Core owns the random live-slot
 * name, lock, and scan exclusion. NULL/"" clears the configured directory but
 * does not relax a recovery requirement. */
void gui_project_enable_recovery(const char *root);
/* Record a non-fatal startup/setup failure. Editing, history, and saving remain
 * available while crash recovery is unavailable. The UI drains the resulting
 * one-shot warning through gui_project_take_recovery_setup_notice(). */
void gui_project_note_recovery_setup_failure(const char *reason);

/* Drains the one-shot "crash recovery is unavailable" notice. The text distinguishes another live
 * owner from path/directory/lock setup failures. */
bool gui_project_take_recovery_setup_notice(char *out, size_t cap);

/* Drains the one-shot warning raised when Save published the file but could
 * not confirm the containing-directory durability barrier. */
bool gui_project_take_save_notice(char *out, size_t cap);

/* Returns true while crash recovery is degraded. A successful healing/rebind
 * publishes the cleared state by returning false on subsequent queries. */
bool gui_project_recovery_notice_query(gui_recovery_notice *out);

/* Thin adapter over tp_recovery_scan_root; returns the bounded result count. */
int gui_recovery_collect(gui_recovery_list *out);

/* Thin adapter over tp_recovery_resolve_journal. */
/* Production modal entry point bound to the selected typed row. */
tp_status gui_recovery_resolve_entry(const gui_recovery_entry *entry, gui_recovery_action action,
                                     const char *target_path, char *err_out, size_t err_cap);

/* --- accessors --- */
/* Immutable read view owned and frame-pinned by gui_session_client. */
const tp_session_snapshot *gui_project_snapshot(void);
/* Changes whenever the client publishes or releases a model snapshot.
 * Borrowing GUI caches include this token in their lifetime key. */
uint64_t gui_project_snapshot_lifetime_generation(void);
/* Coalesced source-runtime observation component. Runtime-only changes do not
 * replace or synthesize a model snapshot. */
uint64_t gui_project_source_runtime_generation(void);
/* Current composite input identity from the same client observation cut:
 * model state is read from the immutable snapshot and runtime state from the
 * typed source-runtime token. */
bool gui_project_observed_input_token(
    tp_session_input_token *out);
/* Host-frame observation seam. begin atomically observes/reduces and pins the
 * immutable snapshot; end releases the pin after render/present. */
tp_status gui_project_frame_begin(tp_error *err);
void gui_project_frame_end(void);
bool gui_project_frame_is_pinned(void);
tp_status gui_project_register_observation_reducer(
    gui_session_client_reducer_fn reduce, void *context, tp_error *err);
bool gui_project_submit_receipt_query(
    const char transaction_id[33],
    gui_session_submit_identity identity,
    gui_session_submit_terminal *out);
/* Host-thread admission facade. Request payloads are copied on enqueue; the
 * queue never exposes or retains the mutable session. */
tp_status gui_project_job_enqueue_pack(
    tp_id128 atlas_id, const char *work_dir,
    const char *preview_exporter_id, tp_error *err);
tp_status gui_project_job_enqueue_export(
    tp_id128 atlas_id, const char *work_dir, tp_error *err);
tp_status gui_project_job_enqueue_cancel(tp_error *err);
tp_status gui_project_lifecycle_begin_new(tp_error *err);
tp_status gui_project_lifecycle_begin_open(
    const char *path, tp_error *err);
tp_status gui_project_lifecycle_begin_shutdown(
    bool discard_recovery, tp_error *err);
tp_status gui_project_lifecycle_pump(
    gui_project_lifecycle_kind *completed,
    tp_error *err);
/* Non-fallible forced teardown for the host's exhausted shutdown budget: the
 * negotiated shutdown is bounded, so the exit path needs a terminal answer that
 * cannot itself fail. It discards the host owner's leases and staged work, kills
 * the worker through the ordinary session teardown, and ESTABLISHES the CLOSED
 * lifecycle that gui_project_shutdown asserts. Nothing else may call it: an
 * interactive New/Open/Close always negotiates. */
void gui_project_lifecycle_force_close(void);
gui_project_lifecycle_state
gui_project_lifecycle_state_query(void);
/* Host-owner ingress for the one staged job completion. The queue's typed
 * completion IS the contract; the caller destroys it with
 * gui_host_completion_destroy. */
bool gui_project_host_take_completion(
    gui_host_completion *out);
bool gui_project_job_busy(void);
tp_session_job_kind gui_project_job_active_kind(void);
tp_session_job_observed_state gui_project_job_observed_state(void);
uint64_t gui_project_session_instance_generation(void);
uint64_t gui_project_snapshot_model_generation(void);
tp_status gui_project_snapshot_serialize(char **out, size_t *out_len,
                                         tp_error *err);
const char *gui_project_path(void);         /* absolute file path, or "" while unsaved */
const char *gui_project_display_name(void); /* file basename, or "untitled" */
bool gui_project_has_path(void);
bool gui_project_is_dirty(void);
bool gui_project_is_stale(void);
/* Single owner for external source-runtime invalidation: drops the scan cache,
 * advances the session source generation/event, and invalidates the GUI view. */
void gui_project_invalidate_sources(void);

/* --- dirty/stale projection --- */
/* Clears preview_stale after a successful pack (unused this round; #282). */
void gui_project_mark_packed(void);
/* Marks the preview stale WITHOUT dirtying the project (Refresh: disk changed, model
 * did not). */
void gui_project_mark_stale(void);
/* Monotonic model-edit counter: bumped once per REAL model mutation (the touch choke point, after the
 * memcmp dedup). Lets a view cheaply detect "the project changed since I snapshotted it" without
 * re-serializing every frame -- the export-target preview uses it to drop a stale preview on an edit. */

/* --- mutation wrappers (all admit typed operations through tp_session) --- */
typedef struct gui_project_create_result {
    bool committed;
    bool observation_pending;
    tp_id128 created_id;
    /* Resolved only from the common observed snapshot; -1 while its exact
     * committed echo is pending. */
    int visible_index;
} gui_project_create_result;
/* Create wrappers return a stable created identity plus an observed index when
 * the exact common echo is already available. The remove wrappers return TRUE
 * iff the removal actually committed (fix3 [0]): false on a
 * failed pending flush, an invalid index, or a commit reject -- so a deferred handler shows
 * "Removed X (Ctrl+Z)" + resets selection ONLY on a real removal, never a false success. */
gui_project_create_result gui_project_add_atlas(void);
bool gui_project_remove_atlas(tp_id128 atlas_id, int64_t expected_revision); /* true iff removed */
/* Adds ONE source with an explicit kind (schema v3): the "Add Files" dialog
 * records TP_SOURCE_KIND_FILE, the folder picker TP_SOURCE_KIND_FOLDER. */
gui_add_status gui_project_add_source_kind(tp_id128 atlas_id, int64_t expected_revision,
                                           const char *path, tp_source_kind kind);
/* Batch-add a multi-select as ONE atomic transaction (one undo step). Skips empties + duplicates (in the
 * atlas or within the batch) into *out_dup; true iff committed or a clean no-op. PRECONDITION: `paths` are
 * '/'-normalized -- the in-batch dedup is a raw strcmp, while core dedups on the normalized form, so two
 * paths equal only after normalization would slip the batch dedup and self-reject the whole txn. See
 * gui_project.c. */
bool gui_project_add_sources(tp_id128 atlas_id, int64_t expected_revision,
                             const char *const *paths, int n_paths, tp_source_kind kind,
                             int *out_added, int *out_dup);
bool gui_project_remove_source(tp_id128 atlas_id, tp_id128 source_id,
                               int64_t expected_revision); /* true iff removed */

/* Every intent carries structural identity + the revision captured with the
 * immutable read view. The session is the sole admission/validation owner. */

/* Address of an identified TEXT submit. `atlas_id` is always the owning atlas;
 * `entity_id` names the animation or export target; `source_id`/`source_key`
 * name the sprite. Unused members are nil/NULL. */
typedef struct gui_text_ref {
    tp_id128 atlas_id;
    tp_id128 entity_id;
    tp_id128 source_id;
    const char *source_key;
    int64_t expected_revision;
} gui_text_ref;

/* The one identified text submit: atlas.rename, animation.rename,
 * sprite.name.set (an EMPTY value clears the rename), or target.set out-path.
 * `value` is required: a NULL value is a structured INVALID_ARGUMENT, never a
 * clear -- callers that mean "clear" pass "". */
tp_status gui_project_submit_text(
    tp_op_kind kind, const gui_text_ref *ref,
    const char *value, gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err);

/* Sets atlas knobs via an atlas.settings.set transaction. The caller supplies the
 * built payload + presence mask; value RANGES are core's (the op validates) and the
 * widget still parse-clamps. */
tp_status gui_project_submit_atlas_settings(
    tp_id128 atlas_id, int64_t expected_revision,
    const tp_op_atlas_settings *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);

/* --- region-panel per-sprite overrides (sparse: a clear that leaves only defaults
 * drops the override entry, keeping byte-identical saves) --- */
tp_status gui_project_submit_sprite_origin(
    const gui_sprite_ref *sprite, int axis, float value,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err);
tp_status gui_project_submit_sprite_slice9(
    const gui_sprite_ref *sprite, int component, int value,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err);
/* Sprite payload submit: the caller supplies the built sprite.settings.set
 * payload + presence mask. Origin/slice9 keep their own entry points because the
 * untouched sibling components are read from the LIVE snapshot here. */
tp_status gui_project_submit_sprite_settings(
    const gui_sprite_ref *sprite,
    const tp_op_sprite_set *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err);

/* --- animations (ux.md §3.7b: explicit manual assembly only) --- */
/* Appends an animation and fills it with `frames` (in the given order) as ONE undo entry. The id is
 * the first free of {base, base"2", base"3", ...}; a NULL/empty base auto-names "anim1"/"anim2"/...
 * `frames` may be NULL/0 for an empty animation. */
gui_project_create_result gui_project_create_animation(
    tp_id128 atlas_id, int64_t expected_revision,
    const char *base, const tp_op_sprite_ref *frames,
    int frame_count);
/* Removes the animation with `id`. Returns true iff removed (false on flush-abort/not-found). */
bool gui_project_remove_animation(const gui_animation_ref *animation);
tp_status gui_project_submit_animation_settings(
    const gui_animation_ref *animation,
    const tp_op_anim_settings *settings,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *terminal, tp_error *err);
/* Appends `frames` (in order) to animation `anim_index` as ONE undo entry. */
bool gui_project_anim_add_frames(const gui_animation_ref *animation,
                                 const tp_op_sprite_ref *frames, int count);
bool gui_project_anim_remove_frame(const gui_animation_ref *animation, int frame_index);
bool gui_project_anim_move_frame(const gui_animation_ref *animation, int frame_index,
                                 int delta);

/* --- export targets (region G, audit I1) --- */
/* Appends a default json-neotolis target "out/<atlas>.<ext>". */
gui_project_create_result gui_project_add_target(
    tp_id128 atlas_id, int64_t expected_revision);
bool gui_project_remove_target(const gui_target_ref *target);
bool gui_project_set_target_enabled(
    const gui_target_ref *target, bool enabled);
bool gui_project_set_target_exporter(
    const gui_target_ref *target, const char *exporter_id);

/* --- undo / redo (diff history) --- */
bool gui_project_can_undo(void); /* true when the session has a committed step */
bool gui_project_can_redo(void);
int gui_project_undo_depth(void); /* committed undoable steps from the session snapshot */
int gui_project_redo_depth(void);
/* Reverse/replay the most recent committed transaction through tp_session.
 * Sets stale, drops display caches; selection re-clamp is the caller's job.
 * Returns false when there is nothing to undo/redo or on structured error. */
bool gui_project_undo(void);
bool gui_project_redo(void);

/* --- file operations (paths explicit; dialogs live in the UI layer) --- */
/* Saves to the current path (must exist). Clears project_dirty. */
tp_status gui_project_save(char *err_out, size_t err_cap);
/* Read-only Save As feasibility check. Canonicalizes the destination, refreshes
 * the observed session identity, and rejects an identity change while the
 * host-owned controller status port reports an attached controller. It never
 * submits or flushes a draft and performs no file write. */
tp_status gui_project_save_as_preflight(
    const char *path, char *err_out, size_t err_cap);
/* Saves to `path`, remembers it, clears project_dirty. Promotes structural ids FIRST
 * and, on RNG failure, returns the error WITHOUT writing (never persists a nil-id file). */
tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap);

void gui_project_set_controller_status_port(
    gui_project_controller_status_port port);

/* Drains a typed-submit REJECT recorded by a mutator whose op(s) core rejected
 * (out-of-range value / bad reference / OOM). The model is left byte-unchanged on a
 * reject; this surfaces the structured status to the status-bar soft-error channel.
 * Returns true once and copies the message into `out` (then clears it). */
bool gui_project_take_op_error(char *out, size_t cap);

#if defined(NTPACKER_GUI_SELFTEST) || defined(TP_ENABLE_TEST_SEAMS)
/* Borrowed test-only access for recovery and external-observer proofs. */
tp_session *gui_project__test_session(void);
#endif
#ifdef TP_ENABLE_TEST_SEAMS
void gui_project__test_fail_next_observe(void);
void gui_project__test_fail_observes(unsigned int count);
bool gui_project__test_host_has_staged_completion(void);
uint64_t gui_project__test_open_call_count(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PROJECT_H */
