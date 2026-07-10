#include "gui_project.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gui_history.h"
#include "gui_scan.h"

// #region state
static tp_project *s_proj;
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_project_dirty;
static bool s_preview_stale;
static char s_name[256]; /* cached basename for the menu bar */

/* Snapshots (serialized project bytes) for undo/dirty recompute (ux.md §3.3c). */
static char *s_last_buf;   /* the CURRENT model, serialized (the pre-mutation snapshot the next touch pushes) */
static size_t s_last_len;
static char *s_saved_buf;  /* the last-SAVED model, serialized (dirty baseline) */
static size_t s_saved_len;
static double s_now;       /* history clock (seconds), fed each frame */
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

static void set_last_from_current(void) {
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
// #endregion

// #region lifecycle
void gui_project_init(void) {
    if (s_proj) {
        return;
    }
    s_proj = tp_project_create();
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
    recompute_dirty();
}

void gui_project_mark_packed(void) { s_preview_stale = false; }
void gui_project_mark_stale(void) { s_preview_stale = true; }
void gui_project_tick(double now_seconds) { s_now = now_seconds; }
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

gui_add_status gui_project_add_source(int atlas_index, const char *path) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !path || path[0] == '\0') {
        return GUI_ADD_FAILED;
    }
    const int before = a->source_count;
    if (tp_project_atlas_add_source(a, path) != TP_STATUS_OK) {
        return GUI_ADD_FAILED;
    }
    if (a->source_count == before) {
        return GUI_ADD_DUPLICATE; /* tp_core dedupe no-op -- no touch, no dirty */
    }
    gui_scan_invalidate_all();
    gui_project_touch(GUI_ACT_ADD_SOURCE);
    return GUI_ADD_ADDED;
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
