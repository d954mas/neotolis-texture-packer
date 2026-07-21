/* U-02 sprite-tree VIEW-layer regression (gui_rows.c / gui_rows.h).
 *
 * Exercises the pure filter/sort/collapse VIEW that sits over the cached row
 * model: build_rows() flattens the selected atlas into s_rows[], build_view()
 * projects s_view[] (indices into s_rows[]) applying {filter, collapse, sort}.
 * A view-only change must never touch the row model, so every case asserts on
 * s_view / s_view_count and the rows they point at. Headless: no window, GL,
 * pack job, or event loop -- same in-memory session harness as
 * test_gui_canonical_identity.c. */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

#include "gui_project.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "tp_core/tp_build_worker.h"
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

/* build_rows only stats/enumerates the directory (gui_scan), it never decodes,
 * so a minimal PNG signature is all a child file needs to be listed. */
static bool write_fixture_file(const char *path) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        return false;
    }
    const unsigned char bytes[] = {0x89U, 0x50U, 0x4eU, 0x47U};
    const bool ok = fwrite(bytes, 1U, sizeof bytes, file) == sizeof bytes;
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
    return write_fixture_file(s_alpha) && write_fixture_file(s_beta) &&
           write_fixture_file(s_gamma) && write_fixture_file(s_solo);
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
    gui_rows_shutdown();
    gui_project_shutdown();
    gui_scan_shutdown();
    remove_fixture_files();
}

/* 1. No filter, ORIGINAL sort, nothing collapsed -> the view is the identity
 *    projection of the whole row model. */
void test_view_passthrough_is_identity(void) {
    add_sources_and_build(NULL, NULL);
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

    /* Baseline: without warn_first the missing row is NOT pinned (original order
     * keeps the folder span first, the now-missing solo span last). */
    gui_rows_set_sort(ROW_SORT_ORIGINAL, false, false);
    build_view();
    TEST_ASSERT_EQUAL_INT(5, s_view_count);
    TEST_ASSERT_FALSE(s_rows[s_view[0]].missing);
    TEST_ASSERT_TRUE(s_rows[s_view[s_view_count - 1]].missing);

    /* warn_first ascending: missing row pinned on top. */
    gui_rows_set_sort(ROW_SORT_ORIGINAL, false, true);
    build_view();
    TEST_ASSERT_EQUAL_INT(5, s_view_count);
    TEST_ASSERT_TRUE(s_rows[s_view[0]].missing);

    /* warn_first descending: still pinned on top (direction-independent). */
    gui_rows_set_sort(ROW_SORT_ORIGINAL, true, true);
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

/* 7. gui_selection_revalidate (the mechanism behind U-02 T5 undo-keeps-selection): after the model
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

/* 11. TYPE sort: folder sources sort ahead of file sources regardless of add order, with a natural
 *     name tiebreak among equal-type siblings (mirrors the NAME-sort structure of test 4). */
void test_view_sort_type_orders_folders_before_files(void) {
    add_sources_reversed_and_build(NULL, NULL); /* FILE added first, FOLDER second */

    gui_rows_set_sort(ROW_SORT_TYPE, false, false);
    build_view();

    const int fpos = view_folder_pos();
    TEST_ASSERT_EQUAL_INT(0, fpos); /* folder source pinned first despite being added second */
    /* children under it keep the natural-name tiebreak (equal type). */
    TEST_ASSERT_EQUAL_STRING("alpha", s_rows[s_view[fpos + 1]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("beta", s_rows[s_view[fpos + 2]].sprite_name);
    TEST_ASSERT_EQUAL_STRING("gamma", s_rows[s_view[fpos + 3]].sprite_name);

    /* the FILE source falls after the whole folder span. */
    int spos = -1;
    for (int k = 0; k < s_view_count; ++k) {
        if (s_rows[s_view[k]].is_source && !s_rows[s_view[k]].is_folder) {
            spos = k;
            break;
        }
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, spos);
    TEST_ASSERT_GREATER_THAN_INT(fpos, spos);
    TEST_ASSERT_EQUAL_STRING("solo", s_rows[s_view[spos]].sprite_name);
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
    RUN_TEST(test_view_sort_type_orders_folders_before_files);
    RUN_TEST(test_view_empty_state_is_safe);
    RUN_TEST(test_view_collapse_pruned_when_folder_source_removed);
    return UNITY_END();
}
