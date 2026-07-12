/* Row model + selection-set helpers for the ntpacker GUI (see gui_rows.h). Split out of main.c
 * (GUI decomposition step 2) as a pure move -- definitions relocated verbatim, no behavior change. */

#include "gui_rows.h"

#include "gui_state.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_pack.h"

#include "tp_core/tp_names.h" /* canonical key / natural order / common prefix (op layer) */

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Growable-storage policy (P1 fix, decomposition step 7) shared by the multi-select set, the
 * selection-sort companions, and the sprite-row array. These used to be fixed 4096/512-cap arrays
 * that silently DROPPED entries past the cap (sprites packed fine but vanished from the UI). Now:
 * realloc-keep-capacity -- grow geometrically (x2) on a new high-water mark, NEVER shrink, so the
 * per-frame producers (build_rows, the preview loop) never malloc in the steady state. Failure
 * policy: on realloc OOM we KEEP the old capacity and raise a STATUS_ERROR ("... truncated") -- the
 * truncation becomes LOUD, never silent, and never a crash/out-of-bounds write. The buffers are
 * process-lifetime singletons (grow-only, always reachable from these globals -- no leak; matches
 * the old BSS arrays, which were likewise never freed). */
#define MULTI_SEL_INIT_CAP 64
#define SEL_SORT_INIT_CAP 64
#define ROWS_INIT_CAP 256

// #region shared path/name string helpers
void normalize_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

const char *path_last(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

// #endregion

// #region multi-select + natural sort (ux.md §3.7b selection gesture)
bool multi_sel_contains(const char *name) {
    if (!name || !name[0]) {
        return false;
    }
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (strcmp(s_multi_sel[i], name) == 0) {
            return true;
        }
    }
    return false;
}
void multi_sel_clear(void) { s_multi_sel_count = 0; }
void multi_sel_add(const char *name) {
    if (!name || !name[0] || multi_sel_contains(name)) {
        return;
    }
    if (s_multi_sel_count >= s_multi_sel_cap) {
        const int newcap = s_multi_sel_cap ? s_multi_sel_cap * 2 : MULTI_SEL_INIT_CAP;
        char (*grown)[192] = realloc(s_multi_sel, (size_t)newcap * sizeof *s_multi_sel);
        if (!grown) {
            set_status_ex(STATUS_ERROR, "Out of memory: selection not fully updated.");
            return; /* keep old capacity + the entries already in it; loud, never silent */
        }
        s_multi_sel = grown;
        s_multi_sel_cap = newcap;
    }
    (void)snprintf(s_multi_sel[s_multi_sel_count], sizeof s_multi_sel[0], "%s", name);
    s_multi_sel_count++;
}
void multi_sel_remove(const char *name) {
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (strcmp(s_multi_sel[i], name) == 0) {
            for (int j = i; j < s_multi_sel_count - 1; j++) {
                memcpy(s_multi_sel[j], s_multi_sel[j + 1], sizeof s_multi_sel[0]);
            }
            s_multi_sel_count--;
            return;
        }
    }
}
void multi_sel_set_single(const char *name) {
    multi_sel_clear();
    multi_sel_add(name);
}

/* qsort adapter over the core natural-order comparator (tp_names). */
int nat_cmp_qsort(const void *a, const void *b) { return tp_nat_cmp((const char *)a, (const char *)b); }

/* Shared scratch for the selection-gesture sort (heap; grows WITH the multi-select set so the sort
 * path can never truncate the selection -- P1 fix, step 7). */
char (*s_sel_sort_buf)[192];
const char **s_sel_sort_ptr;
static int s_sel_sort_cap;

bool sel_sort_reserve(int n) {
    if (n <= s_sel_sort_cap) {
        return true;
    }
    int newcap = s_sel_sort_cap ? s_sel_sort_cap : SEL_SORT_INIT_CAP;
    while (newcap < n) {
        newcap *= 2;
    }
    char (*gb)[192] = realloc(s_sel_sort_buf, (size_t)newcap * sizeof *s_sel_sort_buf);
    if (!gb) {
        return false; /* keep old capacity (s_sel_sort_cap unchanged) */
    }
    s_sel_sort_buf = gb;
    const char **gp = realloc(s_sel_sort_ptr, (size_t)newcap * sizeof *s_sel_sort_ptr);
    if (!gp) {
        return false; /* buf physically grew but cap not bumped -- next call retries both, stays consistent */
    }
    s_sel_sort_ptr = gp;
    s_sel_sort_cap = newcap;
    return true;
}
// #endregion

// #region row model
/* Rename-aware display label: a renamed sprite shows "final (file.png)" so the mapping
 * stays visible; otherwise the file-derived base label. */
static void row_display(tp_project_atlas *a, const char *sprite_name, const char *base_label, const char *paren, char *out, size_t cap) {
    const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, sprite_name);
    if (ov && ov->rename) {
        (void)snprintf(out, cap, "%s (%s)", ov->rename, paren);
    } else {
        (void)snprintf(out, cap, "%s", base_label);
    }
}

sprite_row *s_rows;
int s_row_count;
static int s_rows_cap;

/* Appends one zero-uninitialized row slot and returns it (caller memsets), growing s_rows on a new
 * high-water mark. Returns NULL on OOM (old capacity kept) -- build_rows then stops + raises status.
 * NOTE on realloc-move: a prior `sprite_row *` into s_rows is invalidated by a growth here, so
 * build_rows finishes writing the parent folder row BEFORE pushing any child (see below). */
static sprite_row *rows_push(void) {
    if (s_row_count >= s_rows_cap) {
        const int newcap = s_rows_cap ? s_rows_cap * 2 : ROWS_INIT_CAP;
        sprite_row *grown = realloc(s_rows, (size_t)newcap * sizeof *s_rows);
        if (!grown) {
            return NULL;
        }
        s_rows = grown;
        s_rows_cap = newcap;
    }
    return &s_rows[s_row_count++];
}

void build_rows(tp_project *proj, tp_project_atlas *a) {
    s_row_count = 0;
    if (!a) {
        return;
    }
    for (int si = 0; si < a->source_count; si++) {
        const char *sp = a->sources[si].path;
        char abs[512];
        if (tp_project_resolve_path(proj, sp, abs, sizeof abs) != TP_STATUS_OK) {
            abs[0] = '\0';
        }
        const bool exists = gui_scan_exists(abs);
        const bool is_dir = exists && gui_scan_is_dir(abs);
        sprite_row *r = rows_push();
        if (!r) {
            set_status_ex(STATUS_ERROR, "Out of memory: sprite list truncated.");
            return;
        }
        memset(r, 0, sizeof *r);
        r->src = si;
        r->child = -1;
        r->is_source = true;
        r->is_folder = is_dir;
        r->indent = 0;
        if (!exists) { /* missing source: row stays, warning badge, selectable (§3.7) */
            r->missing = true;
            (void)snprintf(r->label, sizeof r->label, "\xE2\x9A\xA0 %s", path_last(sp)); /* U+26A0 warning */
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        } else if (is_dir) {
            const gui_scan_result *sc = gui_scan_get(abs);
            /* Smart-folder row: name + a child-count suffix (TexturePacker convention -- "animals/ · 60")
             * so the count of packed assets is visible at a glance without expanding. `r` is fully
             * written HERE, before the child loop -- a rows_push() below may realloc-move s_rows and
             * invalidate `r`, but it is never dereferenced again. */
            (void)snprintf(r->label, sizeof r->label, "%s/  \xC2\xB7  %d", path_last(sp), sc->count);
            r->abs[0] = '\0';
            for (int ci = 0; ci < sc->count; ci++) {
                sprite_row *cr = rows_push();
                if (!cr) {
                    set_status_ex(STATUS_ERROR, "Out of memory: sprite list truncated.");
                    return;
                }
                memset(cr, 0, sizeof *cr);
                cr->src = si;
                cr->child = ci;
                cr->is_source = false;
                cr->indent = 1;
                tp_sprite_export_key(sc->entries[ci].rel, cr->sprite_name, sizeof cr->sprite_name);
                row_display(a, cr->sprite_name, sc->entries[ci].rel, path_last(sc->entries[ci].rel), cr->label, sizeof cr->label);
                (void)snprintf(cr->abs, sizeof cr->abs, "%s", sc->entries[ci].abs);
            }
        } else { /* file source: a leaf sprite -- strip the folder (path_last) THEN key */
            tp_sprite_export_key(path_last(sp), r->sprite_name, sizeof r->sprite_name);
            row_display(a, r->sprite_name, r->sprite_name, path_last(sp), r->label, sizeof r->label);
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        }
    }
}
// #endregion

// #region canvas region -> row selection sync
/* Selects the sprite-tree row matching a canvas region (region -> row selection sync). The region's
 * raw name stripped of its extension == the row's override key. */
void select_row_for_region(int region_idx) {
    const tp_result *r = gui_pack_result(s_sel_atlas);
    if (!r || region_idx < 0 || region_idx >= r->sprite_count) {
        return;
    }
    char key[192];
    tp_sprite_export_key(r->sprites[region_idx].name, key, sizeof key);
    for (int i = 0; i < s_row_count; i++) {
        if (!s_rows[i].is_folder && s_rows[i].sprite_name[0] != '\0' && strcmp(s_rows[i].sprite_name, key) == 0) {
            s_sel_src = s_rows[i].src;
            s_sel_child = s_rows[i].child;
            s_sel_missing = false;
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", s_rows[i].abs);
            return;
        }
    }
}
// #endregion
