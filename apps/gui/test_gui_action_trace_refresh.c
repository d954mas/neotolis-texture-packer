/* Source-refresh and fingerprint half of the action-state trace oracle.
 *
 * External refresh reports added/removed/changed against the last successful
 * observation and never mutates revision, dirty state, or Undo history. These
 * cases write and delete real files, so they own a target (and a scratch
 * directory) that no other trace case shares. */

#include "test_gui_action_trace_fixture.h"

void test_refresh_reports_source_stat_failure(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path, "%s/source.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));

    tp_scan__test_set_stat_error(EACCES);
    TEST_ASSERT_FALSE(gui_actions_refresh_diff_headless(NULL, NULL, NULL));
    tp_scan__test_set_stat_error(0);
}

void test_first_refresh_stat_failure_invalidates_runtime_and_preview(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path,
                              "%s/first-refresh-unreadable.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));

    gui_project_mark_packed();
    TEST_ASSERT_FALSE(gui_project_is_stale());
    const uint64_t source_generation_before =
        gui_project_source_runtime_generation();

    tp_scan__test_set_stat_error(EACCES);
    s_pending_refresh = true;
    apply_pending();
    tp_scan__test_set_stat_error(0);

    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_TRUE(
        gui_project_source_runtime_generation() >
        source_generation_before);
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "Refresh warning:"));
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_refresh_modified_file_reports_changed_from_last_success(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path, "%s/changed.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));

    /* Establish an explicit successful runtime observation. Project open/frame
     * pumping must not synchronously scan every source. */
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(7U, fwrite("changed", 1U, 7U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    s_pending_refresh = true;
    apply_pending();

    TEST_ASSERT_EQUAL_INT(STATUS_INFO, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 changed"));
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_refresh_deleted_file_invalidates_preview_without_model_mutation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path, "%s/deleted.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));

    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    gui_project_mark_packed();
    TEST_ASSERT_FALSE(gui_project_is_stale());

    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));

    s_pending_refresh = true;
    apply_pending();

    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(dirty_before, gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 removed"));
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 source unavailable"));
}

void test_refresh_unreadable_source_warns_without_model_mutation(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path,
                              "%s/unreadable.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));
    gui_project_mark_packed();

    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    tp_scan__test_set_stat_error(EACCES);
    s_pending_refresh = true;
    apply_pending();
    tp_scan__test_set_stat_error(0);

    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(dirty_before, gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "Refresh warning:"));
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_refresh_fingerprint_resets_when_session_is_replaced(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path,
                              "%s/old-session.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id, tp_session_snapshot_revision(snapshot), source_path,
            TP_SOURCE_KIND_FILE));
    apply_pending();

    TEST_ASSERT_TRUE(gui_project_test_new());
    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(&added, &removed, &changed));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_refresh_ignores_source_membership_transactions(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    char first_path[1200];
    char second_path[1200];
    TEST_ASSERT_TRUE(snprintf(first_path, sizeof first_path, "%s/first.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(second_path, sizeof second_path, "%s/second.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("a", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    source = fopen(second_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("b", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), first_path,
            TP_SOURCE_KIND_FILE));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), second_path,
            TP_SOURCE_KIND_FILE));
    snapshot = gui_project_snapshot();
    const tp_snapshot_source *second =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(second);
    const tp_id128 second_id = second->id;

    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(&added, &removed, &changed));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(0, changed);

    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas_id, second_id, tp_session_snapshot_revision(snapshot)));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(&added, &removed, &changed));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(0, changed);

    TEST_ASSERT_EQUAL_INT(0, remove(first_path));
    TEST_ASSERT_EQUAL_INT(0, remove(second_path));
}

void test_refresh_same_path_memberships_do_not_double_count_change(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *first_atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(first_atlas);

    char source_path[1200];
    TEST_ASSERT_TRUE(snprintf(source_path, sizeof source_path,
                              "%s/shared-membership.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("a", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            first_atlas->id, tp_session_snapshot_revision(snapshot),
            source_path, TP_SOURCE_KIND_FILE));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        7U, fwrite("changed", 1U, 7U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
    snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *second_atlas =
        tp_session_snapshot_atlas_at(snapshot, 1);
    TEST_ASSERT_NOT_NULL(second_atlas);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            second_atlas->id, tp_session_snapshot_revision(snapshot),
            source_path, TP_SOURCE_KIND_FILE));

    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(&added, &removed, &changed));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_refresh_retains_external_change_when_source_is_added(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    char first_path[1200];
    char second_path[1200];
    TEST_ASSERT_TRUE(snprintf(first_path, sizeof first_path,
                              "%s/changed-before-add.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(second_path, sizeof second_path,
                              "%s/new-membership.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("a", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    source = fopen(second_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("b", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), first_path,
            TP_SOURCE_KIND_FILE));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        17U, fwrite("changed-before-add", 1U, 17U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), second_path,
            TP_SOURCE_KIND_FILE));
    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    const int history_before = gui_project_undo_depth();

    int added = -1;
    int removed = -1;
    int changed = -1;
    const bool refreshed =
        gui_actions_refresh_diff_headless(&added, &removed, &changed);
    const int64_t revision_after =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_after = gui_project_is_dirty();
    const int history_after = gui_project_undo_depth();
    (void)remove(first_path);
    (void)remove(second_path);

    TEST_ASSERT_TRUE(refreshed);
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT64(revision_before, revision_after);
    TEST_ASSERT_EQUAL_INT(dirty_before, dirty_after);
    TEST_ASSERT_EQUAL_INT(history_before, history_after);
}

void test_refresh_retains_external_change_when_source_is_removed(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    char first_path[1200];
    char second_path[1200];
    TEST_ASSERT_TRUE(snprintf(first_path, sizeof first_path,
                              "%s/changed-before-remove.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(second_path, sizeof second_path,
                              "%s/removed-membership.png",
                              TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("a", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    source = fopen(second_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(1U, fwrite("b", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), first_path,
            TP_SOURCE_KIND_FILE));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), second_path,
            TP_SOURCE_KIND_FILE));
    snapshot = gui_project_snapshot();
    const tp_snapshot_source *second =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(second);
    const tp_id128 second_id = second->id;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL));

    source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        20U, fwrite("changed-before-remove", 1U, 20U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas_id, second_id, tp_session_snapshot_revision(snapshot)));
    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    const int history_before = gui_project_undo_depth();

    int added = -1;
    int removed = -1;
    int changed = -1;
    const bool refreshed =
        gui_actions_refresh_diff_headless(&added, &removed, &changed);
    const int64_t revision_after =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_after = gui_project_is_dirty();
    const int history_after = gui_project_undo_depth();
    (void)remove(first_path);
    (void)remove(second_path);

    TEST_ASSERT_TRUE(refreshed);
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT64(revision_before, revision_after);
    TEST_ASSERT_EQUAL_INT(dirty_before, dirty_after);
    TEST_ASSERT_EQUAL_INT(history_before, history_after);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_refresh_reports_source_stat_failure);
    RUN_TEST(test_first_refresh_stat_failure_invalidates_runtime_and_preview);
    RUN_TEST(test_refresh_modified_file_reports_changed_from_last_success);
    RUN_TEST(
        test_refresh_deleted_file_invalidates_preview_without_model_mutation);
    RUN_TEST(test_refresh_unreadable_source_warns_without_model_mutation);
    RUN_TEST(test_refresh_fingerprint_resets_when_session_is_replaced);
    RUN_TEST(test_refresh_ignores_source_membership_transactions);
    RUN_TEST(test_refresh_same_path_memberships_do_not_double_count_change);
    RUN_TEST(test_refresh_retains_external_change_when_source_is_added);
    RUN_TEST(test_refresh_retains_external_change_when_source_is_removed);
    return UNITY_END();
}
