#ifndef NTPACKER_GUI_ACTIONS_INTERNAL_H
#define NTPACKER_GUI_ACTIONS_INTERNAL_H

#include "gui_actions.h"

#include "tp_core/tp_export.h"
#include "tp_core/tp_srckey.h"

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

typedef enum target_intent_kind {
    TARGET_INTENT_FULL = 0,
    TARGET_INTENT_OUT_PATH,
    TARGET_INTENT_ENABLED,
    TARGET_INTENT_EXPORTER
} target_intent_kind;

typedef struct target_edit_intent {
    target_intent_kind kind;
    tp_id128 atlas_id;
    tp_id128 target_id;
    int64_t expected_revision;
    int i0, i1, i2;
    float f0, f1;
    bool b0, b1;
    char s0[TP_EXPORTER_ID_MAX];
    char *out_path;
} target_edit_intent;

typedef struct atlas_setting_intent {
    tp_id128 atlas_id;
    int64_t expected_revision;
    gui_atlas_field field;
    int ivalue;
    float fvalue;
} atlas_setting_intent;

typedef enum sprite_intent_kind {
    SPRITE_INTENT_ORIGIN = 0,
    SPRITE_INTENT_SLICE9,
    SPRITE_INTENT_OVERRIDE
} sprite_intent_kind;

typedef struct sprite_edit_intent {
    sprite_intent_kind kind;
    tp_id128 atlas_id;
    tp_id128 source_id;
    int64_t expected_revision;
    char *source_key;
    int field;
    int ivalue;
    float fvalue;
} sprite_edit_intent;

typedef enum animation_intent_kind {
    ANIMATION_INTENT_FPS = 0,
    ANIMATION_INTENT_PLAYBACK,
    ANIMATION_INTENT_FLIP,
    ANIMATION_INTENT_FRAME_REMOVE,
    ANIMATION_INTENT_FRAME_MOVE,
    ANIMATION_INTENT_ADD_FRAMES
} animation_intent_kind;

typedef struct animation_edit_intent {
    animation_intent_kind kind;
    gui_animation_ref animation;
    int first;
    int second;
    float value;
    bool flip_h;
    bool flip_v;
    tp_op_sprite_ref *frames;
    int frame_count;
} animation_edit_intent;

typedef struct gui_actions_state {
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
    atlas_setting_intent *atlas_setting_intents;
    int atlas_setting_intent_count;
    int atlas_setting_intent_cap;
    sprite_edit_intent *sprite_intents;
    int sprite_intent_count;
    int sprite_intent_cap;
    animation_edit_intent *animation_intents;
    int animation_intent_count;
    int animation_intent_cap;
    bool gesture_commit;
    tp_id128 edit_atlas_id;
    int64_t edit_atlas_revision;
    tp_id128 edit_anim_atlas_id;
    tp_id128 edit_anim_id;
    int64_t edit_anim_revision;
    tp_id128 edit_sprite_atlas_id;
    tp_id128 edit_sprite_source_id;
    int64_t edit_sprite_revision;
    char edit_sprite_source_key[TP_SRCKEY_MAX];
} gui_actions_state;

extern gui_actions_state s_actions;

char *gui_actions__strdup(const char *text);
void gui_actions__frame_refs_dispose(tp_op_sprite_ref *frames, int count);
tp_op_sprite_ref *gui_actions__frame_refs_copy(const tp_op_sprite_ref *frames,
                                               int count);
void gui_actions__drain_edits(void);
void gui_actions__pending_create_animation_dispose(
    pending_create_animation *request);
bool gui_actions__resolve_animation_ref(const gui_animation_ref *animation,
                                        int *atlas_index,
                                        int *animation_index);
bool gui_actions__busy_block(void);
bool gui_actions__flush_failed(void);
void gui_actions__apply_confirm(void);
void gui_actions__apply_file_dialogs(void);
void gui_actions__browse_target(const gui_target_ref *target);
void gui_actions__poll_pack(void);
void gui_actions__apply_pack_requests(void);

#endif /* NTPACKER_GUI_ACTIONS_INTERNAL_H */
