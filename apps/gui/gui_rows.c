/* Row model + selection-set helpers for the ntpacker GUI (see gui_rows.h). Split out of main.c
 * (GUI decomposition step 2) as a pure move -- definitions relocated verbatim, no behavior change. */

#include "gui_rows.h"

#include "gui_state.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_pack.h"

#include "tp_core/tp_names.h" /* canonical key / natural order / common prefix (op layer) */

#include <limits.h>
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
bool multi_sel_contains_ref(tp_id128 source_id, const char *source_key) {
    if (tp_id128_is_nil(source_id) || !source_key || !source_key[0]) {
        return false;
    }
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (tp_id128_eq(s_multi_sel[i].source_id, source_id) &&
            strcmp(s_multi_sel[i].source_key, source_key) == 0) {
            return true;
        }
    }
    return false;
}
void multi_sel_clear(void) { s_multi_sel_count = 0; }
void multi_sel_add_ref(tp_id128 source_id, const char *source_key) {
    if (tp_id128_is_nil(source_id) || !source_key || !source_key[0] ||
        multi_sel_contains_ref(source_id, source_key)) {
        return;
    }
    if (s_multi_sel_count >= s_multi_sel_cap) {
        const int newcap = s_multi_sel_cap ? s_multi_sel_cap * 2 : MULTI_SEL_INIT_CAP;
        gui_selected_sprite *grown = realloc(s_multi_sel,
                                             (size_t)newcap * sizeof *s_multi_sel);
        if (!grown) {
            set_status_ex(STATUS_ERROR, "Out of memory: selection not fully updated.");
            return; /* keep old capacity + the entries already in it; loud, never silent */
        }
        s_multi_sel = grown;
        s_multi_sel_cap = newcap;
    }
    s_multi_sel[s_multi_sel_count].source_id = source_id;
    (void)snprintf(s_multi_sel[s_multi_sel_count].source_key,
                   sizeof s_multi_sel[s_multi_sel_count].source_key, "%s",
                   source_key);
    s_multi_sel_count++;
}
void multi_sel_remove_ref(tp_id128 source_id, const char *source_key) {
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (tp_id128_eq(s_multi_sel[i].source_id, source_id) &&
            strcmp(s_multi_sel[i].source_key, source_key) == 0) {
            for (int j = i; j < s_multi_sel_count - 1; j++) {
                s_multi_sel[j] = s_multi_sel[j + 1];
            }
            s_multi_sel_count--;
            return;
        }
    }
}
void multi_sel_set_single_ref(tp_id128 source_id, const char *source_key) {
    multi_sel_clear();
    multi_sel_add_ref(source_id, source_key);
}

/* qsort adapter over the core natural-order comparator (tp_names). */
int nat_cmp_qsort(const void *a, const void *b) {
    const gui_selected_sprite *left = (const gui_selected_sprite *)a;
    const gui_selected_sprite *right = (const gui_selected_sprite *)b;
    const int key_order = tp_nat_cmp(left->source_key, right->source_key);
    return key_order ? key_order
                     : memcmp(left->source_id.bytes, right->source_id.bytes,
                              sizeof left->source_id.bytes);
}

/* Shared scratch for the selection-gesture sort (heap; grows WITH the multi-select set so the sort
 * path can never truncate the selection -- P1 fix, step 7). */
gui_selected_sprite *s_sel_sort_buf;
const char **s_sel_sort_ptr;
tp_op_sprite_ref *s_sel_sort_refs;
static int s_sel_sort_cap;

bool sel_sort_reserve(int n) {
    if (n <= s_sel_sort_cap) {
        return true;
    }
    int newcap = s_sel_sort_cap ? s_sel_sort_cap : SEL_SORT_INIT_CAP;
    while (newcap < n) {
        newcap *= 2;
    }
    gui_selected_sprite *gb = realloc(s_sel_sort_buf,
                                      (size_t)newcap * sizeof *s_sel_sort_buf);
    if (!gb) {
        return false; /* keep old capacity (s_sel_sort_cap unchanged) */
    }
    s_sel_sort_buf = gb;
    const char **gp = realloc(s_sel_sort_ptr, (size_t)newcap * sizeof *s_sel_sort_ptr);
    if (!gp) {
        return false; /* buf physically grew but cap not bumped -- next call retries both, stays consistent */
    }
    s_sel_sort_ptr = gp;
    tp_op_sprite_ref *gr = realloc(s_sel_sort_refs,
                                   (size_t)newcap * sizeof *s_sel_sort_refs);
    if (!gr) {
        return false;
    }
    s_sel_sort_refs = gr;
    s_sel_sort_cap = newcap;
    return true;
}
// #endregion

// #region row model
sprite_row *s_rows;
int s_row_count;
static int s_rows_cap;

/* Cache-local lookup over the snapshot/project's sparse overrides. Slots borrow
 * immutable DTO pointers while the row key includes the GUI snapshot-lifetime
 * generation. Snapshot destruction changes that token, forcing a rebuild before
 * any view can reuse a borrowed override. */
typedef struct override_slot {
    tp_id128 source_id;
    const char *source_key;
    const tp_snapshot_sprite *sprite;
} override_slot;

static override_slot *s_override_index;
static size_t s_override_index_cap;
static bool s_row_cache_valid;
static tp_id128 s_row_cache_atlas_id;
static uint64_t s_row_cache_snapshot_generation;
static uint64_t s_row_cache_source_generation;
static uint64_t s_row_cache_snapshot_lifetime;
static uint64_t s_row_cache_generation;
static bool s_selected_cache_valid;
static uint64_t s_selected_cache_row_generation;
static int s_selected_cache_src;
static int s_selected_cache_child;
static int s_selected_cache_row;
static const tp_snapshot_sprite *s_selected_cache_override;
#if defined(NTPACKER_GUI_BENCH)
static gui_rows_bench_counters s_bench_counters;
#endif

static uint64_t override_hash(tp_id128 source_id, const char *source_key) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof source_id.bytes; ++i) {
        hash ^= (uint64_t)source_id.bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    for (const unsigned char *p = (const unsigned char *)source_key; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool override_index_reserve(int count) {
    size_t needed = 8U;
    if (count > 0) {
        if ((size_t)count > (SIZE_MAX - 1U) / 2U) {
            return false;
        }
        const size_t wanted = (size_t)count * 2U + 1U;
        while (needed < wanted) {
            if (needed > SIZE_MAX / 2U) {
                return false;
            }
            needed *= 2U;
        }
    }
    if (needed > s_override_index_cap) {
        if (needed > SIZE_MAX / sizeof *s_override_index) {
            return false;
        }
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.override_index_realloc_calls++;
#endif
        override_slot *grown = realloc(s_override_index, needed * sizeof *grown);
        if (!grown) {
            return false;
        }
        s_override_index = grown;
        s_override_index_cap = needed;
    }
    memset(s_override_index, 0, s_override_index_cap * sizeof *s_override_index);
    return true;
}

static bool override_index_build(const tp_session_snapshot *snapshot,
                                 int atlas_index,
                                 const tp_snapshot_atlas *atlas) {
    if (!override_index_reserve(atlas->sprite_count)) {
        return false;
    }
    const size_t mask = s_override_index_cap - 1U;
    for (int i = 0; i < atlas->sprite_count; ++i) {
        const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_at_index(
            snapshot, atlas_index, i);
        if (!sprite || tp_id128_is_nil(sprite->source_id) ||
            !sprite->source_key || sprite->source_key[0] == '\0') {
            continue;
        }
        size_t slot = (size_t)override_hash(sprite->source_id,
                                             sprite->source_key) & mask;
        for (;;) {
#if defined(NTPACKER_GUI_BENCH)
            s_bench_counters.override_probes++;
#endif
            override_slot *entry = &s_override_index[slot];
            if (!entry->source_key) {
                entry->source_id = sprite->source_id;
                entry->source_key = sprite->source_key;
                entry->sprite = sprite;
#if defined(NTPACKER_GUI_BENCH)
                s_bench_counters.override_inserts++;
#endif
                break;
            }
            if (tp_id128_eq(entry->source_id, sprite->source_id) &&
                strcmp(entry->source_key, sprite->source_key) == 0) {
                break;
            }
            slot = (slot + 1U) & mask;
        }
    }
    return true;
}

static const tp_snapshot_sprite *override_by_key(tp_id128 source_id,
                                                 const char *source_key) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.override_lookup_calls++;
#endif
    const size_t mask = s_override_index_cap - 1U;
    size_t slot = (size_t)override_hash(source_id, source_key) & mask;
    for (;;) {
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.override_probes++;
#endif
        const override_slot *entry = &s_override_index[slot];
        if (!entry->source_key) {
            return NULL;
        }
        if (tp_id128_eq(entry->source_id, source_id) &&
            strcmp(entry->source_key, source_key) == 0) {
            return entry->sprite;
        }
        slot = (slot + 1U) & mask;
    }
}

/* Rename-aware display label: a renamed sprite shows "final (file.png)" so the mapping
 * stays visible; otherwise the file-derived base label. */
static void row_display(tp_id128 source_id, const char *source_key,
                        const char *base_label, const char *paren,
                        char *out, size_t cap) {
    const tp_snapshot_sprite *override = override_by_key(source_id, source_key);
    const char *rename = override ? override->rename : NULL;
    if (rename) {
        (void)snprintf(out, cap, "%s (%s)", rename, paren);
    } else {
        (void)snprintf(out, cap, "%s", base_label);
    }
}

/* Appends one zero-uninitialized row slot and returns it (caller memsets), growing s_rows on a new
 * high-water mark. Returns NULL on OOM (old capacity kept) -- build_rows then stops + raises status.
 * NOTE on realloc-move: a prior `sprite_row *` into s_rows is invalidated by a growth here, so
 * build_rows finishes writing the parent folder row BEFORE pushing any child (see below). */
static sprite_row *rows_push(void) {
    if (s_row_count >= s_rows_cap) {
        const int newcap = s_rows_cap ? s_rows_cap * 2 : ROWS_INIT_CAP;
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.row_realloc_calls++;
#endif
        sprite_row *grown = realloc(s_rows, (size_t)newcap * sizeof *s_rows);
        if (!grown) {
            return NULL;
        }
        s_rows = grown;
        s_rows_cap = newcap;
    }
    return &s_rows[s_row_count++];
}

void build_rows(void) {
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.cache_key_checks++;
#endif
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *a = snapshot
                                     ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas)
                                     : NULL;
    const uint64_t snapshot_generation =
        tp_session_snapshot_model_generation(snapshot);
    const uint64_t source_generation =
        tp_session_snapshot_source_generation(snapshot);
    const uint64_t snapshot_lifetime =
        gui_project_snapshot_lifetime_generation();
    if (a && s_row_cache_valid && tp_id128_eq(s_row_cache_atlas_id, a->id) &&
        s_row_cache_snapshot_generation == snapshot_generation &&
        s_row_cache_source_generation == source_generation &&
        s_row_cache_snapshot_lifetime == snapshot_lifetime) {
        return;
    }
    s_row_cache_valid = false;
    s_selected_cache_valid = false;
    s_row_cache_generation++;
    s_row_count = 0;
    if (!a) {
        return;
    }
#if defined(NTPACKER_GUI_BENCH)
    s_bench_counters.rebuilds++;
#endif
    if (!override_index_build(snapshot, s_sel_atlas, a)) {
        set_status_ex(STATUS_ERROR, "Out of memory: sprite override index unavailable.");
        return;
    }
    for (int si = 0; si < a->source_count; ++si) {
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.source_iterations++;
        s_bench_counters.path_resolve_calls++;
#endif
        const tp_snapshot_source *source = NULL;
        char abs[512];
        tp_error error = {0};
        const tp_status path_status = tp_session_snapshot_source_resolved_at(
            snapshot, s_sel_atlas, si, &source, abs, sizeof abs, &error);
        if (!source) {
            continue;
        }
        const char *sp = source->path;
        if (path_status != TP_STATUS_OK) {
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
        r->source_id = source->id;
        if (!exists) {
            r->missing = true;
            (void)snprintf(r->label, sizeof r->label, "\xE2\x9A\xA0 %s", path_last(sp));
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        } else if (is_dir) {
            const gui_scan_result *sc = gui_scan_get(abs);
            (void)snprintf(r->label, sizeof r->label, "%s/  \xC2\xB7  %d",
                           path_last(sp), sc->count);
            for (int ci = 0; ci < sc->count; ++ci) {
#if defined(NTPACKER_GUI_BENCH)
                s_bench_counters.child_iterations++;
#endif
                sprite_row *cr = rows_push();
                if (!cr) {
                    set_status_ex(STATUS_ERROR, "Out of memory: sprite list truncated.");
                    return;
                }
                memset(cr, 0, sizeof *cr);
                cr->src = si;
                cr->child = ci;
                cr->indent = 1;
                cr->source_id = source->id;
                (void)snprintf(cr->source_key, sizeof cr->source_key, "%s", sc->entries[ci].rel);
                tp_sprite_export_key(sc->entries[ci].rel, cr->sprite_name,
                                     sizeof cr->sprite_name);
                row_display(cr->source_id, cr->source_key, sc->entries[ci].rel,
                            path_last(sc->entries[ci].rel), cr->label,
                            sizeof cr->label);
                (void)snprintf(cr->abs, sizeof cr->abs, "%s", sc->entries[ci].abs);
            }
        } else {
            (void)snprintf(r->source_key, sizeof r->source_key, "%s", path_last(sp));
            tp_sprite_export_key(path_last(sp), r->sprite_name, sizeof r->sprite_name);
            row_display(r->source_id, r->source_key, r->sprite_name,
                        path_last(sp), r->label,
                        sizeof r->label);
            (void)snprintf(r->abs, sizeof r->abs, "%s", abs);
        }
    }
    s_row_cache_atlas_id = a->id;
    s_row_cache_snapshot_generation = snapshot_generation;
    s_row_cache_source_generation = source_generation;
    s_row_cache_snapshot_lifetime = snapshot_lifetime;
    s_row_cache_valid = true;
}

static void selected_cache_refresh(void) {
    /* Frame-side actions (notably screenshot pack/refresh) can replace the GUI
     * snapshot after build_rows() ran. Never inspect selected-cache or override
     * slots borrowed from that expired snapshot; refresh the row owner first. */
    if (s_row_cache_snapshot_lifetime !=
        gui_project_snapshot_lifetime_generation()) {
        build_rows();
    }
    if (s_selected_cache_valid &&
        s_selected_cache_row_generation == s_row_cache_generation &&
        s_selected_cache_src == s_sel_src &&
        s_selected_cache_child == s_sel_child) {
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.selected_cache_hits++;
#endif
        return;
    }
    s_selected_cache_valid = true;
    s_selected_cache_row_generation = s_row_cache_generation;
    s_selected_cache_src = s_sel_src;
    s_selected_cache_child = s_sel_child;
    s_selected_cache_row = -1;
    s_selected_cache_override = NULL;
    for (int i = 0; i < s_row_count; ++i) {
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.selected_row_iterations++;
#endif
        const sprite_row *row = &s_rows[i];
        const bool selected = row->is_source
                                  ? (s_sel_src == row->src && s_sel_child == -1)
                                  : (s_sel_src == row->src &&
                                     s_sel_child == row->child);
        if (selected && !row->is_folder && !row->missing &&
            !tp_id128_is_nil(row->source_id) && row->source_key[0] != '\0') {
            s_selected_cache_row = i;
            s_selected_cache_override = override_by_key(row->source_id,
                                                        row->source_key);
            return;
        }
    }
}

const sprite_row *gui_rows_selected_leaf(void) {
    selected_cache_refresh();
    return s_selected_cache_row >= 0 ? &s_rows[s_selected_cache_row] : NULL;
}

const tp_snapshot_sprite *gui_rows_selected_override(void) {
    selected_cache_refresh();
    return s_selected_cache_override;
}

#if defined(NTPACKER_GUI_BENCH)
void gui_rows_bench_reset_counters(void) {
    const int row_capacity = s_rows_cap;
    const int override_capacity = s_override_index_cap > (size_t)INT_MAX ? INT_MAX : (int)s_override_index_cap;
    memset(&s_bench_counters, 0, sizeof s_bench_counters);
    s_bench_counters.row_capacity = row_capacity;
    s_bench_counters.override_index_capacity = override_capacity;
}

gui_rows_bench_counters gui_rows_bench_get_counters(void) {
    gui_rows_bench_counters out = s_bench_counters;
    out.row_capacity = s_rows_cap;
    out.override_index_capacity = s_override_index_cap > (size_t)INT_MAX ? INT_MAX : (int)s_override_index_cap;
    return out;
}

void gui_rows_bench_shutdown(void) {
    free(s_rows);
    free(s_override_index);
    s_rows = NULL;
    s_override_index = NULL;
    s_row_count = 0;
    s_rows_cap = 0;
    s_override_index_cap = 0U;
    s_row_cache_valid = false;
    s_selected_cache_valid = false;
    memset(&s_bench_counters, 0, sizeof s_bench_counters);
}
#endif
// #endregion

// #region canvas region -> row selection sync
/* Selects the sprite-tree row matching a canvas region by canonical source/key. */
void select_row_for_region(int region_idx) {
    const tp_result *r = gui_pack_result(s_sel_atlas);
    if (!r || region_idx < 0 || region_idx >= r->sprite_count) {
        return;
    }
    for (int i = 0; i < s_row_count; i++) {
        if (!s_rows[i].is_folder && s_rows[i].source_key[0] != '\0' &&
            gui_pack_sprite_matches_ref(s_sel_atlas, region_idx,
                                        s_rows[i].source_id,
                                        s_rows[i].source_key)) {
            s_sel_src = s_rows[i].src;
            s_sel_child = s_rows[i].child;
            s_sel_missing = false;
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", s_rows[i].abs);
            return;
        }
    }
}
// #endregion
