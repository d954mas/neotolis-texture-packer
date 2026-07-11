/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). Split out of main.c
 * (GUI decomposition step 2) as a pure move -- definitions relocated verbatim, no behavior change.
 * This TU is Clay-free AND nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"

#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_history.h"
#include "tinyfiledialogs.h"

#include "app/nt_app.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* deferred side effects (dialogs + model mutations), applied at the top of the next frame */
bool s_pending_open, s_pending_save, s_pending_save_as, s_pending_add_files, s_pending_add_folder, s_pending_add_atlas, s_pending_refresh;
bool s_pending_pack, s_pending_export;
bool s_pending_commit_edit; /* a press landed outside the active inline-edit field -> commit it */
bool s_pending_add_anim;    /* "+ Animation" -> append empty animation, select it */
bool s_pending_create_anim; /* "Create animation from selection" */
bool s_pending_open_preview;/* open the anim preview player on s_ctx_anim / s_sel_anim */
int s_pending_remove_atlas = -1;
int s_pending_remove_source = -1;
int s_pending_remove_anim = -1; /* animation index to remove */
bool s_pending_add_target;
int s_pending_remove_target = -1;
int s_pending_browse_target = -1;        /* target whose out-path "..." dialog is queued */
int s_pending_export_browse_atlas = -1;  /* Export dialog: atlas of the queued out-path browse */
int s_pending_export_browse_target = -1; /* Export dialog: target index of the queued browse */
int s_after_confirm;
bool s_confirm_open;
int s_modal_action;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
int s_last_pack_atlas = -1; /* which atlas that timing belongs to */

// #region selection / edit helpers
/* Stops the animation preview player and restores the canvas to its atlas/source view. */
void preview_stop(void) {
    s_preview_active = false;
    s_preview_playing = false;
    s_preview_finished = false;
    s_preview_time = 0.0;
    s_canvas.anim_sprite = -1;
    if (gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_ANIM) {
        s_canvas.mode = gui_canvas_has_atlas(&s_canvas) ? GUI_CANVAS_ATLAS : GUI_CANVAS_SOURCE;
    }
}

void reset_selection(void) {
    s_sel_src = -1;
    s_sel_child = -1;
    s_sel_abs[0] = '\0';
    s_sel_missing = false;
    multi_sel_clear();
    s_sel_anchor_row = -1;
    s_sel_anim = -1;
    s_sel_anim_frame = -1;
    preview_stop();
}

void cancel_edit(void) {
    s_edit_kind = EDIT_NONE;
    s_edit_atlas = -1;
    s_edit_sprite[0] = '\0';
    s_edit_buf[0] = '\0';
}

/* --- start-edit entry points: the entry side of the same edit lifecycle as the inline-rename
 * commits below (commit_atlas_rename/commit_sprite_rename/commit_anim_rename). Moved here in step 4
 * (they are Clay-free) so gui_view_lists and gui_view_settings -- both of which start edits -- share
 * one home instead of step 5 re-deciding. --- */
void start_atlas_edit(int i) {
    tp_project *p = gui_project_get();
    if (!p || i < 0 || i >= p->atlas_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ATLAS;
    s_edit_atlas = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", p->atlases[i].name);
    set_status("Rename atlas: type, Enter to commit, Esc to cancel.");
}
void start_anim_edit(int i) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || i < 0 || i >= a->animation_count) {
        return;
    }
    cancel_edit();
    s_edit_kind = EDIT_ANIM;
    s_edit_anim = i;
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", a->animations[i].id);
    set_status("Rename animation: type, Enter to commit, Esc to cancel.");
}
void start_sprite_edit_named(const char *sprite_name) {
    if (!sprite_name || sprite_name[0] == '\0') {
        return;
    }
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    cancel_edit();
    s_edit_kind = EDIT_SPRITE;
    (void)snprintf(s_edit_sprite, sizeof s_edit_sprite, "%s", sprite_name);
    const tp_project_sprite *ov = a ? tp_project_atlas_find_sprite(a, sprite_name) : NULL;
    /* Seed with the CURRENT name: the rename override if set, else the file-derived final name
     * (sprite_name is the ext-stripped atlas-relative key = the default export name). The input
     * string is game-owned (nt_ui_input edits s_edit_buf in place), so seeding it here is the fix
     * for the "field opens empty" bug -- previously it seeded the (empty) override. */
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", (ov && ov->rename) ? ov->rename : sprite_name);
    set_status("Rename region: type, Enter to commit, Esc clears/cancels.");
}
void start_sprite_edit(const sprite_row *row) {
    if (!row || row->is_folder || row->missing || row->sprite_name[0] == '\0') {
        return;
    }
    start_sprite_edit_named(row->sprite_name);
}

/* Atlas-name validation (F1): non-empty, unique among atlases, and normalization-safe
 * (no path separators, not dots-only). Fills `err` on failure. */
static bool atlas_name_valid(const char *name, int self_idx, char *err, size_t cap) {
    if (!name || name[0] == '\0') {
        (void)snprintf(err, cap, "Atlas name cannot be empty");
        return false;
    }
    bool only_dots = true;
    for (const char *c = name; *c; c++) {
        if (*c == '/' || *c == '\\') {
            (void)snprintf(err, cap, "Atlas name cannot contain / or \\");
            return false;
        }
        if (*c != '.') {
            only_dots = false;
        }
    }
    if (only_dots) {
        (void)snprintf(err, cap, "Atlas name cannot be dots-only");
        return false;
    }
    tp_project *p = gui_project_get();
    for (int i = 0; p && i < p->atlas_count; i++) {
        if (i != self_idx && strcmp(p->atlases[i].name, name) == 0) {
            (void)snprintf(err, cap, "Atlas '%s' already exists", name);
            return false;
        }
    }
    return true;
}

void clamp_selection(void) {
    tp_project *p = gui_project_get();
    if (!p || p->atlas_count == 0) {
        s_sel_atlas = 0;
        reset_selection();
        return;
    }
    if (s_sel_atlas >= p->atlas_count) {
        s_sel_atlas = p->atlas_count - 1;
    }
    if (s_sel_atlas < 0) {
        s_sel_atlas = 0;
    }
}
// #endregion

// #region animation + preview actions (ux.md §3.7b)
/* The selected animation of the selected atlas, or NULL. */
tp_project_anim *current_anim(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || s_sel_anim < 0 || s_sel_anim >= a->animation_count) {
        return NULL;
    }
    return &a->animations[s_sel_anim];
}

/* Copies the multi-selection into the shared buffers, natural-sorted; returns the count. */
static int build_sorted_selection(void) {
    const int n = s_multi_sel_count;
    for (int i = 0; i < n; i++) {
        (void)snprintf(s_sel_sort_buf[i], sizeof s_sel_sort_buf[0], "%s", s_multi_sel[i]);
    }
    qsort(s_sel_sort_buf, (size_t)n, sizeof s_sel_sort_buf[0], nat_cmp_qsort);
    for (int i = 0; i < n; i++) {
        s_sel_sort_ptr[i] = s_sel_sort_buf[i];
    }
    return n;
}

/* Creates an animation from the current multi-selection: frames natural-sorted, id from the common
 * prefix (auto "animN" when there is none). Selects the new animation (opens its editor). */
int create_animation_from_selection(void) {
    if (s_multi_sel_count <= 0) {
        return -1;
    }
    const int n = build_sorted_selection();
    char base[192];
    names_common_prefix(s_sel_sort_buf, n, base, sizeof base);
    const int idx = gui_project_create_animation(s_sel_atlas, base[0] ? base : NULL, s_sel_sort_ptr, n);
    if (idx >= 0) {
        s_sel_anim = idx;
        s_sel_anim_frame = -1;
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        set_statusf("Created animation '%s' with %d frame(s) (Ctrl+Z to undo).", a->animations[idx].id, n);
    }
    return idx;
}

/* Appends the current multi-selection (natural-sorted) as frames of animation `anim_index`. */
void add_selection_frames_to_anim(int anim_index) {
    if (s_multi_sel_count <= 0) {
        return;
    }
    const int n = build_sorted_selection();
    if (gui_project_anim_add_frames(s_sel_atlas, anim_index, s_sel_sort_ptr, n)) {
        set_statusf("Added %d frame(s) to the animation (Ctrl+Z to undo).", n);
    }
}

/* Opens the preview player on animation `anim_index` (plays from the packed regions; if the atlas is
 * not packed yet, the canvas shows a "Pack to preview" hint). */
void open_preview(int anim_index) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || anim_index < 0 || anim_index >= a->animation_count) {
        return;
    }
    cancel_edit();
    s_sel_anim = anim_index;
    s_preview_active = true;
    s_preview_playing = true;
    s_preview_finished = false;
    s_preview_time = 0.0;
    if (!gui_pack_result(s_sel_atlas)) {
        set_status("Pack (Ctrl+P) to preview the animation on packed regions.");
    } else {
        set_statusf("Previewing '%s' \xE2\x80\x94 Space play/pause.", a->animations[anim_index].id);
    }
}

void preview_toggle_play(void) {
    if (!s_preview_active) {
        return;
    }
    if (s_preview_playing) {
        s_preview_playing = false;
    } else {
        if (s_preview_finished) {
            s_preview_time = 0.0;
            s_preview_finished = false;
        }
        s_preview_playing = true;
    }
}

/* Nudges the preview timeline by `delta` frame-ticks (pauses first). */
void preview_step(int delta) {
    if (!s_preview_active) {
        return;
    }
    const tp_project_anim *an = current_anim();
    const float fps = (an && an->fps >= 1.0F) ? an->fps : 1.0F;
    s_preview_playing = false;
    s_preview_finished = false;
    long step = (long)floor(s_preview_time * (double)fps);
    step += delta;
    if (step < 0) {
        step = 0;
    }
    s_preview_time = ((double)step + 0.5) / (double)fps;
}

/* Resolves the selected animation's frames to packed regions and pushes the current frame to the
 * canvas each frame. Advances the clock while playing. No-op (leaves the hint) without a pack result. */
void update_preview(void) {
    if (!s_preview_active) {
        return;
    }
    tp_project_anim *an = current_anim();
    const tp_result *pr = gui_pack_result(s_sel_atlas);
    s_canvas.anim_sprite = -1;
    s_preview_frame_count = 0;
    if (!an || !pr) {
        return; /* declare_canvas draws the "Pack to preview" hint */
    }
    static int idxs[512];
    int n = 0;
    int rw = 1;
    int rh = 1;
    for (int i = 0; i < an->frame_count && n < 512; i++) {
        const int si = gui_pack_find_sprite(s_sel_atlas, an->frames[i]);
        if (si >= 0 && si < pr->sprite_count) {
            idxs[n++] = si;
            if (pr->sprites[si].sourceSize.w > rw) {
                rw = pr->sprites[si].sourceSize.w;
            }
            if (pr->sprites[si].sourceSize.h > rh) {
                rh = pr->sprites[si].sourceSize.h;
            }
        }
    }
    s_preview_frame_count = n;
    if (n == 0) {
        return;
    }
    const float fps = (an->fps >= 1.0F) ? an->fps : 1.0F;
    if (s_preview_playing) {
        s_preview_time += (double)g_nt_app.dt;
    }
    bool finished = false;
    int cur = gui_canvas_anim_frame_at(s_preview_time, fps, an->playback, n, &finished);
    if (finished && s_preview_playing) {
        s_preview_playing = false;
    }
    s_preview_finished = finished;
    if (cur < 0) {
        cur = 0;
    }
    if (cur >= n) {
        cur = n - 1;
    }
    s_preview_cur = cur;
    s_canvas.mode = GUI_CANVAS_ANIM;
    s_canvas.anim_sprite = idxs[cur];
    s_canvas.anim_ref_w = rw;
    s_canvas.anim_ref_h = rh;
    s_canvas.anim_flip_h = an->flip_h;
    s_canvas.anim_flip_v = an->flip_v;
}
// #endregion

// #region file dialogs (tinyfiledialogs)
static void ensure_project_ext(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    const char *base = path_last(out);
    if (strrchr(base, '.') == NULL) {
        size_t len = strlen(out);
        (void)snprintf(out + len, cap - len, ".ntpacker_project");
    }
}

static void do_open(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *path = tinyfd_openFileDialog("Open Project", "", 1, filt, "ntpacker project", 0);
    if (!path) {
        return;
    }
    if (!gui_scan_exists(path)) {
        set_statusf_ex(STATUS_WARNING, "project not found: %s", path); /* never fatal (F6b) */
        return;
    }
    char err[256];
    if (gui_project_open(path, err, sizeof err) == TP_STATUS_OK) {
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_statusf("Opened %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Open failed: %s", err);
    }
}

static void do_save_as(void) {
    static const char *filt[] = {"*.ntpacker_project"};
    const char *def = gui_project_has_path() ? gui_project_path() : "untitled.ntpacker_project";
    const char *path = tinyfd_saveFileDialog("Save Project As", def, 1, filt, "ntpacker project");
    if (!path) {
        return;
    }
    char full[600];
    ensure_project_ext(path, full, sizeof full);
    char err[256];
    if (gui_project_save_as(full, err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

static void do_save(void) {
    if (!gui_project_has_path()) {
        do_save_as();
        return;
    }
    char err[256];
    if (gui_project_save(err, sizeof err) == TP_STATUS_OK) {
        set_statusf("Saved %s", gui_project_display_name());
    } else {
        set_statusf_ex(STATUS_ERROR, "Save failed: %s", err);
    }
}

static void do_add_files(void) {
    static const char *filt[] = {"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"};
    const char *res = tinyfd_openFileDialog("Add Image Files", "", 5, filt, "image files", 1);
    if (!res) {
        return;
    }
    char buf[8192];
    (void)snprintf(buf, sizeof buf, "%s", res);
    int added = 0;
    int dup = 0;
    char *start = buf;
    for (;;) {
        char *bar = strchr(start, '|');
        if (bar) {
            *bar = '\0';
        }
        if (start[0] != '\0') {
            normalize_slashes(start);
            const gui_add_status r = gui_project_add_source(s_sel_atlas, start);
            if (r == GUI_ADD_ADDED) {
                added++;
            } else if (r == GUI_ADD_DUPLICATE) {
                dup++;
            }
        }
        if (!bar) {
            break;
        }
        start = bar + 1;
    }
    if (dup > 0) {
        set_statusf("Added %d file source(s); %d already added", added, dup);
    } else {
        set_statusf("Added %d file source(s)", added);
    }
}

/* Best-effort relativize `abs` against the project dir (targets travel like sources).
 * Absolute paths outside the project dir are kept as-is (usable, save leaves them). */
static void relativize_to_project(const char *abs, char *out, size_t cap) {
    tp_project *p = gui_project_get();
    const char *dir = p ? p->project_dir : NULL;
    if (dir && dir[0] != '\0') {
        const size_t dl = strlen(dir);
        if (strncmp(abs, dir, dl) == 0 && (abs[dl] == '/' || abs[dl] == '\\')) {
            (void)snprintf(out, cap, "%s", abs + dl + 1);
            normalize_slashes(out);
            return;
        }
    }
    (void)snprintf(out, cap, "%s", abs);
    normalize_slashes(out);
}

/* Save dialog for a target's output path, relativized to the project like sources. Atlas-explicit so
 * the Export dialog (which spans all atlases) can browse any target, not just the selected atlas's. */
static void do_browse_target_at(int atlas, int ti) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), atlas);
    if (!a || ti < 0 || ti >= a->target_count) {
        return;
    }
    const tp_project_target *t = &a->targets[ti];
    const char *path = tinyfd_saveFileDialog("Export output path", t->out_path, 0, NULL, NULL);
    if (!path) {
        return;
    }
    char rel[600];
    relativize_to_project(path, rel, sizeof rel);
    if (gui_project_set_target(atlas, ti, t->exporter_id, rel, t->enabled)) {
        set_statusf("Output path: %s", rel);
    }
}
static void do_browse_target(int ti) { do_browse_target_at(s_sel_atlas, ti); }

static void do_add_folder(void) {
    const char *dir = tinyfd_selectFolderDialog("Add Folder", "");
    if (!dir) {
        return;
    }
    char norm[600];
    (void)snprintf(norm, sizeof norm, "%s", dir);
    normalize_slashes(norm);
    const gui_add_status r = gui_project_add_source(s_sel_atlas, norm);
    if (r == GUI_ADD_ADDED) {
        set_statusf("Added folder %s", path_last(norm));
    } else if (r == GUI_ADD_DUPLICATE) {
        set_statusf_ex(STATUS_WARNING, "already added: %s", path_last(norm));
    } else {
        set_status_ex(STATUS_ERROR, "Add folder failed.");
    }
}
// #endregion

// #region new/exit confirm flow
void request_new(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Wait for the pack/export to finish (or Cancel) first.");
        return;
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_NEW;
        s_confirm_open = true;
    } else {
        gui_project_new();
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    }
}
void request_exit(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Wait for the pack/export to finish (or Cancel) first.");
        return;
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_EXIT;
        s_confirm_open = true;
    } else {
        nt_app_quit();
    }
}
/* Open routes through the same unsaved-changes confirm as New/Exit (no silent discard). The actual
 * OS open dialog runs via s_pending_open, either now (clean) or after the modal resolves. */
void request_open(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Wait for the pack/export to finish (or Cancel) first.");
        return;
    }
    if (gui_project_is_dirty()) {
        s_after_confirm = AFTER_OPEN;
        s_confirm_open = true;
    } else {
        s_pending_open = true;
    }
}
static void confirm_perform(void) {
    if (s_after_confirm == AFTER_NEW) {
        gui_project_new();
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        set_status("New project.");
    } else if (s_after_confirm == AFTER_EXIT) {
        nt_app_quit();
    } else if (s_after_confirm == AFTER_OPEN) {
        s_pending_open = true; /* runs the open dialog next frame */
    }
    s_after_confirm = AFTER_NONE;
}
// #endregion

// #region undo/redo + refresh actions
void do_undo(void) {
    if (gui_project_undo()) {
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Undo (undo:%d redo:%d)", gui_history_undo_depth(), gui_history_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
void do_redo(void) {
    if (gui_project_redo()) {
        gui_pack_clear(-1);
        cancel_edit();
        clamp_selection();
        reset_selection();
        gui_canvas_invalidate(&s_canvas);
        set_statusf("Redo (undo:%d redo:%d)", gui_history_undo_depth(), gui_history_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

/* Fingerprint every source (folders expand to their scanned children, files stat
 * directly) so a Refresh can diff added/removed/changed. Missing entries carry
 * size==-1 so a vanish/restore reads as removed/added. */
typedef struct fp_entry {
    char abs[512];
    long long size;
    long long mtime;
} fp_entry;

static void fp_collect(fp_entry **arr, int *count, int *cap) {
    tp_project *p = gui_project_get();
    for (int ai = 0; p && ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        for (int si = 0; si < a->source_count; si++) {
            char abs[512];
            if (tp_project_resolve_path(p, a->sources[si], abs, sizeof abs) != TP_STATUS_OK) {
                continue;
            }
            if (gui_scan_is_dir(abs)) {
                const gui_scan_result *sc = gui_scan_get(abs);
                for (int ci = 0; ci < sc->count; ci++) {
                    if (*count == *cap) {
                        int nc = *cap ? *cap * 2 : 64;
                        fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                        if (!ne) {
                            return;
                        }
                        *arr = ne;
                        *cap = nc;
                    }
                    (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", sc->entries[ci].abs);
                    (*arr)[*count].size = sc->entries[ci].size;
                    (*arr)[*count].mtime = sc->entries[ci].mtime;
                    (*count)++;
                }
            } else {
                if (*count == *cap) {
                    int nc = *cap ? *cap * 2 : 64;
                    fp_entry *ne = (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
                    if (!ne) {
                        return;
                    }
                    *arr = ne;
                    *cap = nc;
                }
                long long sz = -1;
                long long mt = -1;
                (void)gui_scan_stat(abs, &sz, &mt);
                (void)snprintf((*arr)[*count].abs, sizeof (*arr)[0].abs, "%s", abs);
                (*arr)[*count].size = sz;
                (*arr)[*count].mtime = mt;
                (*count)++;
            }
        }
    }
}

static const fp_entry *fp_find(const fp_entry *arr, int n, const char *abs) {
    for (int i = 0; i < n; i++) {
        if (strcmp(arr[i].abs, abs) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

/* F4: rescan all sources, diff, evict the canvas cache, mark preview stale (NOT dirty). */
static void do_refresh(void) {
    fp_entry *before = NULL;
    int bn = 0;
    int bc = 0;
    fp_collect(&before, &bn, &bc);

    gui_scan_invalidate_all(); /* drop per-dir caches so fp_collect below rescans disk */

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    fp_collect(&after, &an, &ac);

    int added = 0;
    int removed = 0;
    int changed = 0;
    for (int i = 0; i < an; i++) {
        const fp_entry *b = fp_find(before, bn, after[i].abs);
        if (!b) {
            added++;
        } else if (b->size != after[i].size || b->mtime != after[i].mtime) {
            changed++;
        }
    }
    for (int i = 0; i < bn; i++) {
        if (!fp_find(after, an, before[i].abs)) {
            removed++;
        }
    }
    free(before);
    free(after);

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed, changed);
}

/* Ctrl+P / Pack: pack the selected atlas in-process (gui_pack -> tp_pack). On success clear the
 * preview-stale bit and upload the packed pages to the canvas (atlas-page view); on failure the
 * previous result + the "outdated" tag stay (ux.md §3.3b). */
/* Blocking pack of the selected atlas (deterministic path for the selftest + --shot). Interactive
 * Pack (do_pack) runs this on a worker thread; the result lands via poll_async. */
void do_pack_blocking(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
        return;
    }
    char err[256] = {0};
    char note[128] = {0};
    double ms = 0.0;
    if (gui_pack_atlas(s_sel_atlas, &ms, err, sizeof err, note, sizeof note)) {
        gui_project_mark_packed(); /* clears preview_stale for the current model */
        s_last_pack_ms = ms;
        s_last_pack_atlas = s_sel_atlas;
        /* the per-frame canvas<->atlas sync (frame()) picks up the new result pointer and uploads. */
        const tp_result *r = gui_pack_result(s_sel_atlas);
        const double shown_ms = s_status_fixed_time ? 0.0 : ms;
        if (note[0] != '\0') {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)", r->sprite_count, r->page_count, shown_ms, note);
        } else {
            set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms", r->sprite_count, r->page_count, shown_ms);
        }
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Interactive Pack (Ctrl+P / strip / stale chip): starts the pack on a worker thread so the window
 * never freezes. Completion is applied at a frame boundary by poll_async (apply_pending). */
void do_pack(void) {
    tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
    if (!a || a->source_count == 0) {
        set_status_ex(STATUS_WARNING, "No sources to pack -- add a smart folder or files first.");
        return;
    }
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_async_start(s_sel_atlas, err, sizeof err)) {
        set_status_ex(STATUS_INFO, "Packing\xE2\x80\xA6"); /* result lands via poll_async */
    } else {
        set_statusf_ex(STATUS_ERROR, "Pack failed: %s", err);
    }
}

/* Ctrl+E / Export: starts an async export of every atlas with sources + >=1 enabled target on a
 * worker thread (per-atlas failures non-fatal, ux.md §3.5). Completion reported via poll_async. */
static void do_export(void) {
    if (gui_pack_async_busy()) {
        set_status_ex(STATUS_WARNING, "Busy -- a pack or export is already running.");
        return;
    }
    char err[256] = {0};
    if (gui_pack_export_async_start(err, sizeof err)) {
        set_status_ex(STATUS_INFO, "Exporting\xE2\x80\xA6"); /* progress + result via poll_async */
    } else {
        set_status_ex(STATUS_WARNING, err);
    }
}

/* Lands a finished async pack/export at a frame boundary: the pack slot swap is done inside
 * gui_pack_poll; here we recompute stale honestly (mark_packed only when the model still matches
 * the packed snapshot) and route the outcome through the severity status. Called from apply_pending. */
static void poll_async(void) {
    gui_pack_result_info info;
    switch (gui_pack_poll(&info)) {
        case GUI_PACK_DONE_PACK_OK: {
            if (!info.model_changed) {
                gui_project_mark_packed(); /* model unchanged since the packed snapshot -> up to date */
            }
            s_last_pack_ms = info.ms;
            s_last_pack_atlas = info.atlas_index;
            const tp_result *r = gui_pack_result(info.atlas_index);
            const char *stale = info.model_changed ? " -- model changed, stale" : "";
            if (r && info.note[0] != '\0') {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms (%s)%s", r->sprite_count, r->page_count, info.ms, info.note, stale);
            } else if (r) {
                set_statusf_ex(STATUS_SUCCESS, "Packed %d sprites, %d page(s) in %.0f ms%s", r->sprite_count, r->page_count, info.ms, stale);
            }
            break;
        }
        case GUI_PACK_DONE_PACK_FAIL:
            set_statusf_ex(STATUS_ERROR, "Pack failed: %s", info.err);
            break;
        case GUI_PACK_DONE_PACK_CANCELLED:
            set_status_ex(STATUS_INFO, "Pack cancelled.");
            break;
        case GUI_PACK_DONE_EXPORT_OK:
            set_statusf_ex(info.notices > 0 ? STATUS_WARNING : STATUS_SUCCESS, "Exported %d target(s)%s", info.targets,
                           info.notices > 0 ? " (metadata notices raised)" : "");
            break;
        case GUI_PACK_DONE_EXPORT_FAIL:
            set_statusf_ex(STATUS_ERROR, "Exported %d target(s); %d atlas(es) failed -- %s", info.targets, info.atlases_fail, info.err);
            break;
        case GUI_PACK_DONE_EXPORT_CANCELLED:
            set_status_ex(STATUS_INFO, "Export cancelled.");
            break;
        case GUI_PACK_DONE_NONE:
        default:
            break;
    }
}
// #endregion

// #region inline rename commit
void commit_atlas_rename(void) {
    char err[128];
    if (!atlas_name_valid(s_edit_buf, s_edit_atlas, err, sizeof err)) {
        set_status_ex(STATUS_WARNING, err); /* keep editing on invalid input */
        return;
    }
    if (gui_project_set_atlas_name(s_edit_atlas, s_edit_buf)) {
        set_statusf("Renamed atlas to '%s'", s_edit_buf);
    }
    cancel_edit();
}
void commit_sprite_rename(void) {
    /* empty input clears the override back to the file-derived name */
    if (gui_project_set_sprite_rename(s_sel_atlas, s_edit_sprite, s_edit_buf)) {
        if (s_edit_buf[0] == '\0') {
            set_statusf("Cleared rename on '%s'", s_edit_sprite);
        } else {
            set_statusf("Renamed '%s' -> '%s'", s_edit_sprite, s_edit_buf);
        }
    }
    cancel_edit();
}
void commit_anim_rename(void) {
    if (s_edit_buf[0] == '\0') {
        set_status_ex(STATUS_WARNING, "Animation name cannot be empty.");
        return; /* keep editing */
    }
    if (gui_project_set_anim_id(s_sel_atlas, s_edit_anim, s_edit_buf)) {
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    } else {
        set_statusf_ex(STATUS_WARNING, "Animation '%s' already exists.", s_edit_buf); /* keep editing */
    }
}

/* Commit the active inline edit as if Enter was pressed (click-outside / model-change path).
 * `force` = the editor is being dismissed involuntarily: an invalid atlas name CANCELS instead of
 * keeping a zombie editor (the validation message stays in the status bar). Sprite rename never
 * zombies (empty clears the override). No-op when nothing is being edited. */
static void commit_active_edit(bool force) {
    if (s_edit_kind == EDIT_ATLAS) {
        char err[128];
        if (!atlas_name_valid(s_edit_buf, s_edit_atlas, err, sizeof err)) {
            set_status_ex(STATUS_WARNING, err);
            if (force) {
                cancel_edit();
            }
            return;
        }
        if (gui_project_set_atlas_name(s_edit_atlas, s_edit_buf)) {
            set_statusf("Renamed atlas to '%s'", s_edit_buf);
        }
        cancel_edit();
    } else if (s_edit_kind == EDIT_SPRITE) {
        commit_sprite_rename();
    } else if (s_edit_kind == EDIT_ANIM) {
        if (s_edit_buf[0] == '\0' || !gui_project_set_anim_id(s_sel_atlas, s_edit_anim, s_edit_buf)) {
            set_status_ex(STATUS_WARNING, s_edit_buf[0] == '\0' ? "Animation name cannot be empty." : "Animation name must be unique.");
            if (force) {
                cancel_edit();
            }
            return;
        }
        set_statusf("Renamed animation to '%s'", s_edit_buf);
        cancel_edit();
    }
}
// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
void apply_pending(void) {
    poll_async(); /* land any finished async pack/export before this frame's canvas pickup */

    /* A press landed outside the active inline editor last frame -> commit it (desktop rename UX).
     * Also fires before any pending model change (remove/refresh/open/new) so no orphaned editor
     * survives a mutation. */
    if (s_edit_kind != EDIT_NONE && s_pending_commit_edit) {
        commit_active_edit(true);
    }
    s_pending_commit_edit = false;

    if (s_modal_action == MODAL_SAVE) {
        do_save();
        s_confirm_open = false;
        if (!gui_project_is_dirty()) {
            confirm_perform();
        } else {
            s_after_confirm = AFTER_NONE; /* save cancelled -> abort the pending action */
        }
    } else if (s_modal_action == MODAL_DISCARD) {
        s_confirm_open = false;
        confirm_perform();
    } else if (s_modal_action == MODAL_CANCEL) {
        s_confirm_open = false;
        s_after_confirm = AFTER_NONE;
    }
    s_modal_action = MODAL_NONE;

    if (s_pending_open) {
        do_open();
    }
    if (s_pending_save) {
        do_save();
    }
    if (s_pending_save_as) {
        do_save_as();
    }
    if (s_pending_add_files) {
        do_add_files();
    }
    if (s_pending_add_folder) {
        do_add_folder();
    }
    if (s_pending_add_atlas) {
        int idx = gui_project_add_atlas();
        if (idx >= 0) {
            s_sel_atlas = idx;
            reset_selection();
            set_statusf("Added atlas '%s'", tp_project_get_atlas(gui_project_get(), idx)->name);
        }
    }
    if (s_pending_remove_source >= 0) {
        gui_project_remove_source(s_sel_atlas, s_pending_remove_source);
        reset_selection();
        set_status("Removed source (Ctrl+Z to undo).");
    }
    if (s_pending_remove_atlas >= 0) {
        gui_project_remove_atlas(s_pending_remove_atlas);
        clamp_selection();
        reset_selection();
        set_status("Removed atlas (Ctrl+Z to undo).");
    }
    if (s_pending_add_target) {
        const int ti = gui_project_add_target(s_sel_atlas);
        if (ti >= 0) {
            set_status("Added export target (Ctrl+Z to undo).");
        }
    }
    if (s_pending_remove_target >= 0) {
        gui_project_remove_target(s_sel_atlas, s_pending_remove_target);
        set_status("Removed export target (Ctrl+Z to undo).");
    }
    if (s_pending_export_browse_atlas >= 0 && s_pending_export_browse_target >= 0) {
        do_browse_target_at(s_pending_export_browse_atlas, s_pending_export_browse_target);
    }
    if (s_pending_browse_target >= 0) {
        do_browse_target(s_pending_browse_target);
    }
    if (s_pending_add_anim) {
        const int idx = gui_project_create_animation(s_sel_atlas, NULL, NULL, 0);
        if (idx >= 0) {
            s_sel_anim = idx;
            s_sel_anim_frame = -1;
            set_statusf("Added animation '%s' (Ctrl+Z to undo).",
                        tp_project_get_atlas(gui_project_get(), s_sel_atlas)->animations[idx].id);
        }
    }
    if (s_pending_create_anim) {
        (void)create_animation_from_selection();
    }
    if (s_pending_remove_anim >= 0) {
        tp_project_atlas *a = tp_project_get_atlas(gui_project_get(), s_sel_atlas);
        if (a && s_pending_remove_anim < a->animation_count) {
            if (s_preview_active && s_sel_anim == s_pending_remove_anim) {
                preview_stop();
            }
            gui_project_remove_animation(s_sel_atlas, a->animations[s_pending_remove_anim].id);
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            set_status("Removed animation (Ctrl+Z to undo).");
        }
    }
    if (s_pending_open_preview) {
        open_preview(s_sel_anim);
    }
    if (s_pending_refresh) {
        do_refresh();
    }
    if (s_pending_pack) {
        do_pack();
    }
    if (s_pending_export) {
        do_export();
    }

    s_pending_open = s_pending_save = s_pending_save_as = false;
    s_pending_add_files = s_pending_add_folder = s_pending_add_atlas = false;
    s_pending_refresh = s_pending_pack = s_pending_export = false;
    s_pending_add_target = false;
    s_pending_add_anim = s_pending_create_anim = s_pending_open_preview = false;
    s_pending_remove_source = -1;
    s_pending_remove_atlas = -1;
    s_pending_remove_target = -1;
    s_pending_export_browse_atlas = -1;
    s_pending_export_browse_target = -1;
    s_pending_remove_anim = -1;
    s_pending_browse_target = -1;
}
// #endregion
