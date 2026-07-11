/* Row model + selection-set helpers for the ntpacker GUI (see gui_rows.h). Split out of main.c
 * (GUI decomposition step 2) as a pure move -- definitions relocated verbatim, no behavior change. */

#include "gui_rows.h"

#include "gui_state.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_pack.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

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

static void path_stem(const char *p, char *buf, size_t cap) {
    (void)snprintf(buf, cap, "%s", path_last(p));
    char *dot = strrchr(buf, '.');
    if (dot && dot != buf) {
        *dot = '\0';
    }
}

/* Strip only a trailing extension on the basename, keeping any folder prefix (so a
 * folder child's override key is atlas-relative, e.g. "tank/walk_01"). */
void strip_ext(const char *in, char *out, size_t cap) {
    (void)snprintf(out, cap, "%s", in);
    char *dot = strrchr(out, '.');
    char *slash = strrchr(out, '/');
    if (dot && dot != out && (!slash || dot > slash)) {
        *dot = '\0';
    }
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
    if (!name || !name[0] || multi_sel_contains(name) || s_multi_sel_count >= MAX_MULTI_SEL) {
        return;
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

/* Natural order: digit runs compare numerically (walk_2 before walk_10), the rest byte-wise. */
static int nat_cmp(const char *a, const char *b) {
    while (*a && *b) {
        const bool da = (*a >= '0' && *a <= '9');
        const bool db = (*b >= '0' && *b <= '9');
        if (da && db) {
            while (*a == '0') {
                a++;
            }
            while (*b == '0') {
                b++;
            }
            const char *sa = a;
            const char *sb = b;
            while (*a >= '0' && *a <= '9') {
                a++;
            }
            while (*b >= '0' && *b <= '9') {
                b++;
            }
            const size_t la = (size_t)(a - sa);
            const size_t lb = (size_t)(b - sb);
            if (la != lb) {
                return (la < lb) ? -1 : 1;
            }
            const int c = strncmp(sa, sb, la);
            if (c != 0) {
                return c;
            }
        } else {
            if (*a != *b) {
                return ((unsigned char)*a < (unsigned char)*b) ? -1 : 1;
            }
            a++;
            b++;
        }
    }
    if (*a) {
        return 1;
    }
    if (*b) {
        return -1;
    }
    return 0;
}
int nat_cmp_qsort(const void *a, const void *b) { return nat_cmp((const char *)a, (const char *)b); }

/* Longest common prefix of `names`, trimmed of trailing digits/separators so walk_01/walk_02 -> "walk". */
void names_common_prefix(char names[][192], int count, char *out, size_t cap) {
    out[0] = '\0';
    if (count <= 0 || cap == 0) {
        return;
    }
    size_t pfx = strlen(names[0]);
    for (int i = 1; i < count; i++) {
        size_t k = 0;
        while (k < pfx && names[i][k] && names[0][k] == names[i][k]) {
            k++;
        }
        pfx = k;
    }
    while (pfx > 0) {
        const char c = names[0][pfx - 1];
        if ((c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == ' ' || c == '/') {
            pfx--;
        } else {
            break;
        }
    }
    if (pfx >= cap) {
        pfx = cap - 1;
    }
    memcpy(out, names[0], pfx);
    out[pfx] = '\0';
}

/* Shared scratch for the selection-gesture sort (frame lists are small; static avoids stack blow-up). */
char s_sel_sort_buf[MAX_MULTI_SEL][192];
const char *s_sel_sort_ptr[MAX_MULTI_SEL];
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

sprite_row s_rows[MAX_ROWS];
int s_row_count;

void build_rows(tp_project *proj, tp_project_atlas *a) {
    s_row_count = 0;
    if (!a) {
        return;
    }
    for (int si = 0; si < a->source_count && s_row_count < MAX_ROWS; si++) {
        const char *sp = a->sources[si];
        char abs[512];
        if (tp_project_resolve_path(proj, sp, abs, sizeof abs) != TP_STATUS_OK) {
            abs[0] = '\0';
        }
        const bool exists = gui_scan_exists(abs);
        const bool is_dir = exists && gui_scan_is_dir(abs);
        sprite_row *r = &s_rows[s_row_count++];
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
             * so the count of packed assets is visible at a glance without expanding. */
            (void)snprintf(r->label, sizeof r->label, "%s/  \xC2\xB7  %d", path_last(sp), sc->count);
            r->abs[0] = '\0';
            for (int ci = 0; ci < sc->count && s_row_count < MAX_ROWS; ci++) {
                sprite_row *cr = &s_rows[s_row_count++];
                memset(cr, 0, sizeof *cr);
                cr->src = si;
                cr->child = ci;
                cr->is_source = false;
                cr->indent = 1;
                strip_ext(sc->entries[ci].rel, cr->sprite_name, sizeof cr->sprite_name);
                row_display(a, cr->sprite_name, sc->entries[ci].rel, path_last(sc->entries[ci].rel), cr->label, sizeof cr->label);
                (void)snprintf(cr->abs, sizeof cr->abs, "%s", sc->entries[ci].abs);
            }
        } else { /* file source: a leaf sprite */
            char stem[192];
            path_stem(sp, stem, sizeof stem);
            (void)snprintf(r->sprite_name, sizeof r->sprite_name, "%s", stem);
            row_display(a, r->sprite_name, stem, path_last(sp), r->label, sizeof r->label);
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
    strip_ext(r->sprites[region_idx].name, key, sizeof key);
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
