#ifndef NTPACKER_GUI_ROWS_H
#define NTPACKER_GUI_ROWS_H

/* Row model + selection-set helpers for the ntpacker GUI: the flattened per-atlas sprite rows
 * (rebuilt each frame), the canonical leaf-sprite multi-select, natural-order sorting +
 * the sort scratch, the common-name-prefix helper, the canvas region -> row selection sync, and
 * the shared path/name string helpers. Include discipline: rows -> gui_state + model headers only;
 * it must never include a sibling view/actions/widgets header. */

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
 * companions to the multi-select set: they MUST hold the whole selection or the sort
 * path would re-introduce the old truncation. sel_sort_reserve grows both to >= n (false == OOM, old
 * capacity kept); callers must reserve before writing. */
extern gui_selected_sprite *s_sel_sort_buf;
extern const char **s_sel_sort_ptr;
extern tp_op_sprite_ref *s_sel_sort_refs;
bool sel_sort_reserve(int n);

/* --- flattened sprite rows for the current atlas (growable) --- */
typedef struct sprite_row {
    int src;
    int child;
    int indent;
    bool is_source;
    bool is_folder;
    tp_id128 source_id;
    char *source_key;         /* malloc-owned exact authoritative key */
    bool missing;             /* source path gone from disk (§3.7) */
    char label[224];          /* display label (rename-aware: "final (file.png)") */
    char *sprite_name;        /* malloc-owned exact export key */
    char *abs;                /* malloc-owned exact resolved decode path */
    /* --- sort inputs (§61.1); populated per-atlas by build_rows, read by the view comparator --- */
    long long size;           /* packed area (frame w*h) from this atlas's pack result; 0 if unpacked/no region */
    long long mtime;          /* live file mtime (scan entry / gui_scan_stat); 0 if missing/unstattable/folder */
    int added_at;             /* source insertion index -- the order the source was added to the project */
} sprite_row;
extern sprite_row *s_rows;
extern int s_row_count;

/* Updates s_rows/s_row_count for the selected atlas (folders expand to their
 * children). The row model is cached by {atlas id, project model
 * generation, source scan generation}; an unchanged call only compares that
 * key and performs no allocation or filesystem work. */
void build_rows(void);

/* --- filtered / sorted / collapsible VIEW over the row model (U-02 paper cuts) ---
 * s_view[k] is an index into s_rows[]: the left panel iterates the VIEW, never s_rows
 * directly, so the text filter, folder collapse, and sort are pure functions over the
 * row model -- a view-only change never invalidates the expensive build_rows() cache,
 * and the whole seam ports to the U-03 unified tree unchanged. View state is session UI
 * state: it is NEVER serialized into the project (§61.3 app-state boundary). */
extern int *s_view;
extern int s_view_count;

/* Sort keys (§61.1): four user-facing keys, each with two directions, plus the independent
 * "warning on top" pin. ROW_SORT_NAME is 0 so the zero-init/default is `name` (the spec default).
 * ROW_SORT_BUILD is an INTERNAL build/scan-order baseline: it is NOT part of the UI cycle
 * (declare_sort_chips never selects it); it exists so the pure-projection invariant -- build_view over
 * an unsorted key yields the identity of s_rows -- stays testable independent of fixture ordering. */
typedef enum {
    ROW_SORT_NAME = 0, /* natural-order by EFFECTIVE display name (override rename, else export key); DEFAULT */
    ROW_SORT_SIZE,     /* packed area (frame w*h) of the current atlas's pack region; 0 sorts smallest */
    ROW_SORT_MTIME,    /* file modification time, read live */
    ROW_SORT_ADDED,    /* order the source was added to the project (source insertion index) */
    ROW_SORT_BUILD     /* INTERNAL build/scan-order baseline; never exposed in the UI sort cycle */
} row_sort_key;

/* Effective export/display name for a row: the sparse project override rename if one is present,
 * else the canonical export key (sprite_name); "" for rows without a name (folders/missing). F10: the
 * NAME sort and "Copy name" both resolve through this so a renamed sprite sorts and copies by its NEW
 * name, consistent with Rename itself. Valid after build_rows() populated the current atlas. */
const char *gui_rows_effective_name(const sprite_row *row);

/* Case-insensitive substring filter over row label + export name. NULL/"" clears it.
 * A matching child keeps its parent folder visible; an active filter overrides collapse. */
void gui_rows_set_filter(const char *query);
const char *gui_rows_filter(void);
bool gui_rows_filter_active(void);

void gui_rows_set_sort(row_sort_key key, bool descending, bool warn_first);
void gui_rows_get_sort(row_sort_key *key, bool *descending, bool *warn_first);

/* Folder-source disclosure (keyed by stable source id; children hidden when collapsed). */
void gui_rows_toggle_collapsed(tp_id128 source_id);
bool gui_rows_is_collapsed(tp_id128 source_id);

/* Rebuilds s_view from s_rows applying {filter, collapse, sort}. Cheap and cached on
 * {row-cache generation, filter, sort, collapse epoch}; call once per frame after build_rows(). */
void build_view(void);

/* Releases all row/selection caches owned by this module. Safe before first
 * build and after a partial/OOM build; call once during GUI shutdown. */
void gui_rows_shutdown(void);

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
    uint64_t row_string_allocs; /* per-row string heap allocs (rows_strdup: source_key/name/abs, ~3x/row) */
    uint64_t cache_key_checks;
    uint64_t rebuilds;
    uint64_t override_inserts;
    uint64_t override_lookup_calls;
    uint64_t override_probes;
    uint64_t override_slot_clears;
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

/* --- selection preservation across Undo/Redo (U-02 T5) ---
 * capture: record the current primary leaf's canonical ref (call BEFORE the undo/redo mutates the
 * model). revalidate: after the rows rebuild, re-resolve the primary selection to the row carrying
 * that ref (clears it if the sprite is gone) and drop multi-select refs no longer present. Both are
 * no-ops unless s_reselect_pending is set. */
void gui_selection_capture_reselect(void);
void gui_selection_revalidate(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_ROWS_H */
