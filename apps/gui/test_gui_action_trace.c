#include <stdbool.h>
#include <errno.h>
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
#include "gui_canvas_internal.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_scan.h"
#include "tp_test_seams.h"

#include "time/nt_time.h"
#include "unity.h"

static char s_save_path[1024];

/* gui_actions links the production shell reset seam; the trace is headless. */
void gui_shell_reset_shown_result(void) {}

static void pump_action_frame(void) {
    apply_pending();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_host_drain(&error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    gui_actions_poll_host_completion();
    gui_project_frame_end();
}

typedef enum trace_owner_class {
    TRACE_SESSION_SHARED = 0,
    TRACE_ACTION_PRIVATE,
    TRACE_VIEW_LOCAL
} trace_owner_class;

typedef struct state_owner_entry {
    const char *group;
    trace_owner_class owner;
} state_owner_entry;

/* Ownership inventory keeps session, action, and view state boundaries explicit. */
static const state_owner_entry k_state_owners[] = {
    {"selection", TRACE_SESSION_SHARED},
    {"status", TRACE_SESSION_SHARED},
    {"animation-preview-projection", TRACE_SESSION_SHARED},
    {"export-preview-selection", TRACE_SESSION_SHARED},
    {"confirmation-modal-visibility", TRACE_SESSION_SHARED},
    {"recovery-modal-visibility", TRACE_SESSION_SHARED},
    {"input-blur-request", TRACE_SESSION_SHARED},
    {"deferred-edit-queue", TRACE_ACTION_PRIVATE},
    {"deferred-side-effect-queue", TRACE_ACTION_PRIVATE},
    {"gesture-boundary", TRACE_ACTION_PRIVATE},
    {"confirmation-intent", TRACE_ACTION_PRIVATE},
    {"recovery-decision", TRACE_ACTION_PRIVATE},
    {"preview-captured-identity", TRACE_ACTION_PRIVATE},
    {"canvas-preview-dropdown", TRACE_VIEW_LOCAL},
    {"settings-edit-buffers", TRACE_VIEW_LOCAL},
    {"settings-dropdowns", TRACE_VIEW_LOCAL},
    {"chrome-menu-contexts", TRACE_VIEW_LOCAL},
};

static const tp_snapshot_atlas *atlas_at(int index) {
    return tp_session_snapshot_atlas_at(gui_project_snapshot(), index);
}

static void reset_public_action_state(void) {
    s_pending_open = false;
    s_pending_save = false;
    s_pending_save_as = false;
    s_pending_add_files = false;
    s_pending_add_folder = false;
    s_pending_add_atlas = false;
    s_pending_refresh = false;
    s_pending_pack = false;
    s_pending_export = false;
    s_pending_commit_edit = false;
    s_pending_commit_edit_enter = false;
    s_pending_remove_atlas = false;
    s_pending_remove_source = false;
    s_pending_preview_target = -1;
    s_after_confirm = AFTER_NONE;
    s_confirm_open = false;
    s_modal_action = MODAL_NONE;
}

void setUp(void) {
    tp_scan__test_reset_all();
    tp_job__test_reset_all();
    gui_actions_refresh_fingerprint_reset();
    (void)snprintf(s_save_path, sizeof s_save_path,
                   "%s/action-trace.ntpacker_project",
                   TP_GUI_TRACE_TEST_DIR);
    (void)remove(s_save_path);
    (void)test_rmdir(TP_GUI_TRACE_TEST_DIR);
    tp_mkdirs(TP_GUI_TRACE_TEST_DIR);

    gui_project_init();
    reset_public_action_state();
    s_sel_atlas = 0;
    reset_selection();
    cancel_edit();
    set_status("trace ready");
}

void tearDown(void) {
    tp_scan__test_reset_all();
    tp_job__test_reset_all();
    gui_actions_refresh_fingerprint_reset();
    multi_sel_clear();
    gui_pack_shutdown();
    gui_project_discard_recovery_on_shutdown();
    gui_project_shutdown();
    gui_scan_shutdown();
    (void)remove(s_save_path);
    (void)test_rmdir(TP_GUI_TRACE_TEST_DIR);
}

void test_state_ownership_inventory_preserves_three_classes(void) {
    int counts[3] = {0, 0, 0};
    for (size_t i = 0; i < sizeof k_state_owners / sizeof k_state_owners[0];
         ++i) {
        TEST_ASSERT_NOT_NULL(k_state_owners[i].group);
        TEST_ASSERT_NOT_EQUAL(0, (int)strlen(k_state_owners[i].group));
        TEST_ASSERT_TRUE(k_state_owners[i].owner >= TRACE_SESSION_SHARED);
        TEST_ASSERT_TRUE(k_state_owners[i].owner <= TRACE_VIEW_LOCAL);
        counts[k_state_owners[i].owner]++;
        for (size_t j = i + 1U;
             j < sizeof k_state_owners / sizeof k_state_owners[0]; ++j) {
            TEST_ASSERT_NOT_EQUAL(0,
                                  strcmp(k_state_owners[i].group,
                                         k_state_owners[j].group));
        }
    }
    TEST_ASSERT_EQUAL_INT(7, counts[TRACE_SESSION_SHARED]);
    TEST_ASSERT_EQUAL_INT(6, counts[TRACE_ACTION_PRIVATE]);
    TEST_ASSERT_EQUAL_INT(4, counts[TRACE_VIEW_LOCAL]);

    /* Compile-time anchors for the shared representatives that cross modules. */
    const void *const shared_anchors[] = {
        &s_sel_atlas,      &s_status,    &s_preview_active,
        &s_preview_target, &s_confirm_open, &s_recovery_open,
        &s_blur_inputs,
    };
    TEST_ASSERT_EQUAL_size_t(7U,
                             sizeof shared_anchors / sizeof shared_anchors[0]);
}

void test_deferred_edit_coalesces_then_undo_redo_trace_is_exact(void) {
    const tp_snapshot_atlas *atlas = atlas_at(0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int initial_max_size = atlas->max_size;
    const int64_t revision0 =
        tp_session_snapshot_revision(gui_project_snapshot());

    gui_queue_atlas_setting(atlas_id, revision0, GUI_ATLAS_MAX_SIZE, 512,
                            0.0F);
    gui_queue_atlas_setting(atlas_id, revision0, GUI_ATLAS_MAX_SIZE, 1024,
                            0.0F);

    TEST_ASSERT_EQUAL_INT64(revision0,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());

    apply_pending();
    TEST_ASSERT_EQUAL_INT64(revision0,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_TRUE(gui_project_can_undo());
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());

    gui_request_gesture_commit();
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());
    apply_pending();

    const int64_t revision1 =
        tp_session_snapshot_revision(gui_project_snapshot());
    TEST_ASSERT_EQUAL_INT64(revision0 + 1, revision1);
    TEST_ASSERT_EQUAL_INT(1024, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(1, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(0, gui_project_redo_depth());
    TEST_ASSERT_TRUE(gui_project_is_dirty());

    s_sel_src = 7;
    do_undo();
    TEST_ASSERT_EQUAL_INT64(revision1 + 1,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(1, gui_project_redo_depth());
    /* Selection remains captured for canonical revalidation in the frame loop. */
    TEST_ASSERT_EQUAL_INT(7, s_sel_src);
    TEST_ASSERT_TRUE(s_reselect_pending);
    TEST_ASSERT_EQUAL_STRING("Undo (undo:0 redo:1)", s_status);

    do_redo();
    TEST_ASSERT_EQUAL_INT64(revision1 + 2,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(1024, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(1, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(0, gui_project_redo_depth());
    /* Redo preserves the same pending canonical selection as Undo. */
    TEST_ASSERT_TRUE(s_reselect_pending);
    TEST_ASSERT_EQUAL_STRING("Redo (undo:1 redo:0)", s_status);
}

void test_undo_redo_preserves_selected_animation_by_stable_id(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    TEST_ASSERT_EQUAL_INT(
        0, gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "idle", NULL,
               0));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        1, gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "walk", NULL,
               0));
    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *selected =
        tp_session_snapshot_animation_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(selected);
    const tp_id128 selected_id = selected->id;
    s_sel_anim = 1;

    TEST_ASSERT_TRUE(gui_project_set_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot), GUI_ATLAS_MAX_SIZE,
        1024, 0.0F));

    do_undo();
    TEST_ASSERT_GREATER_OR_EQUAL(0, s_sel_anim);
    selected = tp_session_snapshot_animation_at(
        gui_project_snapshot(), atlas_id, s_sel_anim);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_TRUE(tp_id128_eq(selected_id, selected->id));

    do_redo();
    TEST_ASSERT_GREATER_OR_EQUAL(0, s_sel_anim);
    selected = tp_session_snapshot_animation_at(
        gui_project_snapshot(), atlas_id, s_sel_anim);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_TRUE(tp_id128_eq(selected_id, selected->id));
}

void test_deferred_action_mutates_before_publishing_success_status(void) {
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    set_status("atlas queued");
    s_pending_add_atlas = true;

    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(0, s_sel_atlas);
    TEST_ASSERT_EQUAL_STRING("atlas queued", s_status);

    apply_pending();
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(1, s_sel_atlas);
    TEST_ASSERT_FALSE(s_pending_add_atlas);
    TEST_ASSERT_EQUAL_STRING("Added atlas 'atlas2'", s_status);
}

void test_preview_request_is_deferred_and_selection_reset_stops_it(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int animation_index = gui_project_create_animation(
        atlas->id, tp_session_snapshot_revision(snapshot), "walk", NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, animation_index);

    gui_animation_ref animation;
    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(0, animation_index, &animation));
    s_sel_anim = -1;
    s_preview_active = false;
    s_preview_playing = false;
    set_status("preview queued");

    gui_request_open_preview(&animation);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_anim);
    TEST_ASSERT_FALSE(s_preview_active);
    TEST_ASSERT_EQUAL_STRING("preview queued", s_status);

    apply_pending();
    TEST_ASSERT_EQUAL_INT(0, s_sel_atlas);
    TEST_ASSERT_EQUAL_INT(animation_index, s_sel_anim);
    TEST_ASSERT_TRUE(s_preview_active);
    TEST_ASSERT_TRUE(s_preview_playing);
    TEST_ASSERT_EQUAL_STRING(
        "Pack (Ctrl+P) to preview the animation on packed regions.", s_status);

    s_sel_src = 4;
    s_sel_child = 2;
    s_preview_target = 1;
    reset_selection();
    TEST_ASSERT_EQUAL_INT(-1, s_sel_src);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_child);
    TEST_ASSERT_EQUAL_INT(-1, s_sel_anim);
    TEST_ASSERT_FALSE(s_preview_active);
    TEST_ASSERT_FALSE(s_preview_playing);
    TEST_ASSERT_EQUAL_INT(0, s_preview_target);
}

void test_confirm_save_publishes_before_new_and_new_message_wins(void) {
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  gui_project_save_as(s_save_path, error,
                                                      sizeof error),
                                  error);
    TEST_ASSERT_FALSE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas());
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    request_new();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(AFTER_NEW, s_after_confirm);
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    s_modal_action = MODAL_CANCEL;
    apply_pending();
    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(AFTER_NONE, s_after_confirm);
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    request_new();
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(AFTER_NONE, s_after_confirm);
    TEST_ASSERT_FALSE(gui_project_has_path());
    TEST_ASSERT_FALSE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    TEST_ASSERT_EQUAL_STRING("New project.", s_status);

    tp_project *published = NULL;
    tp_error load_error = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_load(s_save_path, &published,
                                          &load_error));
    TEST_ASSERT_NOT_NULL(published);
    TEST_ASSERT_EQUAL_INT(2, published->atlas_count);
    tp_project_destroy(published);
}

void test_recovery_decision_runs_next_frame_and_failure_keeps_row(void) {
    gui_recovery_list list;
    memset(&list, 0, sizeof list);
    list.count = 1U;
    (void)snprintf(list.items[0].name, sizeof list.items[0].name,
                   "orphan project");
    (void)snprintf(list.items[0].journal_path,
                   sizeof list.items[0].journal_path,
                   "%s/missing.journal", TP_GUI_TRACE_TEST_DIR);
    (void)snprintf(list.items[0].original_path,
                   sizeof list.items[0].original_path,
                   "%s/missing.ntpacker_project", TP_GUI_TRACE_TEST_DIR);

    gui_actions_open_recovery(&list);
    set_status("recovery queued");
    gui_actions_recovery_request(0, GUI_RECOVERY_SAVE_ORIGINAL);

    TEST_ASSERT_TRUE(s_recovery_open);
    TEST_ASSERT_EQUAL_INT(1, gui_actions_recovery_count());
    TEST_ASSERT_EQUAL_STRING("recovery queued", s_status);

    apply_pending();
    TEST_ASSERT_TRUE(s_recovery_open);
    TEST_ASSERT_EQUAL_INT(1, gui_actions_recovery_count());
    TEST_ASSERT_EQUAL_INT(STATUS_ERROR, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status,
                                "Recover 'orphan project' failed:"));
}

void test_canvas_buffer_readiness_requires_every_gpu_handle(void) {
    gui_canvas canvas = {0};
    canvas.ibo.id = 1U;
    canvas.vbo.id = 2U;
    canvas.sampler.id = 3U;
    canvas.vbo_checker.id = 4U;
    canvas.checker_tex.id = 5U;
    canvas.checker_sampler.id = 6U;

    TEST_ASSERT_TRUE(gui_canvas_resource_handles_ready(&canvas));

    canvas.vbo_checker.id = 0U;
    TEST_ASSERT_FALSE(gui_canvas_resource_handles_ready(&canvas));

    canvas.vbo_checker.id = 4U;
    canvas.checker_sampler.id = 0U;
    TEST_ASSERT_FALSE(gui_canvas_resource_handles_ready(&canvas));
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

void test_external_save_is_visible_through_the_observation_reducer(void) {
    tp_session_save_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_as(
            gui_project__test_session(), s_save_path,
            &result, &error));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    TEST_ASSERT_TRUE(gui_project_has_path());
    TEST_ASSERT_EQUAL_STRING(
        "action-trace.ntpacker_project",
        gui_project_display_name());
    gui_project_frame_end();
}

void test_open_propagates_non_oom_attach_rejection(void) {
    tp_rng rng = tp_rng_os();
    tp_session *candidate = NULL;
    tp_error error = {{0}};
    tp_session_save_result save_result = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_default_project(
            &rng, &candidate, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_as(
            candidate, s_save_path, &save_result, &error));
    tp_session_destroy(candidate);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    char open_error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_project_open(
            s_save_path, open_error,
            sizeof open_error));
    TEST_ASSERT_NOT_NULL(
        strstr(open_error, "pinned frame"));
    TEST_ASSERT_EQUAL_STRING(
        "untitled", gui_project_display_name());
    gui_project_frame_end();
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

    TEST_ASSERT_TRUE(gui_project_new());
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

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas());
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

void test_export_cancel_formatter_distinguishes_uncertain_partial_and_clean(void) {
    const gui_pack_result_info uncertain = {
        .publication_uncertain = true,
    };
    char status[128] = {0};
    TEST_ASSERT_TRUE(gui_pack_format_export_cancelled(
        &uncertain, status, sizeof status));
    TEST_ASSERT_NOT_NULL(strstr(status, "output may be partially updated"));

    const gui_pack_result_info partial = {
        .targets = 2,
        .files = 5,
        .partial_publication = true,
    };
    TEST_ASSERT_TRUE(gui_pack_format_export_cancelled(
        &partial, status, sizeof status));
    TEST_ASSERT_NOT_NULL(strstr(status, "Export cancelled after publishing"));
    TEST_ASSERT_NOT_NULL(strstr(status, "2 target"));
    TEST_ASSERT_NOT_NULL(strstr(status, "5 file"));

    const gui_pack_result_info clean = {0};
    TEST_ASSERT_FALSE(gui_pack_format_export_cancelled(
        &clean, status, sizeof status));
    TEST_ASSERT_EQUAL_STRING("Export cancelled.", status);
}

void test_export_failure_formatter_warns_about_uncertain_publication(void) {
    const gui_pack_result_info uncertain = {
        .targets = 1,
        .atlases_fail = 1,
        .publication_uncertain = true,
        .err = "intentional failure",
    };
    char status[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_format_export_failed(&uncertain, status, sizeof status));
    TEST_ASSERT_NOT_NULL(strstr(status, "Export failed"));
    TEST_ASSERT_NOT_NULL(strstr(status, "output may be partially updated"));
    TEST_ASSERT_NOT_NULL(strstr(status, "1 target"));
    TEST_ASSERT_NOT_NULL(strstr(status, "1 atlas"));

    const gui_pack_result_info certain = {0};
    TEST_ASSERT_FALSE(
        gui_pack_format_export_failed(&certain, status, sizeof status));
}

void test_late_export_cancel_keeps_completed_success_outcome(void) {
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_TRACE_TEST_DIR));

    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_export_async_start(error, sizeof error));

    tp_session_job_observed_state state = {0};
    for (int i = 0; i < 5000; ++i) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_host_drain(NULL));
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_frame_begin(NULL));
        state =
            gui_project_job_observed_state();
        gui_project_frame_end();
        if (state.present && state.terminal) {
            break;
        }
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_TRUE(state.present);
    TEST_ASSERT_TRUE(state.terminal);

    /* Terminal admission wins before this late cancellation request. */
    tp_error cancel_error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_NOT_FOUND,
        gui_pack_async_cancel(
            &cancel_error));
    TEST_ASSERT_NOT_EQUAL(
        '\0', cancel_error.msg[0]);
    TEST_ASSERT_FALSE(gui_pack_async_cancelling());

    gui_pack_result_info info;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(NULL));
    const gui_pack_done done =
        gui_pack_poll(&info);
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_EXPORT_OK, done);
    TEST_ASSERT_EQUAL_INT(1, info.atlases_skipped);
}

void test_observation_failure_leaves_terminal_receipt_retryable(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_TRACE_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error, sizeof error));
    for (int attempt = 0;
         attempt < 5000 &&
         !gui_project__test_host_has_staged_completion();
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_host_drain(NULL));
        if (!gui_project__test_host_has_staged_completion()) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(
        gui_project__test_host_has_staged_completion());
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_host_begin_drain(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_HOST_DRAINING,
        gui_project_host_lifecycle_query());

    gui_project__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_frame_begin(NULL));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_NONE,
        gui_pack_poll(NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_host_drain(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_HOST_DRAINING,
        gui_project_host_lifecycle_query());

    gui_project__test_fail_next_observe();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_frame_begin(NULL));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_host_drain(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_HOST_DRAINING,
        gui_project_host_lifecycle_query());

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(NULL));
    gui_pack_result_info info = {0};
    const gui_pack_done done =
        gui_pack_poll(&info);
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_EXPORT_OK, done);
}

void test_empty_export_surfaces_skipped_atlas_warning(void) {
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_TRACE_TEST_DIR));

    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_export_async_start(error, sizeof error));

    for (int i = 0; i < 5000 && gui_pack_async_busy(); ++i) {
        pump_action_frame();
        if (gui_pack_async_busy()) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_FALSE(gui_pack_async_busy());
    TEST_ASSERT_EQUAL_INT(STATUS_WARNING, s_status_sev);
    TEST_ASSERT_NOT_NULL(strstr(s_status, "1 atlas(es) skipped"));
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_state_ownership_inventory_preserves_three_classes);
    RUN_TEST(test_deferred_edit_coalesces_then_undo_redo_trace_is_exact);
    RUN_TEST(test_undo_redo_preserves_selected_animation_by_stable_id);
    RUN_TEST(test_deferred_action_mutates_before_publishing_success_status);
    RUN_TEST(test_preview_request_is_deferred_and_selection_reset_stops_it);
    RUN_TEST(test_confirm_save_publishes_before_new_and_new_message_wins);
    RUN_TEST(test_recovery_decision_runs_next_frame_and_failure_keeps_row);
    RUN_TEST(test_canvas_buffer_readiness_requires_every_gpu_handle);
    RUN_TEST(test_refresh_reports_source_stat_failure);
    RUN_TEST(
        test_first_refresh_stat_failure_invalidates_runtime_and_preview);
    RUN_TEST(
        test_external_save_is_visible_through_the_observation_reducer);
    RUN_TEST(test_open_propagates_non_oom_attach_rejection);
    RUN_TEST(test_refresh_modified_file_reports_changed_from_last_success);
    RUN_TEST(
        test_refresh_deleted_file_invalidates_preview_without_model_mutation);
    RUN_TEST(test_refresh_unreadable_source_warns_without_model_mutation);
    RUN_TEST(test_refresh_fingerprint_resets_when_session_is_replaced);
    RUN_TEST(test_refresh_ignores_source_membership_transactions);
    RUN_TEST(
        test_refresh_same_path_memberships_do_not_double_count_change);
    RUN_TEST(test_refresh_retains_external_change_when_source_is_added);
    RUN_TEST(test_refresh_retains_external_change_when_source_is_removed);
    RUN_TEST(
        test_export_cancel_formatter_distinguishes_uncertain_partial_and_clean);
    RUN_TEST(test_export_failure_formatter_warns_about_uncertain_publication);
    RUN_TEST(test_late_export_cancel_keeps_completed_success_outcome);
    RUN_TEST(
        test_observation_failure_leaves_terminal_receipt_retryable);
    RUN_TEST(test_empty_export_surfaces_skipped_atlas_warning);
    return UNITY_END();
}
