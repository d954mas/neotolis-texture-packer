#ifndef NTPACKER_GUI_PROJECT_H
#define NTPACKER_GUI_PROJECT_H

/* GUI projection over one core-owned tp_session. The session is the sole mutable
 * project/history/idempotency/recovery owner; GUI code keeps only immutable snapshots,
 * the display path/name, pending gesture intent, and derived presentation state.
 * The two independently observed state axes are:
 *   - dirty        : session-owned semantic identity vs the last saved baseline, so
 *                    undoing back to the saved state clears it. Save/Open/New rebaseline it.
 *                    Menu-bar dot. (A buffered-but-uncommitted gesture is NOT yet in the identity;
 *                    the destructive gates flush the pending buffer BEFORE checking dirty.)
 *   - preview_stale : model changed since the last successful pack. Since in-process packing is
 *                    blocked (engine #282), nothing clears it this round.
 *
 * Every mutation becomes typed operation intent and commits atomically through tp_session;
 * one accepted transaction captures one semantic diff and one undo step. Undo/Redo also
 * run through tp_session, and cached snapshots are invalidated after each transition.
 * Value edits (slider/field/etc.) coalesce through gui_project's ONE pending transaction and commit
 * per GESTURE (gui_project_flush_pending), so one interaction == one undo step (decision 0015).
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
#include "tp_core/tp_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result of an add-source attempt (the GUI surfaces "already added" distinctly). */
typedef enum { GUI_ADD_FAILED = 0, GUI_ADD_ADDED, GUI_ADD_DUPLICATE } gui_add_status;

/* Per-sprite packing-override field selector (region-panel "Packing overrides"). */
typedef enum {
    GUI_SPRITE_OV_SHAPE = 0,
    GUI_SPRITE_OV_ROTATE,
    GUI_SPRITE_OV_MAXVERT,
    GUI_SPRITE_OV_MARGIN,
    GUI_SPRITE_OV_EXTRUDE
} gui_sprite_ov;

typedef struct gui_sprite_ref {
    tp_id128 atlas_id;
    tp_id128 source_id;
    const char *source_key;
    int64_t expected_revision;
} gui_sprite_ref;

typedef struct gui_animation_ref {
    tp_id128 atlas_id;
    tp_id128 animation_id;
    int64_t expected_revision;
} gui_animation_ref;

typedef struct gui_target_ref {
    tp_id128 atlas_id;
    tp_id128 target_id;
    int64_t expected_revision;
} gui_target_ref;

bool gui_project_animation_ref_at(int atlas_index, int animation_index,
                                  gui_animation_ref *out);
bool gui_project_target_ref_at(int atlas_index, int target_index,
                               gui_target_ref *out);

/* Creates the initial fresh in-memory project (one default atlas, no path, clean). Crash recovery is
 * collected and resolved separately through the R6 APIs below; startup never adopts an orphan live. */
void gui_project_init(void);
/* Tears the model down and, when recovery is enabled, deletes the recovery slot (clean-exit reset:
 * a cleanly-exited session leaves NO journal to recover). */
void gui_project_shutdown(void);
/* Called only after the user explicitly confirms Exit -> Discard. Without it, shutdown preserves a
 * dirty recovery journal so a raw window close remains recoverable. */
void gui_project_discard_recovery_on_shutdown(void);

/* F2-05b-ii-B crash recovery. `root` is the app-data recovery directory;
 * core owns the random live-slot name, lock, and scan exclusion. NULL/""
 * disables recovery. Call once before the first interactive gui_project_init. */
void gui_project_enable_recovery(const char *root);
/* Record a non-fatal startup/setup failure that disables crash recovery. The UI drains the resulting
 * one-shot warning through gui_project_take_recovery_setup_notice(). */
void gui_project_note_recovery_setup_failure(const char *reason);

/* R6 recovery lists are bounded so a pathological folder cannot grow startup memory without limit. */
#define GUI_RECOVERY_MAX_CANDIDATES 16
#define GUI_RECOVERY_PATH_CAP 1200
/* Drains the one-shot "crash recovery is unavailable" notice. The text distinguishes another live
 * owner from path/directory/lock setup failures. */
bool gui_project_take_recovery_setup_notice(char *out, size_t cap);

/* ============================ R6a: recovery-resolution decision/action layer ============================
 * The headless-testable layer the R6b startup modal drives: collect recovered-journal entries WITH metadata
 * + status (incl. an old-format VERSION_MISMATCH journal surfaced for Discard), then resolve ONE per the
 * user's choice (Discard / Save to original / Save As). NON-DESTRUCTIVE ON FAILURE: a recovered journal
 * is deleted ONLY after a SUCCESSFUL save (or an explicit Discard) -- a failed save leaves the journal for a
 * retry and never clobbers the user's original file: a failed save leaves the original untouched (the atomic
 * core save writes a temp then renames). Unlike the retired R5b-2 auto-adopt, this NEVER adopts the
 * recovered work into the live editor model. R6b drives it from the startup modal; the protocol is
 * exercised headlessly by selftests J26-J37. */
typedef enum {
    GUI_RECOVERY_DISCARD = 0,   /* delete the journal; the permanent lock file remains */
    GUI_RECOVERY_SAVE_ORIGINAL, /* atomically save the recovered state over its ORIGINAL file (no backup; atomic replace) */
    GUI_RECOVERY_SAVE_AS        /* save the recovered state to a NEW target file (the original is untouched) */
} gui_recovery_action;

/* One recovered orphan journal the modal lists: its on-disk journal path + the recovered project's original
 * file path / display name + when + how peek classified it. `adoptable` is true for a genuinely recoverable
 * journal (OK/TRUNCATED/CORRUPT + checkpoint + post-checkpoint unsaved work) and false for an old-format
 * (VERSION_MISMATCH) journal surfaced only so the user can Discard it. */
typedef struct {
    char journal_path[GUI_RECOVERY_PATH_CAP]; /* the orphan .ntpjournal on disk */
    char orig_path[GUI_RECOVERY_PATH_CAP];    /* recovered project's ORIGINAL file path; "" = untitled */
    char name[256];                           /* display name; "untitled" when no orig path */
    int64_t timestamp;                        /* unix-seconds (meta.timestamp; 0 if none) */
    int status;                               /* tp_journal_recovery_status */
    bool adoptable;                           /* true = recoverable; false = old-format, Discard-only */
    tp_id128 file_fingerprint;                 /* saved original baseline, when present */
    bool has_file_fingerprint;
} gui_recovery_entry;
typedef struct {
    gui_recovery_entry items[GUI_RECOVERY_MAX_CANDIDATES];
    int count;
    bool has_more; /* actionable entries were omitted by the bounded UI list cap */
} gui_recovery_list;

/* Collect recovered-journal entries from the configured recovery domain. Core
 * excludes its live slot and any live-locked orphan. For each remaining journal, peek + classify:
 *  - adoptable (status OK/TRUNCATED/CORRUPT && has_checkpoint && record_count > 1) -> entry, adoptable=true;
 *  - VERSION_MISMATCH (our file, old format) -> entry, adoptable=false (surfaced for Discard);
 *  - BAD_MAGIC (foreign) / EMPTY / no-work / STALE_KEY -> SKIP (left untouched, not listed).
 * Entries NEWEST-FIRST by timestamp, capped at GUI_RECOVERY_MAX_CANDIDATES. Reads only (no-create io);
 * DELETES NOTHING. `out` is zeroed first; returns out->count. */
int gui_recovery_collect(gui_recovery_list *out);

/* Resolve ONE recovered journal per the user's `action`. NON-DESTRUCTIVE ON FAILURE: the journal
 * is deleted ONLY after a successful save, or on an explicit Discard -- a failed save LEAVES the journal for
 * a retry and never clobbers the user's original: a failed save leaves it untouched (atomic temp+rename in
 * the core save). Recovers the state into a standalone
 * clone (NEVER adopted into the live editor model) and always destroys the clone.
 *  - GUI_RECOVERY_DISCARD       : delete the journal. orig_path / target_path ignored.
 *  - GUI_RECOVERY_SAVE_ORIGINAL : atomically save the recovered state over `orig_path` (no backup);
 *                                 requires orig_path != "" (a saved project), else INVALID_ARGUMENT.
 *  - GUI_RECOVERY_SAVE_AS       : save the recovered state to `target_path`.
 * Returns TP_STATUS_OK on success; on a fault fills err_out (NUL-terminated within err_cap) and the journal
 * is left intact on disk. */
#ifdef NTPACKER_GUI_SELFTEST
/* Raw-path seam for protocol fault tests. Production exposes only the typed entry API below. */
tp_status gui_recovery_resolve(const char *journal_path, const char *orig_path, gui_recovery_action action,
                               const char *target_path, char *err_out, size_t err_cap);
#endif

/* Production/modal entry point: keeps the decision tied to the exact typed row collected from disk.
 * The resolver re-reads metadata under an exclusive claim before mutating anything; the entry is the
 * stable user-visible identity, not an authority that can bypass the persisted fingerprint. */
tp_status gui_recovery_resolve_entry(const gui_recovery_entry *entry, gui_recovery_action action,
                                     const char *target_path, char *err_out, size_t err_cap);

/* --- accessors --- */
/* Cached immutable read view. Invalidated only at model-change chokepoints. */
const tp_session_snapshot *gui_project_snapshot(void);
/* Changes whenever the cached snapshot object is destroyed, including Save
 * paths whose model/source generations remain unchanged. Borrowing GUI caches
 * include this token in their lifetime key. */
uint64_t gui_project_snapshot_lifetime_generation(void);
/* Narrow orchestration seam for the derived Pack / side-effect Export job
 * adapter. The session remains opaque and owns the one active typed handle. */
tp_session *gui_project_session_for_jobs(void);
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

/* Commit the ONE buffered coalescable gesture NOW (no-op when nothing is buffered): the gesture-end
 * flush called at every commit boundary (save/save-as/new/open/exit/undo/redo/pack/export and before
 * each dirty gate) and by the view layer at a widget's gesture boundary. Committing folds the whole
 * gesture into ONE undo step. Returns FALSE iff a buffered gesture existed and its commit FAILED (a
 * journal append failure) -- i.e. an edit could not be made durable; callers that then persist or
 * discard (save/save-as, undo/redo) MUST abort on false so a journal-failed flush is never mistaken
 * for a clean state (no false "saved"). Returns true when nothing was pending / it was a no-op /
 * it committed OK. */
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
/* The remove wrappers return TRUE iff the removal actually committed (fix3 [0]): false on a
 * journal-failed flush-abort, an invalid index, or a commit reject -- so a deferred handler shows
 * "Removed X (Ctrl+Z)" + resets selection ONLY on a real removal, never a false success. */
int gui_project_add_atlas(void);                          /* returns new atlas index, or -1 */
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

/* The 10 atlas packing knobs, as a closed selector for gui_project_set_atlas_setting
 * (F2-05b-i). Each maps to one tp_atlas_field_mask bit; the panel edits ONE knob per
 * gesture, so the op carries a single-bit mask (byte-identical to the old in-place write). */
typedef enum {
    GUI_ATLAS_MAX_SIZE = 0,
    GUI_ATLAS_PADDING,
    GUI_ATLAS_MARGIN,
    GUI_ATLAS_EXTRUDE,
    GUI_ATLAS_ALPHA_THRESHOLD,
    GUI_ATLAS_MAX_VERTICES,
    GUI_ATLAS_SHAPE,
    GUI_ATLAS_ALLOW_TRANSFORM,
    GUI_ATLAS_POWER_OF_TWO,
    GUI_ATLAS_PIXELS_PER_UNIT
} gui_atlas_field;

/* Sets ONE atlas knob via an atlas.settings.set transaction. The int/bool knobs read
 * `ivalue` (bool as 0/1); pixels_per_unit reads `fvalue`. Value RANGES are core's now
 * (the op validates); the widget still parse-clamps. Returns true on commit. */
bool gui_project_set_atlas_setting(tp_id128 atlas_id, int64_t expected_revision,
                                   gui_atlas_field field, int ivalue, float fvalue);

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
 * `frames` may be NULL/0 for an empty animation. Returns the new animation index, or -1. */
int gui_project_create_animation(tp_id128 atlas_id, int64_t expected_revision,
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
/* Appends a default json-neotolis target "out/<atlas>.<ext>"; returns its index or -1. */
int gui_project_add_target(tp_id128 atlas_id, int64_t expected_revision);
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

/* --- undo / redo (F2-03 diff history) --- */
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
/* Fresh empty project: replaces the current one, clears path + both bits. Returns false
 * (KEEPING the current project intact) only when creating/wrapping the fresh project OOMs
 * (F2-05b-i F3: never lose the open project on an allocation failure). */
bool gui_project_new(void);
/* Loads `path`; on failure fills err_out (from tp_error) and leaves the current project
 * intact. On success replaces it, sets path, clears dirty, marks preview stale. */
tp_status gui_project_open(const char *path, char *err_out, size_t err_cap);
/* Saves to the current path (must exist). Clears project_dirty. */
tp_status gui_project_save(char *err_out, size_t err_cap);
/* Saves to `path`, remembers it, clears project_dirty. Promotes structural ids FIRST
 * and, on RNG failure, returns the error WITHOUT writing (never persists a nil-id file). */
tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap);

/* Drains a pending transaction REJECT recorded by a mutator whose op(s) core rejected
 * (out-of-range value / bad reference / OOM). The model is left byte-unchanged on a
 * reject; this surfaces the structured status to the status-bar soft-error channel.
 * Returns true once and copies the message into `out` (then clears it). */
bool gui_project_take_op_error(char *out, size_t cap);

/* Fills `out` with the reason the last flush's commit failed (fix3 [2]): the drained op-error, else a
 * NEUTRAL fallback that fits save + pack + the dirty gate. Consumes the op-error. NULL-safe. Shared by
 * every flush-failure abort path so they use one wording. */
void gui_project_flush_error(char *out, size_t cap);

#ifdef NTPACKER_GUI_SELFTEST
/* Dev seam: session owns the in-memory recovery backend; GUI may only attach it
 * and arm a bounded number of write failures. */
bool gui_project__test_attach_memory_recovery(void);
void gui_project__test_fail_next_recovery_writes(int count);
/* Dev seam (selftest only, fix [1]): hold / release a FOREIGN single-instance lock on `slot` from a
 * separate handle (simulates another live editor), and query whether recovery is ACTIVE (slot owned).
 * Lets the selftest prove a 2nd instance that cannot lock skips recovery + never touches the slot. */
bool gui_project__test_hold_foreign_lock(const char *slot);
void gui_project__test_release_foreign_lock(void);
bool gui_project__test_recovery_active(void);
/* Arm a one-shot skip of the next recovery-slot reset so a pre-seeded stale slot reaches the fresh
 * attach -- lets the selftest prove attach_recovery_journal fails CLOSED (journal-less + degraded
 * notice) rather than building a journal on foreign bytes. */
void gui_project__test_skip_next_recovery_reset(void);
/* Pin the recovery-metadata clock to `t` (>= 0) so ordering/classification tests are deterministic despite
 * time()'s 1-second resolution; pass < 0 to restore the real clock. */
void gui_project__test_set_recovery_now(int64_t t);
/* Dev seam (selftest only, R6a fix [2]): the REAL recovery-journal key, so a test can craft an orphan that
 * gui_recovery_collect / the candidate scan will actually classify adoptable (collect now key-filters). */
tp_id128 gui_project__test_recovery_key(void);
/* Dev seam (selftest only, fix2 F2): insert one fully-built entry into `out` through the real (adoptable desc,
 * timestamp desc) cap eviction, so the cap-regression guard is deterministic (no readdir-order dependence). */
void gui_project__test_recovery_insert(gui_recovery_list *out, const gui_recovery_entry *e);
/* Dev seam (selftest only, fix2 F3): arm a one-shot failure of gui_recovery_resolve's post-save load-verify so
 * the "save OK but the written file fails to reload -> journal KEPT" backstop has a regression test. */
void gui_project__test_fail_next_load_verify(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PROJECT_H */
