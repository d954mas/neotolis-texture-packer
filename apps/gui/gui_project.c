#include "gui_project.h"

#include <stdio.h>
#include <string.h>

#include "gui_scan.h"

// #region state
static tp_project *s_proj;
static char s_path[1024]; /* absolute file path; "" while unsaved */
static bool s_project_dirty;
static bool s_preview_stale;
static char s_name[256]; /* cached basename for the menu bar */
// #endregion

// #region helpers
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
}

void gui_project_shutdown(void) {
    tp_project_destroy(s_proj);
    s_proj = NULL;
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
void gui_project_touch(void) {
    s_project_dirty = true;
    s_preview_stale = true;
}
void gui_project_mark_packed(void) { s_preview_stale = false; }
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
    gui_project_touch();
    return idx;
}

void gui_project_remove_atlas(int index) {
    if (!s_proj) {
        return;
    }
    if (tp_project_remove_atlas(s_proj, index) == TP_STATUS_OK) {
        gui_scan_invalidate_all();
        gui_project_touch();
    }
}

bool gui_project_add_source(int atlas_index, const char *path) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a || !path || path[0] == '\0') {
        return false;
    }
    if (tp_project_atlas_add_source(a, path) != TP_STATUS_OK) {
        return false;
    }
    gui_scan_invalidate_all();
    gui_project_touch();
    return true;
}

void gui_project_remove_source(int atlas_index, int source_index) {
    tp_project_atlas *a = tp_project_get_atlas(s_proj, atlas_index);
    if (!a) {
        return;
    }
    if (tp_project_atlas_remove_source(a, source_index) == TP_STATUS_OK) {
        gui_scan_invalidate_all();
        gui_project_touch();
    }
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
    return TP_STATUS_OK;
}
// #endregion
