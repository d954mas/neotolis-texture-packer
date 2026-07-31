/* Source-refresh and fingerprint half of the action-state trace oracle.
 *
 * External refresh reports added/removed/changed against the last successful
 * observation and never mutates revision, dirty state, or Undo history. These
 * cases write and delete real files, so they own a target (and a scratch
 * directory) that no other trace case shares. */

#include "test_gui_action_trace_fixture.h"

#ifdef _WIN32
#include <direct.h>
#define refresh_test_rmdir _rmdir
#else
#include <unistd.h>
#define refresh_test_rmdir rmdir
#endif

static void current_session_semantics(
    int64_t *out_revision, bool *out_dirty, int *out_undo_depth) {
    tp_session_snapshot *snapshot = NULL;
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_snapshot_create(
            gui_project__test_session(), &snapshot, &error));
    TEST_ASSERT_NOT_NULL(snapshot);
    if (out_revision) {
        *out_revision =
            tp_session_snapshot_revision(snapshot);
    }
    if (out_dirty) {
        *out_dirty =
            tp_session_snapshot_dirty(snapshot);
    }
    if (out_undo_depth) {
        *out_undo_depth =
            gui_project_undo_depth();
    }
    tp_session_snapshot_destroy(snapshot);
}

void test_dev_settle_rejects_failed_automatic_refresh_admission(void) {
    const uint64_t generation_before =
        gui_project_source_runtime_generation();
    gui_project_refresh_sources();
    tp_refresh_job__test_fail_next_start();

    tp_error error = {{0}};
    TEST_ASSERT_FALSE(
        gui_actions_dev_settle_task(&error));
    TEST_ASSERT_NOT_NULL(
        strstr(error.msg,
               "Refresh job allocation failed"));
    TEST_ASSERT_FALSE(gui_project_job_busy());
    TEST_ASSERT_NOT_NULL(gui_project_snapshot());
    TEST_ASSERT_EQUAL_UINT64(
        generation_before,
        gui_project_source_runtime_generation());
}

static void assert_refresh_completion(
    gui_pack_done expected_done, int expected_added, int expected_removed,
    int expected_changed, int expected_unavailable) {
    gui_pack_done done = GUI_PACK_DONE_NONE;
    gui_pack_result_info info = {0};
    TEST_ASSERT_TRUE(gui_actions__test_take_refresh_completion(
        &done, &info));
    TEST_ASSERT_EQUAL_INT(expected_done, done);
    TEST_ASSERT_EQUAL_INT(expected_added, info.added);
    TEST_ASSERT_EQUAL_INT(expected_removed, info.removed);
    TEST_ASSERT_EQUAL_INT(expected_changed, info.changed);
    TEST_ASSERT_EQUAL_INT(expected_unavailable, info.unavailable);
}

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

    int64_t revision_before = -1;
    bool dirty_before = false;
    int undo_before = -1;
    current_session_semantics(
        &revision_before, &dirty_before, &undo_before);
    int added = -1;
    int removed = -1;
    int changed = -1;
    int unavailable = -1;
    tp_scan__test_set_stat_error(EACCES);
    TEST_ASSERT_TRUE(gui_actions_refresh_diff_headless(
        &added, &removed, &changed, &unavailable));
    tp_scan__test_set_stat_error(0);
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(0, changed);
    TEST_ASSERT_EQUAL_INT(1, unavailable);
    int64_t revision_after = -1;
    bool dirty_after = false;
    int undo_after = -1;
    current_session_semantics(
        &revision_after, &dirty_after, &undo_after);
    TEST_ASSERT_EQUAL_INT64(revision_before, revision_after);
    TEST_ASSERT_EQUAL_INT(dirty_before, dirty_after);
    TEST_ASSERT_EQUAL_INT(undo_before, undo_after);
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 source unavailable"));
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

    TEST_ASSERT_TRUE(gui_actions_refresh_diff_headless(
        NULL, NULL, NULL, NULL));
    gui_project_mark_packed();
    TEST_ASSERT_FALSE(gui_project_is_stale());
    const uint64_t source_generation_before =
        gui_project_source_runtime_generation();
    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    const int undo_before = gui_project_undo_depth();

    tp_scan__test_set_stat_error(EACCES);
    gui_request_refresh();
    gui_actions__test_drain_intents();
    TEST_ASSERT_TRUE(gui_project_job_busy());
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_NONE,
        gui_project_job_active_kind());
    settle_project_job();
    tp_scan__test_set_stat_error(0);

    assert_refresh_completion(
        GUI_PACK_DONE_REFRESH_OK, 0, 1, 0, 1);
    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_TRUE(
        gui_project_source_runtime_generation() >
        source_generation_before);
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(dirty_before, gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(undo_before, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 source unavailable"));
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));
    int64_t revision_before = -1;
    bool dirty_before = false;
    int undo_before = -1;
    current_session_semantics(
        &revision_before, &dirty_before, &undo_before);

    source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(7U, fwrite("changed", 1U, 7U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    gui_request_refresh();
    pump_action_frame();
    settle_project_job();

    assert_refresh_completion(
        GUI_PACK_DONE_REFRESH_OK, 0, 0, 1, 0);
    int64_t revision_after = -1;
    bool dirty_after = false;
    int undo_after = -1;
    current_session_semantics(
        &revision_after, &dirty_after, &undo_after);
    TEST_ASSERT_EQUAL_INT64(revision_before, revision_after);
    TEST_ASSERT_EQUAL_INT(dirty_before, dirty_after);
    TEST_ASSERT_EQUAL_INT(undo_before, undo_after);
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));

    gui_project_mark_packed();
    TEST_ASSERT_FALSE(gui_project_is_stale());

    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    const int undo_before = gui_project_undo_depth();
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));

    gui_request_refresh();
    pump_action_frame();
    settle_project_job();

    assert_refresh_completion(
        GUI_PACK_DONE_REFRESH_OK, 0, 1, 0, 1);
    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(dirty_before, gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(undo_before, gui_project_undo_depth());
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));
    gui_project_mark_packed();

    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const bool dirty_before = gui_project_is_dirty();
    const int undo_before = gui_project_undo_depth();
    tp_scan__test_set_stat_error(EACCES);
    gui_request_refresh();
    pump_action_frame();
    settle_project_job();
    tp_scan__test_set_stat_error(0);

    assert_refresh_completion(
        GUI_PACK_DONE_REFRESH_OK, 0, 1, 0, 1);
    TEST_ASSERT_TRUE(gui_project_is_stale());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(dirty_before, gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(undo_before, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 source unavailable"));
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
    settle_project_job();

    TEST_ASSERT_TRUE(gui_project_test_new());
    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL));
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));

    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), second_path,
            TP_SOURCE_KIND_FILE));
    settle_project_job();
    snapshot = gui_project_snapshot();
    const tp_snapshot_source *second =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(second);
    const tp_id128 second_id = second->id;

    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(0, removed);
    TEST_ASSERT_EQUAL_INT(0, changed);

    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas_id, second_id, tp_session_snapshot_revision(snapshot)));
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL));
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(1, removed);
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));

    source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        7U, fwrite("changed", 1U, 7U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));

    const gui_project_create_result second_atlas =
        gui_project_add_atlas();
    TEST_ASSERT_TRUE(second_atlas.committed);
    publish_project_frame();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            second_atlas.created_id,
            gui_project_committed_revision(),
            source_path, TP_SOURCE_KIND_FILE));

    int added = -1;
    int removed = -1;
    int changed = -1;
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL));
    TEST_ASSERT_EQUAL_INT(1, added);
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
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));

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
    int64_t revision_before = -1;
    bool dirty_before = false;
    int history_before = -1;
    current_session_semantics(
        &revision_before, &dirty_before, &history_before);

    int added = -1;
    int removed = -1;
    int changed = -1;
    const bool refreshed =
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL);
    int64_t revision_after = -1;
    bool dirty_after = false;
    int history_after = -1;
    current_session_semantics(
        &revision_after, &dirty_after, &history_after);
    (void)remove(first_path);
    (void)remove(second_path);

    TEST_ASSERT_TRUE(refreshed);
    TEST_ASSERT_EQUAL_INT(1, added);
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

    const char *paths[] = {first_path, second_path};
    int added = 0;
    int duplicates = 0;
    TEST_ASSERT_TRUE(
        gui_project_add_sources(
            atlas_id, tp_session_snapshot_revision(snapshot),
            paths, 2, TP_SOURCE_KIND_FILE,
            &added, &duplicates));
    TEST_ASSERT_EQUAL_INT(2, added);
    TEST_ASSERT_EQUAL_INT(0, duplicates);
    TEST_ASSERT_TRUE(
        gui_actions_refresh_diff_headless(NULL, NULL, NULL, NULL));
    snapshot = gui_project_snapshot();
    const tp_snapshot_source *second =
        tp_session_snapshot_source_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(second);
    const tp_id128 second_id = second->id;

    source = fopen(first_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        20U, fwrite("changed-before-remove", 1U, 20U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_remove_source(
        atlas_id, second_id, tp_session_snapshot_revision(snapshot)));
    int64_t revision_before = -1;
    bool dirty_before = false;
    int history_before = -1;
    current_session_semantics(
        &revision_before, &dirty_before, &history_before);

    added = -1;
    int removed = -1;
    int changed = -1;
    const bool refreshed =
        gui_actions_refresh_diff_headless(
            &added, &removed, &changed, NULL);
    int64_t revision_after = -1;
    bool dirty_after = false;
    int history_after = -1;
    current_session_semantics(
        &revision_after, &dirty_after, &history_after);
    (void)remove(first_path);
    (void)remove(second_path);

    TEST_ASSERT_TRUE(refreshed);
    TEST_ASSERT_EQUAL_INT(0, added);
    TEST_ASSERT_EQUAL_INT(1, removed);
    TEST_ASSERT_EQUAL_INT(1, changed);
    TEST_ASSERT_EQUAL_INT64(revision_before, revision_after);
    TEST_ASSERT_EQUAL_INT(dirty_before, dirty_after);
    TEST_ASSERT_EQUAL_INT(history_before, history_after);
}

void test_user_refresh_returns_async_busy_and_publishes_terminal_once(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    char folder_path[1200];
    char source_path[1280];
    TEST_ASSERT_TRUE(snprintf(
        folder_path, sizeof folder_path,
        "%s/user-refresh", TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(
        source_path, sizeof source_path,
        "%s/source.png", folder_path) > 0);
    tp_mkdirs(folder_path);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            folder_path, TP_SOURCE_KIND_FOLDER));
    settle_project_job();

    gui_actions__test_reset_refresh_completion();
    tp_scan__test_arm_walk_gate();
    gui_request_refresh();
    pump_action_frame();
    for (int attempt = 0;
         attempt < 5000 &&
         !tp_scan__test_walk_gate_entered();
         ++attempt) {
        nt_time_sleep(0.001);
    }

    TEST_ASSERT_TRUE(tp_scan__test_walk_gate_entered());
    TEST_ASSERT_TRUE(gui_project_job_busy());
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REFRESH,
        gui_project_job_active_kind());
    TEST_ASSERT_EQUAL_INT(STATUS_INFO, s_status_sev);
    TEST_ASSERT_EQUAL_STRING("Refreshing sources...", s_status);

    gui_request_refresh();
    pump_action_frame();
    TEST_ASSERT_TRUE(gui_project_job_busy());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "Busy"));

    tp_scan__test_release_walk_gate();
    settle_project_job();
    gui_pack_done done = GUI_PACK_DONE_NONE;
    gui_pack_result_info info = {0};
    TEST_ASSERT_TRUE(gui_actions__test_take_refresh_completion(
        &done, &info));
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_REFRESH_OK, done);
    TEST_ASSERT_EQUAL_INT(0, info.added);
    TEST_ASSERT_EQUAL_INT(0, info.removed);
    TEST_ASSERT_EQUAL_INT(0, info.changed);
    TEST_ASSERT_EQUAL_INT(0, info.unavailable);
    TEST_ASSERT_FALSE(gui_actions__test_take_refresh_completion(
        &done, &info));

    publish_project_frame();
    TEST_ASSERT_FALSE(gui_actions__test_take_refresh_completion(
        &done, &info));
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
    TEST_ASSERT_EQUAL_INT(0, refresh_test_rmdir(folder_path));
}

void test_refresh_lifecycle_cancel_drains_before_session_cutover(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char folder_path[1200];
    char source_path[1280];
    TEST_ASSERT_TRUE(snprintf(
        folder_path, sizeof folder_path,
        "%s/cancel-refresh", TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(
        source_path, sizeof source_path,
        "%s/source.png", folder_path) > 0);
    tp_mkdirs(folder_path);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            folder_path, TP_SOURCE_KIND_FOLDER));
    /* Finish the membership-triggered automatic Refresh first. The lifecycle
     * case below owns a distinct explicitly admitted worker; arming its gate
     * after an already-running automatic worker would race the scan entry. */
    settle_project_job();
    gui_actions__test_reset_refresh_completion();

    tp_scan__test_arm_walk_gate();
    gui_request_refresh();
    gui_actions__test_drain_intents();
    TEST_ASSERT_TRUE(gui_project_job_busy());
    for (int attempt = 0;
         attempt < 5000 &&
         !tp_scan__test_walk_gate_entered();
         ++attempt) {
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_TRUE(tp_scan__test_walk_gate_entered());

    const uint64_t instance_before =
        gui_project_session_instance_generation();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());
    tp_scan__test_release_walk_gate();

    gui_project_lifecycle_kind completed =
        GUI_PROJECT_LIFECYCLE_NONE;
    for (int attempt = 0;
         attempt < 5000 &&
         gui_project_lifecycle_state_query() !=
             GUI_PROJECT_LIFECYCLE_ACTIVE;
         ++attempt) {
        gui_project_step_result result = {0};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_step(
                &result, NULL));
        if (result.lifecycle_completed !=
            GUI_PROJECT_LIFECYCLE_NONE) {
            completed =
                result.lifecycle_completed;
        }
        tp_session_job_result_destroy(
            &result.completion);
        if (gui_project_lifecycle_state_query() !=
            GUI_PROJECT_LIFECYCLE_ACTIVE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW, completed);
    TEST_ASSERT_EQUAL_UINT64(
        instance_before + 1U,
        gui_project_session_instance_generation());
    TEST_ASSERT_FALSE(gui_project_job_busy());

    gui_pack_done done = GUI_PACK_DONE_NONE;
    gui_pack_result_info info = {0};
    /* New supersedes the retired session's cancellation receipt; it is
     * discarded by the project step and never becomes current UI state. */
    TEST_ASSERT_FALSE(gui_actions__test_take_refresh_completion(
        &done, &info));

    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
    TEST_ASSERT_EQUAL_INT(0, refresh_test_rmdir(folder_path));
}

void test_refresh_lifecycle_deadline_retires_blocked_worker(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    char folder_path[1200];
    char source_path[1280];
    TEST_ASSERT_TRUE(snprintf(
        folder_path, sizeof folder_path,
        "%s/deadline-refresh", TP_GUI_TRACE_TEST_DIR) > 0);
    TEST_ASSERT_TRUE(snprintf(
        source_path, sizeof source_path,
        "%s/source.png", folder_path) > 0);
    tp_mkdirs(folder_path);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            folder_path, TP_SOURCE_KIND_FOLDER));
    /* Finish the membership-triggered automatic Refresh first. The lifecycle
     * case below owns a distinct explicitly admitted worker; arming its gate
     * after an already-running automatic worker would race the scan entry. */
    settle_project_job();
    gui_actions__test_reset_refresh_completion();

    tp_scan__test_arm_walk_gate();
    gui_request_refresh();
    gui_actions__test_drain_intents();
    for (int attempt = 0;
         attempt < 5000 &&
         !tp_scan__test_walk_gate_entered();
         ++attempt) {
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_TRUE(tp_scan__test_walk_gate_entered());
    TEST_ASSERT_EQUAL_INT(
        1, tp_refresh_job__test_active_workers());

    const uint64_t instance_before =
        gui_project_session_instance_generation();
    gui_project__test_set_drain_grace_ms(0);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    const double started_at = nt_time_now();
    gui_project_step_result result = {0};
    const tp_status step_status =
        gui_project_step(&result, NULL);
    const double elapsed = nt_time_now() - started_at;
    const gui_project_lifecycle_state state_after_step =
        gui_project_lifecycle_state_query();
    const gui_project_lifecycle_kind receipt =
        result.lifecycle_completed;
    const uint64_t instance_after =
        gui_project_session_instance_generation();
    tp_session_job_result_destroy(&result.completion);

    /* Always unpark and drain before asserting so a failing implementation
     * cannot strand a worker across Unity teardown. */
    tp_scan__test_release_walk_gate();
    for (int attempt = 0;
         attempt < 5000 &&
         tp_refresh_job__test_active_workers() != 0;
         ++attempt) {
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_EQUAL_INT(
        0, tp_refresh_job__test_active_workers());
    if (gui_project_test_state_is_transitioning(
            gui_project_lifecycle_state_query())) {
        gui_project_lifecycle_kind drained =
            GUI_PROJECT_LIFECYCLE_NONE;
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_test_drain(&drained, NULL));
    }
    gui_project__test_set_drain_grace_ms(-1);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, step_status);
    TEST_ASSERT_TRUE(elapsed < 0.5);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        state_after_step);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW, receipt);
    TEST_ASSERT_EQUAL_UINT64(
        instance_before + 1U, instance_after);
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
    TEST_ASSERT_EQUAL_INT(0, refresh_test_rmdir(folder_path));
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(
        test_dev_settle_rejects_failed_automatic_refresh_admission);
    RUN_TEST(test_refresh_reports_source_stat_failure);
    RUN_TEST(
        test_first_refresh_stat_failure_invalidates_runtime_and_preview);
    RUN_TEST(test_refresh_modified_file_reports_changed_from_last_success);
    RUN_TEST(
        test_refresh_deleted_file_invalidates_preview_without_model_mutation);
    RUN_TEST(
        test_refresh_unreadable_source_warns_without_model_mutation);
    RUN_TEST(test_refresh_fingerprint_resets_when_session_is_replaced);
    RUN_TEST(test_refresh_ignores_source_membership_transactions);
    RUN_TEST(
        test_refresh_same_path_memberships_do_not_double_count_change);
    RUN_TEST(
        test_refresh_retains_external_change_when_source_is_added);
    RUN_TEST(
        test_refresh_retains_external_change_when_source_is_removed);
    RUN_TEST(
        test_user_refresh_returns_async_busy_and_publishes_terminal_once);
    RUN_TEST(
        test_refresh_lifecycle_cancel_drains_before_session_cutover);
    RUN_TEST(
        test_refresh_lifecycle_deadline_retires_blocked_worker);
    return UNITY_END();
}
