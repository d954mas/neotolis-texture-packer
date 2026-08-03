#ifndef NTPACKER_GUI_PROJECT_VIEW_H
#define NTPACKER_GUI_PROJECT_VIEW_H

/* Passive GUI-facing project contract. Views may read immutable session
 * snapshots and enqueue action intents carrying these value identities; live
 * session ownership, lifecycle, admission, and mutation functions are
 * deliberately absent. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_format.h"
#include "tp_core/tp_recovery_query.h"
#include "tp_core/tp_session_snapshot_query.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GUI_SPRITE_OV_SHAPE = 0,
    GUI_SPRITE_OV_ROTATE,
    GUI_SPRITE_OV_MAXVERT,
    GUI_SPRITE_OV_MARGIN,
    GUI_SPRITE_OV_EXTRUDE
} gui_sprite_ov;

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

#define GUI_RECOVERY_MAX_CANDIDATES TP_RECOVERY_MAX_CANDIDATES
#define GUI_RECOVERY_PATH_CAP TP_IDENTITY_PATH_MAX

typedef enum {
    GUI_RECOVERY_DISCARD = 0,
    GUI_RECOVERY_SAVE_ORIGINAL,
    GUI_RECOVERY_SAVE_AS
} gui_recovery_action;

typedef tp_recovery_candidate gui_recovery_entry;
typedef tp_recovery_candidates gui_recovery_list;

typedef struct gui_recovery_notice {
    const char *notice_id;
    uint64_t generation;
    tp_status status;
    bool has_last_durable_revision;
    int64_t last_durable_revision;
    char message[256];
} gui_recovery_notice;

const tp_session_snapshot *gui_project_snapshot(void);
/* Borrowed active immutable format generation plus available-row helpers used
 * by passive GUI selectors. */
const tp_format_catalog *gui_project_format_catalog(void);
int gui_project_format_count(void);
const tp_format_descriptor *gui_project_format_at(int index);
const tp_format_descriptor *gui_project_format_find(const char *id);
uint64_t gui_project_snapshot_model_generation(void);
const char *gui_project_path(void);
const char *gui_project_display_name(void);
bool gui_project_has_path(void);
bool gui_project_is_dirty(void);
bool gui_project_is_stale(void);
bool gui_project_can_undo(void);
bool gui_project_can_redo(void);
bool gui_project_recovery_notice_query(gui_recovery_notice *out);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PROJECT_VIEW_H */
