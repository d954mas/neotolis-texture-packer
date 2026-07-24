/* Row model + selection-set helpers for the ntpacker GUI (see gui_rows.h). */

#include "gui_rows.h"

#include "gui_state.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_pack.h"
#include "gui_left_layout.h"

#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h" /* canonical key / natural order / common prefix (op layer) */
#include "tp_core/tp_srckey.h"

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Module-owned buffers grow geometrically, never shrink during a session, and
 * retain their prior capacity on OOM. Callers surface truncation as a status
 * error and never write beyond the retained capacity. */
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

#if defined(NTPACKER_GUI_BENCH)
/* Bench counters, defined up here (not with the other row-cache state) so this shared string helper can
 * count every row-string heap alloc it performs; the reset/get seam and other counter sites are below. */
static gui_rows_bench_counters s_bench_counters;
#endif

static char *rows_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);
    if (copy) {
        memcpy(copy, text, length);
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.row_string_allocs++; /* count real per-row strdup allocs (~3x/row) */
#endif
    }
    return copy;
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
void multi_sel_clear(void) {
    for (int i = 0; i < s_multi_sel_count; ++i) {
        free(s_multi_sel[i].source_key);
        s_multi_sel[i].source_key = NULL;
    }
    s_multi_sel_count = 0;
}
void multi_sel_add_ref(tp_id128 source_id, const char *source_key) {
    if (tp_id128_is_nil(source_id) || !source_key || !source_key[0] ||
        multi_sel_contains_ref(source_id, source_key)) {
        return;
    }
    char *key_copy = rows_strdup(source_key);
    if (!key_copy) {
        set_status_ex(STATUS_ERROR, "Out of memory: selection not fully updated.");
        return;
    }
    if (s_multi_sel_count >= s_multi_sel_cap) {
        const int newcap = s_multi_sel_cap ? s_multi_sel_cap * 2 : MULTI_SEL_INIT_CAP;
        gui_selected_sprite *grown = realloc(s_multi_sel,
                                             (size_t)newcap * sizeof *s_multi_sel);
        if (!grown) {
            free(key_copy);
            set_status_ex(STATUS_ERROR, "Out of memory: selection not fully updated.");
            return; /* keep old capacity + the entries already in it; loud, never silent */
        }
        s_multi_sel = grown;
        s_multi_sel_cap = newcap;
    }
    s_multi_sel[s_multi_sel_count].source_id = source_id;
    s_multi_sel[s_multi_sel_count].source_key = key_copy;
    s_multi_sel_count++;
}
void multi_sel_remove_ref(tp_id128 source_id, const char *source_key) {
    for (int i = 0; i < s_multi_sel_count; i++) {
        if (tp_id128_eq(s_multi_sel[i].source_id, source_id) &&
            strcmp(s_multi_sel[i].source_key, source_key) == 0) {
            free(s_multi_sel[i].source_key);
            for (int j = i; j < s_multi_sel_count - 1; j++) {
                s_multi_sel[j] = s_multi_sel[j + 1];
            }
            s_multi_sel_count--;
            s_multi_sel[s_multi_sel_count].source_key = NULL;
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
 * path can never truncate the selection). */
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

static void row_drop(sprite_row *row) {
    if (!row) {
        return;
    }
    free(row->source_key);
    free(row->sprite_name);
    free(row->abs);
    row->source_key = NULL;
    row->sprite_name = NULL;
    row->abs = NULL;
}

static void rows_clear(void) {
    for (int i = 0; i < s_row_count; ++i) {
        row_drop(&s_rows[i]);
    }
    s_row_count = 0;
}

static char *row_export_key_dup(const char *raw) {
    char key[TP_SRCKEY_MAX];
    tp_sprite_export_key(raw, key, sizeof key);
    return rows_strdup(key);
}

static bool row_set_strings(sprite_row *row, const char *source_key,
                            const char *raw_name, const char *abs) {
    if (source_key) {
        row->source_key = rows_strdup(source_key);
    }
    if (raw_name) {
        row->sprite_name = row_export_key_dup(raw_name);
    }
    if (abs) {
        row->abs = rows_strdup(abs);
    }
    if ((source_key && !row->source_key) ||
        (raw_name && !row->sprite_name) || (abs && !row->abs)) {
        row_drop(row);
        return false;
    }
    return true;
}

/* Cache-local lookup over the snapshot/project's sparse overrides. Slots borrow
 * immutable DTO pointers while the row key includes the GUI snapshot-lifetime
 * generation. Snapshot destruction changes that token, forcing a rebuild before
 * any view can reuse a borrowed override. */
typedef struct override_slot {
    uint64_t generation;
    tp_id128 source_id;
    const tp_snapshot_sprite *sprite;
} override_slot;

static override_slot *s_override_index;
static size_t s_override_index_cap;
static uint64_t s_override_index_generation;
static bool s_row_cache_valid;
static tp_id128 s_row_cache_atlas_id;
static uint64_t s_row_cache_snapshot_generation;
static uint64_t s_row_cache_source_generation;
static uint64_t s_row_cache_snapshot_lifetime;
static uint64_t s_row_cache_pack_version; /* packed-area (§61.1 size) source: a repack must re-stamp row sizes */
static uint64_t s_row_cache_generation;
static bool s_anchor_ref_pending;
static bool s_anchor_ref_is_source;
static tp_id128 s_anchor_ref_source_id;
static char s_anchor_ref_source_key[TP_SRCKEY_MAX];
static bool s_selected_cache_valid;
static uint64_t s_selected_cache_row_generation;
static int s_selected_cache_src;
static int s_selected_cache_child;
static int s_selected_cache_row;
static const tp_snapshot_sprite *s_selected_cache_override;

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
        const size_t old_capacity = s_override_index_cap;
        override_slot *grown = realloc(s_override_index, needed * sizeof *grown);
        if (!grown) {
            return false;
        }
        s_override_index = grown;
        s_override_index_cap = needed;
        memset(&s_override_index[old_capacity], 0,
               (needed - old_capacity) * sizeof *s_override_index);
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.override_slot_clears += needed - old_capacity;
#endif
    }
    return true;
}

static bool override_index_build(const tp_session_snapshot *snapshot,
                                 int atlas_index,
                                 const tp_snapshot_atlas *atlas) {
    if (!override_index_reserve(atlas->sprite_count)) {
        return false;
    }
    s_override_index_generation++;
    if (s_override_index_generation == 0U) {
        memset(s_override_index, 0,
               s_override_index_cap * sizeof *s_override_index);
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.override_slot_clears += s_override_index_cap;
#endif
        s_override_index_generation = 1U;
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
            if (entry->generation != s_override_index_generation) {
                entry->generation = s_override_index_generation;
                entry->source_id = sprite->source_id;
                entry->sprite = sprite;
#if defined(NTPACKER_GUI_BENCH)
                s_bench_counters.override_inserts++;
#endif
                break;
            }
            if (tp_id128_eq(entry->source_id, sprite->source_id) &&
                strcmp(entry->sprite->source_key, sprite->source_key) == 0) {
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
    if (s_override_index_cap == 0U) {
        return NULL; /* no index built yet (no atlas / empty) -- never index a NULL table */
    }
    const size_t mask = s_override_index_cap - 1U;
    size_t slot = (size_t)override_hash(source_id, source_key) & mask;
    for (;;) {
#if defined(NTPACKER_GUI_BENCH)
        s_bench_counters.override_probes++;
#endif
        const override_slot *entry = &s_override_index[slot];
        if (entry->generation != s_override_index_generation) {
            return NULL;
        }
        if (tp_id128_eq(entry->source_id, source_id) &&
            strcmp(entry->sprite->source_key, source_key) == 0) {
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

/* §61.1 `size` = packed AREA of the sprite's placed region (frame w*h, rotation-invariant), read from
 * the current atlas's pack result by canonical {source_id, source_key}. 0 when the atlas is not packed
 * or the sprite has no region (sorts as smallest). Bounded to the current atlas (build_rows is per-atlas). */
static long long row_packed_area(tp_id128 source_id, const char *source_key) {
    if (tp_id128_is_nil(source_id) || !source_key || source_key[0] == '\0') {
        return 0;
    }
    const tp_result *result = gui_pack_result(s_sel_atlas);
    if (!result) {
        return 0;
    }
    const int idx = gui_pack_find_sprite_ref(s_sel_atlas, source_id, source_key);
    if (idx < 0 || idx >= result->sprite_count) {
        return 0;
    }
    const tp_sprite *sprite = &result->sprites[idx];
    return (long long)sprite->frame.w * (long long)sprite->frame.h;
}

/* §61.1 `mtime` = live file modification time via the shared stat facility. 0 when the path is empty,
 * a directory, or unstattable (sorts as oldest). Folder children reuse the scan entry's mtime instead. */
static long long row_stat_mtime(const char *abs) {
    long long mtime = 0;
    if (abs && abs[0] != '\0') {
        (void)gui_scan_stat(abs, NULL, &mtime); /* false leaves mtime at 0 */
    }
    return mtime;
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
    /* §61.1 size reads live pack regions: a repack publishes a new result (new version) without touching
     * the model/source generations, so fold the pack version into the key or sort-by-size would stay stale. */
    const uint64_t pack_version = gui_pack_result_version(s_sel_atlas);
    if (a && s_row_cache_valid && tp_id128_eq(s_row_cache_atlas_id, a->id) &&
        s_row_cache_snapshot_generation == snapshot_generation &&
        s_row_cache_source_generation == source_generation &&
        s_row_cache_snapshot_lifetime == snapshot_lifetime &&
        s_row_cache_pack_version == pack_version) {
        return;
    }
    s_anchor_ref_pending = false;
    if (s_sel_anchor_row >= 0 && s_sel_anchor_row < s_view_count) {
        const int row_index = s_view[s_sel_anchor_row];
        if (row_index >= 0 && row_index < s_row_count) {
            const sprite_row *anchor = &s_rows[row_index];
            s_anchor_ref_is_source = anchor->is_source;
            s_anchor_ref_source_id = anchor->source_id;
            (void)snprintf(s_anchor_ref_source_key,
                           sizeof s_anchor_ref_source_key, "%s",
                           anchor->source_key ? anchor->source_key : "");
            s_anchor_ref_pending =
                !tp_id128_is_nil(s_anchor_ref_source_id);
        }
    }
    s_row_cache_valid = false;
    s_selected_cache_valid = false;
    s_row_cache_generation++;
    rows_clear();
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
        char abs[TP_IDENTITY_PATH_MAX];
        tp_error error = {0};
        const tp_status path_status = tp_session_snapshot_source_resolved_at(
            snapshot, s_sel_atlas, si, &source, abs, sizeof abs, &error);
        if (!source) {
            continue;
        }
        const char *sp = source->path;
        tp_scan_kind disk_kind = TP_SCAN_KIND_MISSING;
        tp_status probe_status = path_status;
        if (probe_status == TP_STATUS_OK) {
            probe_status =
                gui_scan_classify_checked(abs, &disk_kind, &error);
        } else {
            abs[0] = '\0';
        }
        /* Source type belongs to the model. A temporarily missing/unreadable
         * folder remains a folder row and retains disclosure state. */
        const bool is_dir = source->kind == TP_SNAPSHOT_SOURCE_FOLDER;
        sprite_row *r = rows_push();
        if (!r) {
            rows_clear();
            set_status_ex(STATUS_ERROR, "Out of memory: sprite list truncated.");
            return;
        }
        memset(r, 0, sizeof *r);
        r->src = si;
        /* Sources are append-only and non-reorderable, so their current array
         * position is their insertion order. */
        r->added_at = si;
        r->child = -1;
        r->is_source = true;
        r->is_folder = is_dir;
        r->source_id = source->id;
        if (probe_status != TP_STATUS_OK &&
            probe_status != TP_STATUS_NOT_FOUND) {
            r->runtime_status = probe_status;
            (void)snprintf(r->label, sizeof r->label,
                           "\xE2\x9A\xA0 %s%s  \xC2\xB7  inspect failed",
                           path_last(sp), is_dir ? "/" : "");
            if (!row_set_strings(r, NULL, NULL, abs)) {
                rows_clear();
                set_status_ex(STATUS_ERROR,
                              "Out of memory: sprite list unavailable.");
                return;
            }
            set_statusf_ex(STATUS_WARNING, "Could not inspect source: %s",
                           error.msg);
            continue;
        }
        if (probe_status == TP_STATUS_NOT_FOUND) {
            r->missing = true;
            (void)snprintf(r->label, sizeof r->label, "\xE2\x9A\xA0 %s", path_last(sp));
            if (!row_set_strings(r, NULL, NULL, abs)) {
                rows_clear();
                set_status_ex(STATUS_ERROR,
                              "Out of memory: sprite list unavailable.");
                return;
            }
        } else if (is_dir) {
            const gui_scan_result *sc = NULL;
            const tp_status scan_status = gui_scan_get(abs, &sc, &error);
            if (scan_status != TP_STATUS_OK) {
                r->runtime_status = scan_status;
                (void)snprintf(r->label, sizeof r->label,
                               "\xE2\x9A\xA0 %s/  \xC2\xB7  scan failed",
                               path_last(sp));
                if (!row_set_strings(r, NULL, NULL, abs)) {
                    rows_clear();
                    set_status_ex(STATUS_ERROR,
                                  "Out of memory: sprite list unavailable.");
                    return;
                }
                set_statusf_ex(STATUS_WARNING, "Could not scan source: %s",
                               error.msg);
                continue;
            }
            (void)snprintf(r->label, sizeof r->label, "%s/  \xC2\xB7  %d",
                           path_last(sp), sc->count);
            if (!row_set_strings(r, NULL, NULL, abs)) {
                rows_clear();
                set_status_ex(STATUS_ERROR,
                              "Out of memory: sprite list unavailable.");
                return;
            }
            for (int ci = 0; ci < sc->count; ++ci) {
#if defined(NTPACKER_GUI_BENCH)
                s_bench_counters.child_iterations++;
#endif
                sprite_row *cr = rows_push();
                if (!cr) {
                    rows_clear();
                    set_status_ex(STATUS_ERROR, "Out of memory: sprite list truncated.");
                    return;
                }
                memset(cr, 0, sizeof *cr);
                cr->src = si;
                cr->added_at = si; /* children inherit their folder source's add order */
                cr->child = ci;
                cr->indent = 1;
                cr->source_id = source->id;
                cr->mtime = sc->entries[ci].mtime; /* scan already stat'd every child -- no re-stat */
                if (!row_set_strings(cr, sc->entries[ci].rel,
                                     sc->entries[ci].rel,
                                     sc->entries[ci].abs)) {
                    rows_clear();
                    set_status_ex(STATUS_ERROR,
                                  "Out of memory: sprite list unavailable.");
                    return;
                }
                row_display(cr->source_id, cr->source_key, sc->entries[ci].rel,
                            path_last(sc->entries[ci].rel), cr->label,
                            sizeof cr->label);
            }
        } else {
            if (!row_set_strings(r, path_last(sp), path_last(sp), abs)) {
                rows_clear();
                set_status_ex(STATUS_ERROR,
                              "Out of memory: sprite list unavailable.");
                return;
            }
            r->mtime = row_stat_mtime(abs); /* file source: not part of a scan result, stat it live */
            row_display(r->source_id, r->source_key, r->sprite_name,
                        path_last(sp), r->label,
                        sizeof r->label);
        }
    }
    s_row_cache_atlas_id = a->id;
    s_row_cache_snapshot_generation = snapshot_generation;
    s_row_cache_source_generation = source_generation;
    s_row_cache_snapshot_lifetime = snapshot_lifetime;
    s_row_cache_pack_version = pack_version;
    s_row_cache_valid = true;
}
// #endregion

// #region filtered/sorted/collapsible view over the row model
int *s_view;
int s_view_count;
static int s_view_cap;

static char s_view_filter[256];
static char s_view_filter_folded[TP_SRCKEY_MAX];
static bool s_view_filter_folded_valid;
static bool s_view_filter_ascii;
static row_sort_key s_view_sort_key;
static bool s_view_sort_desc;
static bool s_view_sort_warn_first;
static bool s_double_click_ref_valid;
static tp_id128 s_double_click_source_id;
static char *s_double_click_source_key;

/* Collapsed folder sources by stable id. Bounded by the folder-source count (tiny); linear scan. */
static tp_id128 *s_collapsed;
static int s_collapsed_count;
static int s_collapsed_cap;

/* Bumped on any view-control change; part of the view cache key alongside the row generation. */
static uint64_t s_view_epoch = 1;
static bool s_view_cache_valid;
static uint64_t s_view_cache_row_generation;
static uint64_t s_view_cache_epoch;
static bool s_focus_reveal_pending;

/* Reused per-frame scratch (grow-only, freed in shutdown): one span per source row + a child buffer. */
typedef struct view_span {
    int row;         /* index of the source row in s_rows */
    int child_start; /* first child row index (inclusive) */
    int child_end;   /* one past the last child (exclusive) */
} view_span;
static view_span *s_spans;
static int s_spans_cap;
static int *s_child_scratch;
static int s_child_scratch_cap;

#if defined(TP_GUI_VIEW_TEST_DIR)
static bool s_test_fail_next_view_alloc;

void gui_rows_test_fail_next_view_alloc(void) {
    s_test_fail_next_view_alloc = true;
}
#endif

static void *view_realloc(void *ptr, size_t size) {
#if defined(TP_GUI_VIEW_TEST_DIR)
    if (s_test_fail_next_view_alloc) {
        s_test_fail_next_view_alloc = false;
        return NULL;
    }
#endif
    return realloc(ptr, size);
}

static bool ascii_ci_contains(const char *hay, const char *needle) {
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!hay) {
        return false;
    }
    for (const char *h = hay; *h; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b) {
            unsigned char ca = (unsigned char)*a;
            unsigned char cb = (unsigned char)*b;
            if (ca >= 'A' && ca <= 'Z') {
                ca = (unsigned char)(ca - 'A' + 'a');
            }
            if (cb >= 'A' && cb <= 'Z') {
                cb = (unsigned char)(cb - 'A' + 'a');
            }
            if (ca != cb) {
                break;
            }
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

static bool text_is_ascii(const char *text) {
    if (!text) {
        return true;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p >= 0x80U) {
            return false;
        }
    }
    return true;
}

bool gui_rows_identity_matches(tp_id128 captured_id, const char *captured_key,
                               tp_id128 current_id, const char *current_key) {
    const char *left = captured_key ? captured_key : "";
    const char *right = current_key ? current_key : "";
    return !tp_id128_is_nil(captured_id) &&
           tp_id128_eq(captured_id, current_id) &&
           strcmp(left, right) == 0;
}

bool gui_rows_text_contains_ci(const char *haystack, const char *needle) {
    if (!needle || needle[0] == '\0') {
        return true;
    }
    if (!haystack) {
        return false;
    }
    char folded_haystack[TP_SRCKEY_MAX];
    char folded_needle[TP_SRCKEY_MAX];
    if (tp_srckey_casefold(haystack, folded_haystack,
                           sizeof folded_haystack, NULL) != TP_STATUS_OK ||
        tp_srckey_casefold(needle, folded_needle, sizeof folded_needle,
                           NULL) != TP_STATUS_OK) {
        return ascii_ci_contains(haystack, needle);
    }
    return strstr(folded_haystack, folded_needle) != NULL;
}

static bool text_contains_folded_filter(const char *haystack) {
    if (!haystack) {
        return false;
    }
    /* The overwhelmingly common all-ASCII path allocates nothing. A Unicode
     * candidate is folded once; the query was already folded when it changed. */
    if (s_view_filter_ascii && text_is_ascii(haystack)) {
        return ascii_ci_contains(haystack, s_view_filter);
    }
    if (!s_view_filter_folded_valid) {
        return ascii_ci_contains(haystack, s_view_filter);
    }
    char folded_haystack[TP_SRCKEY_MAX];
    if (tp_srckey_casefold(haystack, folded_haystack,
                           sizeof folded_haystack, NULL) != TP_STATUS_OK) {
        return ascii_ci_contains(haystack, s_view_filter);
    }
    return strstr(folded_haystack, s_view_filter_folded) != NULL;
}

static bool row_matches_filter(const sprite_row *r) {
    if (s_view_filter[0] == '\0') {
        return true;
    }
    /* Also search the effective name (override rename, else export key). r->label is a bounded
     * char[224] that TRUNCATES a long "rename (file.png)" display string, and r->sprite_name holds only
     * the canonical key -- so a rename longer than ~223 bytes would be unfindable by Ctrl+F without this.
     * One override_by_key lookup per row, only on the active-filter path. */
    const char *effective = gui_rows_effective_name(r);
    return text_contains_folded_filter(r->label) ||
           (r->sprite_name && strcmp(r->sprite_name, r->label) != 0 &&
            text_contains_folded_filter(r->sprite_name)) ||
           (effective &&
            (!r->sprite_name || strcmp(effective, r->sprite_name) != 0) &&
            strcmp(effective, r->label) != 0 &&
            text_contains_folded_filter(effective));
}

const char *gui_rows_effective_name(const sprite_row *row) {
    if (!row) {
        return "";
    }
    if (row->source_key && row->source_key[0] != '\0' &&
        !tp_id128_is_nil(row->source_id)) {
        const tp_snapshot_sprite *ov = override_by_key(row->source_id, row->source_key);
        if (ov && ov->rename && ov->rename[0] != '\0') {
            return ov->rename; /* The renamed export name wins over the canonical key. */
        }
    }
    return (row->sprite_name && row->sprite_name[0] != '\0') ? row->sprite_name : "";
}

/* NAME-sort key: the EFFECTIVE name (override rename else export key), falling back to the display
 * label for nameless rows (folder/missing sources) so they still order deterministically. */
static const char *row_sort_name(const sprite_row *r) {
    const char *eff = gui_rows_effective_name(r);
    return (eff && eff[0] != '\0') ? eff : r->label;
}

/* Single-threaded UI: the sort key/dir live in module statics the qsort adapters read. */
static int view_row_cmp(int ia, int ib) {
    const sprite_row *a = &s_rows[ia];
    const sprite_row *b = &s_rows[ib];
    const bool a_warning = a->missing || a->runtime_status != TP_STATUS_OK;
    const bool b_warning = b->missing || b->runtime_status != TP_STATUS_OK;
    if (s_view_sort_warn_first && a_warning != b_warning) {
        return a_warning ? -1 : 1; /* warnings pinned on top, independent of direction */
    }
    int c = 0;
    switch (s_view_sort_key) {
    case ROW_SORT_SIZE:
        c = (a->size > b->size) - (a->size < b->size);
        if (c == 0) {
            c = tp_nat_cmp(row_sort_name(a), row_sort_name(b)); /* stable NAME tiebreak */
        }
        break;
    case ROW_SORT_MTIME:
        c = (a->mtime > b->mtime) - (a->mtime < b->mtime);
        if (c == 0) {
            c = tp_nat_cmp(row_sort_name(a), row_sort_name(b));
        }
        break;
    case ROW_SORT_ADDED:
        c = (a->added_at > b->added_at) - (a->added_at < b->added_at);
        if (c == 0) {
            c = tp_nat_cmp(row_sort_name(a), row_sort_name(b));
        }
        break;
    case ROW_SORT_BUILD:
        c = (ia > ib) - (ia < ib); /* internal build/scan-order baseline */
        break;
    case ROW_SORT_NAME:
    default:
        c = tp_nat_cmp(row_sort_name(a), row_sort_name(b));
        break;
    }
    if (s_view_sort_desc) {
        c = -c;
    }
    if (c == 0) {
        /* Tiebreak by original row index so equal keys keep a STABLE order (qsort is not stable);
         * without this, same-named siblings could reshuffle frame-to-frame. */
        c = (ia > ib) - (ia < ib);
    }
    return c;
}
static int view_qsort_child(const void *pa, const void *pb) {
    return view_row_cmp(*(const int *)pa, *(const int *)pb);
}
static int view_qsort_span(const void *pa, const void *pb) {
    return view_row_cmp(((const view_span *)pa)->row, ((const view_span *)pb)->row);
}

static bool view_push(int row_index) {
    if (s_view_count >= s_view_cap) {
        const int newcap = s_view_cap ? s_view_cap * 2 : ROWS_INIT_CAP;
        int *grown = view_realloc(s_view, (size_t)newcap * sizeof *s_view);
        if (!grown) {
            return false;
        }
        s_view = grown;
        s_view_cap = newcap;
    }
    s_view[s_view_count++] = row_index;
    return true;
}

/* OOM fallback: show every row unfiltered/unsorted so the list is never silently emptied. */
static void view_fallback_identity(void) {
    /* build_view stamped the cache valid (+ epoch/generation) before the realloc that just OOM'd, so
     * leave it invalid here or the next frame early-returns this identity view and never retries the
     * real filtered/sorted build once memory frees. */
    s_view_cache_valid = false;
    s_focus_view = -1;
    s_sel_anchor_row = -1;
    s_focus_reveal_pending = false;
    s_anchor_ref_pending = false;
    s_view_count = 0;
    for (int i = 0; i < s_row_count; ++i) {
        if (!view_push(i)) {
            break;
        }
    }
    set_status_ex(STATUS_ERROR, "Out of memory: sprite view fell back to unfiltered.");
}

void gui_rows_set_filter(const char *query) {
    char norm[sizeof s_view_filter];
    (void)snprintf(norm, sizeof norm, "%s", query ? query : "");
    if (strcmp(norm, s_view_filter) != 0) {
        (void)snprintf(s_view_filter, sizeof s_view_filter, "%s", norm);
        s_view_filter_ascii = text_is_ascii(s_view_filter);
        s_view_filter_folded_valid =
            s_view_filter[0] == '\0' ||
            tp_srckey_casefold(s_view_filter, s_view_filter_folded,
                               sizeof s_view_filter_folded,
                               NULL) == TP_STATUS_OK;
        if (!s_view_filter_folded_valid) {
            s_view_filter_folded[0] = '\0';
        }
        s_view_epoch++;
    }
}
const char *gui_rows_filter(void) {
    return s_view_filter;
}
bool gui_rows_filter_active(void) {
    return s_view_filter[0] != '\0';
}

void gui_rows_set_sort(row_sort_key key, bool descending, bool warn_first) {
    if (key != s_view_sort_key || descending != s_view_sort_desc ||
        warn_first != s_view_sort_warn_first) {
        s_view_sort_key = key;
        s_view_sort_desc = descending;
        s_view_sort_warn_first = warn_first;
        s_view_epoch++;
    }
}
void gui_rows_get_sort(row_sort_key *key, bool *descending, bool *warn_first) {
    if (key) {
        *key = s_view_sort_key;
    }
    if (descending) {
        *descending = s_view_sort_desc;
    }
    if (warn_first) {
        *warn_first = s_view_sort_warn_first;
    }
}

float gui_rows_left_section_cap(float panel_height, float ui_scale,
                                bool filter_visible) {
    if (ui_scale <= 0.0F) {
        ui_scale = 1.0F;
    }

    /* Worst case: both bounded scrollers exist. Reserve panel padding, fixed
     * chrome, every direct-child gap, and two sprite rows before splitting the
     * remainder between atlas and animation lists. */
    const gui_left_layout_budget budget =
        gui_left_layout_budget_make(ui_scale, filter_visible);
    float cap = (panel_height - budget.padding - budget.fixed_chrome -
                 budget.gaps - budget.sprite_min) *
                0.5F;
    if (cap < 0.0F) {
        cap = 0.0F;
    }
    const float max_cap = GUI_LEFT_SECTION_MAX_H * ui_scale;
    return cap < max_cap ? cap : max_cap;
}

void gui_rows_sort_chip_click(row_sort_key clicked) {
    row_sort_key key = ROW_SORT_NAME;
    bool descending = false;
    bool warn_first = false;
    gui_rows_get_sort(&key, &descending, &warn_first);
    if (clicked == key) {
        descending = !descending;
    } else {
        key = clicked;
        descending = false;
    }
    gui_rows_set_sort(key, descending, warn_first);
}

int gui_rows_focus_scroll_top(int focus_row, int first_visible_row,
                              int visible_rows) {
    if (focus_row < 0 || first_visible_row < 0 || visible_rows <= 0) {
        return -1;
    }
    if (focus_row < first_visible_row) {
        return focus_row;
    }
    if (focus_row >= first_visible_row + visible_rows) {
        const int top = focus_row - visible_rows + 1;
        return top > 0 ? top : 0;
    }
    return -1;
}

void gui_rows_double_click_reset(void) {
    free(s_double_click_source_key);
    s_double_click_source_key = NULL;
    s_double_click_source_id = tp_id128_nil();
    s_double_click_ref_valid = false;
}

bool gui_rows_double_click_press(tp_id128 source_id, const char *source_key,
                                 bool engine_double_clicked) {
    const char *key = source_key ? source_key : "";
    const bool same_ref =
        s_double_click_ref_valid &&
        tp_id128_eq(s_double_click_source_id, source_id) &&
        strcmp(s_double_click_source_key, key) == 0;
    char *key_copy = rows_strdup(key);
    if (!key_copy) {
        gui_rows_double_click_reset();
        return false;
    }
    free(s_double_click_source_key);
    s_double_click_source_key = key_copy;
    s_double_click_source_id = source_id;
    s_double_click_ref_valid = !tp_id128_is_nil(source_id);
    return engine_double_clicked && same_ref;
}

void gui_rows_entity_double_click_reset(gui_rows_entity_double_click_ref *ref) {
    if (!ref) {
        return;
    }
    ref->entity_id = tp_id128_nil();
    ref->valid = false;
}

bool gui_rows_entity_double_click_press(gui_rows_entity_double_click_ref *ref,
                                        tp_id128 entity_id,
                                        bool engine_double_clicked) {
    if (!ref) {
        return false;
    }
    const bool same_ref =
        ref->valid && tp_id128_eq(ref->entity_id, entity_id);
    ref->entity_id = entity_id;
    ref->valid = !tp_id128_is_nil(entity_id);
    return engine_double_clicked && same_ref;
}

int gui_rows_result_region_for_primary(const sprite_row *row,
                                       const tp_result *result) {
    if (!row || !result || row->is_folder || row->missing ||
        !row->sprite_name || row->sprite_name[0] == '\0' ||
        !row->source_key || row->source_key[0] == '\0') {
        return -1;
    }
    char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
    if (tp_pack_input_format_sprite_name(
            row->source_id, row->source_key, canonical_name,
            sizeof canonical_name, NULL) != TP_STATUS_OK) {
        return -1;
    }
    for (int i = 0; i < result->sprite_count; ++i) {
        const char *packed_name = result->sprites[i].name;
        if (packed_name && strcmp(packed_name, canonical_name) == 0) {
            return i;
        }
    }
    return -1;
}

bool gui_rows_is_collapsed(tp_id128 source_id) {
    for (int i = 0; i < s_collapsed_count; ++i) {
        if (tp_id128_eq(s_collapsed[i], source_id)) {
            return true;
        }
    }
    return false;
}
void gui_rows_toggle_collapsed(tp_id128 source_id) {
    for (int i = 0; i < s_collapsed_count; ++i) {
        if (tp_id128_eq(s_collapsed[i], source_id)) {
            for (int j = i; j < s_collapsed_count - 1; ++j) {
                s_collapsed[j] = s_collapsed[j + 1];
            }
            s_collapsed_count--;
            s_view_epoch++;
            return;
        }
    }
    if (s_collapsed_count >= s_collapsed_cap) {
        const int newcap = s_collapsed_cap ? s_collapsed_cap * 2 : 16;
        tp_id128 *grown = realloc(s_collapsed, (size_t)newcap * sizeof *s_collapsed);
        if (!grown) {
            set_status_ex(STATUS_ERROR, "Out of memory: folder collapse not recorded.");
            return;
        }
        s_collapsed = grown;
        s_collapsed_cap = newcap;
    }
    s_collapsed[s_collapsed_count++] = source_id;
    s_view_epoch++;
}

/* Drop collapse state for folder source owners that no longer exist anywhere in
 * the current snapshot. Collapse state spans atlas switches, so comparing only
 * against the displayed atlas would either forget live off-screen folders or
 * retain ids from deleted atlases forever. */
static void collapsed_prune_missing(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count =
        snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int i = 0; i < s_collapsed_count;) {
        bool present = false;
        for (int ai = 0; ai < atlas_count && !present; ++ai) {
            const tp_snapshot_atlas *atlas =
                tp_session_snapshot_atlas_at(snapshot, ai);
            if (!atlas) {
                continue;
            }
            for (int si = 0; si < atlas->source_count; ++si) {
                const tp_snapshot_source *source =
                    tp_session_snapshot_source_at(snapshot, atlas->id, si);
                if (source && source->kind == TP_SNAPSHOT_SOURCE_FOLDER &&
                    tp_id128_eq(source->id, s_collapsed[i])) {
                    present = true;
                    break;
                }
            }
        }
        if (present) {
            ++i;
        } else {
            for (int j = i; j < s_collapsed_count - 1; ++j) {
                s_collapsed[j] = s_collapsed[j + 1];
            }
            s_collapsed_count--;
        }
    }
}

/* Forward decl: defined in the selection-preservation region below. build_view calls it at the END of a
 * MODEL REBUILD to re-anchor the keyboard focus onto the row still carrying the primary selection -- the
 * view-only re-pin only covers reorders where the row model itself is unchanged. */
static void focus_sync_to_selection(void);

static bool row_carries_primary(const sprite_row *row) {
    return row &&
           (row->is_source
                ? (s_sel_src == row->src && s_sel_child == -1)
                : (s_sel_src == row->src && s_sel_child == row->child));
}

void build_view(void) {
    if (s_view_cache_valid &&
        s_view_cache_row_generation == s_row_cache_generation &&
        s_view_cache_epoch == s_view_epoch) {
        return;
    }
    /* A view-only change (filter/sort/collapse) reorders s_view but not s_rows, so the keyboard
     * focus + shift-anchor (both s_view positions) can be re-pinned to the SAME row afterwards.
     * Capture their target rows first; only meaningful when the row model itself is unchanged. */
    const bool rows_unchanged = s_view_cache_valid &&
                                s_view_cache_row_generation == s_row_cache_generation;
    const int keep_focus_row =
        (rows_unchanged && s_focus_view >= 0 && s_focus_view < s_view_count)
            ? s_view[s_focus_view]
            : -1;
    const bool keep_focus_is_primary =
        keep_focus_row >= 0 &&
        row_carries_primary(&s_rows[keep_focus_row]);
    const int keep_anchor_row =
        (rows_unchanged && s_sel_anchor_row >= 0 && s_sel_anchor_row < s_view_count)
            ? s_view[s_sel_anchor_row]
            : -1;
    if (!rows_unchanged) {
        /* A structural rebuild is the pruning boundary. The snapshot-wide scan
         * preserves live off-screen folders while dropping deleted owners. */
        collapsed_prune_missing();
    }
    s_view_count = 0;
    s_view_cache_valid = true;
    s_view_cache_row_generation = s_row_cache_generation;
    s_view_cache_epoch = s_view_epoch;
    if (s_row_count == 0) {
        s_sel_anchor_row = -1;
        s_anchor_ref_pending = false;
        return;
    }
    /* Sort-by-size needs each leaf's packed area, one pack-ref lookup per row. Compute it lazily HERE
     * (build_view re-runs on a sort-key change; the cached build_rows does not) and ONLY when the SIZE
     * key is active, so the default path -- and the O(S+F) canonical-preview invariant asserted by the
     * GUI selftest -- pay nothing. build_rows leaves size at 0; view_row_cmp reads it only for SIZE. */
    if (s_view_sort_key == ROW_SORT_SIZE) {
        for (int i = 0; i < s_row_count; ++i) {
            /* size 0 is the "packed area not yet resolved for this row generation" sentinel: build_rows
             * memsets it, and a repack bumps the row generation (pack_version is in the build_rows cache
             * key) so a rebuild re-zeros then re-resolves. A real packed region has area >= 1, so a
             * non-zero value is authoritative and skipped -- which also lets the headless unit test
             * inject a controlled area (it cannot run the packer). */
            if (s_rows[i].size == 0 && !s_rows[i].is_folder && s_rows[i].source_key &&
                s_rows[i].source_key[0] != '\0') {
                s_rows[i].size = row_packed_area(s_rows[i].source_id, s_rows[i].source_key);
            }
        }
    }
    /* 1. Partition s_rows into source spans (each source row + its contiguous child run). */
    int nspans = 0;
    for (int i = 0; i < s_row_count;) {
        int j = i + 1;
        while (j < s_row_count && !s_rows[j].is_source) {
            j++;
        }
        if (nspans >= s_spans_cap) {
            const int newcap = s_spans_cap ? s_spans_cap * 2 : 64;
            view_span *grown =
                view_realloc(s_spans, (size_t)newcap * sizeof *s_spans);
            if (!grown) {
                view_fallback_identity();
                return;
            }
            s_spans = grown;
            s_spans_cap = newcap;
        }
        s_spans[nspans].row = i;
        s_spans[nspans].child_start = i + 1;
        s_spans[nspans].child_end = j;
        nspans++;
        i = j;
    }
    /* 2. Order the source spans (warn-first + key/dir; a no-op reorder for ORIGINAL asc). */
    if (nspans > 0) { /* qsort's base is nonnull; guard the empty case defensively */
        qsort(s_spans, (size_t)nspans, sizeof *s_spans, view_qsort_span);
    }
    /* 3. Emit: source (if visible under the filter) then its filtered+sorted, non-collapsed children. */
    const bool filtering = s_view_filter[0] != '\0';
    for (int s = 0; s < nspans; ++s) {
        const view_span *sp = &s_spans[s];
        const sprite_row *src = &s_rows[sp->row];
        const bool collapsed = src->is_folder && gui_rows_is_collapsed(src->source_id);
        int nch = 0;
        if (!collapsed || filtering) { /* an active filter overrides collapse so matches surface */
            for (int c = sp->child_start; c < sp->child_end; ++c) {
                if (filtering && !row_matches_filter(&s_rows[c])) {
                    continue;
                }
                if (nch >= s_child_scratch_cap) {
                    const int newcap = s_child_scratch_cap ? s_child_scratch_cap * 2 : 256;
                    int *grown = view_realloc(
                        s_child_scratch,
                        (size_t)newcap * sizeof *s_child_scratch);
                    if (!grown) {
                        view_fallback_identity();
                        return;
                    }
                    s_child_scratch = grown;
                    s_child_scratch_cap = newcap;
                }
                s_child_scratch[nch++] = c;
            }
            if (nch > 0) { /* qsort's base is nonnull; s_child_scratch may still be NULL when nch==0 */
                qsort(s_child_scratch, (size_t)nch, sizeof *s_child_scratch, view_qsort_child);
            }
        }
        if (filtering && !row_matches_filter(src) && nch == 0) {
            continue; /* neither the source nor any child matched */
        }
        if (!view_push(sp->row)) {
            view_fallback_identity();
            return;
        }
        for (int k = 0; k < nch; ++k) {
            if (!view_push(s_child_scratch[k])) {
                view_fallback_identity();
                return;
            }
        }
    }
    /* Re-pin keyboard focus + shift-anchor onto their original rows in the reordered view. If a row
     * was filtered/collapsed out of the rebuilt view, clear the index to -1: leaving the stale
     * numeric index in place would silently alias a DIFFERENT visible row (or point out of range). */
    if (keep_focus_row >= 0) {
        bool found = false;
        for (int k = 0; k < s_view_count; ++k) {
            if (s_view[k] == keep_focus_row) {
                s_focus_view = k;
                found = true;
                break;
            }
        }
        if (!found) {
            s_focus_view = -1;
            if (keep_focus_is_primary) {
                s_focus_reveal_pending = true;
            }
        }
    }
    if (keep_anchor_row >= 0) {
        bool found = false;
        for (int k = 0; k < s_view_count; ++k) {
            if (s_view[k] == keep_anchor_row) {
                s_sel_anchor_row = k;
                found = true;
                break;
            }
        }
        if (!found) {
            s_sel_anchor_row = -1;
        }
    }
    if (!rows_unchanged) {
        s_sel_anchor_row = -1;
        if (s_anchor_ref_pending) {
            for (int k = 0; k < s_view_count; ++k) {
                const sprite_row *row = &s_rows[s_view[k]];
                const char *key = row->source_key ? row->source_key : "";
                if (row->is_source == s_anchor_ref_is_source &&
                    tp_id128_eq(row->source_id, s_anchor_ref_source_id) &&
                    strcmp(key, s_anchor_ref_source_key) == 0) {
                    s_sel_anchor_row = k;
                    break;
                }
            }
        }
        s_anchor_ref_pending = false;
    }
    /* A model rebuild invalidates row indices. Re-anchor focus to the visible
     * primary selection; selection revalidation corrects it again after undo. */
    if (!rows_unchanged || s_focus_reveal_pending) {
        focus_sync_to_selection();
    }
}
// #endregion

// #region selected leaf cache
static void selected_cache_refresh(void) {
    /* Frame-side actions (notably screenshot pack/refresh) can replace the GUI
     * snapshot after build_rows() ran. Never inspect selected-cache or override
     * slots borrowed from that expired snapshot; refresh the row owner first. */
    if (s_row_cache_snapshot_lifetime !=
        gui_project_snapshot_lifetime_generation()) {
        build_rows();
        /* The row model was just replaced mid-frame; rebuild the view so s_view never indexes a
         * stale/shrunk s_rows during the rest of the declaration pass (generation-cached, cheap). */
        build_view();
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
            !tp_id128_is_nil(row->source_id) && row->source_key &&
            row->source_key[0] != '\0') {
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

void gui_rows_shutdown(void) {
    multi_sel_clear();
    free(s_multi_sel);
    s_multi_sel = NULL;
    s_multi_sel_count = 0;
    s_multi_sel_cap = 0;

    free(s_sel_sort_buf);
    free(s_sel_sort_ptr);
    free(s_sel_sort_refs);
    s_sel_sort_buf = NULL;
    s_sel_sort_ptr = NULL;
    s_sel_sort_refs = NULL;
    s_sel_sort_cap = 0;

    rows_clear();
    free(s_rows);
    s_rows = NULL;
    s_row_count = 0;
    s_rows_cap = 0;

    free(s_view);
    s_view = NULL;
    s_view_count = 0;
    s_view_cap = 0;
    free(s_collapsed);
    s_collapsed = NULL;
    s_collapsed_count = 0;
    s_collapsed_cap = 0;
    free(s_spans);
    s_spans = NULL;
    s_spans_cap = 0;
    free(s_child_scratch);
    s_child_scratch = NULL;
    s_child_scratch_cap = 0;
    s_view_filter[0] = '\0';
    s_view_sort_key = ROW_SORT_NAME; /* §61.1 default sort key is `name` */
    s_view_sort_desc = false;
    s_view_sort_warn_first = false;
    gui_rows_double_click_reset();
    s_view_epoch = 1;
    s_view_cache_valid = false;
    s_view_cache_row_generation = 0U;
    s_view_cache_epoch = 0U;
    s_focus_reveal_pending = false;

    free(s_override_index);
    s_override_index = NULL;
    s_override_index_cap = 0U;
    s_override_index_generation = 0U;

    s_row_cache_valid = false;
    s_row_cache_atlas_id = tp_id128_nil();
    s_row_cache_snapshot_generation = 0U;
    s_row_cache_source_generation = 0U;
    s_row_cache_snapshot_lifetime = 0U;
    s_row_cache_pack_version = 0U;
    s_row_cache_generation = 0U;
    s_anchor_ref_pending = false;
    s_anchor_ref_is_source = false;
    s_anchor_ref_source_id = tp_id128_nil();
    s_anchor_ref_source_key[0] = '\0';
    s_selected_cache_valid = false;
    s_selected_cache_row_generation = 0U;
    s_selected_cache_src = -1;
    s_selected_cache_child = -1;
    s_selected_cache_row = -1;
    s_selected_cache_override = NULL;
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
    gui_rows_shutdown();
    memset(&s_bench_counters, 0, sizeof s_bench_counters);
}
#endif
// #endregion

// #region canvas region -> row selection sync
/* Selects the sprite-tree row matching a displayed result region by the
 * canonical packed name encoded from {source_id, source_key}. */
void select_row_for_result_region(const tp_result *result, int region_idx) {
    if (!result || region_idx < 0 || region_idx >= result->sprite_count) {
        return;
    }
    const char *packed_name = result->sprites[region_idx].name;
    if (!packed_name) {
        return;
    }
    for (int i = 0; i < s_row_count; i++) {
        const sprite_row *row = &s_rows[i];
        if (row->is_folder || !row->source_key ||
            row->source_key[0] == '\0') {
            continue;
        }
        char canonical_name[TP_PACK_INTERNAL_NAME_CAP];
        if (tp_pack_input_format_sprite_name(
                row->source_id, row->source_key, canonical_name,
                sizeof canonical_name, NULL) == TP_STATUS_OK &&
            strcmp(packed_name, canonical_name) == 0) {
            s_sel_src = s_rows[i].src;
            s_sel_child = s_rows[i].child;
            s_sel_missing = false;
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", s_rows[i].abs);
            /* Re-pin keyboard focus + the Shift-range anchor onto the newly selected row so F2/arrows
             * act on THIS sprite, not the previously focused one. focus_sync lands -1 when the row is
             * hidden (filtered/collapsed) -- clearing the anchor too, so a stale Shift anchor cannot
             * mis-range (mirrors undo_redo_settle). Request an ensure-visible only for a visible focus. */
            focus_sync_to_selection();
            s_sel_anchor_row = s_focus_view;
            if (s_focus_view >= 0) {
                s_focus_follow = true;
            }
            return;
        }
    }
}

void select_row_for_region(int region_idx) {
    select_row_for_result_region(gui_pack_result(s_sel_atlas), region_idx);
}
// #endregion

// #region selection preservation across Undo/Redo
static bool row_ref_present(tp_id128 source_id, const char *source_key) {
    for (int i = 0; i < s_row_count; ++i) {
        if (!s_rows[i].is_folder && s_rows[i].source_key && s_rows[i].source_key[0] != '\0' &&
            tp_id128_eq(s_rows[i].source_id, source_id) &&
            strcmp(s_rows[i].source_key, source_key) == 0) {
            return true;
        }
    }
    return false;
}

/* Re-pin the keyboard focus (an s_view index) onto whichever visible row now carries the primary
 * selection, so the focus ring and the selection highlight stay together after an undo/redo
 * re-resolves the selection onto a shifted row. -1 when the selected row is hidden (collapsed or
 * filtered out) or nothing is selected; focus_clamp then restarts nav from a list edge. */
static void focus_sync_to_selection(void) {
    s_focus_view = -1;
    s_focus_reveal_pending = false;
    if (s_sel_src < 0) {
        return;
    }
    for (int k = 0; k < s_view_count; ++k) {
        const sprite_row *row = &s_rows[s_view[k]];
        const bool match = row->is_source
                               ? (s_sel_src == row->src && s_sel_child == -1)
                               : (s_sel_src == row->src && s_sel_child == row->child);
        if (match) {
            s_focus_view = k;
            return;
        }
    }
    s_focus_reveal_pending = true;
}

void gui_selection_capture_reselect(void) {
    const sprite_row *leaf = gui_rows_selected_leaf();
    s_reselect_source_id = tp_id128_nil();
    s_reselect_key[0] = '\0';
    /* Preserve the viewed atlas by its stable id regardless of row kind, so an
     * undo/redo that inserts or removes an atlas before it -- shifting s_sel_atlas -- re-resolves onto the
     * same atlas, not a positional neighbour. nil when there is no atlas at s_sel_atlas. */
    s_reselect_atlas_id = tp_id128_nil();
    {
        const tp_session_snapshot *snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *atlas =
            snapshot ? tp_session_snapshot_atlas_at(snapshot, s_sel_atlas) : NULL;
        if (atlas) {
            s_reselect_atlas_id = atlas->id;
        }
    }
    if (leaf && leaf->source_key && leaf->source_key[0] != '\0') {
        const int n = snprintf(s_reselect_key, sizeof s_reselect_key, "%s", leaf->source_key);
        if (n >= 0 && (size_t)n < sizeof s_reselect_key) {
            s_reselect_source_id = leaf->source_id;
        } else {
            /* Defensive: tp_scan bounds rel by TP_SRCKEY_MAX so this cannot happen today, but a
             * truncated key would silently re-resolve onto the wrong row -- drop the ref instead. */
            s_reselect_key[0] = '\0';
        }
    } else if (s_sel_src >= 0 && s_sel_child == -1) {
        /* A folder/source row is the primary (no leaf ref). Preserve it by its stable source id so
         * an undo that re-inserts sources does not leave s_sel_src pointing at a shifted neighbour. */
        for (int i = 0; i < s_row_count; ++i) {
            if (s_rows[i].is_source && s_rows[i].src == s_sel_src &&
                !tp_id128_is_nil(s_rows[i].source_id)) {
                s_reselect_source_id = s_rows[i].source_id;
                break;
            }
        }
    }
    s_reselect_pending = true;
}

void gui_selection_revalidate(void) {
    if (!s_reselect_pending) {
        return;
    }
    s_reselect_pending = false;
    /* Re-resolve the primary selection to the row still carrying the captured ref (else clear it).
     * A non-empty key means a leaf sprite (match by source id + key); an empty key with a non-nil id
     * means a folder/source primary (match the source row that owns the id). */
    if (!tp_id128_is_nil(s_reselect_source_id)) {
        bool found = false;
        for (int i = 0; i < s_row_count; ++i) {
            const sprite_row *row = &s_rows[i];
            if (!tp_id128_eq(row->source_id, s_reselect_source_id)) {
                continue;
            }
            if (s_reselect_key[0] != '\0') {
                if (row->is_folder || !row->source_key || row->source_key[0] == '\0' ||
                    strcmp(row->source_key, s_reselect_key) != 0) {
                    continue;
                }
                s_sel_child = row->child;
                s_sel_missing = row->missing;
            } else {
                if (!row->is_source) {
                    continue;
                }
                s_sel_child = -1;
                s_sel_missing = row->missing; /* Missing state survives reselect. */
            }
            s_sel_src = row->src;
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", row->abs);
            found = true;
            break;
        }
        if (!found) {
            s_sel_src = -1;
            s_sel_child = -1;
            s_sel_missing = false;
            s_sel_abs[0] = '\0';
        }
    }
    /* Prune multi-select refs whose sprite no longer exists after the undo/redo. */
    for (int i = 0; i < s_multi_sel_count;) {
        if (!row_ref_present(s_multi_sel[i].source_id, s_multi_sel[i].source_key)) {
            free(s_multi_sel[i].source_key);
            for (int j = i; j < s_multi_sel_count - 1; ++j) {
                s_multi_sel[j] = s_multi_sel[j + 1];
            }
            s_multi_sel_count--;
            s_multi_sel[s_multi_sel_count].source_key = NULL;
        } else {
            ++i;
        }
    }
    /* Keep the keyboard focus ring on the row that now carries the selection. revalidate runs right
     * after build_view (main loop), so s_view is current and this cannot read a stale index. */
    focus_sync_to_selection();
}
// #endregion
