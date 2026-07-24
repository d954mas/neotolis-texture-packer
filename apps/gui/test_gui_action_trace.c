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

#include "tp_core/tp_scan.h"

#include "unity.h"

static char s_save_path[1024];

void tp_scan__test_set_stat_error(int error);

/* gui_actions links the production shell reset seam; the trace is headless. */
void gui_shell_reset_shown_result(void) {}

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
    tp_scan__test_set_stat_error(0);
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
    tp_scan__test_set_stat_error(0);
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

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_state_ownership_inventory_preserves_three_classes);
    RUN_TEST(test_deferred_edit_coalesces_then_undo_redo_trace_is_exact);
    RUN_TEST(test_deferred_action_mutates_before_publishing_success_status);
    RUN_TEST(test_preview_request_is_deferred_and_selection_reset_stops_it);
    RUN_TEST(test_confirm_save_publishes_before_new_and_new_message_wins);
    RUN_TEST(test_recovery_decision_runs_next_frame_and_failure_keeps_row);
    RUN_TEST(test_canvas_buffer_readiness_requires_every_gpu_handle);
    RUN_TEST(test_refresh_reports_source_stat_failure);
    return UNITY_END();
}
