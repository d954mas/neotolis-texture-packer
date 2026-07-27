#ifndef NTPACKER_GUI_ACTIONS_INTERNAL_H
#define NTPACKER_GUI_ACTIONS_INTERNAL_H

#include "gui_actions.h"

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
 *   steps 2-6    gui_actions__apply_file_dialogs' if-sequence
 *   steps 7-16   gui_actions__apply_structural_edits' if-sequence
 *   step 17      do_refresh
 *   steps 18-20  gui_actions__apply_pack_requests' if-sequence
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
    GUI_INTENT_OPEN,
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
    GUI_INTENT_PACK,
    GUI_INTENT_EXPORT,
    GUI_INTENT_PREVIEW_TARGET
} gui_intent_kind;

/* Contiguous kind ranges of the one queue, named after the boundary each one
 * runs at inside apply_pending(). A phase is drained on its own because the
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
        /* GUI_INTENT_CREATE_ANIMATION */
        struct {
            tp_id128 atlas_id;
            int64_t expected_revision;
            char *name;               /* owned */
            tp_op_sprite_ref *frames; /* owned */
            int frame_count;
        } create_animation;
        /* GUI_INTENT_PREVIEW_TARGET: exporter option, not a target entity index */
        int exporter_slot;
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

typedef struct gui_actions_state {
    /* Modal/dialog state, NOT queue intents: each is owned by a modal FSM that
     * consumes and re-arms it (the unsaved-changes confirm flow and the R6b
     * startup recovery modal), and each carries its own NONE/-1 sentinel. */
    gui_lifecycle_request pending_lifecycle_request;
    gui_recovery_list recovery_list;
    int recovery_pending_row;
    int recovery_pending_action;

    gui_animation_ref preview_animation_ref;
    preview_frame_cache preview_frames;
#ifdef NTPACKER_GUI_SELFTEST
    gui_preview_frame_work preview_frame_work;
#endif
    gui_draft_owner draft;
    bool draft_initialized;
    bool draft_reducer_registered;
    bool draft_apply_mine;
    bool gesture_commit;

    gui_intent *intents;
    int intent_count;
    int intent_cap;
} gui_actions_state;

extern gui_actions_state s_actions;

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
 * gui_intent_kind. Returns true when an intent started a project lifecycle
 * transition (only GUI_INTENT_OPEN can). */
bool gui_actions__intent_drain(gui_intent_phase phase);
bool gui_actions__intent_queued(gui_intent_kind kind);
void gui_actions__rebase_deferred_edits(
    int64_t revision_before,
    int64_t revision_after);
void gui_actions__discard_deferred_edits(void);
void gui_actions__discard_edits(void);
bool gui_actions__submit_draft(void);
bool gui_actions__apply_draft_mine(void);
void gui_actions__apply_confirm(void);
bool gui_actions__apply_lifecycle_request(void);
void gui_actions__request_open_dialog(void);
/* Intent executors that live in their owning TU (dialogs, refresh, pack). The
 * one drain switch calls them; nothing else does. */
bool gui_actions__open_dialog(void);
bool gui_actions__save(void);
bool gui_actions__save_as(void);
void gui_actions__add_files(void);
void gui_actions__add_folder(void);
void gui_actions__browse_target(const gui_target_ref *target);
void gui_actions__refresh(void);
void gui_actions__export(void);
void gui_actions__preview_target_start(int combo_index);
void gui_actions__poll_pack(void);
void gui_actions__apply_recovery(void);
void gui_actions__clear_pending(void);
void gui_actions__clear_history_request(void);

#endif /* NTPACKER_GUI_ACTIONS_INTERNAL_H */
