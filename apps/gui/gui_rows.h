#ifndef NTPACKER_GUI_ROWS_H
#define NTPACKER_GUI_ROWS_H

/* Row model + selection-set helpers for the ntpacker GUI: the flattened per-atlas sprite rows
 * (rebuilt each frame), the multi-select set over leaf sprite NAMES, natural-order sorting +
 * the sort scratch, the common-name-prefix helper, the canvas region -> row selection sync, and
 * the shared path/name string helpers. Split out of main.c (GUI decomposition step 2) as a pure
 * move -- no behavior change. Include discipline: rows -> gui_state + model headers only; it must
 * never include a sibling view/actions/widgets header. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_project.h" /* tp_project / tp_project_atlas (build_rows signature) */

#include "gui_state.h" /* shared editor state the row/selection helpers read + grow */

#ifdef __cplusplus
extern "C" {
#endif

/* --- shared path/name string helpers --- */
/* Rewrites backslashes to forward slashes in place (path normalization). */
void normalize_slashes(char *s);
/* Returns the basename (past the last / or \) of `p`. */
const char *path_last(const char *p);

/* --- multi-select set over leaf sprite NAMES (ux.md §3.7b selection gesture) --- */
bool multi_sel_contains(const char *name);
void multi_sel_clear(void);
void multi_sel_add(const char *name);
void multi_sel_remove(const char *name);
void multi_sel_set_single(const char *name);

/* qsort adapter for natural order (wraps tp_nat_cmp; digit runs compare numerically). */
int nat_cmp_qsort(const void *a, const void *b);
/* Shared scratch for the selection-gesture sort (filled by the animation ops in gui_actions). Growable
 * companions to the multi-select set (P1 fix, step 7): they MUST hold the whole selection or the sort
 * path would re-introduce the old truncation. sel_sort_reserve grows both to >= n (false == OOM, old
 * capacity kept); callers must reserve before writing. */
extern char (*s_sel_sort_buf)[192];
extern const char **s_sel_sort_ptr;
bool sel_sort_reserve(int n);

/* --- flattened sprite rows for the current atlas, rebuilt each frame (growable; P1 fix, step 7) --- */
typedef struct sprite_row {
    int src;
    int child;
    int indent;
    bool is_source;
    bool is_folder;
    bool missing;             /* source path gone from disk (§3.7) */
    char label[224];          /* display label (rename-aware: "final (file.png)") */
    char sprite_name[192];    /* atlas-relative override key ("" for folders / missing) */
    char abs[512];
} sprite_row;
extern sprite_row *s_rows;
extern int s_row_count;

/* Rebuilds s_rows/s_row_count for atlas `a` of project `proj` (folders expand to their children). */
void build_rows(tp_project *proj, tp_project_atlas *a);

#if defined(NTPACKER_GUI_BENCH)
typedef struct gui_rows_bench_counters {
    uint64_t row_realloc_calls;
    int row_capacity;
} gui_rows_bench_counters;

void gui_rows_bench_reset_counters(void);
gui_rows_bench_counters gui_rows_bench_get_counters(void);
void gui_rows_bench_shutdown(void);
#endif

/* Selects the sprite-tree row matching a packed-atlas region (region -> row selection sync). */
void select_row_for_region(int region_idx);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ROWS_H */
