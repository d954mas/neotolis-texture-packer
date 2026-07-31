#include "test_gui_action_trace_fixture.h"

#ifdef _WIN32
#include <direct.h>
#define test_rmdir _rmdir
#else
#include <unistd.h>
#define test_rmdir rmdir
#endif

char s_save_path[1024];

/* gui_actions links the production shell reset seam; the trace is headless. */
void gui_shell_reset_shown_result(void) {}

void pump_action_frame(void) {
    publish_project_frame();
}

void publish_project_frame(void) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(NULL, &error));
}

void settle_project_job(void) {
    /* Automatic Refresh is admitted only at gui_actions_step. Cross that boundary
     * once before waiting so a pending membership refresh becomes observable. */
    publish_project_frame();
    for (int attempt = 0;
         attempt < 5000 && gui_project_job_busy();
         ++attempt) {
        publish_project_frame();
        if (gui_project_job_busy()) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_FALSE(gui_project_job_busy());
}

gui_project_create_result create_animation_observed(
    tp_id128 atlas_id, int64_t expected_revision,
    const char *name, const tp_op_sprite_ref *frames,
    int frame_count) {
    gui_project_create_result created =
        gui_project_create_animation(
            atlas_id, expected_revision, name,
            frames, frame_count);
    if (!created.committed) {
        return created;
    }
    publish_project_frame();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            snapshot, atlas_id);
    created.visible_index = -1;
    for (int index = 0;
         atlas && index < atlas->animation_count;
         ++index) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(
                snapshot, atlas_id, index);
        if (animation &&
            tp_id128_eq(
                animation->id,
                created.created_id)) {
            created.visible_index = index;
            break;
        }
    }
    created.observation_pending = false;
    return created;
}

const tp_snapshot_atlas *atlas_at(int index) {
    return tp_session_snapshot_atlas_at(gui_project_snapshot(), index);
}

bool trace_animation_ref_at(int atlas_index, int animation_index,
                            gui_animation_ref *out) {
    if (!out) {
        return false;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, atlas_index);
    const tp_snapshot_animation *animation =
        atlas ? tp_session_snapshot_animation_at(
                    snapshot, atlas->id, animation_index)
              : NULL;
    if (!atlas || !animation) {
        return false;
    }
    *out = (gui_animation_ref){
        atlas->id,
        animation->id,
        tp_session_snapshot_revision(snapshot),
    };
    return true;
}

bool trace_target_ref_at(int atlas_index, int target_index,
                         gui_target_ref *out) {
    if (!out) {
        return false;
    }
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, atlas_index);
    const tp_snapshot_target *target =
        atlas ? tp_session_snapshot_target_at(
                    snapshot, atlas->id, target_index)
              : NULL;
    if (!atlas || !target) {
        return false;
    }
    *out = (gui_target_ref){
        atlas->id,
        target->id,
        tp_session_snapshot_revision(snapshot),
    };
    return true;
}

gui_sprite_ref add_test_sprite_ref(const char *source_path,
                                   const char *source_key) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot),
            source_path, TP_SOURCE_KIND_FILE));
    settle_project_job();
    snapshot = gui_project_snapshot();
    const tp_snapshot_source *source =
        tp_session_snapshot_source_at(snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source);
    const gui_sprite_ref sprite = {
        atlas_id,
        source->id,
        source_key,
        tp_session_snapshot_revision(snapshot),
    };
    return sprite;
}

void apply_foreign_operation(tp_operation *operation,
                             const char *transaction_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    tp_txn_request request = {
        .schema = TP_TXN_SCHEMA,
        .expected_revision =
            tp_session_snapshot_revision(snapshot),
        .label = "test.foreign",
        .author = "test",
        .ops = operation,
        .op_count = 1,
    };
    (void)snprintf(request.id_hex, sizeof request.id_hex,
                   "%s", transaction_id);
    tp_txn_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(gui_project__test_session(),
                         &request, &result, &error));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(NULL, &error));
}

void reset_public_action_state(void) {
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    gui_actions__clear_pending();
    gui_actions__discard_deferred_edits();
    s_after_confirm = GUI_LIFECYCLE_REQUEST_NONE;
    s_confirm_open = false;
    s_confirm_draft = false;
    s_modal_action = MODAL_NONE;
}

void setUp(void) {
    tp_scan__test_reset_all();
    tp_job__test_reset_all();
    gui_project__test_set_drain_grace_ms(-1);
    gui_actions__test_reset_refresh_completion();
    (void)snprintf(s_save_path, sizeof s_save_path,
                   "%s/action-trace.ntpacker_project",
                   TP_GUI_TRACE_TEST_DIR);
    (void)remove(s_save_path);
    (void)test_rmdir(TP_GUI_TRACE_TEST_DIR);
    tp_mkdirs(TP_GUI_TRACE_TEST_DIR);

    gui_project_init();
    gui_view_reset();
    gui_view_reconcile_observation(gui_project_snapshot());
    gui_view_adopt_default_atlas(gui_project_snapshot());
    reset_public_action_state();
    reset_selection();
    cancel_edit();
    set_status("trace ready");
}

void tearDown(void) {
    gui_actions__test_reset_refresh_completion();
    multi_sel_clear();
    gui_pack_shutdown();
    gui_project_test_shutdown(true);
    gui_project__test_set_drain_grace_ms(-1);
    tp_scan__test_reset_all();
    tp_job__test_reset_all();
    (void)remove(s_save_path);
    (void)test_rmdir(TP_GUI_TRACE_TEST_DIR);
}
