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
 * Atlas scalars use one view-local draft reducer and build a narrow typed operation only at
 * submit. Older edit families still use gui_project's pending transaction until their R3 cutover.
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

typedef struct gui_project_job_completion {
    bool publish_result;
    uint64_t session_instance_generation;
    uint64_t request_id;
    tp_session_job_kind kind;
    tp_session_job_state state;
    tp_session_job_rejection rejection;
    tp_status status;
    tp_error error;
    tp_session_job_result result;
} gui_project_job_completion;

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
gui_project_lifecycle_state
gui_project_lifecycle_state_query(void);
bool gui_project_host_take_completion(
    gui_project_job_completion *out);
void gui_project_job_completion_destroy(
    gui_project_job_completion *completion);
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
/* Advances the coalescing clock (seconds) each frame -- feeds the gated fallback flush only. */
void gui_project_tick(double now_seconds);

/* Commit the pending gesture families still awaiting their R3 cutover. Atlas scalar drafts are
 * owned and submitted by gui_actions instead. Returns false when a buffered operation is rejected
 * so persistence/history callers cannot act on older committed state. */
bool gui_project_flush_pending(void);
/* FALLBACK ONLY: commit a buffered gesture that never got a release/blur/discrete boundary, once the
 * 0.30 s window has elapsed. The caller MUST gate this on no active gesture so it can never split a
 * live drag or a mid-typing field. */
void gui_project_flush_elapsed(void);

/* EFFECTIVE slice9 peek for the on-canvas guides (#5): true + fills out_lrtb[4] with the buffered
 * slice9 when a slice9 gesture is buffered for this atlas+sprite (else false -> read the committed
 * record). Lets the guides track typing this frame instead of freezing at the committed value while
 * the gesture buffers. Read-only (no commit). */
bool gui_project_peek_pending_slice9(const gui_sprite_ref *sprite, int out_lrtb[4]);

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
gui_add_status gui_project_add_source(tp_id128 atlas_id, int64_t expected_revision,
                                      const char *path); /* kind=folder */
/* Kind-aware variant (schema v3): the "Add Files" dialog records TP_SOURCE_KIND_FILE. */
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

/* Atlas-family intents carry structural identity + the revision captured with the
 * immutable read view. The session is the sole admission/validation owner. */
bool gui_project_set_atlas_name(tp_id128 atlas_id, int64_t expected_revision, const char *name);
tp_status gui_project_copy_atlas_name(tp_id128 atlas_id, char *out, size_t capacity,
                                      tp_error *err);
/* Sets/clears a sprite's rename export-name override (empty/NULL clears it). */
bool gui_project_set_sprite_rename(const gui_sprite_ref *sprite, const char *rename);

/* Sets ONE atlas knob via an atlas.settings.set transaction. The int/bool knobs read
 * `ivalue` (bool as 0/1); pixels_per_unit reads `fvalue`. Value RANGES are core's now
 * (the op validates); the widget still parse-clamps. Returns true on commit. */
tp_status gui_project_submit_atlas_setting(
    tp_id128 atlas_id, int64_t expected_revision,
    gui_atlas_field field, int ivalue, float fvalue,
    gui_session_submit_identity identity,
    const char transaction_id[33],
    gui_session_submit_terminal *out_terminal,
    tp_error *err);

/* --- region-panel per-sprite overrides (sparse: a clear that leaves only defaults
 * drops the override entry, keeping byte-identical saves) --- */
/* Sets ONE origin/pivot component (axis 0 = Pivot X, 1 = Pivot Y) via a coalescable
 * sprite.override.set. Component-keyed + read-modify-write INSIDE the setter (mirrors slice9): the
 * non-edited component is seeded from the current record AFTER the other axis's buffered edit flushes,
 * so editing X then Y never merges against a stale model (no lost edit). */
bool gui_project_set_sprite_origin(const gui_sprite_ref *sprite, int axis, float value);
bool gui_project_set_sprite_slice9(const gui_sprite_ref *sprite, int lrtb_index, int value);
/* Per-sprite packing override; `value` == TP_PROJECT_OV_INHERIT clears it. */
bool gui_project_set_sprite_override(const gui_sprite_ref *sprite, gui_sprite_ov which, int value);

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
/* Renames animation `anim_index`; fails on empty or a name already used by another animation. */
bool gui_project_set_anim_id(const gui_animation_ref *animation, const char *new_id);
bool gui_project_set_anim_fps(const gui_animation_ref *animation, float fps);
bool gui_project_set_anim_playback(const gui_animation_ref *animation, int playback);
bool gui_project_set_anim_flip(const gui_animation_ref *animation, bool flip_h, bool flip_v);
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
bool gui_project_set_target(const gui_target_ref *target, const char *exporter_id,
                            const char *out_path, bool enabled);
/* H/G3: COALESCABLE out-path-only setter (the path text field). Buffers under a per-target key so the
 * field's Enter/blur gesture-commit flushes the whole edit as ONE undo step; RMW-seeds exporter_id +
 * enabled from the committed record. Discrete target edits keep using gui_project_set_target (immediate). */
bool gui_project_set_target_out_path(const gui_target_ref *target,
                                     const char *out_path);
/* H/G3: discrete target-field setters (IMMEDIATE, one undo step each). They flush any buffered out-path
 * gesture FIRST, then RMW-seed the un-edited fields from the NOW-committed record -- so a discrete
 * enabled/exporter edit made mid-typing never reverts the just-typed out_path (the hazard of re-sending a
 * stale committed out_path). Use these from the checkbox / exporter dropdown instead of gui_project_set_target. */
bool gui_project_set_target_enabled(const gui_target_ref *target, bool enabled);
bool gui_project_set_target_exporter(const gui_target_ref *target,
                                     const char *exporter_id);

/* --- undo / redo (diff history) --- */
bool gui_project_can_undo(void); /* true if a committed step OR a buffered gesture can be reverted */
bool gui_project_can_redo(void);
int gui_project_undo_depth(void); /* committed undoable steps from the session snapshot */
int gui_project_redo_depth(void);
/* Reverse/replay the most recent committed transaction through tp_session. A buffered gesture is
 * flushed to its own step first, so
 * Ctrl+Z reverts an in-flight edit. Sets stale, drops the display caches; selection re-clamp is the
 * caller's job. Returns false when there is nothing to undo/redo (or on a structured restore error). */
bool gui_project_undo(void);
bool gui_project_redo(void);

/* --- file operations (paths explicit; dialogs live in the UI layer) --- */
/* Saves to the current path (must exist). Clears project_dirty. */
tp_status gui_project_save(char *err_out, size_t err_cap);
/* Saves to `path`, remembers it, clears project_dirty. Promotes structural ids FIRST
 * and, on RNG failure, returns the error WITHOUT writing (never persists a nil-id file). */
tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap);

void gui_project_set_controller_status_port(
    gui_project_controller_status_port port);

/* Drains a pending transaction REJECT recorded by a mutator whose op(s) core rejected
 * (out-of-range value / bad reference / OOM). The model is left byte-unchanged on a
 * reject; this surfaces the structured status to the status-bar soft-error channel.
 * Returns true once and copies the message into `out` (then clears it). */
bool gui_project_take_op_error(char *out, size_t cap);

/* Fills `out` with the reason the last flush's commit failed (fix3 [2]): the drained op-error, else a
 * NEUTRAL fallback that fits save + pack + the dirty gate. Consumes the op-error. NULL-safe. Shared by
 * every flush-failure abort path so they use one wording. */
void gui_project_flush_error(char *out, size_t cap);

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
