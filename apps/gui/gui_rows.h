#ifndef NTPACKER_GUI_ROWS_H
#define NTPACKER_GUI_ROWS_H

/* Row model + selection-set helpers for the ntpacker GUI: the flattened per-atlas sprite rows
 * (rebuilt each frame), the canonical leaf-sprite multi-select, natural-order sorting +
 * the sort scratch, the common-name-prefix helper, the canvas region -> row selection sync, and
 * the shared path/name string helpers. Split out of main.c (GUI decomposition step 2) as a pure
 * move -- no behavior change. Include discipline: rows -> gui_state + model headers only; it must
 * never include a sibling view/actions/widgets header. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"

#include "gui_state.h" /* shared editor state the row/selection helpers read + grow */

#ifdef __cplusplus
extern "C" {
#endif

/* --- shared path/name string helpers --- */
/* Rewrites backslashes to forward slashes in place (path normalization). */
void normalize_slashes(char *s);
/* Returns the basename (past the last / or \) of `p`. */
const char *path_last(const char *p);

/* --- canonical multi-select set (ux.md §3.7b selection gesture) --- */
bool multi_sel_contains_ref(tp_id128 source_id, const char *source_key);
void multi_sel_clear(void);
void multi_sel_add_ref(tp_id128 source_id, const char *source_key);
void multi_sel_remove_ref(tp_id128 source_id, const char *source_key);
void multi_sel_set_single_ref(tp_id128 source_id, const char *source_key);

/* qsort adapter for natural order (wraps tp_nat_cmp; digit runs compare numerically). */
int nat_cmp_qsort(const void *a, const void *b);
/* Shared scratch for the selection-gesture sort (filled by the animation ops in gui_actions). Growable
 * companions to the multi-select set (P1 fix, step 7): they MUST hold the whole selection or the sort
 * path would re-introduce the old truncation. sel_sort_reserve grows both to >= n (false == OOM, old
 * capacity kept); callers must reserve before writing. */
extern gui_selected_sprite *s_sel_sort_buf;
extern const char **s_sel_sort_ptr;
extern tp_op_sprite_ref *s_sel_sort_refs;
bool sel_sort_reserve(int n);

/* --- flattened sprite rows for the current atlas (growable; P1 fix, step 7) --- */
typedef struct sprite_row {
    int src;
    int child;
    int indent;
    bool is_source;
    bool is_folder;
    tp_id128 source_id;
    char source_key[TP_SCAN_REL_CAP];
    bool missing;             /* source path gone from disk (§3.7) */
    char label[224];          /* display label (rename-aware: "final (file.png)") */
    char sprite_name[TP_SCAN_REL_CAP]; /* export key; "" for folders / missing */
    char abs[512];
} sprite_row;
extern sprite_row *s_rows;
extern int s_row_count;

/* Updates s_rows/s_row_count for the selected atlas (folders expand to their
 * children). The row model is cached by {atlas id, project model
 * generation, source scan generation}; an unchanged call only compares that
 * key and performs no allocation or filesystem work. */
void build_rows(void);

/* Cached selected leaf and its sparse project override. The first lookup after
 * a row rebuild or selection change is bounded by the row/index rebuild data;
 * unchanged frames perform no linear row/override scan. Returned pointers are
 * owned by the current row/snapshot caches. */
const sprite_row *gui_rows_selected_leaf(void);
const tp_snapshot_sprite *gui_rows_selected_override(void);

#if defined(NTPACKER_GUI_BENCH)
typedef struct gui_rows_bench_counters {
    uint64_t row_realloc_calls;
    uint64_t override_index_realloc_calls;
    uint64_t cache_key_checks;
    uint64_t rebuilds;
    uint64_t override_inserts;
    uint64_t override_lookup_calls;
    uint64_t override_probes;
    uint64_t source_iterations;
    uint64_t path_resolve_calls;
    uint64_t child_iterations;
    uint64_t selected_row_iterations;
    uint64_t selected_cache_hits;
    int row_capacity;
    int override_index_capacity;
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
