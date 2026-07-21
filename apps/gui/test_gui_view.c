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
    return UNITY_END();
}
