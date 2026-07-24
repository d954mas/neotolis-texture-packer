/* Sprite-tree view-layer regression (gui_rows.c / gui_rows.h).
 *
 * Exercises the pure filter/sort/collapse VIEW that sits over the cached row
 * model: build_rows() flattens the selected atlas into s_rows[], build_view()
 * projects s_view[] (indices into s_rows[]) applying {filter, collapse, sort}.
 * A view-only change must never touch the row model, so every case asserts on
 * s_view / s_view_count and the rows they point at. Headless: no window, GL,
 * pack job, or event loop -- same in-memory session harness as
 * test_gui_canonical_identity.c. */

#include <stdbool.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

#include "gui_actions.h"
#include "gui_canvas.h"
#include "gui_left_layout.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_scan.h"

#include "unity.h"

/* gui_actions/gui_project own several shell-facing commands linked into this
 * headless target but never exercised here. */
void gui_shell_reset_shown_result(void) {}

/* One FOLDER source (pack/ with alpha/beta/gamma.png children) + one FILE
 * source (solo.png) -- enough to drive filter, sort, collapse, and the missing
 * warn-first pin without touching the packer. */
static char s_pack_dir[512];
static char s_alpha[512];
static char s_beta[512];
static char s_gamma[512];
static char s_solo[512];

static void remove_fixture_files(void) {
    (void)remove(s_alpha);
    (void)remove(s_beta);
    (void)remove(s_gamma);
    (void)remove(s_solo);
    (void)test_rmdir(s_pack_dir);
    (void)test_rmdir(TP_GUI_VIEW_TEST_DIR);
}

/* Small valid RGBA PNGs with deliberately different dimensions. Most view
 * tests only stat/enumerate them; the SIZE-sort regression runs the real pack
 * path and verifies that build_rows obtains packed areas through gui_pack. */
static const unsigned char s_png_alpha_3x2[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x03U,
    0x00U, 0x00U, 0x00U, 0x02U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x9dU,
    0x74U, 0x66U, 0x1aU, 0x00U, 0x00U, 0x00U, 0x11U, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0xf8U, 0xcfU, 0xc0U, 0xf0U, 0x1fU, 0x86U,
    0x19U, 0x90U, 0x39U, 0x00U, 0x9bU, 0x7eU, 0x0bU, 0xf5U, 0x0fU, 0x5fU,
    0x26U, 0x22U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U,
    0xaeU, 0x42U, 0x60U, 0x82U};
static const unsigned char s_png_beta_1x1[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x01U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x1fU,
    0x15U, 0xc4U, 0x89U, 0x00U, 0x00U, 0x00U, 0x0dU, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0x60U, 0xf8U, 0xcfU, 0xf0U, 0x1fU, 0x00U,
    0x04U, 0x01U, 0x01U, 0xffU, 0xaeU, 0xb5U, 0x55U, 0xf5U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU, 0x42U, 0x60U, 0x82U};
static const unsigned char s_png_gamma_2x2[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x02U,
    0x00U, 0x00U, 0x00U, 0x02U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x72U,
    0xb6U, 0x0dU, 0x24U, 0x00U, 0x00U, 0x00U, 0x10U, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0x60U, 0x60U, 0xf8U, 0xffU, 0x1fU, 0x82U,
    0xa1U, 0x0cU, 0x00U, 0x3fU, 0xd2U, 0x07U, 0xf9U, 0x5cU, 0x13U, 0xe0U,
    0x42U, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U, 0xaeU,
    0x42U, 0x60U, 0x82U};
static const unsigned char s_png_solo_1x2[] = {
    0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU, 0x00U, 0x00U,
    0x00U, 0x0dU, 0x49U, 0x48U, 0x44U, 0x52U, 0x00U, 0x00U, 0x00U, 0x01U,
    0x00U, 0x00U, 0x00U, 0x02U, 0x08U, 0x06U, 0x00U, 0x00U, 0x00U, 0x99U,
    0x81U, 0xb6U, 0x27U, 0x00U, 0x00U, 0x00U, 0x11U, 0x49U, 0x44U, 0x41U,
    0x54U, 0x78U, 0xdaU, 0x63U, 0xf8U, 0xffU, 0x9fU, 0x01U, 0x08U, 0xffU,
    0x33U, 0xfcU, 0x07U, 0x00U, 0x1eU, 0xebU, 0x05U, 0xfbU, 0xd5U, 0x42U,
    0x5bU, 0x8aU, 0x00U, 0x00U, 0x00U, 0x00U, 0x49U, 0x45U, 0x4eU, 0x44U,
    0xaeU, 0x42U, 0x60U, 0x82U};

static bool write_fixture_file(const char *path, const unsigned char *bytes,
                               size_t byte_count) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    const bool ok = fwrite(bytes, 1U, byte_count, file) == byte_count;
    return fclose(file) == 0 && ok;
}

static bool prepare_files(void) {
    (void)snprintf(s_pack_dir, sizeof s_pack_dir, "%s/pack",
                   TP_GUI_VIEW_TEST_DIR);
    (void)snprintf(s_alpha, sizeof s_alpha, "%s/alpha.png", s_pack_dir);
    (void)snprintf(s_beta, sizeof s_beta, "%s/beta.png", s_pack_dir);
    (void)snprintf(s_gamma, sizeof s_gamma, "%s/gamma.png", s_pack_dir);
    (void)snprintf(s_solo, sizeof s_solo, "%s/solo.png", TP_GUI_VIEW_TEST_DIR);
    remove_fixture_files();
    tp_mkdirs(s_pack_dir);
    return write_fixture_file(s_alpha, s_png_alpha_3x2,
                              sizeof s_png_alpha_3x2) &&
           write_fixture_file(s_beta, s_png_beta_1x1,
                              sizeof s_png_beta_1x1) &&
           write_fixture_file(s_gamma, s_png_gamma_2x2,
                              sizeof s_png_gamma_2x2) &&
           write_fixture_file(s_solo, s_png_solo_1x2,
                              sizeof s_png_solo_1x2);
}

/* Adds the folder source (index 0) then the file source (index 1) to the
 * default atlas, rescans, and builds the row model. build_rows classifies by
 * DISK state (gui_scan_is_dir), so pack/ expands to its three children and
 * solo.png is a single leaf source row. */
static void add_sources_and_build(tp_id128 *folder_id, tp_id128 *file_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(atlas_id,
                                    tp_session_snapshot_revision(snapshot),
                                    s_pack_dir, TP_SOURCE_KIND_FOLDER));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(atlas_id,
                                    tp_session_snapshot_revision(snapshot),
                                    s_solo, TP_SOURCE_KIND_FILE));

    gui_project_invalidate_sources();
    s_sel_atlas = 0;
    build_rows();

    snapshot = gui_project_snapshot();
    const tp_snapshot_source *folder =
        tp_session_snapshot_source_at(snapshot, atlas_id, 0);
    const tp_snapshot_source *file =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(folder);
    TEST_ASSERT_NOT_NULL(file);
    if (folder_id) {
        *folder_id = folder->id;
    }
    if (file_id) {
        *file_id = file->id;
    }
}

/* Same fixture but with the FILE source at index 0 and the FOLDER source at index 1
 * (reverse add order). Removing source index 0 then shifts the folder's stable source
 * index 1 -> 0 -- the shift the id-stable re-resolution + the TYPE sort must survive. */
static void add_sources_reversed_and_build(tp_id128 *folder_id,
                                           tp_id128 *file_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(atlas_id,
                                    tp_session_snapshot_revision(snapshot),
                                    s_solo, TP_SOURCE_KIND_FILE));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(atlas_id,
                                    tp_session_snapshot_revision(snapshot),
                                    s_pack_dir, TP_SOURCE_KIND_FOLDER));

    gui_project_invalidate_sources();
    s_sel_atlas = 0;
    build_rows();

    snapshot = gui_project_snapshot();
    const tp_snapshot_source *file =
        tp_session_snapshot_source_at(snapshot, atlas_id, 0);
    const tp_snapshot_source *folder =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_NOT_NULL(folder);
    if (file_id) {
        *file_id = file->id;
    }
    if (folder_id) {
        *folder_id = folder->id;
    }
}

/* Row index (in s_rows) of the leaf whose export name matches, or -1. */
static int find_row_by_name(const char *name) {
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].sprite_name && strcmp(s_rows[i].sprite_name, name) == 0) {
            return i;
        }
    }
    return -1;
}

/* View position of the (only) folder-source row, or -1. build_view emits a
 * source then its children contiguously, so its children are at pos+1.. */
static int view_folder_pos(void) {
    for (int k = 0; k < s_view_count; ++k) {
        const sprite_row *row = &s_rows[s_view[k]];
        if (row->is_source && row->is_folder) {
            return k;
        }
    }
    return -1;
}

void setUp(void) {
    TEST_ASSERT_TRUE(prepare_files());
    gui_project_init();
    s_sel_atlas = 0;
    multi_sel_clear();
}

void tearDown(void) {
    gui_pack_shutdown();
    gui_rows_shutdown();
    gui_project_shutdown();
    gui_scan_shutdown();
    remove_fixture_files();
}

/* 1. No filter, internal BUILD-order baseline, nothing collapsed -> the view is the identity
 *    projection of the whole row model (the pure-projection invariant). ROW_SORT_BUILD is the
 *    unsorted baseline (not a UI-exposed key); the user-facing default is `name` (tested separately). */
void test_view_passthrough_is_identity(void) {
    add_sources_and_build(NULL, NULL);
    gui_rows_set_sort(ROW_SORT_BUILD, false, false);
    build_view();

    TEST_ASSERT_EQUAL_INT(5, s_row_count); /* folder + 3 children + solo */
    TEST_ASSERT_EQUAL_INT(s_row_count, s_view_count);
    for (int i = 0; i < s_view_count; ++i) {
        TEST_ASSERT_EQUAL_INT(i, s_view[i]);
    }
    TEST_ASSERT_FALSE(gui_rows_filter_active());
}

/* 2. Case-insensitive substring filter keeps matching children AND their parent
 *    folder-source row; clearing it restores the identity projection. */
void test_view_filter_is_case_insensitive_and_keeps_parent(void) {
    add_sources_and_build(NULL, NULL);

    gui_rows_set_filter("ALP"); /* upper-case query, lower-case sprite name */
    build_view();

    TEST_ASSERT_TRUE(gui_rows_filter_active());
    TEST_ASSERT_EQUAL_STRING("ALP", gui_rows_filter());
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder parent + alpha */
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_source);
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_folder);
    TEST_ASSERT_FALSE(s_rows[s_view[1]].is_source);
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[1]].sprite_name);

    gui_rows_set_filter(""); /* clearing restores passthrough */
    build_view();
    TEST_ASSERT_FALSE(gui_rows_filter_active());
    TEST_ASSERT_EQUAL_INT(s_row_count, s_view_count);
    for (int i = 0; i < s_view_count; ++i) {
        TEST_ASSERT_EQUAL_INT(i, s_view[i]);
    }
}

/* 3. A query matching neither a source nor any child empties the view. */
void test_view_filter_no_match_empties_view(void) {
    add_sources_and_build(NULL, NULL);

    gui_rows_set_filter("zzz-nothing-here");
    build_view();

    TEST_ASSERT_TRUE(gui_rows_filter_active());
    TEST_ASSERT_EQUAL_INT(0, s_view_count);
}

/* 4. NAME sort orders siblings by natural name; descending reverses, and a
 *    folder's children stay contiguous under it (hierarchy preserved). */
void test_view_sort_name_orders_children_asc_and_desc(void) {
    add_sources_and_build(NULL, NULL);

    gui_rows_set_sort(ROW_SORT_NAME, false, false);
    build_view();
    int pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(pos + 3, s_view_count - 1);
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 1]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 2]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 3]].sprite_name);
    TEST_ASSERT_EQUAL_INT(1, s_rows[s_view[pos + 1]].indent); /* under parent */

    gui_rows_set_sort(ROW_SORT_NAME, true, false);
    build_view();
    pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 1]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 2]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 3]].sprite_name);
}

/* 5. warn_first pins a MISSING source row to the top of its sibling group
 *    regardless of sort direction. The file source is made missing by deleting
 *    its backing file and rescanning (the task's suggested reliable method). */
void test_view_warn_first_pins_missing_regardless_of_direction(void) {
    add_sources_and_build(NULL, NULL);

    TEST_ASSERT_EQUAL_INT(0, remove(s_solo)); /* solo.png -> gone from disk */
    gui_project_invalidate_sources();
    build_rows();
    TEST_ASSERT_EQUAL_INT(5, s_row_count);

    /* Baseline: without warn_first the missing row is NOT pinned (build order
     * keeps the folder span first, the now-missing solo span last). */
    gui_rows_set_sort(ROW_SORT_BUILD, false, false);
    build_view();
    TEST_ASSERT_EQUAL_INT(5, s_view_count);
    TEST_ASSERT_FALSE(s_rows[s_view[0]].missing);
    TEST_ASSERT_TRUE(s_rows[s_view[s_view_count - 1]].missing);

    /* warn_first ascending: missing row pinned on top. */
    gui_rows_set_sort(ROW_SORT_BUILD, false, true);
    build_view();
    TEST_ASSERT_EQUAL_INT(5, s_view_count);
    TEST_ASSERT_TRUE(s_rows[s_view[0]].missing);

    /* warn_first descending: still pinned on top (direction-independent). */
    gui_rows_set_sort(ROW_SORT_BUILD, true, true);
    build_view();
    TEST_ASSERT_EQUAL_INT(5, s_view_count);
    TEST_ASSERT_TRUE(s_rows[s_view[0]].missing);
}

/* 6. Collapsing a folder hides its children (folder row stays); toggling again
 *    restores them; and an active filter overrides collapse so matches surface. */
void test_view_collapse_hides_children_and_filter_overrides(void) {
    tp_id128 folder_id = {{0}};
    add_sources_and_build(&folder_id, NULL);

    gui_rows_toggle_collapsed(folder_id);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder_id));
    build_view();
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder source + solo source */
    for (int k = 0; k < s_view_count; ++k) {
        TEST_ASSERT_TRUE(s_rows[s_view[k]].is_source); /* no children shown */
        TEST_ASSERT_EQUAL_INT(0, s_rows[s_view[k]].indent);
    }

    gui_rows_toggle_collapsed(folder_id); /* toggle restores children */
    TEST_ASSERT_FALSE(gui_rows_is_collapsed(folder_id));
    build_view();
    TEST_ASSERT_EQUAL_INT(s_row_count, s_view_count);

    /* Collapse again, then filter: the filter overrides collapse and the
     * matching child reappears under its (still-collapsed) parent. */
    gui_rows_toggle_collapsed(folder_id);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder_id));
    gui_rows_set_filter("beta");
    build_view();
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder parent + beta */
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_folder);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[1]].sprite_name);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder_id)); /* state unchanged */
}

/* 7. After the model
 *    changes, the primary selection is re-resolved by canonical ref (kept if the sprite survives,
 *    cleared if gone) and multi-select refs pointing at removed sprites are pruned. Here the "model
 *    change" is deleting a child file + rescanning, standing in for an undo that drops a sprite. */
void test_selection_revalidate_reresolves_primary_and_prunes_multi(void) {
    add_sources_and_build(NULL, NULL);
    build_view();

    const int ai = find_row_by_name("alpha");
    const int bi = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ai);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);

    /* primary = alpha; multi-select = {alpha, beta}. */
    s_sel_src = s_rows[ai].src;
    s_sel_child = s_rows[ai].child;
    multi_sel_clear();
    multi_sel_add_ref(s_rows[ai].source_id, s_rows[ai].source_key);
    multi_sel_add_ref(s_rows[bi].source_id, s_rows[bi].source_key);
    TEST_ASSERT_EQUAL_INT(2, s_multi_sel_count);

    gui_selection_capture_reselect(); /* capture alpha's ref BEFORE the model shifts */
    TEST_ASSERT_TRUE(s_reselect_pending);

    /* beta vanishes from disk; alpha survives. */
    TEST_ASSERT_EQUAL_INT(0, remove(s_beta));
    gui_project_invalidate_sources();
    build_rows();
    build_view();
    TEST_ASSERT_EQUAL_INT(-1, find_row_by_name("beta"));
    const int ai2 = find_row_by_name("alpha");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ai2);

    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);
    /* primary re-resolved to alpha's (possibly shifted) row indices. */
    TEST_ASSERT_EQUAL_INT(s_rows[ai2].src, s_sel_src);
    TEST_ASSERT_EQUAL_INT(s_rows[ai2].child, s_sel_child);
    /* beta pruned, alpha kept. */
    TEST_ASSERT_EQUAL_INT(1, s_multi_sel_count);
    TEST_ASSERT_TRUE(
        multi_sel_contains_ref(s_rows[ai2].source_id, s_rows[ai2].source_key));
}

/* View position (index into s_view) of a given s_rows index, or -1 if not shown. */
static int view_pos_of_row(int row_index) {
    for (int k = 0; k < s_view_count; ++k) {
        if (s_view[k] == row_index) {
            return k;
        }
    }
    return -1;
}

/* 8. gui_selection_revalidate, PRIMARY-DELETED branch: when the sprite carrying the primary selection
 *    is itself the one that vanishes, revalidate's `if (!found)` path clears the dangling primary to
 *    -1 (rather than snapping it onto a surviving neighbour). Here beta is the primary AND the removed
 *    child. */
void test_selection_revalidate_clears_primary_when_it_is_deleted(void) {
    add_sources_and_build(NULL, NULL);
    build_view();

    const int bi = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);
    s_sel_src = s_rows[bi].src; /* primary = beta (the child about to disappear) */
    s_sel_child = s_rows[bi].child;

    gui_selection_capture_reselect();
    TEST_ASSERT_TRUE(s_reselect_pending);

    TEST_ASSERT_EQUAL_INT(0, remove(s_beta)); /* the primary's backing file is gone */
    gui_project_invalidate_sources();
    build_rows();
    build_view();
    TEST_ASSERT_EQUAL_INT(-1, find_row_by_name("beta"));

    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);
    /* the captured primary no longer resolves -> cleared, never re-pinned to alpha/gamma. */
    TEST_ASSERT_EQUAL_INT(-1, s_sel_src);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_child);
    TEST_ASSERT_FALSE(s_sel_missing);
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view); /* nothing selected -> focus has no home */
}

/* 9. focus_sync_to_selection (new): after a revalidate re-resolves the primary onto a SHIFTED row,
 *    the keyboard focus (an s_view index) must re-pin to that row's new view position. And when the
 *    still-selected row is hidden (collapsed folder), focus falls back to -1. */
void test_selection_revalidate_resyncs_focus_to_shifted_row(void) {
    tp_id128 folder_id = {{0}};
    add_sources_and_build(&folder_id, NULL);
    build_view();

    const int gi = find_row_by_name("gamma"); /* last child -> a later beta-delete shifts it up */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    s_sel_src = s_rows[gi].src;
    s_sel_child = s_rows[gi].child;
    const int gview = view_pos_of_row(gi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gview);
    s_focus_view = gview; /* focus pinned on gamma's current view row */

    gui_selection_capture_reselect();
    TEST_ASSERT_TRUE(s_reselect_pending);

    /* a child AHEAD of gamma (beta) disappears -> gamma's row + view index both move up by one. */
    TEST_ASSERT_EQUAL_INT(0, remove(s_beta));
    gui_project_invalidate_sources();
    build_rows();
    build_view();

    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);

    const int gi2 = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi2);
    TEST_ASSERT_EQUAL_INT(s_rows[gi2].src, s_sel_src);
    TEST_ASSERT_EQUAL_INT(s_rows[gi2].child, s_sel_child);
    const int gview2 = view_pos_of_row(gi2);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gview2);
    TEST_ASSERT_EQUAL_INT(gview2, s_focus_view); /* focus followed gamma to its new view position */

    /* Now hide the still-selected row by collapsing its folder: the primary stays resolved in the row
     * model, but with no visible home the focus index re-pins to -1. */
    gui_rows_toggle_collapsed(folder_id);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder_id));
    gui_selection_capture_reselect();
    TEST_ASSERT_TRUE(s_reselect_pending);
    build_view();
    gui_selection_revalidate();
    TEST_ASSERT_EQUAL_INT(s_rows[gi2].src, s_sel_src); /* primary unchanged in the model */
    TEST_ASSERT_EQUAL_INT(s_rows[gi2].child, s_sel_child);
    TEST_ASSERT_EQUAL_INT(-1, view_pos_of_row(gi2)); /* gamma hidden by the collapse */
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);
}

/* 10. Folder/source PRIMARY preserved by STABLE id (new capture branch): a selected folder source row
 *     (s_sel_child == -1, no leaf ref) is captured by its source id with an EMPTY key, so an undo that
 *     shifts the source ordering re-resolves it onto the SAME folder, not a shifted neighbour. */
void test_selection_revalidate_folder_primary_follows_stable_id(void) {
    tp_id128 folder_id = {{0}};
    tp_id128 file_id = {{0}};
    add_sources_reversed_and_build(&folder_id, &file_id); /* file @ src 0, folder @ src 1 */
    build_view();

    int folder_row = -1;
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].is_source && s_rows[i].is_folder &&
            tp_id128_eq(s_rows[i].source_id, folder_id)) {
            folder_row = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, folder_row);
    TEST_ASSERT_EQUAL_INT(1, s_rows[folder_row].src); /* folder is source index 1 here */
    s_sel_src = s_rows[folder_row].src;
    s_sel_child = -1; /* primary is the folder/source row, not a leaf */

    gui_selection_capture_reselect();
    TEST_ASSERT_TRUE(s_reselect_pending);
    TEST_ASSERT_TRUE(tp_id128_eq(s_reselect_source_id, folder_id));
    TEST_ASSERT_EQUAL_INT('\0', s_reselect_key[0]); /* empty key == folder/source primary */

    /* remove the FILE source at index 0 -> the folder's source index shifts 1 -> 0. */
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(gui_project_snapshot(), 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas->id, file_id,
        tp_session_snapshot_revision(gui_project_snapshot())));
    gui_project_invalidate_sources();
    build_rows();
    build_view();

    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_child);
    TEST_ASSERT_EQUAL_INT(0, s_sel_src); /* index genuinely moved 1 -> 0 */
    /* and it is the SAME folder (by id), not whatever else landed at index 0. */
    int primary_row = -1;
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].is_source && s_rows[i].src == s_sel_src) {
            primary_row = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, primary_row);
    TEST_ASSERT_TRUE(s_rows[primary_row].is_folder);
    TEST_ASSERT_TRUE(tp_id128_eq(s_rows[primary_row].source_id, folder_id));
}

/* 11. ADDED sort (§61.1): source spans order by the order they were added to the project (source
 *     insertion index), independent of name or type. The reversed fixture adds the FILE first (added_at
 *     0) and the FOLDER second (added_at 1); ascending leads with the file, descending with the folder. */
void test_view_sort_added_orders_by_add_order(void) {
    add_sources_reversed_and_build(NULL, NULL); /* FILE @ src 0 (added first), FOLDER @ src 1 */

    gui_rows_set_sort(ROW_SORT_ADDED, false, false);
    build_view();
    /* ascending: the file source (added first) leads; the folder span follows. */
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_source);
    TEST_ASSERT_FALSE(s_rows[s_view[0]].is_folder);
    TEST_ASSERT_EQUAL_STRING("solo", s_rows[s_view[0]].sprite_name);
    TEST_ASSERT_EQUAL_INT(0, s_rows[s_view[0]].added_at);
    const int fpos_asc = view_folder_pos();
    TEST_ASSERT_GREATER_THAN_INT(0, fpos_asc); /* folder (added second) after the file */
    TEST_ASSERT_EQUAL_INT(1, s_rows[s_view[fpos_asc]].added_at);

    gui_rows_set_sort(ROW_SORT_ADDED, true, false);
    build_view();
    /* descending: the folder (added last) leads; the file trails. */
    TEST_ASSERT_EQUAL_INT(0, view_folder_pos());
    TEST_ASSERT_TRUE(s_rows[s_view[s_view_count - 1]].is_source);
    TEST_ASSERT_FALSE(s_rows[s_view[s_view_count - 1]].is_folder);
    TEST_ASSERT_EQUAL_STRING("solo", s_rows[s_view[s_view_count - 1]].sprite_name);
}

/* 12. Empty state: an atlas with zero sources yields an empty row model + view, and revalidate +
 *     focus-sync stay crash-free (focus -> -1) against that empty s_view. */
void test_view_empty_state_is_safe(void) {
    s_sel_atlas = 0; /* default project: one atlas, no sources added */
    build_rows();
    build_view();
    TEST_ASSERT_EQUAL_INT(0, s_row_count);
    TEST_ASSERT_EQUAL_INT(0, s_view_count);

    /* Arm a stale primary + focus, then revalidate against the empty view: focus_sync must scan the
     * empty s_view without dereferencing anything and settle on -1. */
    s_sel_src = 0;
    s_sel_child = -1;
    s_focus_view = 5;
    s_reselect_pending = true;
    s_reselect_source_id = tp_id128_nil();
    s_reselect_key[0] = '\0';
    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);
}

/* 13. collapsed_prune_missing: a collapse entry for a folder source that is later REMOVED from the
 *     atlas must be dropped on the next rebuild, so the collapsed set never accumulates stale ids and
 *     the surviving sources are unaffected. */
void test_view_collapse_pruned_when_folder_source_removed(void) {
    tp_id128 folder_id = {{0}};
    tp_id128 file_id = {{0}};
    add_sources_and_build(&folder_id, &file_id); /* folder @ src 0, solo file @ src 1 */
    build_view();

    gui_rows_toggle_collapsed(folder_id);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder_id));
    build_view();
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder source + solo (children hidden) */

    /* remove the folder source entirely, then rebuild the row model + view. */
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(gui_project_snapshot(), 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas->id, folder_id,
        tp_session_snapshot_revision(gui_project_snapshot())));
    gui_project_invalidate_sources();
    build_rows();
    build_view();

    /* the stale collapse entry was pruned; only the surviving file source remains, untouched. */
    TEST_ASSERT_FALSE(gui_rows_is_collapsed(folder_id));
    TEST_ASSERT_EQUAL_INT(1, s_row_count);
    TEST_ASSERT_EQUAL_INT(1, s_view_count);
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_source);
    TEST_ASSERT_FALSE(s_rows[s_view[0]].is_folder);
    TEST_ASSERT_TRUE(tp_id128_eq(s_rows[s_view[0]].source_id, file_id));
}

/* 14. A missing file source selected as the primary keeps its missing state
 * through revalidation. */
void test_selection_revalidate_keeps_missing_state_on_source_reselect(void) {
    add_sources_and_build(NULL, NULL);

    TEST_ASSERT_EQUAL_INT(0, remove(s_solo)); /* solo.png gone -> its file source goes missing */
    gui_project_invalidate_sources();
    build_rows();
    build_view();

    int mi = -1; /* the missing file-source row */
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].is_source && !s_rows[i].is_folder && s_rows[i].missing) {
            mi = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, mi);
    const tp_id128 missing_id = s_rows[mi].source_id;

    /* primary = the missing source row (no leaf ref -> captured by stable id with an empty key). */
    s_sel_src = s_rows[mi].src;
    s_sel_child = -1;
    gui_selection_capture_reselect();
    TEST_ASSERT_TRUE(s_reselect_pending);
    TEST_ASSERT_EQUAL_INT('\0', s_reselect_key[0]);
    TEST_ASSERT_TRUE(tp_id128_eq(s_reselect_source_id, missing_id));

    s_sel_missing = false; /* stand in for the spurious clear the source branch used to hardcode */
    gui_selection_revalidate();
    TEST_ASSERT_FALSE(s_reselect_pending);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_child);
    TEST_ASSERT_TRUE(s_sel_missing); /* Missing state survives source reselect. */

    int mi2 = -1;
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].is_source && s_rows[i].src == s_sel_src) {
            mi2 = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, mi2);
    TEST_ASSERT_TRUE(s_rows[mi2].missing);
    TEST_ASSERT_TRUE(tp_id128_eq(s_rows[mi2].source_id, missing_id));
}

/* 15. When the keyboard-focused (or shift-anchor) row is filtered out of the rebuilt view, its
 *     stale numeric index must be cleared to -1, never left aliasing a different visible row. A
 *     surviving focused row still re-pins to its new view position. */
void test_view_focus_cleared_when_focused_row_filtered_out(void) {
    add_sources_and_build(NULL, NULL);
    build_view(); /* identity; view cache now valid so the re-pin path engages on the next build */

    const int alpha = find_row_by_name("alpha");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, alpha);
    const int alpha_view = view_pos_of_row(alpha);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, alpha_view);
    s_focus_view = alpha_view;      /* focus + anchor both pinned on a folder child */
    s_sel_anchor_row = alpha_view;

    gui_rows_set_filter("solo"); /* only the solo file source survives; the whole folder span drops */
    build_view();
    TEST_ASSERT_EQUAL_INT(-1, view_pos_of_row(alpha)); /* alpha gone from the view */
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);           /* Stale focus cannot alias another row. */
    TEST_ASSERT_EQUAL_INT(-1, s_sel_anchor_row);       /* The range anchor is cleared too. */

    /* Positive control: a SURVIVING focused row re-pins to its new position under the filter. */
    gui_rows_set_filter("");
    build_view();
    const int solo = find_row_by_name("solo");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, solo);
    s_focus_view = view_pos_of_row(solo);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, s_focus_view);
    gui_rows_set_filter("solo");
    build_view();
    const int solo_view = view_pos_of_row(solo);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, solo_view);
    TEST_ASSERT_EQUAL_INT(solo_view, s_focus_view); /* focus followed the surviving row */
}

/* 16. Folder collapse is keyed by stable source id and must survive an atlas round-trip. Two
 *     atlases each carry a folder source; snapshot-wide pruning must retain atlas A's live source
 *     while atlas B is displayed. */
void test_view_collapse_survives_atlas_switch(void) {
    tp_id128 folder0 = {{0}};
    add_sources_and_build(&folder0, NULL); /* A0: folder @ src 0, solo @ src 1 */

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas()); /* A1 at index 1 */
    const tp_session_snapshot *snap = gui_project_snapshot();
    const tp_snapshot_atlas *a1 = tp_session_snapshot_atlas_at(snap, 1);
    TEST_ASSERT_NOT_NULL(a1);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(a1->id, tp_session_snapshot_revision(snap),
                                    s_pack_dir, TP_SOURCE_KIND_FOLDER));
    gui_project_invalidate_sources();

    /* View A0, collapse its folder. */
    s_sel_atlas = 0;
    build_rows();
    build_view();
    gui_rows_toggle_collapsed(folder0);
    build_view();
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder0));
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder (collapsed) + solo */

    /* Switch to A1 (B). Building B must NOT prune A0's collapse id. */
    s_sel_atlas = 1;
    build_rows();
    build_view();
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder0)); /* Survives the switch. */

    /* Back to A0: still collapsed, children still hidden. */
    s_sel_atlas = 0;
    build_rows();
    build_view();
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(folder0));
    TEST_ASSERT_EQUAL_INT(2, s_view_count);
}

/* 17. The viewed atlas is preserved by stable id across an Undo that re-inserts an atlas before it.
 *     Two atlases; view A1 with a sprite selected; remove A0 (A1 shifts 1->0); Undo re-inserts A0 (A1
 *     back to 1). do_undo's settle must re-resolve s_sel_atlas onto A1 by id (not leave the positional
 *     index on the now-different atlas), so the sprite selection re-resolves in the correct atlas. */
void test_undo_preserves_selected_atlas_by_stable_id(void) {
    const tp_session_snapshot *snap = gui_project_snapshot();
    const tp_snapshot_atlas *a0 = tp_session_snapshot_atlas_at(snap, 0);
    TEST_ASSERT_NOT_NULL(a0);
    const tp_id128 a0_id = a0->id;
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(a0_id, tp_session_snapshot_revision(snap),
                                    s_pack_dir, TP_SOURCE_KIND_FOLDER));

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas()); /* A1 at index 1 */
    snap = gui_project_snapshot();
    const tp_snapshot_atlas *a1 = tp_session_snapshot_atlas_at(snap, 1);
    TEST_ASSERT_NOT_NULL(a1);
    const tp_id128 a1_id = a1->id;
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(a1_id, tp_session_snapshot_revision(snap),
                                    s_pack_dir, TP_SOURCE_KIND_FOLDER));
    gui_project_invalidate_sources();

    /* View A1 (index 1); select the beta leaf in it. */
    s_sel_atlas = 1;
    build_rows();
    build_view();
    int bi = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);
    s_sel_src = s_rows[bi].src;
    s_sel_child = s_rows[bi].child;

    /* Remove A0 (the op we will undo): A1 shifts index 1 -> 0. */
    snap = gui_project_snapshot();
    TEST_ASSERT_TRUE(
        gui_project_remove_atlas(a0_id, tp_session_snapshot_revision(snap)));
    TEST_ASSERT_TRUE(tp_id128_eq(
        tp_session_snapshot_atlas_at(gui_project_snapshot(), 0)->id, a1_id));
    /* Re-establish a clean selection while viewing A1 at its new index 0. */
    s_sel_atlas = 0;
    build_rows();
    build_view();
    bi = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);
    s_sel_src = s_rows[bi].src;
    s_sel_child = s_rows[bi].child;

    /* Undo the removal: A0 re-inserted at 0, A1 shifts back to 1. */
    do_undo();
    TEST_ASSERT_TRUE(s_reselect_pending);
    /* Complete the frame loop that follows an undo: rebuild + revalidate the preserved selection. */
    build_rows();
    build_view();
    gui_selection_revalidate();

    /* The viewed atlas is A1 again, not the positional neighbor A0. */
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(1, s_sel_atlas);
    TEST_ASSERT_TRUE(tp_id128_eq(
        tp_session_snapshot_atlas_at(gui_project_snapshot(), 1)->id, a1_id));
    /* and the beta selection survived, re-resolved in the correct atlas (not cleared). */
    const int bi2 = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi2);
    TEST_ASSERT_EQUAL_INT(s_rows[bi2].src, s_sel_src);
    TEST_ASSERT_EQUAL_INT(s_rows[bi2].child, s_sel_child);
}

/* 18. SIZE sort (§61.1): run the real blocking pack, then force the row cache
 *     through its pack-version key. The areas below come only from production
 *     canonical lookup (gui_pack_result -> gui_pack_find_sprite_ref), so a
 *     broken publication, invalidation, or source/key join fails this test. */
void test_view_sort_size_orders_by_packed_area(void) {
    add_sources_and_build(NULL, NULL); /* folder children alpha,beta,gamma + solo */
    build_view();

    for (int i = 0; i < s_row_count; ++i) {
        TEST_ASSERT_EQUAL_INT(0, (int)s_rows[i].size); /* unpacked atlas -> no region -> area 0 */
    }

    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_VIEW_TEST_DIR));
    do_pack_blocking();
    TEST_ASSERT_NOT_NULL(gui_pack_result(0));
    build_rows(); /* pack-result version changed: production cache must rebuild */

    const int ai = find_row_by_name("alpha");
    const int bi = find_row_by_name("beta");
    const int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ai);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);

    /* Packed areas are intentionally resolved only when SIZE is active. This
     * drives the production lazy lookup after the pack-version rebuild. */
    gui_rows_set_sort(ROW_SORT_SIZE, false, false);
    build_view();
    const tp_result *packed = gui_pack_result(0);
    const int ap = gui_pack_find_sprite_ref(
        0, s_rows[ai].source_id, s_rows[ai].source_key);
    const int bp = gui_pack_find_sprite_ref(
        0, s_rows[bi].source_id, s_rows[bi].source_key);
    const int gp = gui_pack_find_sprite_ref(
        0, s_rows[gi].source_id, s_rows[gi].source_key);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ap);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bp);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gp);
    TEST_ASSERT_EQUAL_INT(packed->sprites[ap].frame.w *
                              packed->sprites[ap].frame.h,
                          (int)s_rows[ai].size);
    TEST_ASSERT_EQUAL_INT(packed->sprites[bp].frame.w *
                              packed->sprites[bp].frame.h,
                          (int)s_rows[bi].size);
    TEST_ASSERT_EQUAL_INT(packed->sprites[gp].frame.w *
                              packed->sprites[gp].frame.h,
                          (int)s_rows[gi].size);

    int pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 1]].sprite_name);  /* 1 */
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 2]].sprite_name); /* 4 */
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 3]].sprite_name); /* 6 */

    gui_rows_set_sort(ROW_SORT_SIZE, true, false);
    build_view();
    pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 1]].sprite_name); /* 6 */
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 2]].sprite_name); /* 4 */
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 3]].sprite_name);  /* 1 */
}

/* 19. MTIME sort (§61.1): orders leaves by live file mtime. build_rows populates mtime for existing
 *     files (> 0); the comparator is then exercised with controlled mtimes (granularity-independent). */
void test_view_sort_mtime_orders_by_modification_time(void) {
    add_sources_and_build(NULL, NULL);
    build_view();

    const int ai = find_row_by_name("alpha");
    const int bi = find_row_by_name("beta");
    const int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ai);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    TEST_ASSERT_TRUE(s_rows[ai].mtime > 0); /* existing files carry a real live mtime */

    s_rows[ai].mtime = 3000; /* controlled: gamma oldest, beta newest */
    s_rows[bi].mtime = 3000000;
    s_rows[gi].mtime = 1000;

    gui_rows_set_sort(ROW_SORT_MTIME, false, false);
    build_view();
    const int pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 1]].sprite_name); /* 1000 */
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 2]].sprite_name); /* 3000 */
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 3]].sprite_name);  /* 3000000 */
}

/* 20. A rename override changes the effective name used by both sort and Copy name. gamma is the
 *     natural-last child; renaming it to "aaa" makes it sort FIRST and copy as "aaa", while its
 *     canonical export key stays "gamma". */
void test_view_sort_and_copy_use_effective_rename(void) {
    add_sources_and_build(NULL, NULL);

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    const gui_sprite_ref ref = {atlas->id, s_rows[gi].source_id,
                                s_rows[gi].source_key,
                                tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_sprite_rename(&ref, "aaa"));

    build_rows(); /* model changed -> row cache rebuilds; the override now carries the rename */
    build_view();

    gi = find_row_by_name("gamma"); /* find_row_by_name matches the canonical key, still "gamma" */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    /* effective name (what Copy name copies) is the rename, not the canonical key. */
    TEST_ASSERT_EQUAL_STRING("aaa", gui_rows_effective_name(&s_rows[gi]));

    gui_rows_set_sort(ROW_SORT_NAME, false, false);
    build_view();
    const int pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    /* "aaa" (renamed gamma) now sorts ahead of "alpha" and "beta". */
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[pos + 1]].sprite_name); /* row identity unchanged */
    TEST_ASSERT_EQUAL_STRING("aaa", gui_rows_effective_name(&s_rows[s_view[pos + 1]]));
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[pos + 2]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[pos + 3]].sprite_name);
}

/* 21. §61.1 four-key sort UI mapping: from Name-asc, re-clicking Name flips it to Name-desc and back to
 *     Name-asc; clicking Size (a different key) selects Size-asc; re-clicking Size flips it to Size-desc.
 *     The warn-first pin is independent of key clicks. This invokes the same
 *     production helper used by declare_sort_chips; there is no test mirror. */
void test_sort_chip_click_selects_and_flips(void) {
    gui_rows_set_sort(ROW_SORT_NAME, false, false); /* default: Name ascending, warn-first off */
    row_sort_key key = ROW_SORT_SIZE;
    bool desc = true;
    bool warn = true;

    gui_rows_sort_chip_click(ROW_SORT_NAME); /* re-click active Name: asc -> desc */
    gui_rows_get_sort(&key, &desc, &warn);
    TEST_ASSERT_EQUAL_INT(ROW_SORT_NAME, key);
    TEST_ASSERT_TRUE(desc);
    TEST_ASSERT_FALSE(warn);

    gui_rows_sort_chip_click(ROW_SORT_NAME); /* re-click active Name again: desc -> asc */
    gui_rows_get_sort(&key, &desc, &warn);
    TEST_ASSERT_EQUAL_INT(ROW_SORT_NAME, key);
    TEST_ASSERT_FALSE(desc);

    gui_rows_sort_chip_click(ROW_SORT_SIZE); /* different key: ascending */
    gui_rows_get_sort(&key, &desc, &warn);
    TEST_ASSERT_EQUAL_INT(ROW_SORT_SIZE, key);
    TEST_ASSERT_FALSE(desc);

    gui_rows_sort_chip_click(ROW_SORT_SIZE); /* active Size: asc -> desc */
    gui_rows_get_sort(&key, &desc, &warn);
    TEST_ASSERT_EQUAL_INT(ROW_SORT_SIZE, key);
    TEST_ASSERT_TRUE(desc);
    TEST_ASSERT_FALSE(warn); /* key clicks never disturb the warn-first pin */
}

/* 22. Keyboard focus follows the selected sprite across a model-rebuild
 * reorder instead of retaining a numeric slot that now names another row. */
void test_view_focus_follows_selection_across_model_rebuild(void) {
    add_sources_and_build(NULL, NULL);
    gui_rows_set_sort(ROW_SORT_NAME, false, false);
    build_view();

    int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    s_sel_src = s_rows[gi].src; /* primary selection = gamma */
    s_sel_child = s_rows[gi].child;
    const int gview = view_pos_of_row(gi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gview);
    s_focus_view = gview; /* focus pinned on gamma's current (natural-last) view slot */

    /* Rename gamma -> "aaa": bumps the model generation (rebuild) and makes it sort first. */
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const gui_sprite_ref ref = {atlas->id, s_rows[gi].source_id, s_rows[gi].source_key,
                                tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_sprite_rename(&ref, "aaa"));

    build_rows(); /* model generation bumped -> row cache rebuilds */
    build_view(); /* model-rebuild path: view-only re-pin skipped, focus re-anchored at the end */

    gi = find_row_by_name("gamma"); /* row identity/canonical key unchanged by the rename */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    const int pos = view_folder_pos();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, pos);
    const int gview2 = view_pos_of_row(gi);
    TEST_ASSERT_EQUAL_INT(pos + 1, gview2);      /* renamed gamma now leads its siblings */
    TEST_ASSERT_TRUE(gview2 != gview);           /* its view slot genuinely moved */
    TEST_ASSERT_EQUAL_INT(gview2, s_focus_view); /* Focus followed gamma. */
}

/* A model rebuild can reorder rows while the Shift-range anchor is stored as
 * a view index. The anchor must follow the same canonical sprite, not keep an
 * index that now names a different row. */
void test_view_anchor_follows_sprite_across_model_rebuild(void) {
    add_sources_and_build(NULL, NULL);
    gui_rows_set_sort(ROW_SORT_NAME, false, false);
    build_view();

    int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    const int gview = view_pos_of_row(gi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gview);
    s_sel_anchor_row = gview;

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const gui_sprite_ref ref = {
        atlas->id, s_rows[gi].source_id, s_rows[gi].source_key,
        tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_sprite_rename(&ref, "aaa"));

    build_rows();
    build_view();

    gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    const int gview2 = view_pos_of_row(gi);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gview2);
    TEST_ASSERT_TRUE(gview2 != gview);
    TEST_ASSERT_EQUAL_INT(gview2, s_sel_anchor_row);
}

/* The OOM identity projection is a different row ordering. Numeric interaction
 * indices from the abandoned projection must not survive into it. */
void test_view_oom_fallback_clears_focus_and_anchor(void) {
    add_sources_and_build(NULL, NULL);
    s_focus_view = 1;
    s_sel_anchor_row = 2;

    gui_rows_test_fail_next_view_alloc();
    build_view();

    TEST_ASSERT_EQUAL_INT(s_row_count, s_view_count);
    for (int i = 0; i < s_view_count; ++i) {
        TEST_ASSERT_EQUAL_INT(i, s_view[i]);
    }
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_anchor_row);
}

/* 23. Ctrl+F must find a sprite by a long rename whose searchable tail is truncated out of the
 *     224-byte display label. gamma is renamed to 250 'a's + a unique marker; the marker sits past the
 *     label cutoff (and is absent from the canonical key "gamma"), so ONLY the effective-name search
 *     added to row_matches_filter can surface the row. */
void test_view_filter_finds_long_rename_beyond_label(void) {
    add_sources_and_build(NULL, NULL);

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    int gi = find_row_by_name("gamma");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);

    /* 250 'a's then a unique marker -> length 257 > 223, so the marker lands past the label truncation. */
    char long_rename[258];
    memset(long_rename, 'a', 250);
    memcpy(long_rename + 250, "zqmark9", sizeof "zqmark9"); /* 7 marker chars + NUL */
    TEST_ASSERT_EQUAL_INT(257, (int)strlen(long_rename));

    const gui_sprite_ref ref = {atlas->id, s_rows[gi].source_id, s_rows[gi].source_key,
                                tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_sprite_rename(&ref, long_rename));

    build_rows();
    build_view();

    gi = find_row_by_name("gamma"); /* canonical key still "gamma" */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, gi);
    /* the full rename is retained (it would overflow the 224-byte label) and the marker is NOT in the
     * label -- so label + canonical-key search alone cannot find it. */
    TEST_ASSERT_EQUAL_STRING(long_rename, gui_rows_effective_name(&s_rows[gi]));
    TEST_ASSERT_NULL(strstr(s_rows[gi].label, "zqmark9"));
    TEST_ASSERT_NULL(strstr(s_rows[gi].sprite_name, "zqmark9"));

    /* Filtering by the marker (present only in the effective rename) surfaces gamma + its parent. */
    gui_rows_set_filter("zqmark9");
    build_view();
    TEST_ASSERT_EQUAL_INT(2, s_view_count); /* folder parent + gamma */
    TEST_ASSERT_TRUE(s_rows[s_view[0]].is_folder);
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[1]].sprite_name);
}

/* 24. A short left panel must budget its two bounded list caps only
 *     after reserving fixed chrome, gaps/padding, and at least two sprite
 *     rows. This is the worst case where both capped lists exist. */
static void assert_short_panel_keeps_sprite_rows(float panel_height,
                                                 float scale,
                                                 bool filter_visible) {
    const float cap =
        gui_rows_left_section_cap(panel_height, scale, filter_visible);
    const gui_left_layout_budget budget =
        gui_left_layout_budget_make(scale, filter_visible);
    const float worst_case_used =
        budget.padding + budget.fixed_chrome + budget.gaps +
        budget.sprite_min + 2.0F * cap;

    TEST_ASSERT_TRUE(cap >= budget.sprite_min);
    TEST_ASSERT_TRUE(worst_case_used <= panel_height + 0.01F);
}

void test_left_section_caps_preserve_sprite_vlist_at_short_heights(void) {
    assert_short_panel_keeps_sprite_rows(466.0F, 1.0F, false);
    assert_short_panel_keeps_sprite_rows(466.0F, 1.0F, true);
    assert_short_panel_keeps_sprite_rows(500.0F, 1.0F, false);
    assert_short_panel_keeps_sprite_rows(500.0F, 1.0F, true);
    assert_short_panel_keeps_sprite_rows(699.0F, 1.5F, false);
    assert_short_panel_keeps_sprite_rows(699.0F, 1.5F, true);
}

/* 25. nt_ui_vlist recycles a bounded id ring by VIEW slot. An engine-level
 *     double-click is therefore only actionable when the prior pressed row's
 *     canonical {source_id, source_key} is still the same after a view remap. */
void test_recycled_view_id_double_click_requires_same_canonical_row(void) {
    add_sources_and_build(NULL, NULL);
    const int ai = find_row_by_name("alpha");
    const int bi = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, ai);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, bi);

    const sprite_row *alpha = &s_rows[ai];
    const sprite_row *beta = &s_rows[bi];
    TEST_ASSERT_TRUE(tp_id128_eq(alpha->source_id, beta->source_id));
    TEST_ASSERT_NOT_EQUAL(0, strcmp(alpha->source_key, beta->source_key));

    gui_rows_double_click_reset();
    TEST_ASSERT_FALSE(gui_rows_double_click_press(
        alpha->source_id, alpha->source_key, false));
    TEST_ASSERT_TRUE(gui_rows_double_click_press(
        alpha->source_id, alpha->source_key, true));

    gui_rows_double_click_reset();
    TEST_ASSERT_FALSE(gui_rows_double_click_press(
        alpha->source_id, alpha->source_key, false));
    TEST_ASSERT_FALSE(gui_rows_double_click_press(
        beta->source_id, beta->source_key, true)); /* recycled id, remapped row */

    /* The mismatched engine pair was consumed. A fresh B pair must still work. */
    TEST_ASSERT_FALSE(gui_rows_double_click_press(
        beta->source_id, beta->source_key, false));
    TEST_ASSERT_TRUE(gui_rows_double_click_press(
        beta->source_id, beta->source_key, true));
}

/* 26. Focus-follow must reason about the actual viewport, not the vlist's
 *     overscanned render window. With five complete rows visible and three
 *     overscan rows, focus on row 7 is rendered but still below the viewport. */
void test_focus_scroll_top_ignores_overscan_rows(void) {
    TEST_ASSERT_EQUAL_INT(
        3, gui_rows_focus_scroll_top(7, 0, 5));
    TEST_ASSERT_EQUAL_INT(
        -1, gui_rows_focus_scroll_top(4, 0, 5));
    TEST_ASSERT_EQUAL_INT(
        6, gui_rows_focus_scroll_top(6, 9, 5));
}

/* 27. A selected child can temporarily disappear from the projection under a
 *     filter or collapse. When the view reveals it again, keyboard focus must
 *     return to that selected row rather than remaining detached at -1. */
void test_view_focus_returns_when_selected_row_is_revealed(void) {
    tp_id128 folder_id = {{0}};
    add_sources_and_build(&folder_id, NULL);
    build_view();

    const int alpha = find_row_by_name("alpha");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, alpha);
    s_sel_src = s_rows[alpha].src;
    s_sel_child = s_rows[alpha].child;
    s_focus_view = view_pos_of_row(alpha);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, s_focus_view);

    gui_rows_set_filter("solo");
    build_view();
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);
    gui_rows_set_filter("");
    build_view();
    TEST_ASSERT_EQUAL_INT(view_pos_of_row(alpha), s_focus_view);

    gui_rows_toggle_collapsed(folder_id);
    build_view();
    TEST_ASSERT_EQUAL_INT(-1, s_focus_view);
    gui_rows_toggle_collapsed(folder_id);
    build_view();
    TEST_ASSERT_EQUAL_INT(view_pos_of_row(alpha), s_focus_view);
}

/* 28. Collapse state is session-global across atlases, so pruning must compare
 *     against the whole snapshot. Removing an atlas while another atlas is
 *     displayed must discard the deleted atlas's retained source ids. */
void test_view_collapse_prunes_sources_owned_by_deleted_atlas(void) {
    tp_id128 deleted_folder_id = {{0}};
    add_sources_and_build(&deleted_folder_id, NULL);
    build_view();
    gui_rows_toggle_collapsed(deleted_folder_id);
    TEST_ASSERT_TRUE(gui_rows_is_collapsed(deleted_folder_id));

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas());
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *kept_atlas =
        tp_session_snapshot_atlas_at(snapshot, 1);
    TEST_ASSERT_NOT_NULL(kept_atlas);
    const tp_id128 kept_atlas_id = kept_atlas->id;
    s_sel_atlas = 1;
    build_rows();
    build_view();

    snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *deleted_atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(deleted_atlas);
    TEST_ASSERT_TRUE(gui_project_remove_atlas(
        deleted_atlas->id, tp_session_snapshot_revision(snapshot)));
    TEST_ASSERT_TRUE(tp_id128_eq(
        tp_session_snapshot_atlas_at(gui_project_snapshot(), 0)->id,
        kept_atlas_id));

    s_sel_atlas = 0;
    build_rows();
    build_view();
    TEST_ASSERT_FALSE(gui_rows_is_collapsed(deleted_folder_id));
}

/* 29. Atlas and animation row ids are positional and can be recycled after
 *     Undo. Double-click dispatch therefore validates the stable entity id. */
void test_recycled_entity_double_click_requires_same_stable_id(void) {
    gui_rows_entity_double_click_ref ref = {0};
    tp_id128 a = {{1}};
    tp_id128 b = {{2}};

    TEST_ASSERT_FALSE(gui_rows_entity_double_click_press(&ref, a, false));
    TEST_ASSERT_FALSE(gui_rows_entity_double_click_press(&ref, b, true));
    TEST_ASSERT_FALSE(gui_rows_entity_double_click_press(&ref, b, false));
    TEST_ASSERT_TRUE(gui_rows_entity_double_click_press(&ref, b, true));

    gui_rows_entity_double_click_reset(&ref);
    TEST_ASSERT_FALSE(gui_rows_entity_double_click_press(&ref, a, false));
    TEST_ASSERT_TRUE(gui_rows_entity_double_click_press(&ref, a, true));
}

/* 30. Once Refresh invalidates source runtime state, any later fingerprint
 *     failure must still make the retained preview stale. */
void test_refresh_failure_after_invalidation_requires_stale_preview(void) {
    TEST_ASSERT_FALSE(gui_actions_refresh_should_mark_stale(
        TP_STATUS_OOM, false));
    TEST_ASSERT_TRUE(gui_actions_refresh_should_mark_stale(
        TP_STATUS_OOM, true));
    TEST_ASSERT_TRUE(gui_actions_refresh_should_mark_stale(
        TP_STATUS_OK, true));
}

/* 31. Region-to-row selection must resolve against the displayed result, not
 *     an unrelated native result whose sprite ordering may differ. */
void test_result_region_selection_uses_provided_result(void) {
    add_sources_and_build(NULL, NULL);
    build_view();
    const int alpha = find_row_by_name("alpha");
    const int beta = find_row_by_name("beta");
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, alpha);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, beta);

    char alpha_name[TP_PACK_INTERNAL_NAME_CAP];
    char beta_name[TP_PACK_INTERNAL_NAME_CAP];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(
            s_rows[alpha].source_id, s_rows[alpha].source_key,
            alpha_name, sizeof alpha_name, NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(
            s_rows[beta].source_id, s_rows[beta].source_key,
            beta_name, sizeof beta_name, NULL));
    tp_sprite sprites[2] = {
        {.name = beta_name},
        {.name = alpha_name},
    };
    const tp_result displayed = {
        .sprites = sprites,
        .sprite_count = 2,
    };

    s_sel_src = -1;
    s_sel_child = -1;
    select_row_for_result_region(&displayed, 0);
    TEST_ASSERT_EQUAL_INT(s_rows[beta].src, s_sel_src);
    TEST_ASSERT_EQUAL_INT(s_rows[beta].child, s_sel_child);
}

/* 32. Primary tree selection maps against the displayed result, while a
 *     folder/source primary clears any stale region outline. */
void test_primary_row_mapping_uses_displayed_result_and_rejects_non_leaf(void) {
    add_sources_and_build(NULL, NULL);
    const int alpha = find_row_by_name("alpha");
    const int beta = find_row_by_name("beta");
    int folder = -1;
    for (int i = 0; i < s_row_count; ++i) {
        if (s_rows[i].is_folder) {
            folder = i;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, alpha);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, beta);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, folder);

    char alpha_name[TP_PACK_INTERNAL_NAME_CAP];
    char beta_name[TP_PACK_INTERNAL_NAME_CAP];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(
            s_rows[alpha].source_id, s_rows[alpha].source_key,
            alpha_name, sizeof alpha_name, NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(
            s_rows[beta].source_id, s_rows[beta].source_key,
            beta_name, sizeof beta_name, NULL));
    tp_sprite sprites[2] = {
        {.name = beta_name},
        {.name = alpha_name},
    };
    const tp_result displayed = {
        .sprites = sprites,
        .sprite_count = 2,
    };
    TEST_ASSERT_EQUAL_INT(
        1, gui_rows_result_region_for_primary(&s_rows[alpha], &displayed));
    TEST_ASSERT_EQUAL_INT(
        -1, gui_rows_result_region_for_primary(&s_rows[folder], &displayed));
    TEST_ASSERT_EQUAL_INT(
        -1, gui_rows_result_region_for_primary(&s_rows[alpha], NULL));
}

/* 33. Region zoom uses transformed placed dimensions and centers that region
 *     in the current canvas without changing modes or requiring GL. */
void test_canvas_zoom_to_sprite_centers_transformed_region(void) {
    tp_page page = {.w = 100, .h = 80};
    tp_sprite sprite = {
        .page = 0,
        .frame = {.x = 20, .y = 10, .w = 10, .h = 20},
        .transform = TP_TRANSFORM_DIAGONAL,
    };
    tp_result result = {
        .pages = &page,
        .page_count = 1,
        .sprites = &sprite,
        .sprite_count = 1,
    };
    gui_canvas canvas = {
        .result = &result,
        .page_count = 1,
        .cur_page = 0,
        .scale = 1.0F,
        .fit_pending = true,
    };
    canvas.page_w[0] = page.w;
    canvas.page_h[0] = page.h;
    const float box[4] = {0.0F, 0.0F, 200.0F, 100.0F};

    TEST_ASSERT_TRUE(gui_canvas_zoom_to_sprite(&canvas, box, 0));
    TEST_ASSERT_TRUE(fabsf(canvas.scale - 9.6F) < 0.0001F);
    TEST_ASSERT_FALSE(canvas.fit_pending);

    int32_t out_w = 0;
    int32_t out_h = 0;
    tp_transform_out_dims(sprite.transform, sprite.frame.w, sprite.frame.h,
                          &out_w, &out_h);
    const float page_origin_x =
        box[0] + box[2] * 0.5F + canvas.cam_x -
        (float)page.w * canvas.scale * 0.5F;
    const float page_origin_y =
        box[1] + box[3] * 0.5F + canvas.cam_y -
        (float)page.h * canvas.scale * 0.5F;
    const float projected_center_x =
        page_origin_x +
        ((float)sprite.frame.x + (float)out_w * 0.5F) * canvas.scale;
    const float projected_center_y =
        page_origin_y +
        ((float)sprite.frame.y + (float)out_h * 0.5F) * canvas.scale;
    TEST_ASSERT_TRUE(fabsf(projected_center_x -
                           (box[0] + box[2] * 0.5F)) < 0.0001F);
    TEST_ASSERT_TRUE(fabsf(projected_center_y -
                           (box[1] + box[3] * 0.5F)) < 0.0001F);

    const float old_scale = canvas.scale;
    const float old_cam_x = canvas.cam_x;
    const float old_cam_y = canvas.cam_y;
    TEST_ASSERT_FALSE(gui_canvas_zoom_to_sprite(&canvas, box, 1));
    TEST_ASSERT_TRUE(canvas.scale == old_scale);
    TEST_ASSERT_TRUE(canvas.cam_x == old_cam_x);
    TEST_ASSERT_TRUE(canvas.cam_y == old_cam_y);

    gui_canvas_double_click_ref click_ref = {0};
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &result, 0, false));
    TEST_ASSERT_TRUE(gui_canvas_double_click_press(
        &click_ref, &result, 0, true));
    gui_canvas_double_click_reset(&click_ref);
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &result, 0, false));
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &result, 1, true));
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &result, 1, false));
    TEST_ASSERT_TRUE(gui_canvas_double_click_press(
        &click_ref, &result, 1, true));

    tp_result replacement = result;
    gui_canvas_double_click_reset(&click_ref);
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &result, 0, false));
    TEST_ASSERT_FALSE(gui_canvas_double_click_press(
        &click_ref, &replacement, 0, true));
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_view_passthrough_is_identity);
    RUN_TEST(test_view_filter_is_case_insensitive_and_keeps_parent);
    RUN_TEST(test_view_filter_no_match_empties_view);
    RUN_TEST(test_view_sort_name_orders_children_asc_and_desc);
    RUN_TEST(test_view_warn_first_pins_missing_regardless_of_direction);
    RUN_TEST(test_view_collapse_hides_children_and_filter_overrides);
    RUN_TEST(test_selection_revalidate_reresolves_primary_and_prunes_multi);
    RUN_TEST(test_selection_revalidate_clears_primary_when_it_is_deleted);
    RUN_TEST(test_selection_revalidate_resyncs_focus_to_shifted_row);
    RUN_TEST(test_selection_revalidate_folder_primary_follows_stable_id);
    RUN_TEST(test_view_sort_added_orders_by_add_order);
    RUN_TEST(test_view_empty_state_is_safe);
    RUN_TEST(test_view_collapse_pruned_when_folder_source_removed);
    RUN_TEST(test_selection_revalidate_keeps_missing_state_on_source_reselect);
    RUN_TEST(test_view_focus_cleared_when_focused_row_filtered_out);
    RUN_TEST(test_view_collapse_survives_atlas_switch);
    RUN_TEST(test_undo_preserves_selected_atlas_by_stable_id);
    RUN_TEST(test_view_sort_size_orders_by_packed_area);
    RUN_TEST(test_view_sort_mtime_orders_by_modification_time);
    RUN_TEST(test_view_sort_and_copy_use_effective_rename);
    RUN_TEST(test_sort_chip_click_selects_and_flips);
    RUN_TEST(test_view_focus_follows_selection_across_model_rebuild);
    RUN_TEST(test_view_anchor_follows_sprite_across_model_rebuild);
    RUN_TEST(test_view_oom_fallback_clears_focus_and_anchor);
    RUN_TEST(test_view_filter_finds_long_rename_beyond_label);
    RUN_TEST(test_left_section_caps_preserve_sprite_vlist_at_short_heights);
    RUN_TEST(test_recycled_view_id_double_click_requires_same_canonical_row);
    RUN_TEST(test_focus_scroll_top_ignores_overscan_rows);
    RUN_TEST(test_view_focus_returns_when_selected_row_is_revealed);
    RUN_TEST(test_view_collapse_prunes_sources_owned_by_deleted_atlas);
    RUN_TEST(test_recycled_entity_double_click_requires_same_stable_id);
    RUN_TEST(test_refresh_failure_after_invalidation_requires_stale_preview);
    RUN_TEST(test_result_region_selection_uses_provided_result);
    RUN_TEST(test_primary_row_mapping_uses_displayed_result_and_rejects_non_leaf);
    RUN_TEST(test_canvas_zoom_to_sprite_centers_transformed_region);
    return UNITY_END();
}
