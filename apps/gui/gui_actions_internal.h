#ifndef NTPACKER_GUI_ACTIONS_INTERNAL_H
#define NTPACKER_GUI_ACTIONS_INTERNAL_H

#include "gui_actions.h"

#include "tp_core/tp_export.h"
#include "tp_core/tp_srckey.h"

typedef enum target_intent_kind {
    TARGET_INTENT_ENABLED = 0,
    TARGET_INTENT_EXPORTER
} target_intent_kind;

typedef struct target_edit_intent {
    target_intent_kind kind;
    tp_id128 atlas_id;
    tp_id128 target_id;
    int64_t expected_revision;
    bool enabled;
    char exporter_id[TP_EXPORTER_ID_MAX];
} target_edit_intent;

typedef struct pending_create_animation {
    bool active;
    tp_id128 atlas_id;
    int64_t expected_revision;
    char *name;
    tp_op_sprite_ref *frames;
    int frame_count;
} pending_create_animation;

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

typedef enum animation_intent_kind {
    ANIMATION_INTENT_FRAME_REMOVE = 0,
    ANIMATION_INTENT_FRAME_MOVE,
    ANIMATION_INTENT_ADD_FRAMES
} animation_intent_kind;

typedef struct animation_edit_intent {
    animation_intent_kind kind;
    gui_animation_ref animation;
    int first;
    int second;
    bool follow_selection;
    tp_op_sprite_ref *frames;
    int frame_count;
} animation_edit_intent;

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
    gui_lifecycle_request pending_lifecycle_request;
    bool pending_add_anim;
    tp_id128 pending_add_anim_atlas_id;
    int64_t pending_add_anim_revision;
    pending_create_animation pending_create_anim;
    bool pending_open_preview;
    gui_animation_ref pending_open_preview_ref;
    gui_animation_ref preview_animation_ref;
    preview_frame_cache preview_frames;
#ifdef NTPACKER_GUI_SELFTEST
    gui_preview_frame_work preview_frame_work;
#endif
    bool pending_remove_anim;
    gui_animation_ref pending_remove_anim_ref;
    bool pending_add_target;
    tp_id128 pending_add_target_atlas_id;
    int64_t pending_add_target_revision;
    bool pending_remove_target;
    gui_target_ref pending_remove_target_ref;
    bool pending_browse_target;
    gui_target_ref pending_browse_target_ref;
    gui_recovery_list recovery_list;
    int recovery_pending_row;
    int recovery_pending_action;
    target_edit_intent *target_intents;
    int target_intent_count;
    int target_intent_cap;
    gui_draft_owner draft;
    bool draft_initialized;
    bool draft_reducer_registered;
    bool draft_apply_mine;
    animation_edit_intent *animation_intents;
    int animation_intent_count;
    int animation_intent_cap;
    bool gesture_commit;
} gui_actions_state;

extern gui_actions_state s_actions;

char *gui_actions__strdup(const char *text);
void gui_actions__frame_refs_dispose(tp_op_sprite_ref *frames, int count);
tp_op_sprite_ref *gui_actions__frame_refs_copy(const tp_op_sprite_ref *frames,
                                               int count);
void gui_actions__drain_edits(void);
void gui_actions__rebase_deferred_edits(
    int64_t revision_before,
    int64_t revision_after);
void gui_actions__discard_deferred_edits(void);
void gui_actions__discard_edits(void);
bool gui_actions__submit_draft(void);
bool gui_actions__apply_draft_mine(void);
void gui_actions__pending_create_animation_dispose(
    pending_create_animation *request);
/* Index of the atlas carrying `atlas_id` in the snapshot, or -1. Used by undo_redo_settle (F2) to
 * re-resolve the viewed atlas after an undo/redo shifts atlas ordering, and by the preview player. */
int gui_actions__snapshot_atlas_index_by_id(const tp_session_snapshot *snapshot,
                                            tp_id128 atlas_id);
void gui_actions__apply_confirm(void);
bool gui_actions__apply_file_dialogs(void);
bool gui_actions__apply_lifecycle_request(void);
void gui_actions__browse_target(const gui_target_ref *target);
void gui_actions__poll_pack(void);
void gui_actions__apply_pack_requests(void);
void gui_actions__apply_recovery(void);
void gui_actions__apply_structural_edits(void);
void gui_actions__clear_pending(void);
void gui_actions__clear_history_request(void);

#endif /* NTPACKER_GUI_ACTIONS_INTERNAL_H */
