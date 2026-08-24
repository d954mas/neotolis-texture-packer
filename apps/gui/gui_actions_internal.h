#ifndef NTPACKER_GUI_ACTIONS_INTERNAL_H
#define NTPACKER_GUI_ACTIONS_INTERNAL_H

#include "gui_actions.h"
#include "gui_actions_dev.h"
#include "gui_actions_driver.h"
#include "gui_project.h"
#include "gui_project_driver.h"
#include "gui_state.h"

#include "tp_core/tp_export.h"
#include "tp_core/tp_srckey.h"

/* --- the one deferred-intent queue ---------------------------------------
 * Every between-frame request a view or an action owner defers is ONE entry
 * here. The enum is declared in DRAIN ORDER and that order is the contract:
 * it transcribes, one kind per line, the fixed sequence the four legacy
 * mechanisms had (two growable arrays + two if-sequences + a bare flag).
 *
 *   step 0   the animation_edit_intent[] loop     (enqueue order across kinds)
 *   step 1   the target_edit_intent[] loop        (enqueue order across kinds)
 *   steps 2-5    gui_actions__apply_file_dialogs' if-sequence
 *   steps 6-15   gui_actions__apply_structural_edits' if-sequence
 *   step 16      do_refresh
 *   steps 17-19  gui_actions__apply_pack_requests' if-sequence
 *
 * The two families that were arrays (ANIM_*, TARGET_*) share one step each,
 * because inside those arrays the legacy drain ran in ENQUEUE order across
 * kinds. Every other kind was a single boolean slot consumed by a fixed
 * if-sequence, so kind order IS the legacy order and same-kind re-requests
 * coalesce into the one slot exactly as setting a bool twice did. */
typedef enum gui_intent_kind {
    GUI_INTENT_ANIM_FRAME_REMOVE = 0,
    GUI_INTENT_ANIM_FRAME_MOVE,
    GUI_INTENT_ANIM_ADD_FRAMES,
    GUI_INTENT_TARGET_ENABLED,
    GUI_INTENT_TARGET_EXPORTER,
    GUI_INTENT_SAVE,
    GUI_INTENT_SAVE_AS,
    GUI_INTENT_ADD_FILES,
    GUI_INTENT_ADD_FOLDER,
    GUI_INTENT_ADD_ATLAS,
    GUI_INTENT_REMOVE_SOURCE,
    GUI_INTENT_REMOVE_ATLAS,
    GUI_INTENT_ADD_TARGET,
    GUI_INTENT_REMOVE_TARGET,
    GUI_INTENT_BROWSE_TARGET,
    GUI_INTENT_ADD_ANIMATION,
    GUI_INTENT_CREATE_ANIMATION,
    GUI_INTENT_REMOVE_ANIMATION,
    GUI_INTENT_OPEN_PREVIEW,
    GUI_INTENT_REFRESH,
    GUI_INTENT_CANCEL,
    GUI_INTENT_PACK,
    GUI_INTENT_EXPORT,
    GUI_INTENT_PREVIEW_TARGET
} gui_intent_kind;

/* Contiguous kind ranges of the one queue, named after the boundary each one
 * runs inside gui_actions_step. A phase is drained on its own because the
 * lifecycle/confirm/recovery owners sit BETWEEN them and may abort the frame. */
typedef enum gui_intent_phase {
    GUI_INTENT_PHASE_EDIT = 0,   /* after the draft prerequisite settles */
    GUI_INTENT_PHASE_DIALOG,     /* tinyfiledialogs, outside nt_ui_begin/end */
    GUI_INTENT_PHASE_STRUCTURAL, /* create/remove/browse/preview commands */
    GUI_INTENT_PHASE_REFRESH,
    GUI_INTENT_PHASE_PACK
} gui_intent_phase;

typedef struct gui_intent {
    gui_intent_kind kind;
    union {
        /* GUI_INTENT_ANIM_* */
        struct {
            gui_animation_ref animation;
            int first;
            int second;
            bool follow_selection;
            tp_op_sprite_ref *frames; /* owned */
            int frame_count;
        } animation_edit;
        /* GUI_INTENT_TARGET_* */
        struct {
            tp_id128 atlas_id;
            tp_id128 target_id;
            int64_t expected_revision;
            bool enabled;
            char exporter_id[TP_EXPORTER_ID_MAX];
        } target_edit;
        /* GUI_INTENT_REMOVE_ATLAS, ADD_TARGET, ADD_ANIMATION */
        struct {
            tp_id128 atlas_id;
            int64_t expected_revision;
        } scope;
        /* GUI_INTENT_REMOVE_SOURCE */
        struct {
            tp_id128 atlas_id;
            tp_id128 source_id;
            int64_t expected_revision;
        } source;
        /* GUI_INTENT_REMOVE_TARGET, BROWSE_TARGET */
        gui_target_ref target;
        /* GUI_INTENT_REMOVE_ANIMATION, OPEN_PREVIEW */
        gui_animation_ref animation;
        /* GUI_INTENT_SAVE_AS: prepared before a draft closes the current cut. */
        struct {
            gui_project_save_as_plan plan;
            bool prepared;
        } save_as;
        /* GUI_INTENT_CREATE_ANIMATION */
        struct {
            tp_id128 atlas_id;
            int64_t expected_revision;
            char *name;               /* owned */
            tp_op_sprite_ref *frames; /* owned */
            int frame_count;
        } create_animation;
        /* GUI_INTENT_PREVIEW_TARGET: explicit Native or copied stable id. */
        gui_preview_target preview_target;
    } payload;
} gui_intent;

typedef struct preview_frame_cache {
    int *indices;
    int capacity;
    int count;
    int ref_w;
    int ref_h;
    tp_id128 atlas_id;
    tp_id128 animation_id;
    uint64_t model_generation;
    uint64_t pack_result_version;
    bool valid;
} preview_frame_cache;

typedef enum gui_draft_family {
    GUI_DRAFT_NONE = 0,
    GUI_DRAFT_ATLAS_SCALAR,
    GUI_DRAFT_TEXT,
    GUI_DRAFT_SPRITE,
    GUI_DRAFT_ANIMATION
} gui_draft_family;

typedef enum gui_text_edit_kind {
    GUI_TEXT_EDIT_ATLAS_NAME = 0,
    GUI_TEXT_EDIT_ANIMATION_NAME,
    GUI_TEXT_EDIT_SPRITE_RENAME,
    GUI_TEXT_EDIT_TARGET_OUT_PATH
} gui_text_edit_kind;

typedef struct gui_draft_owner {
    gui_edit_state lifecycle;
    gui_draft_family family;
    struct {
        gui_atlas_field component;
        int integer;
        float real;
    } atlas;
    struct {
        gui_text_edit_kind kind;
        tp_id128 atlas_id;
        tp_id128 source_id;
        char source_key[TP_SRCKEY_MAX];
        char value[TP_IDENTITY_PATH_MAX];
    } text;
    struct {
        gui_sprite_edit_kind kind;
        tp_id128 atlas_id;
        tp_id128 source_id;
        char source_key[TP_SRCKEY_MAX];
        int component;
        int integer;
        float real;
    } sprite;
    struct {
        gui_animation_edit_kind kind;
        tp_id128 atlas_id;
        int component;
        int integer;
        float real;
        bool flag;
    } animation;
} gui_draft_owner;

typedef struct gui_lifecycle_flow {
    gui_lifecycle_phase phase;
    gui_lifecycle_request request;
    gui_lifecycle_choice choice;
} gui_lifecycle_flow;

typedef struct gui_recovery_flow {
    gui_recovery_phase phase;
    gui_recovery_list list;
    int pending_row;
    gui_recovery_action pending_action;
} gui_recovery_flow;

typedef struct gui_actions_state {
    /* Modal/dialog state, NOT queue intents: each is owned by a modal FSM that
     * consumes and re-arms it (the unsaved-changes confirm flow and the R6b
     * startup recovery modal), and each carries its own NONE/-1 sentinel. */
    gui_lifecycle_request pending_lifecycle_request;
    gui_lifecycle_flow lifecycle;
    gui_recovery_flow recovery;

    double last_pack_ms;
    tp_id128 last_pack_atlas_id;
    gui_animation_ref preview_animation_ref;
    /* The player has resolved at least one real pack result since it was armed.
     * It is what separates "this atlas was never packed" (show the hint) from
     * "the result we were playing is gone" (stop, and say so). */
    bool preview_had_result;
    preview_frame_cache preview_frames;
#ifdef NTPACKER_GUI_SELFTEST
    gui_preview_frame_work preview_frame_work;
#endif
    gui_draft_owner draft;
    bool draft_initialized;
    bool draft_apply_mine;
    bool gesture_commit;

    struct {
        tp_id128 atlas_id;
        tp_id128 animation_id;
        int frame_index;
        bool present;
    } pending_frame_selection;
    tp_id128 pending_added_atlas_status_id;
    struct {
        tp_id128 atlas_id;
        tp_id128 animation_id;
        int frame_count;
        bool with_frames;
        bool present;
    } pending_created_animation;
    struct {
        tp_id128 animation_id;
        bool present;
    } pending_history_reconcile;
    bool pending_view_reconcile;
    bool format_reload_requested;
    gui_intent *intents;
    int intent_count;
    int intent_cap;
    gui_actions_step_result *active_step_result;
} gui_actions_state;

extern gui_actions_state s_actions;

/* Internal executor reached only from the typed intent drain. */
void do_pack(void);

char *gui_actions__strdup(const char *text);
void gui_actions__frame_refs_dispose(tp_op_sprite_ref *frames, int count);
tp_op_sprite_ref *gui_actions__frame_refs_copy(const tp_op_sprite_ref *frames,
                                               int count);

/* The one enqueue. Kinds outside the two edit families are single-slot: a
 * second request of the same kind REPLACES the queued one (its payload is
 * destroyed first), which is what setting the same bool twice used to mean.
 * Returns false only when the queue could not grow; the caller keeps ownership
 * of nothing -- an owned payload in `intent` is destroyed on failure. */
bool gui_actions__intent_push(const gui_intent *intent);
/* Runs every queued intent of `phase` in the drain order documented on
 * gui_intent_kind. Project replacement belongs to the lifecycle FSM, not this
 * ordinary semantic-intent queue. */
void gui_actions__intent_drain(gui_intent_phase phase);
bool gui_actions__intent_queued(gui_intent_kind kind);
/* Exit-time release of what each owner grew: the undrained queue with its owned
 * payloads, and the grow-only preview frame map. Called only by
 * gui_actions_shutdown, in that order. */
void gui_actions__intent_shutdown(void);
void gui_actions__preview_shutdown(void);
void gui_actions__rebase_deferred_edits(
    int64_t revision_before,
    int64_t revision_after);
void gui_actions__discard_deferred_edits(void);
void gui_actions__discard_edits(void);
void gui_actions__reconcile_observation(void);
void gui_actions__record_created_animation(
    tp_id128 atlas_id, tp_id128 animation_id,
    int frame_count, bool with_frames);
bool gui_actions__submit_draft(void);
bool gui_actions__apply_draft_mine(void);
void gui_actions__apply_confirm(void);
bool gui_actions__apply_lifecycle_request(void);
void gui_actions__run_open_lifecycle_dialog(void);
/* Intent executors that live in their owning TU (dialogs, refresh, pack). The
 * one drain switch calls them; nothing else does. */
bool gui_actions__open_dialog(void);
bool gui_actions__save(void);
bool gui_actions__save_as(
    const gui_project_save_as_plan *prepared);
void gui_actions__add_files(void);
void gui_actions__add_folder(void);
void gui_actions__browse_target(const gui_target_ref *target);
void gui_actions__refresh(void);
void gui_actions__cancel(void);
void gui_actions__export(void);
void gui_actions__preview_target_start(
    const gui_preview_target *target);
gui_pack_done gui_actions__consume_completion(
    tp_session_job_result *completion,
    gui_pack_result_info *out);
void gui_actions__complete_lifecycle(
    gui_project_lifecycle_kind completed);
void gui_actions__record_job_request(
    gui_job_request_kind kind, bool admitted,
    const char *detail);
#ifdef TP_ENABLE_TEST_SEAMS
/* Copies only value fields from the already-consumed Refresh terminal. These
 * seams never poll, retain a job owner, or influence scheduling. */
void gui_actions__test_reset_refresh_completion(void);
bool gui_actions__test_take_refresh_completion(
    gui_pack_done *out_done, gui_pack_result_info *out_info);
#endif
void gui_actions__apply_recovery(void);
void gui_actions__clear_pending(void);
void gui_actions__clear_history_request(void);

#endif /* NTPACKER_GUI_ACTIONS_INTERNAL_H */
