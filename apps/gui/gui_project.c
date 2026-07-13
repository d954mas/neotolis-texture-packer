#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gui_history.h"
#include "gui_scan.h"

#include "tp_core/tp_id.h"             /* tp_rng_os for id promotion */
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */

// #region state
static tp_project *s_proj;
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_project_dirty;
static bool s_preview_stale;
static unsigned s_model_ver; /* bumped per real mutation (gui_project_model_version) */
static char s_name[256]; /* cached basename for the menu bar */

/* Snapshots (serialized project bytes) for undo/dirty recompute (ux.md §3.3c). */
static char *s_last_buf;   /* the CURRENT model, serialized (the pre-mutation snapshot the next touch pushes) */
static size_t s_last_len;
static char *s_saved_buf;  /* the last-SAVED model, serialized (dirty baseline) */
static size_t s_saved_len;
static double s_now;       /* history clock (seconds), fed each frame */

/* Pending id-promotion failure from a void-context ensure_ids() (a snapshot/touch):
 * OS-RNG failure would leave nil structural ids, which must never be silently
 * swallowed. Drained + surfaced once by the UI (gui_project_take_id_error). The
 * save path fails closed on its own, so it does NOT set this. */
static bool s_id_error;
static char s_id_error_msg[256];
// #endregion

// #region helpers
static char *dupbytes(const char *src, size_t len) {
    char *c = (char *)malloc(len ? len : 1U);
    if (c && len) {
        memcpy(c, src, len);
    }
    return c;
}

static char *dupstr(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *c = (char *)malloc(n);
    if (c) {
        memcpy(c, s, n);
    }
    return c;
}

static void recompute_name(void) {
    if (s_path[0] == '\0') {
        (void)snprintf(s_name, sizeof s_name, "untitled");
        return;
    }
    const char *base = s_path;
    for (const char *p = s_path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    (void)snprintf(s_name, sizeof s_name, "%s", base);
}

static void set_path(const char *path) {
    (void)snprintf(s_path, sizeof s_path, "%s", path ? path : "");
    recompute_name();
}

/* Serialize the live model; on OOM leaves *buf NULL. */
static void serialize_current(char **buf, size_t *len) {
    *buf = NULL;
    *len = 0;
    if (!s_proj) {
        return;
    }
    tp_error e = {0};
    char *b = NULL;
    size_t n = 0;
    if (tp_project_save_buffer(s_proj, &b, &n, &e) == TP_STATUS_OK) {
        *buf = b;
        *len = n;
    }
}

/* Assign a random persistent ID to any structural entity that lacks one -- nil (a
 * freshly created project/atlas/anim/target) OR loader-synthesized for a migrated
 * legacy file (§5.5: the first writable save persists fresh random IDs, not the
 * stable synthetic ones). A real loaded ID (v3/v4) is preserved. Idempotent after the
 * first call: once no ID is nil or synthetic this is a no-op and never re-changes an
 * ID. Called before every snapshot so undo/redo bytes -- and the saved file -- always
 * carry stable, non-nil structural IDs (a writable session gets its final IDs before
 * the first mutation). Returns the promote status (RNG failure -> TP_STATUS_RNG_FAILED,
 * ids left untouched); `err` (may be NULL) carries prose. */
static tp_status ensure_ids(tp_error *err) {
    if (!s_proj) {
        return TP_STATUS_OK;
    }
    tp_rng rng = tp_rng_os();
    return tp_project_promote_ids(s_proj, &rng, err);
}

/* Record a void-context id-promotion failure so the UI can surface it (never silently
 * swallowed): a nil-id model would serialize nil ids that fail on reload/save. */
static void note_id_error(tp_status st, const tp_error *err) {
    s_id_error = true;
    (void)snprintf(s_id_error_msg, sizeof s_id_error_msg, "%s", (err && err->msg[0]) ? err->msg : tp_status_str(st));
}

bool gui_project_take_id_error(char *out, size_t cap) {
    if (!s_id_error) {
        return false;
    }
    if (out && cap) {
        (void)snprintf(out, cap, "%s", s_id_error_msg);
    }
    s_id_error = false;
    return true;
}

static void set_last_from_current(void) {
    tp_error err = {0};
    tp_status st = ensure_ids(&err);
    if (st != TP_STATUS_OK) {
        note_id_error(st, &err); /* keep snapshotting: surfaces the fault, never crashes */
    }
    free(s_last_buf);
    serialize_current(&s_last_buf, &s_last_len);
}

/* Adopt the current model bytes as the last-SAVED baseline (dirty == 0 after). */
static void set_saved_baseline(void) {
    free(s_saved_buf);
    s_saved_buf = NULL;
    s_saved_len = 0;
    if (s_last_buf) {
        s_saved_buf = dupbytes(s_last_buf, s_last_len);
        s_saved_len = s_last_len;
    }
}

static void recompute_dirty(void) {
    s_project_dirty = !(s_last_buf && s_saved_buf && s_last_len == s_saved_len &&
                        memcmp(s_last_buf, s_saved_buf, s_last_len) == 0);
}

/* An override entry that would serialize to just its name (safe to drop -> sparse). */
static bool sprite_all_default(const tp_project_sprite *s) {
    return s->origin_x == TP_PROJECT_ORIGIN_DEFAULT && s->origin_y == TP_PROJECT_ORIGIN_DEFAULT &&
           s->slice9_lrtb[0] == 0 && s->slice9_lrtb[1] == 0 && s->slice9_lrtb[2] == 0 && s->slice9_lrtb[3] == 0 &&
           s->rename == NULL && s->ov_shape == TP_PROJECT_OV_INHERIT && s->ov_allow_rotate == TP_PROJECT_OV_INHERIT &&
           s->ov_max_vertices == TP_PROJECT_OV_INHERIT && s->ov_margin == TP_PROJECT_OV_INHERIT &&
           s->ov_extrude == TP_PROJECT_OV_INHERIT;
}

/* Seeds a fresh atlas with the default target (core helper owns the exporter id +
 * "out/<name>" path -- review §3.1). Only a target-free atlas is seeded; no touch
 * (callers snapshot around it). */
static void seed_default_target(tp_project *p, int atlas_index) {
    tp_project_atlas *a = tp_project_get_atlas(p, atlas_index);
    if (!a || a->target_count > 0) {
        return;
    }
    (void)tp_project_atlas_seed_default_target(p, atlas_index);
}
// #endregion

// #region lifecycle
void gui_project_init(void) {
    if (s_proj) {
        return;
    }
    s_proj = tp_project_create();
    seed_default_target(s_proj, 0); /* clean baseline includes it (I1) */
    set_path("");
    s_project_dirty = false;
    s_preview_stale = false;
    gui_history_init();
    set_last_from_current();
    set_saved_baseline();
}

void gui_project_shutdown(void) {
    gui_history_shutdown();
    tp_project_destroy(s_proj);
    s_proj = NULL;
    free(s_last_buf);
    s_last_buf = NULL;
    s_last_len = 0;
    free(s_saved_buf);
    s_saved_buf = NULL;
    s_saved_len = 0;
}
// #endregion

// #region accessors
tp_project *gui_project_get(void) { return s_proj; }
const char *gui_project_path(void) { return s_path; }
const char *gui_project_display_name(void) { return s_name; }
bool gui_project_has_path(void) { return s_path[0] != '\0'; }
bool gui_project_is_dirty(void) { return s_project_dirty; }
bool gui_project_is_stale(void) { return s_preview_stale; }
// #endregion

// #region dirty/stale choke point
void gui_project_touch(gui_action act) {
    s_preview_stale = true;
    tp_error id_err = {0};
    tp_status id_st = ensure_ids(&id_err); /* a just-added atlas/anim/target gets its ID before this snapshot */
    if (id_st != TP_STATUS_OK) {
        note_id_error(id_st, &id_err); /* do not swallow an RNG failure */
    }
    char *nb = NULL;
    size_t nl = 0;
    serialize_current(&nb, &nl);
    if (!nb) {
        s_project_dirty = true; /* fallback: can't snapshot, assume changed */
        return;
    }
    if (s_last_buf && nl == s_last_len && memcmp(nb, s_last_buf, nl) == 0) {
        free(nb); /* memcmp dedup: no real change -> no history, no dirty flip */
        return;
    }
    if (s_last_buf) {
        gui_history_push(s_last_buf, s_last_len, (uint32_t)act, s_now); /* PRE-mutation snapshot */
    }
    free(s_last_buf);
    s_last_buf = nb;
    s_last_len = nl;
    s_model_ver++; /* a real change committed -> a view watching this drops its stale derived state */
    recompute_dirty();
}

void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
unsigned gui_project_model_version(void) { return s_model_ver; }
// #endregion

// #region mutation wrappers
int gui_project_add_atlas(void) {
    if (!s_proj) {
        return -1;
    }
    char name[64];
    (void)snprintf(name, sizeof name, "atlas%d", s_proj->atlas_count + 1);
    int idx = -1;
    if (tp_project_add_atlas(s_proj, name, &idx) != TP_STATUS_OK) {
        return -1;
    }
    seed_default_target(s_proj, idx); /* fresh atlas exports something (I1) */
    gui_project_touch(GUI_ACT_ADD_ATLAS);
    return idx;
}

void gui_project_remove_atlas(int index) {
    if (!s_proj) {
        return;
    }
    if (tp_project_remove_atlas(s_proj, index) == TP_STATUS_OK) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_ATLAS);
    }
}

gui_add_status gui_project_add_source_kind(int atlas_index, const char *path, tp_source_kind kind) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !path || path[0] == '\0') {
        return GUI_ADD_FAILED;
    }
    const int before = a->source_count;
    if (tp_project_atlas_add_source_kind(a, path, kind) != TP_STATUS_OK) {
        return GUI_ADD_FAILED;
    }
    if (a->source_count == before) {
        return GUI_ADD_DUPLICATE; /* tp_core dedupe no-op -- no touch, no dirty */
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_ADD_SOURCE);
    return GUI_ADD_ADDED;
}

gui_add_status gui_project_add_source(int atlas_index, const char *path) {
    /* Folder default: the "Add Folder" dialog and other kind-agnostic callers. The
     * "Add Files" dialog records TP_SOURCE_KIND_FILE via add_source_kind directly. */
    return gui_project_add_source_kind(atlas_index, path, TP_SOURCE_KIND_FOLDER);
}

void gui_project_remove_source(int atlas_index, int source_index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return;
    }
    if (tp_project_atlas_remove_source(a, source_index) == TP_STATUS_OK) {
        gui_scan_invalidate_all();
        gui_project_touch(GUI_ACT_REMOVE_SOURCE);
    }
}

bool gui_project_set_atlas_name(int atlas_index, const char *name) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !name || name[0] == '\0') {
        return false;
    }
    char *copy = dupstr(name);
    if (!copy) {
        return false;
    }
    free(a->name);
    a->name = copy;
    gui_project_touch(GUI_ACT_RENAME_ATLAS);
    return true;
}

bool gui_project_set_sprite_rename(int atlas_index, const char *sprite_name, const char *rename) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    if (tp_project_atlas_set_sprite_rename(a, sprite_name, rename) != TP_STATUS_OK) {
        return false;
    }
    gui_project_touch(GUI_ACT_RENAME_SPRITE); /* touch dedups a no-op rename */
    return true;
}

void gui_project_touch_setting(void) { gui_project_touch(GUI_ACT_SET_SETTING); }

/* Ensures the override entry for `sprite_name`, hands it to `set`, then drops it if
 * it became all-default (sparse) and funnels through the touch choke point. */
static bool sprite_override_edit(int atlas_index, const char *sprite_name,
                                 void (*set)(tp_project_sprite *, int, int), int arg0, int arg1) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !sprite_name || sprite_name[0] == '\0') {
        return false;
    }
    tp_project_sprite *s = NULL;
    if (tp_project_atlas_add_sprite(a, sprite_name, &s) != TP_STATUS_OK) {
        return false;
    }
    set(s, arg0, arg1);
    if (sprite_all_default(s)) {
        (void)tp_project_atlas_remove_sprite(a, sprite_name);
    }
    gui_project_touch(GUI_ACT_SET_SETTING);
    return true;
}

static void apply_origin(tp_project_sprite *s, int ox_bits, int oy_bits) {
    float ox;
    float oy;
    memcpy(&ox, &ox_bits, sizeof ox);
    memcpy(&oy, &oy_bits, sizeof oy);
    s->origin_x = ox;
    s->origin_y = oy;
}
bool gui_project_set_sprite_origin(int atlas_index, const char *sprite_name, float ox, float oy) {
    int ox_bits;
    int oy_bits;
    memcpy(&ox_bits, &ox, sizeof ox_bits);
    memcpy(&oy_bits, &oy, sizeof oy_bits);
    return sprite_override_edit(atlas_index, sprite_name, apply_origin, ox_bits, oy_bits);
}

static void apply_slice9(tp_project_sprite *s, int idx, int value) {
    if (idx >= 0 && idx < 4) {
        s->slice9_lrtb[idx] = (uint16_t)(value < 0 ? 0 : value);
    }
}
bool gui_project_set_sprite_slice9(int atlas_index, const char *sprite_name, int lrtb_index, int value) {
    return sprite_override_edit(atlas_index, sprite_name, apply_slice9, lrtb_index, value);
}

static void apply_override(tp_project_sprite *s, int which, int value) {
    const int16_t v = (int16_t)value;
    switch ((gui_sprite_ov)which) {
        case GUI_SPRITE_OV_SHAPE: s->ov_shape = v; break;
        case GUI_SPRITE_OV_ROTATE: s->ov_allow_rotate = v; break;
        case GUI_SPRITE_OV_MAXVERT: s->ov_max_vertices = v; break;
        case GUI_SPRITE_OV_MARGIN: s->ov_margin = v; break;
        case GUI_SPRITE_OV_EXTRUDE: s->ov_extrude = v; break;
    }
}
bool gui_project_set_sprite_override(int atlas_index, const char *sprite_name, gui_sprite_ov which, int value) {
    return sprite_override_edit(atlas_index, sprite_name, apply_override, (int)which, value);
}

int gui_project_add_target(int atlas_index) {
    /* Same default-target op as fresh-atlas seeding (core owns id + path). */
    if (tp_project_atlas_seed_default_target(s_proj, atlas_index) != TP_STATUS_OK) {
        return -1;
    }
    gui_project_touch(GUI_ACT_ADD_TARGET);
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    return a ? a->target_count - 1 : -1;
}

void gui_project_remove_target(int atlas_index, int index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (a && tp_project_atlas_remove_target(a, index) == TP_STATUS_OK) {
        gui_project_touch(GUI_ACT_REMOVE_TARGET);
    }
}

bool gui_project_set_target(int atlas_index, int index, const char *exporter_id, const char *out_path, bool enabled) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || tp_project_atlas_set_target(a, index, exporter_id, out_path, enabled) != TP_STATUS_OK) {
        return false;
    }
    gui_project_touch(GUI_ACT_SET_TARGET);
    return true;
}
// #endregion

// #region animations
static tp_project_anim *anim_at(int atlas_index, int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return NULL;
    }
    return &a->animations[anim_index];
}

bool gui_project_anim_id_exists(int atlas_index, const char *id) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !id) {
        return false;
    }
    for (int i = 0; i < a->animation_count; i++) {
        if (a->animations[i].name && strcmp(a->animations[i].name, id) == 0) {
            return true;
        }
    }
    return false;
}

int gui_project_create_animation(int atlas_index, const char *base, const char *const *frames, int frame_count) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return -1;
    }
    /* unique id: prefer `base` verbatim, else base"2"/"3"...; a NULL/empty base auto-names "animN". */
    char id[128];
    if (base && base[0]) {
        (void)snprintf(id, sizeof id, "%s", base);
        for (int n = 2; gui_project_anim_id_exists(atlas_index, id); n++) {
            (void)snprintf(id, sizeof id, "%s%d", base, n);
        }
    } else {
        for (int n = 1;; n++) {
            (void)snprintf(id, sizeof id, "anim%d", n);
            if (!gui_project_anim_id_exists(atlas_index, id)) {
                break;
            }
        }
    }
    tp_project_anim *an = NULL;
    if (tp_project_atlas_add_animation(a, id, &an) != TP_STATUS_OK) {
        return -1;
    }
    for (int i = 0; frames && i < frame_count; i++) {
        if (frames[i] && frames[i][0]) {
            (void)tp_project_anim_add_frame(an, frames[i]);
        }
    }
    gui_project_touch(GUI_ACT_ADD_ANIM);
    return a->animation_count - 1;
}

void gui_project_remove_animation(int atlas_index, const char *id) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (a && id && tp_project_atlas_remove_animation(a, id) == TP_STATUS_OK) {
        gui_project_touch(GUI_ACT_REMOVE_ANIM);
    }
}

bool gui_project_set_anim_id(int atlas_index, int anim_index, const char *new_id) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !new_id || new_id[0] == '\0') {
        return false;
    }
    if (an->name && strcmp(an->name, new_id) == 0) {
        return true; /* no-op */
    }
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    for (int i = 0; a && i < a->animation_count; i++) {
        if (i != anim_index && a->animations[i].name && strcmp(a->animations[i].name, new_id) == 0) {
            return false; /* clashes with another animation */
        }
    }
    char *copy = dupstr(new_id);
    if (!copy) {
        return false;
    }
    free(an->name); /* rename edits the logical name; the structural id is unchanged */
    an->name = copy;
    gui_project_touch(GUI_ACT_RENAME_ANIM);
    return true;
}

bool gui_project_set_anim_fps(int atlas_index, int anim_index, float fps) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (!(fps >= 1.0F)) {
        fps = 1.0F;
    }
    if (an->fps == fps) {
        return true;
    }
    an->fps = fps;
    gui_project_touch(GUI_ACT_SET_ANIM);
    return true;
}

bool gui_project_set_anim_playback(int atlas_index, int anim_index, int playback) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (playback < 0) {
        playback = 0;
    }
    if (playback > 6) {
        playback = 6;
    }
    if (an->playback == playback) {
        return true;
    }
    an->playback = playback;
    gui_project_touch(GUI_ACT_SET_ANIM);
    return true;
}

bool gui_project_set_anim_flip(int atlas_index, int anim_index, bool flip_h, bool flip_v) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an) {
        return false;
    }
    if (an->flip_h == flip_h && an->flip_v == flip_v) {
        return true;
    }
    an->flip_h = flip_h;
    an->flip_v = flip_v;
    gui_project_touch(GUI_ACT_SET_ANIM);
    return true;
}

bool gui_project_anim_add_frames(int atlas_index, int anim_index, const char *const *frames, int count) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || !frames || count <= 0) {
        return false;
    }
    int added = 0;
    for (int i = 0; i < count; i++) {
        if (frames[i] && frames[i][0] && tp_project_anim_add_frame(an, frames[i]) == TP_STATUS_OK) {
            added++;
        }
    }
    if (added == 0) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}

bool gui_project_anim_remove_frame(int atlas_index, int anim_index, int frame_index) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || tp_project_anim_remove_frame(an, frame_index) != TP_STATUS_OK) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}

bool gui_project_anim_move_frame(int atlas_index, int anim_index, int frame_index, int delta) {
    tp_project_anim *an = anim_at(atlas_index, anim_index);
    if (!an || tp_project_anim_move_frame(an, frame_index, delta) != TP_STATUS_OK) {
        return false;
    }
    gui_project_touch(GUI_ACT_ANIM_FRAMES);
    return true;
}
// #endregion

// #region undo / redo
bool gui_project_can_undo(void) { return gui_history_can_undo(); }
bool gui_project_can_redo(void) { return gui_history_can_redo(); }

/* Loads `buf` (owned; adopted as the new last snapshot) into the live model. The file
 * path is invariant across undo/redo, so project_dir is carried over from the old model
 * (tp_project_load_buffer leaves it NULL). */
static bool restore_from_buffer(char *buf, size_t len) {
    tp_project *np = NULL;
    tp_error e = {0};
    if (tp_project_load_buffer(buf, len, &np, &e) != TP_STATUS_OK) {
        return false;
    }
    char *dir = (s_proj && s_proj->project_dir) ? dupstr(s_proj->project_dir) : NULL;
    tp_project_destroy(s_proj);
    np->project_dir = dir;
    s_proj = np;

    free(s_last_buf);
    s_last_buf = buf; /* the restored bytes ARE the current serialization */
    s_last_len = len;

    recompute_dirty();
    s_preview_stale = true; /* restored model != last-packed; since packing is blocked, always stale */
    gui_scan_invalidate_all();
    return true;
}

bool gui_project_undo(void) {
    char *out = NULL;
    size_t olen = 0;
    if (!gui_history_undo(s_last_buf, s_last_len, &out, &olen)) {
        return false;
    }
    if (!restore_from_buffer(out, olen)) {
        free(out);
        return false;
    }
    return true;
}

bool gui_project_redo(void) {
    char *out = NULL;
    size_t olen = 0;
    if (!gui_history_redo(s_last_buf, s_last_len, &out, &olen)) {
        return false;
    }
    if (!restore_from_buffer(out, olen)) {
        free(out);
        return false;
    }
    return true;
}
// #endregion

// #region file operations
void gui_project_new(void) {
    tp_project *fresh = tp_project_create();
    if (!fresh) {
        return;
    }
    seed_default_target(fresh, 0); /* fresh GUI project exports something (I1) */
    tp_project_destroy(s_proj);
    s_proj = fresh;
    set_path("");
    s_project_dirty = false;
    s_preview_stale = false;
    gui_scan_invalidate_all();
    gui_history_reset();
    set_last_from_current();
    set_saved_baseline();
}

tp_status gui_project_open(const char *path, char *err_out, size_t err_cap) {
    tp_error err = {0};
    tp_project *loaded = NULL;
    tp_status st = tp_project_load(path, &loaded, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    tp_project_destroy(s_proj);
    s_proj = loaded;
    set_path(path);
    s_project_dirty = false;
    s_preview_stale = true; /* nothing packed this session yet */
    gui_scan_invalidate_all();
    gui_history_reset();
    set_last_from_current();
    set_saved_baseline();
    return TP_STATUS_OK;
}

tp_status gui_project_save(char *err_out, size_t err_cap) {
    if (s_path[0] == '\0') {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "no path (use Save As)");
        }
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return gui_project_save_as(s_path, err_out, err_cap);
}

tp_status gui_project_save_as(const char *path, char *err_out, size_t err_cap) {
    tp_error err = {0};
    /* Promote to final random ids BEFORE writing: on OS-RNG failure promote returns
     * RNG_FAILED with every id left nil, so persisting now would write a nil-id file
     * that fails on reload. Fail closed -- report and return WITHOUT saving. */
    tp_status ids = ensure_ids(&err);
    if (ids != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(ids));
        }
        return ids;
    }
    tp_status st = tp_project_save(s_proj, path, &err);
    if (st != TP_STATUS_OK) {
        if (err_out && err_cap) {
            (void)snprintf(err_out, err_cap, "%s", err.msg[0] ? err.msg : tp_status_str(st));
        }
        return st;
    }
    set_path(path);
    s_project_dirty = false;
    /* Save may have relativized absolute sources -> re-snapshot the on-disk form and
     * adopt it as the saved baseline (undo history is preserved). */
    set_last_from_current();
    set_saved_baseline();
    return TP_STATUS_OK;
}
// #endregion
