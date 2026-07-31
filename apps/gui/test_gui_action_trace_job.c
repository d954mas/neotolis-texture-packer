/* Job and lifecycle half of the action-state trace oracle.
 *
 * New/Open/Exit declaration and the explicit draft choice they require,
 * drain-on-busy session replacement, and Pack/Export completion ordering.
 * The pack/export cases re-exec this binary as the build worker, so main()
 * keeps the worker dispatch. */

#include "test_gui_action_trace_fixture.h"

void test_lifecycle_apply_mine_resolves_conflict_before_continuing(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int draft_padding = atlas->padding + 2;
    gui_edit_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PADDING, draft_padding, 0.0F);

    char foreign_name[] = "lifecycle-foreign";
    tp_operation operation = {
        .kind = TP_OP_ATLAS_RENAME,
        .atlas_id = {{0}},
        .u.atlas_rename.name = foreign_name,
    };
    operation.atlas_id = atlas_id;
    tp_txn_request request = {
        .schema = TP_TXN_SCHEMA,
        .expected_revision =
            tp_session_snapshot_revision(snapshot),
        .label = "atlas.rename",
        .author = "human",
        .ops = &operation,
        .op_count = 1,
    };
    (void)snprintf(
        request.id_hex, sizeof request.id_hex,
        "%s", "88888888888888888888888888888888");
    tp_txn_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(
            gui_project__test_session(),
            &request, &result, &error));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(NULL, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED,
        gui_draft_phase());

    request_new();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_FALSE(s_confirm_open);
    publish_project_frame();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    atlas = tp_session_snapshot_atlas_by_id(
        gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        draft_padding, atlas->padding);
    s_modal_action = MODAL_CANCEL;
    apply_pending();
}

void test_exit_failed_apply_keeps_confirmation_and_draft_open(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    gui_edit_atlas_setting(
        atlas->id, revision,
        GUI_ATLAS_PADDING, -1, 0.0F);

    request_exit();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        GUI_LIFECYCLE_REQUEST_EXIT,
        s_after_confirm);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    s_modal_action = MODAL_CANCEL;
    apply_pending();
    gui_draft_discard();
}

void test_pack_request_submits_active_draft_before_starting_job(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(TP_GUI_TRACE_TEST_DIR));
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            "__pack_after_draft__.png",
            TP_SOURCE_KIND_FILE));
    settle_project_job();
    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_at(
        snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const int new_padding = atlas->padding + 3;
    gui_edit_atlas_setting(
        atlas->id, revision,
        GUI_ATLAS_PADDING, new_padding, 0.0F);

    gui_request_pack();
    apply_pending();
    publish_project_frame();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT(
        new_padding, atlas_at(0)->padding);
    TEST_ASSERT_EQUAL_INT64(
        revision + 1,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    TEST_ASSERT_EQUAL_STRING(
        "Packing\xE2\x80\xA6", s_status);
    TEST_ASSERT_TRUE(gui_project_job_busy());
}

void test_pack_result_slots_reject_ownerless_results(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);

    tp_result borrowed = {0};
    borrowed.atlas_name = "ownerless";
    tp_session_job_result result = {
        .kind = TP_SESSION_JOB_PACK,
        .state = TP_SESSION_JOB_SUCCEEDED,
        .status = TP_STATUS_OK,
        .pack = {
            .atlas_id = atlas->id,
            .result = &borrowed,
        },
    };
    gui_pack_result_info info = {0};

    TEST_ASSERT_FALSE(
        gui_pack_publish_native(&result, 0.0, &info));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, info.status);
    TEST_ASSERT_NOT_NULL(strstr(info.err, "retained owner"));
    TEST_ASSERT_NULL(gui_pack_result(0));

    memset(&info, 0, sizeof info);
    TEST_ASSERT_FALSE(
        gui_pack_preview_publish(&result, 0.0, &info));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, info.status);
    TEST_ASSERT_NOT_NULL(strstr(info.err, "retained owner"));
    TEST_ASSERT_NULL(gui_pack_preview_result(0));
}

static gui_pack_done drain_current_job(
    gui_pack_result_info *info) {
    gui_pack_done done = GUI_PACK_DONE_NONE;
    for (int attempt = 0;
         attempt < 5000 &&
         done == GUI_PACK_DONE_NONE;
        ++attempt) {
        tp_error error = {{0}};
        gui_project_step_result step = {0};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_step(&step, &error));
        done = gui_pack_consume_completion(
            &step.completion, info);
        if (done == GUI_PACK_DONE_NONE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_NOT_EQUAL(
        GUI_PACK_DONE_NONE, done);
    return done;
}

static void admit_and_drain_pending_refresh(
    uint64_t source_generation_before) {
    TEST_ASSERT_FALSE(gui_project_job_busy());

    gui_pack_result_info refresh = {0};
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_REFRESH_OK,
        drain_current_job(&refresh));
    TEST_ASSERT_TRUE(
        gui_project_source_runtime_generation() >
        source_generation_before);

    /* Duplicate automatic requests coalesce into the one Refresh just drained. */
    tp_error error = {{0}};
    gui_project_step_result step = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&step, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_NONE,
        gui_pack_consume_completion(
            &step.completion, NULL));
    TEST_ASSERT_FALSE(gui_project_job_busy());
}

void test_pending_auto_refresh_survives_pack_and_export_contention(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(TP_GUI_TRACE_TEST_DIR));
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    char error_text[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_async_start(
            atlas_id, error_text,
            sizeof error_text));
    const uint64_t before_pack_refresh =
        gui_project_source_runtime_generation();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id,
            gui_project_committed_revision(),
            "__pending_during_pack__.png",
            TP_SOURCE_KIND_FILE));
    gui_project_refresh_sources();
    gui_project_refresh_sources();

    gui_pack_result_info pack = {0};
    (void)drain_current_job(&pack);
    admit_and_drain_pending_refresh(
        before_pack_refresh);

    snapshot = gui_project_snapshot();
    const tp_snapshot_source *source =
        tp_session_snapshot_source_at(
            snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error_text, sizeof error_text));
    const uint64_t before_export_refresh =
        gui_project_source_runtime_generation();
    TEST_ASSERT_TRUE(
        gui_project_remove_source(
            atlas_id, source->id,
            gui_project_committed_revision()));
    gui_project_refresh_sources();
    gui_project_refresh_sources();

    gui_pack_result_info export_result = {0};
    (void)drain_current_job(&export_result);
    admit_and_drain_pending_refresh(
        before_export_refresh);
}

void test_confirm_save_publishes_before_new_and_new_message_wins(void) {
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  gui_project_save_as(s_save_path, error,
                                                      sizeof error),
                                  error);
    publish_project_frame();
    TEST_ASSERT_FALSE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    const gui_project_create_result created =
        gui_project_add_atlas();
    TEST_ASSERT_TRUE(created.committed);
    TEST_ASSERT_TRUE(created.observation_pending);
    TEST_ASSERT_EQUAL_INT(-1, created.visible_index);
    publish_project_frame();
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    request_new();
    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(
        2, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(GUI_LIFECYCLE_REQUEST_NEW, s_after_confirm);
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    s_modal_action = MODAL_CANCEL;
    apply_pending();
    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(GUI_LIFECYCLE_REQUEST_NONE, s_after_confirm);
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));

    request_new();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(GUI_LIFECYCLE_REQUEST_NONE, s_after_confirm);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_TRUE(gui_project_has_path());
    TEST_ASSERT_EQUAL_INT(
        2, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(NULL, NULL));
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

typedef void (*lifecycle_request_fn)(void);

static void assert_declaration_only_request(
    lifecycle_request_fn request,
    gui_lifecycle_request expected) {
    const uint64_t generation =
        gui_project_session_instance_generation();
    const int64_t revision =
        tp_session_revision(
            gui_project__test_session());
    const int undo_depth =
        gui_project_undo_depth();
    set_status("declaration sentinel");
    request();
    TEST_ASSERT_EQUAL_INT(
        expected,
        s_actions.pending_lifecycle_request);
    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_FALSE(gui_actions__intent_queued(GUI_INTENT_OPEN));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        generation,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        undo_depth,
        gui_project_undo_depth());
    TEST_ASSERT_EQUAL_STRING(
        "declaration sentinel", s_status);
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
}

void test_lifecycle_requests_are_declaration_only(void) {
    assert_declaration_only_request(
        request_new,
        GUI_LIFECYCLE_REQUEST_NEW);
    assert_declaration_only_request(
        request_open,
        GUI_LIFECYCLE_REQUEST_OPEN);
    assert_declaration_only_request(
        request_exit,
        GUI_LIFECYCLE_REQUEST_EXIT);
}

static void assert_lifecycle_requires_draft_choice(
    void (*request)(void),
    gui_lifecycle_request expected) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    gui_edit_atlas_setting(
        atlas->id, revision, GUI_ATLAS_PADDING,
        atlas->padding + 1, 0.0F);

    request();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(expected, s_after_confirm);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));

    s_modal_action = MODAL_CANCEL;
    apply_pending();
    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        GUI_LIFECYCLE_REQUEST_NONE,
        s_after_confirm);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    gui_draft_discard();
}

/* USA-31 partial: the lifecycle-trigger table. The failed-prerequisite and
 * preflight-rejection half is proven by the sibling lifecycle cases. */
void test_lifecycle_requests_require_explicit_draft_choice(void) {
    assert_lifecycle_requires_draft_choice(
        request_new, GUI_LIFECYCLE_REQUEST_NEW);
    assert_lifecycle_requires_draft_choice(
        request_open, GUI_LIFECYCLE_REQUEST_OPEN);
    assert_lifecycle_requires_draft_choice(
        request_exit, GUI_LIFECYCLE_REQUEST_EXIT);
}

void test_lifecycle_apply_continues_only_after_terminal_draft_submit(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int new_padding = atlas->padding + 2;
    gui_edit_atlas_setting(
        atlas->id,
        tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PADDING, new_padding, 0.0F);

    request_new();
    publish_project_frame();
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_SAVE;
    publish_project_frame();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_FALSE(s_confirm_open);
    publish_project_frame();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        new_padding, atlas_at(0)->padding);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    s_modal_action = MODAL_CANCEL;
    publish_project_frame();
}

void test_lifecycle_discard_continues_without_submitting_draft(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    gui_edit_atlas_setting(
        atlas->id, revision, GUI_ATLAS_PADDING,
        atlas->padding + 2, 0.0F);

    const uint64_t generation =
        gui_project_session_instance_generation();
    request_new();
    publish_project_frame();
    s_modal_action = MODAL_DISCARD;
    publish_project_frame();
    publish_project_frame();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        generation + 1U,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_INT64(
        0,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
}

void test_failed_atlas_gesture_aborts_dependent_action_batch(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        trace_target_ref_at(0, 0, &target));
    const tp_snapshot_target *target_before =
        tp_session_snapshot_target_by_id(
            snapshot, atlas->id, target.target_id);
    TEST_ASSERT_NOT_NULL(target_before);
    const bool target_enabled =
        target_before->enabled;
    gui_edit_atlas_setting(
        atlas->id, revision,
        GUI_ATLAS_PADDING, -1, 0.0F);
    gui_edit_target_enabled(
        &target, !target_enabled);
    gui_request_gesture_commit();
    gui_request_add_atlas();

    apply_pending();
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        1, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));
    const tp_snapshot_target *target_after =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(), atlas->id,
            target.target_id);
    TEST_ASSERT_NOT_NULL(target_after);
    TEST_ASSERT_EQUAL_INT(
        target_enabled, target_after->enabled);
    TEST_ASSERT_EQUAL_INT(
        0, s_actions.intent_count);
    TEST_ASSERT_FALSE(gui_actions__intent_queued(GUI_INTENT_ADD_ATLAS));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        1, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));
    gui_draft_discard();
}

void test_sequential_drafts_and_dependent_intent_advance_exactly(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const gui_project_create_result created =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "dependent", NULL, 0);
    TEST_ASSERT_TRUE(created.committed);
    TEST_ASSERT_TRUE(created.observation_pending);
    TEST_ASSERT_EQUAL_INT(-1, created.visible_index);
    publish_project_frame();

    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_by_id(
        snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    gui_animation_ref animation = {0};
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(
            0, 0, &animation));
    TEST_ASSERT_TRUE(
        trace_target_ref_at(0, 0, &target));
    const tp_snapshot_animation *animation_before =
        tp_session_snapshot_animation_at(
            snapshot, atlas_id, 0);
    const tp_snapshot_target *target_before =
        tp_session_snapshot_target_by_id(
            snapshot, atlas_id, target.target_id);
    TEST_ASSERT_NOT_NULL(animation_before);
    TEST_ASSERT_NOT_NULL(target_before);
    const float new_fps =
        animation_before->fps + 1.0F;
    const bool new_enabled =
        !target_before->enabled;
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);

    gui_edit_atlas_setting(
        atlas_id, revision,
        GUI_ATLAS_PADDING,
        atlas->padding + 1, 0.0F);
    gui_request_gesture_commit();
    apply_pending();
    publish_project_frame();

    TEST_ASSERT_TRUE(
        trace_animation_ref_at(
            0, 0, &animation));
    gui_edit_anim_fps(
        &animation, new_fps);
    gui_request_gesture_commit();
    apply_pending();
    publish_project_frame();

    TEST_ASSERT_TRUE(
        trace_target_ref_at(0, 0, &target));
    gui_edit_target_enabled(
        &target, new_enabled);
    apply_pending();
    publish_project_frame();

    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation_after =
        tp_session_snapshot_animation_by_id(
            snapshot, atlas_id,
            animation.animation_id);
    const tp_snapshot_target *target_after =
        tp_session_snapshot_target_by_id(
            snapshot, atlas_id, target.target_id);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_NOT_NULL(animation_after);
    TEST_ASSERT_NOT_NULL(target_after);
    TEST_ASSERT_TRUE(
        animation_after->fps == new_fps);
    TEST_ASSERT_EQUAL_INT(
        new_enabled, target_after->enabled);
    TEST_ASSERT_EQUAL_INT64(
        revision + 3,
        tp_session_snapshot_revision(snapshot));
}

void test_busy_new_enters_drain_and_resets_only_after_completion(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_TRACE_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error, sizeof error));
    TEST_ASSERT_TRUE(
        gui_project_job_busy());
    const uint64_t old_generation =
        gui_project_session_instance_generation();
    set_status("old session remains visible");
    request_new();
    gui_request_refresh();
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_STRING(
        "old session remains visible", s_status);
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_STRING(
        "old session remains visible", s_status);
    TEST_ASSERT_TRUE(gui_actions__intent_queued(GUI_INTENT_REFRESH));
    for (int attempt = 0;
         attempt < 5000 &&
         gui_project_test_state_is_transitioning(
             gui_project_lifecycle_state_query());
         ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_actions_step(NULL, NULL));
        if (gui_project_test_state_is_transitioning(
                gui_project_lifecycle_state_query())) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        old_generation + 1U,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_STRING(
        "New project.", s_status);
    TEST_ASSERT_FALSE(gui_actions__intent_queued(GUI_INTENT_REFRESH));
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
        gui_actions_step(NULL, &error));
    TEST_ASSERT_TRUE(gui_project_has_path());
    TEST_ASSERT_EQUAL_STRING(
        "action-trace.ntpacker_project",
        gui_project_display_name());
}

void test_open_succeeds_without_a_manual_frame_protocol(void) {
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

    char open_error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_test_open(
            s_save_path, open_error,
            sizeof open_error));
    TEST_ASSERT_EQUAL_STRING(
        "action-trace.ntpacker_project",
        gui_project_display_name());
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
    gui_project_step_result terminal = {0};
    for (int i = 0; i < 5000; ++i) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_step(&terminal, NULL));
        state =
            gui_project_job_observed_state();
        if (terminal.completion.kind !=
            TP_SESSION_JOB_NONE) {
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
    const gui_pack_done done =
        gui_pack_consume_completion(
            &terminal.completion, &info);
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_EXPORT_OK, done);
    TEST_ASSERT_EQUAL_INT(1, info.atlases_skipped);
}

void test_owned_terminal_receipt_survives_session_cutover(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_TRACE_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error, sizeof error));
    gui_project_step_result terminal = {0};
    for (int attempt = 0; attempt < 5000; ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_step(&terminal, NULL));
        if (terminal.completion.kind !=
            TP_SESSION_JOB_NONE) {
            break;
        }
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_EXPORT,
        terminal.completion.kind);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());

    gui_project_step_result cutover = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&cutover, NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW,
        cutover.lifecycle_completed);
    tp_session_job_result_destroy(
        &cutover.completion);
    gui_pack_result_info info = {0};
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_EXPORT_OK,
        gui_pack_consume_completion(
            &terminal.completion, &info));
}

void test_project_step_completes_new_open_and_shutdown_transitions(void) {
    const uint64_t generation =
        gui_project_session_instance_generation();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_INVALID_ARGUMENT,
        gui_project_lifecycle_begin_new(NULL));
    gui_project_step_result step = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&step, NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW,
        step.lifecycle_completed);
    tp_session_job_result_destroy(
        &step.completion);
    TEST_ASSERT_EQUAL_UINT64(
        generation + 1U,
        gui_project_session_instance_generation());
    settle_project_job();

    tp_rng rng = tp_rng_os();
    tp_session *saved = NULL;
    tp_session_save_result save_result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_create_default_project(
            &rng, &saved, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_save_as(
            saved, s_save_path,
            &save_result, &error));
    tp_session_destroy(saved);

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_open(
            s_save_path, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_DRAINING,
        gui_project_lifecycle_state_query());
    step = (gui_project_step_result){0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&step, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN,
        step.lifecycle_completed);
    tp_session_job_result_destroy(
        &step.completion);
    settle_project_job();

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_shutdown(
            true, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_SHUTDOWN_DRAINING,
        gui_project_lifecycle_state_query());
    step = (gui_project_step_result){0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&step, &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_CLOSED,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_SHUTDOWN,
        step.lifecycle_completed);
    tp_session_job_result_destroy(
        &step.completion);
}

void test_snapshot_lifetime_epoch_advances_across_session_cutover(void) {
    const uint64_t lifetime_before =
        gui_project_snapshot_lifetime_generation();
    const tp_session_snapshot *before =
        gui_project_snapshot();
    TEST_ASSERT_NOT_NULL(before);
    const uint64_t local_generation_before =
        tp_session_view(
            gui_project__test_session())
            ->snapshot_generation;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    gui_project_step_result step = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_step(&step, NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW,
        step.lifecycle_completed);
    tp_session_job_result_destroy(
        &step.completion);

    const tp_session_snapshot *after =
        gui_project_snapshot();
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_UINT64(
        local_generation_before,
        tp_session_view(
            gui_project__test_session())
            ->snapshot_generation);
    TEST_ASSERT_GREATER_THAN_UINT64(
        lifetime_before,
        gui_project_snapshot_lifetime_generation());
}

void test_project_step_drives_ready_cutover_and_reports_one_terminal(void) {
    const uint64_t generation =
        gui_project_session_instance_generation();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW_DRAINING,
        gui_project_lifecycle_state_query());

    gui_project_step_result result = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        gui_project_step(&result, &error),
        error.msg);

    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW,
        result.lifecycle_completed);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_NONE,
        result.completion.kind);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        generation + 1U,
        gui_project_session_instance_generation());
    TEST_ASSERT_NOT_NULL(gui_project_snapshot());
    tp_session_job_result_destroy(
        &result.completion);
}

void test_terminal_completion_survives_edit_and_recovery_sync(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_TRACE_TEST_DIR));
    char error_text[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error_text, sizeof error_text));
    gui_project_step_result job_step = {0};
    for (int attempt = 0; attempt < 5000; ++attempt) {
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            gui_project_step(
                &job_step, NULL));
        if (job_step.completion.kind !=
            TP_SESSION_JOB_NONE) {
            break;
        }
        nt_time_sleep(0.001);
    }
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_EXPORT,
        job_step.completion.kind);

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t observed_revision =
        tp_session_snapshot_revision(snapshot);
    const tp_op_atlas_settings settings = {
        .mask = TP_AF_PADDING,
        .padding = atlas->padding + 1,
    };
    gui_project_operation_submit_terminal terminal = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_submit_atlas_settings(
            atlas->id, observed_revision,
            &settings,
            (gui_project_operation_submit_identity){0},
            "abababababababababababababababab",
            &terminal, &error));
    TEST_ASSERT_TRUE(terminal.committed);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, terminal.status);
    TEST_ASSERT_EQUAL_INT64(
        observed_revision + 1,
        terminal.revision);
    TEST_ASSERT_EQUAL_INT64(
        observed_revision + 1,
        gui_project_committed_revision());
    TEST_ASSERT_EQUAL_INT64(
        observed_revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));

    gui_recovery_notice notice = {0};
    TEST_ASSERT_FALSE(
        gui_project_recovery_notice_query(
            &notice));
    gui_pack_result_info info = {0};
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_EXPORT_OK,
        gui_pack_consume_completion(
            &job_step.completion, &info));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(NULL, &error));
    TEST_ASSERT_EQUAL_INT64(
        observed_revision + 1,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
}

void test_cancel_request_is_deferred_until_actions_step(void) {
    set_status("borrowed view remains stable");
    gui_request_cancel();
    TEST_ASSERT_EQUAL_STRING(
        "borrowed view remains stable", s_status);

    tp_error error = {{0}};
    gui_actions_step_result step = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(&step, &error));
    TEST_ASSERT_EQUAL_INT(
        1, step.job_receipt_count);
    TEST_ASSERT_EQUAL_INT(
        GUI_JOB_REQUEST_CANCEL,
        step.job_receipts[0].kind);
    TEST_ASSERT_FALSE(
        step.job_receipts[0].admitted);
    TEST_ASSERT_EQUAL_INT(
        STATUS_ERROR, s_status_sev);
    TEST_ASSERT_NOT_NULL(
        strstr(s_status, "Cancel rejected"));
}

void test_rejected_pack_request_is_a_typed_step_receipt(void) {
    gui_request_pack();
    gui_actions_step_result step = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_actions_step(&step, &error));

    TEST_ASSERT_EQUAL_INT(
        1, step.job_receipt_count);
    TEST_ASSERT_EQUAL_INT(
        GUI_JOB_REQUEST_PACK,
        step.job_receipts[0].kind);
    TEST_ASSERT_FALSE(
        step.job_receipts[0].admitted);
    TEST_ASSERT_NOT_EQUAL(
        '\0', step.job_receipts[0].detail[0]);
    TEST_ASSERT_FALSE(gui_project_job_busy());
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
    RUN_TEST(test_pack_result_slots_reject_ownerless_results);
    RUN_TEST(
        test_pending_auto_refresh_survives_pack_and_export_contention);
    RUN_TEST(test_lifecycle_requests_are_declaration_only);
    RUN_TEST(test_lifecycle_requests_require_explicit_draft_choice);
    RUN_TEST(test_lifecycle_apply_continues_only_after_terminal_draft_submit);
    RUN_TEST(test_lifecycle_discard_continues_without_submitting_draft);
    RUN_TEST(test_failed_atlas_gesture_aborts_dependent_action_batch);
    RUN_TEST(test_sequential_drafts_and_dependent_intent_advance_exactly);
    RUN_TEST(test_busy_new_enters_drain_and_resets_only_after_completion);
    RUN_TEST(test_lifecycle_apply_mine_resolves_conflict_before_continuing);
    RUN_TEST(test_exit_failed_apply_keeps_confirmation_and_draft_open);
    RUN_TEST(test_pack_request_submits_active_draft_before_starting_job);
    RUN_TEST(test_confirm_save_publishes_before_new_and_new_message_wins);
    RUN_TEST(test_recovery_decision_runs_next_frame_and_failure_keeps_row);
    RUN_TEST(test_external_save_is_visible_through_the_observation_reducer);
    RUN_TEST(test_open_succeeds_without_a_manual_frame_protocol);
    RUN_TEST(
        test_export_cancel_formatter_distinguishes_uncertain_partial_and_clean);
    RUN_TEST(test_export_failure_formatter_warns_about_uncertain_publication);
    RUN_TEST(test_late_export_cancel_keeps_completed_success_outcome);
    RUN_TEST(test_owned_terminal_receipt_survives_session_cutover);
    RUN_TEST(
        test_project_step_completes_new_open_and_shutdown_transitions);
    RUN_TEST(
        test_snapshot_lifetime_epoch_advances_across_session_cutover);
    RUN_TEST(
        test_project_step_drives_ready_cutover_and_reports_one_terminal);
    RUN_TEST(
        test_terminal_completion_survives_edit_and_recovery_sync);
    RUN_TEST(
        test_cancel_request_is_deferred_until_actions_step);
    RUN_TEST(
        test_rejected_pack_request_is_a_typed_step_receipt);
    RUN_TEST(test_empty_export_surfaces_skipped_atlas_warning);
    return UNITY_END();
}
