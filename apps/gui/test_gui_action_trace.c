#include <stdbool.h>
#include <errno.h>
#include <limits.h>
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
#include "gui_actions_internal.h"
#include "gui_canvas.h"
#include "gui_canvas_internal.h"
#include "gui_pack.h"
#include "gui_pack_internal.h"
#include "gui_project.h"
#include "gui_project_test_driver.h"
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
        gui_project_lifecycle_pump(NULL, &error));
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
    {"draft-reducer", TRACE_ACTION_PRIVATE},
    {"deferred-structural-edit-queue", TRACE_ACTION_PRIVATE},
    {"deferred-side-effect-queue", TRACE_ACTION_PRIVATE},
    {"gesture-boundary", TRACE_ACTION_PRIVATE},
    {"confirmation-intent", TRACE_ACTION_PRIVATE},
    {"recovery-decision", TRACE_ACTION_PRIVATE},
    {"preview-captured-identity", TRACE_ACTION_PRIVATE},
    {"canvas-preview-dropdown", TRACE_VIEW_LOCAL},
    {"settings-dropdowns", TRACE_VIEW_LOCAL},
    {"chrome-menu-contexts", TRACE_VIEW_LOCAL},
};

static const tp_snapshot_atlas *atlas_at(int index) {
    return tp_session_snapshot_atlas_at(gui_project_snapshot(), index);
}

static gui_sprite_ref add_test_sprite_ref(const char *source_path,
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

static void apply_foreign_operation(tp_operation *operation,
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
        gui_project_frame_begin(&error));
    gui_project_frame_end();
}

static void reset_public_action_state(void) {
    s_actions.pending_lifecycle_request =
        GUI_LIFECYCLE_REQUEST_NONE;
    s_pending_open = false;
    s_pending_save = false;
    s_pending_save_as = false;
    s_pending_add_files = false;
    s_pending_add_folder = false;
    s_pending_add_atlas = false;
    s_pending_refresh = false;
    s_pending_pack = false;
    s_pending_export = false;
    s_pending_remove_atlas = false;
    s_pending_remove_source = false;
    s_pending_preview_target = -1;
    s_after_confirm = GUI_LIFECYCLE_REQUEST_NONE;
    s_confirm_open = false;
    s_confirm_draft = false;
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
    gui_project_test_shutdown(true);
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
    TEST_ASSERT_EQUAL_INT(7, counts[TRACE_ACTION_PRIVATE]);
    TEST_ASSERT_EQUAL_INT(3, counts[TRACE_VIEW_LOCAL]);

    /* Compile-time anchors for the shared representatives that cross modules. */
    const void *const shared_anchors[] = {
        &s_sel_atlas,      &s_status,    &s_preview_active,
        &s_preview_target, &s_confirm_open, &s_recovery_open,
        &s_blur_inputs,
    };
    TEST_ASSERT_EQUAL_size_t(7U,
                             sizeof shared_anchors / sizeof shared_anchors[0]);
}

void test_atlas_draft_updates_then_undo_redo_trace_is_exact(void) {
    const tp_snapshot_atlas *atlas = atlas_at(0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int initial_max_size = atlas->max_size;
    const int64_t revision0 =
        tp_session_snapshot_revision(gui_project_snapshot());
    const uint64_t event_sequence0 =
        tp_session_event_sequence(gui_project__test_session());

    gui_edit_atlas_setting(atlas_id, revision0, GUI_ATLAS_MAX_SIZE, 512,
                           0.0F);
    gui_edit_atlas_setting(atlas_id, revision0, GUI_ATLAS_MAX_SIZE, 1024,
                           0.0F);

    TEST_ASSERT_EQUAL_INT64(revision0,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());

    apply_pending();
    TEST_ASSERT_EQUAL_INT64(revision0,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_FALSE(gui_project_can_undo());
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
    TEST_ASSERT_EQUAL_UINT64(
        event_sequence0 + 1U,
        tp_session_event_sequence(gui_project__test_session()));

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
        0, (gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "idle", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        1, (gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "walk", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *selected =
        tp_session_snapshot_animation_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(selected);
    const tp_id128 selected_id = selected->id;
    s_sel_anim = 1;

    gui_edit_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_MAX_SIZE, 1024, 0.0F);
    gui_request_gesture_commit();
    apply_pending();

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

void test_atlas_draft_maps_every_scalar_component(void) {
    const tp_snapshot_atlas *atlas = atlas_at(0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const struct {
        gui_atlas_field field;
        int integer;
        float real;
    } edits[] = {
        {GUI_ATLAS_SHAPE, 0, 0.0F},
        {GUI_ATLAS_MAX_SIZE, 1024, 0.0F},
        {GUI_ATLAS_PADDING, 7, 0.0F},
        {GUI_ATLAS_MARGIN, 3, 0.0F},
        {GUI_ATLAS_EXTRUDE, 2, 0.0F},
        {GUI_ATLAS_ALPHA_THRESHOLD, 100, 0.0F},
        {GUI_ATLAS_MAX_VERTICES, 7, 0.0F},
        {GUI_ATLAS_ALLOW_TRANSFORM, 0, 0.0F},
        {GUI_ATLAS_POWER_OF_TWO, 0, 0.0F},
        {GUI_ATLAS_PIXELS_PER_UNIT, 0, 2.0F},
    };
    for (size_t index = 0U;
         index < sizeof edits / sizeof edits[0];
         ++index) {
        const tp_session_snapshot *snapshot =
            gui_project_snapshot();
        const int64_t revision =
            tp_session_snapshot_revision(snapshot);
        gui_edit_atlas_setting(
            atlas_id, revision, edits[index].field,
            edits[index].integer, edits[index].real);
        gui_request_gesture_commit();
        apply_pending();
        TEST_ASSERT_EQUAL_INT(
            GUI_EDIT_IDLE, gui_draft_phase());
        TEST_ASSERT_EQUAL_INT64(
            revision + 1,
            tp_session_snapshot_revision(
                gui_project_snapshot()));
    }
    atlas = tp_session_snapshot_atlas_by_id(
        gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(1024, atlas->max_size);
    TEST_ASSERT_EQUAL_INT(7, atlas->padding);
    TEST_ASSERT_EQUAL_INT(3, atlas->margin);
    TEST_ASSERT_EQUAL_INT(2, atlas->extrude);
    TEST_ASSERT_EQUAL_INT(100, atlas->alpha_threshold);
    TEST_ASSERT_EQUAL_INT(7, atlas->max_vertices);
    TEST_ASSERT_FALSE(atlas->allow_transform);
    TEST_ASSERT_FALSE(atlas->power_of_two);
    TEST_ASSERT_TRUE(atlas->pixels_per_unit == 2.0F);
    TEST_ASSERT_EQUAL_INT(0, atlas->shape);
}

void test_undo_blocks_without_submitting_active_atlas_draft(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    gui_edit_atlas_setting(
        atlas->id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PADDING, atlas->padding + 1, 0.0F);
    const int64_t revision =
        tp_session_snapshot_revision(
            gui_project_snapshot());
    do_undo();
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        "Apply or discard the active edit before Undo.",
        s_status);
    gui_draft_discard();
}

void test_foreign_model_transaction_conflicts_active_atlas_draft(void) {
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

    char foreign_name[] = "foreign-commit";
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
        "%s", "77777777777777777777777777777777");
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
        gui_project_frame_begin(&error));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED,
        gui_draft_phase());
    int effective = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas_id, GUI_ATLAS_PADDING,
            &effective, NULL));
    TEST_ASSERT_EQUAL_INT(draft_padding, effective);
    gui_draft_discard();
}

void test_origin_apply_mine_preserves_foreign_sibling_component(void) {
    const gui_sprite_ref initial = add_test_sprite_ref(
        "__origin_component_source__.png", "origin-sprite.png");
    char source_key[] = "origin-sprite.png";
    tp_operation baseline = {
        .kind = TP_OP_SPRITE_OVERRIDE_SET,
        .atlas_id = {{0}},
        .u.sprite_set = {
            .source_id = {{0}},
            .src_key = source_key,
            .mask = TP_SPF_ORIGIN,
            .origin_x = 0.5F,
            .origin_y = 0.5F,
        },
    };
    baseline.atlas_id = initial.atlas_id;
    baseline.u.sprite_set.source_id = initial.source_id;
    apply_foreign_operation(
        &baseline, "a1000000000000000000000000000001");

    gui_sprite_ref sprite = initial;
    sprite.expected_revision =
        tp_session_snapshot_revision(gui_project_snapshot());
    gui_edit_sprite_origin(&sprite, 0, 0.25F);

    tp_operation foreign = baseline;
    foreign.u.sprite_set.origin_y = 0.75F;
    apply_foreign_operation(
        &foreign, "a1000000000000000000000000000002");
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_TRUE(gui_draft_can_apply());

    gui_draft_apply_mine();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    const tp_snapshot_sprite *committed =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), sprite.atlas_id,
            sprite.source_id, sprite.source_key);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_TRUE(committed->origin_x == 0.25F);
    TEST_ASSERT_TRUE(committed->origin_y == 0.75F);
}

void test_slice9_apply_mine_preserves_newest_untouched_components(void) {
    const gui_sprite_ref initial = add_test_sprite_ref(
        "__slice9_component_source__.png", "slice9-sprite.png");
    char source_key[] = "slice9-sprite.png";
    tp_operation baseline = {
        .kind = TP_OP_SPRITE_OVERRIDE_SET,
        .atlas_id = {{0}},
        .u.sprite_set = {
            .source_id = {{0}},
            .src_key = source_key,
            .mask = TP_SPF_SLICE9,
            .slice9 = {1, 2, 3, 4},
        },
    };
    baseline.atlas_id = initial.atlas_id;
    baseline.u.sprite_set.source_id = initial.source_id;
    apply_foreign_operation(
        &baseline, "a2000000000000000000000000000001");

    gui_sprite_ref sprite = initial;
    sprite.expected_revision =
        tp_session_snapshot_revision(gui_project_snapshot());
    gui_edit_sprite_slice9(&sprite, 0, 10);

    tp_operation foreign = baseline;
    foreign.u.sprite_set.slice9[1] = 22;
    foreign.u.sprite_set.slice9[2] = 33;
    foreign.u.sprite_set.slice9[3] = 44;
    apply_foreign_operation(
        &foreign, "a2000000000000000000000000000002");
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());

    gui_draft_apply_mine();
    apply_pending();
    const tp_snapshot_sprite *committed =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), sprite.atlas_id,
            sprite.source_id, sprite.source_key);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_EQUAL_UINT16(10, committed->slice9_lrtb[0]);
    TEST_ASSERT_EQUAL_UINT16(22, committed->slice9_lrtb[1]);
    TEST_ASSERT_EQUAL_UINT16(33, committed->slice9_lrtb[2]);
    TEST_ASSERT_EQUAL_UINT16(44, committed->slice9_lrtb[3]);
}

void test_flip_h_apply_mine_preserves_foreign_flip_v(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        0, (gui_project_create_animation(
               atlas->id,
               tp_session_snapshot_revision(snapshot),
               "component-flip", NULL, 0))
               .visible_index);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(0, 0, &animation));
    gui_edit_anim_flip(&animation, 0, true);

    tp_operation foreign = {
        .kind = TP_OP_ANIMATION_SETTINGS_SET,
        .atlas_id = {{0}},
        .u.anim_settings = {
            .anim_id = {{0}},
            .mask = TP_ANF_FLIP_V,
            .flip_v = true,
        },
    };
    foreign.atlas_id = animation.atlas_id;
    foreign.u.anim_settings.anim_id =
        animation.animation_id;
    apply_foreign_operation(
        &foreign, "a3000000000000000000000000000001");
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());

    gui_draft_apply_mine();
    apply_pending();
    const tp_snapshot_animation *committed =
        tp_session_snapshot_animation_by_id(
            gui_project_snapshot(), animation.atlas_id,
            animation.animation_id);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_TRUE(committed->flip_h);
    TEST_ASSERT_TRUE(committed->flip_v);
}

void test_one_sprite_gesture_creates_one_undo_entry(void) {
    gui_sprite_ref sprite = add_test_sprite_ref(
        "__sprite_gesture_source__.png", "gesture-sprite.png");
    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const int undo_before = gui_project_undo_depth();

    gui_edit_sprite_origin(&sprite, 0, 0.25F);
    gui_edit_sprite_origin(&sprite, 0, 0.375F);
    apply_pending();
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(undo_before, gui_project_undo_depth());

    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT64(
        revision_before + 1,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        undo_before + 1, gui_project_undo_depth());
    const tp_snapshot_sprite *committed =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), sprite.atlas_id,
            sprite.source_id, sprite.source_key);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_TRUE(committed->origin_x == 0.375F);

    do_undo();
    const tp_snapshot_sprite *undone =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), sprite.atlas_id,
            sprite.source_id, sprite.source_key);
    TEST_ASSERT_TRUE(
        !undone ||
        undone->origin_x == TP_PROJECT_ORIGIN_DEFAULT);
}

void test_invalid_sprite_value_is_rejected_without_losing_draft(void) {
    const gui_sprite_ref sprite = add_test_sprite_ref(
        "__invalid_sprite_value_source__.png",
        "invalid-sprite.png");
    const int64_t revision_before =
        tp_session_snapshot_revision(
            gui_project_snapshot());

    gui_edit_sprite_override(
        &sprite, GUI_SPRITE_OV_MARGIN,
        INT_MAX);
    gui_request_gesture_commit();
    apply_pending();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    int retained = 0;
    TEST_ASSERT_TRUE(gui_sprite_edit_value(
        &sprite, GUI_SPRITE_EDIT_OVERRIDE,
        GUI_SPRITE_OV_MARGIN, &retained, NULL));
    TEST_ASSERT_EQUAL_INT(INT_MAX, retained);
    gui_draft_discard();
}

void test_deleted_source_preserves_sprite_draft_and_disables_apply(void) {
    const gui_sprite_ref sprite = add_test_sprite_ref(
        "__deleted_source_draft__.png",
        "deleted-source-sprite.png");
    gui_edit_sprite_origin(
        &sprite, 0, 0.25F);

    tp_operation remove = {
        .kind = TP_OP_SOURCE_REMOVE,
        .atlas_id = {{0}},
        .u.source_ref = {
            .source_id = {{0}},
            .key = NULL,
        },
    };
    remove.atlas_id = sprite.atlas_id;
    remove.u.source_ref.source_id =
        sprite.source_id;
    apply_foreign_operation(
        &remove,
        "a4000000000000000000000000000001");

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_FALSE(gui_draft_can_apply());
    float retained = 0.0F;
    TEST_ASSERT_TRUE(gui_sprite_edit_value(
        &sprite, GUI_SPRITE_EDIT_ORIGIN,
        0, NULL, &retained));
    TEST_ASSERT_TRUE(retained == 0.25F);
    gui_draft_discard();
}

void test_deleted_animation_preserves_draft_and_disables_apply(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        0, (gui_project_create_animation(
                atlas->id,
                tp_session_snapshot_revision(snapshot),
                "deleted-animation", NULL, 0))
               .visible_index);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(gui_project_animation_ref_at(
        0, 0, &animation));
    gui_edit_anim_fps(&animation, 24.0F);

    tp_operation remove = {
        .kind = TP_OP_ANIMATION_REMOVE,
        .atlas_id = {{0}},
        .u.anim_ref = {.anim_id = {{0}}},
    };
    remove.atlas_id = animation.atlas_id;
    remove.u.anim_ref.anim_id =
        animation.animation_id;
    apply_foreign_operation(
        &remove,
        "a5000000000000000000000000000001");

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_FALSE(gui_draft_can_apply());
    float retained = 0.0F;
    TEST_ASSERT_TRUE(gui_animation_edit_value(
        &animation, GUI_ANIMATION_EDIT_FPS,
        0, NULL, &retained));
    TEST_ASSERT_TRUE(retained == 24.0F);
    gui_draft_discard();
}

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
        gui_project_frame_begin(&error));
    gui_project_frame_end();
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

void test_failed_lifecycle_apply_keeps_explicit_draft_choice_open(void) {
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

    request_new();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        GUI_LIFECYCLE_REQUEST_NEW,
        s_after_confirm);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    s_modal_action = MODAL_CANCEL;
    apply_pending();
    gui_draft_discard();
}

void test_one_draft_owner_rejects_text_begin_while_atlas_scalar_is_active(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int draft_padding = atlas->padding + 3;
    gui_edit_atlas_setting(
        atlas->id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PADDING, draft_padding, 0.0F);

    TEST_ASSERT_FALSE(
        gui_text_edit_begin_atlas_name(
            atlas->id, tp_session_snapshot_revision(snapshot),
            atlas->name));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    int effective_padding = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas->id, GUI_ATLAS_PADDING,
            &effective_padding, NULL));
    TEST_ASSERT_EQUAL_INT(
        draft_padding, effective_padding);

    gui_draft_discard();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
}

void test_failed_target_path_submit_blocks_dependent_actions_and_preserves_text(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        gui_project_target_ref_at(0, 0, &target));
    const tp_snapshot_target *before =
        tp_session_snapshot_target_by_id(
            snapshot, atlas->id, target.target_id);
    TEST_ASSERT_NOT_NULL(before);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const bool enabled_before = before->enabled;
    static const char invalid_utf8[] = {
        (char)0xc3, '(', '\0'};

    TEST_ASSERT_TRUE(
        gui_text_edit_begin_target_out_path(
            &target, before->out_path));
    TEST_ASSERT_TRUE(
        gui_text_edit_update(invalid_utf8));
    gui_edit_target_enabled(
        &target, !enabled_before);
    s_pending_pack = true;
    gui_request_gesture_commit();
    apply_pending();

    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    const tp_snapshot_target *after =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(), atlas->id,
            target.target_id);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(
        enabled_before, after->enabled);
    TEST_ASSERT_FALSE(s_pending_pack);
    TEST_ASSERT_EQUAL_INT(
        0, s_actions.target_intent_count);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_MEMORY(
        invalid_utf8, gui_text_edit_value(),
        sizeof invalid_utf8);
    gui_draft_discard();
}

static tp_id128 begin_conflicted_atlas_name_draft(
    const char *mine) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_atlas_name(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            atlas->name));
    TEST_ASSERT_TRUE(gui_text_edit_update(mine));

    char foreign_name[] = "foreign-name";
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
        "%s", "99999999999999999999999999999999");
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
        gui_project_frame_begin(&error));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        mine, gui_text_edit_value());
    return atlas_id;
}

void test_conflicted_atlas_rename_new_cancel_preserves_draft(void) {
    (void)begin_conflicted_atlas_name_draft(
        "mine-after-cancel");

    request_new();
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_CANCEL;
    apply_pending();

    TEST_ASSERT_FALSE(s_confirm_open);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        "mine-after-cancel", gui_text_edit_value());
    gui_draft_discard();
}

void test_conflicted_atlas_rename_new_discard_continues_without_submit(void) {
    const tp_id128 atlas_id =
        begin_conflicted_atlas_name_draft(
            "mine-to-discard");

    request_new();
    apply_pending();
    s_modal_action = MODAL_DISCARD;
    apply_pending();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(
        "foreign-name", atlas->name);
    s_modal_action = MODAL_CANCEL;
    apply_pending();
}

void test_conflicted_atlas_rename_new_apply_mine_precedes_dirty_choice(void) {
    const tp_id128 atlas_id =
        begin_conflicted_atlas_name_draft(
            "mine-to-apply");

    request_new();
    apply_pending();
    s_modal_action = MODAL_SAVE;
    apply_pending();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(
        "mine-to-apply", atlas->name);
    s_modal_action = MODAL_CANCEL;
    apply_pending();
}

void test_text_drafts_submit_exact_atlas_animation_sprite_and_target_ops(void) {
    const tp_session_snapshot *initial =
        gui_project_snapshot();
    const tp_snapshot_atlas *initial_atlas =
        tp_session_snapshot_atlas_at(initial, 0);
    TEST_ASSERT_NOT_NULL(initial_atlas);
    const tp_id128 atlas_id = initial_atlas->id;

    TEST_ASSERT_TRUE(
        gui_text_edit_begin_atlas_name(
            atlas_id,
            tp_session_snapshot_revision(initial),
            initial_atlas->name));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("atlas-text-draft"));
    gui_request_gesture_commit();
    apply_pending();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(
        "atlas-text-draft", atlas->name);

    const gui_project_create_result created =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(
                gui_project_snapshot()),
            "animation-before", NULL, 0);
    TEST_ASSERT_TRUE(created.committed);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(
            0, created.visible_index, &animation));
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_animation_name(
            &animation, "animation-before"));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("animation-after"));
    gui_request_gesture_commit();
    apply_pending();
    const tp_snapshot_animation *animation_after =
        tp_session_snapshot_animation_by_id(
            gui_project_snapshot(), atlas_id,
            animation.animation_id);
    TEST_ASSERT_NOT_NULL(animation_after);
    TEST_ASSERT_EQUAL_STRING(
        "animation-after", animation_after->name);

    const tp_session_snapshot *before_source =
        gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id,
            tp_session_snapshot_revision(before_source),
            "__action_trace_text_source__.png",
            TP_SOURCE_KIND_FILE));
    const tp_session_snapshot *with_source =
        gui_project_snapshot();
    const tp_snapshot_source *source =
        tp_session_snapshot_source_at(
            with_source, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source);
    const tp_id128 source_id = source->id;
    const gui_sprite_ref sprite = {
        atlas_id, source_id, "sprite.png",
        tp_session_snapshot_revision(with_source)};
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_sprite_rename(
            &sprite, ""));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("sprite-after"));
    gui_request_gesture_commit();
    apply_pending();
    const tp_snapshot_sprite *sprite_after =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), atlas_id,
            source_id, "sprite.png");
    TEST_ASSERT_NOT_NULL(sprite_after);
    TEST_ASSERT_EQUAL_STRING(
        "sprite-after", sprite_after->rename);

    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        gui_project_target_ref_at(0, 0, &target));
    const tp_snapshot_target *target_before =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(), atlas_id,
            target.target_id);
    TEST_ASSERT_NOT_NULL(target_before);
    char exporter_before[TP_EXPORTER_ID_MAX];
    (void)snprintf(
        exporter_before, sizeof exporter_before,
        "%s", target_before->exporter_id);
    const bool enabled_before =
        target_before->enabled;
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_target_out_path(
            &target, target_before->out_path));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("exact-target-path"));
    gui_request_gesture_commit();
    apply_pending();
    const tp_snapshot_target *target_after =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(), atlas_id,
            target.target_id);
    TEST_ASSERT_NOT_NULL(target_after);
    TEST_ASSERT_EQUAL_STRING(
        "exact-target-path", target_after->out_path);
    TEST_ASSERT_EQUAL_STRING(
        exporter_before, target_after->exporter_id);
    TEST_ASSERT_EQUAL_INT(
        enabled_before, target_after->enabled);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
}

void test_target_browse_submits_typed_path_before_starting_dialog_gesture(void) {
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        gui_project_target_ref_at(0, 0, &target));
    const tp_snapshot_target *initial =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(),
            target.atlas_id, target.target_id);
    TEST_ASSERT_NOT_NULL(initial);
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_target_out_path(
            &target, initial->out_path));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("typed-before-browse"));

    TEST_ASSERT_TRUE(gui_actions__submit_draft());
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    const tp_snapshot_target *typed =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(),
            target.atlas_id, target.target_id);
    TEST_ASSERT_NOT_NULL(typed);
    TEST_ASSERT_EQUAL_STRING(
        "typed-before-browse", typed->out_path);

    target.expected_revision =
        tp_session_snapshot_revision(
            gui_project_snapshot());
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_target_out_path(
            &target, typed->out_path));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("selected-by-browse"));
    TEST_ASSERT_TRUE(gui_actions__submit_draft());
    const tp_snapshot_target *selected =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(),
            target.atlas_id, target.target_id);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_EQUAL_STRING(
        "selected-by-browse", selected->out_path);
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
        gui_pack_publish_native(&result, 0, 0.0, &info));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, info.status);
    TEST_ASSERT_NOT_NULL(strstr(info.err, "retained owner"));
    TEST_ASSERT_NULL(gui_pack_result(0));

    memset(&info, 0, sizeof info);
    TEST_ASSERT_FALSE(
        gui_pack_preview_publish(&result, 0, 0.0, &info));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT, info.status);
    TEST_ASSERT_NOT_NULL(strstr(info.err, "retained owner"));
    TEST_ASSERT_NULL(gui_pack_preview_result(0));
}

void test_deferred_frame_delete_clears_selection_only_after_commit(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    char source_path[1200];
    TEST_ASSERT_TRUE(
        snprintf(
            source_path, sizeof source_path,
            "%s/delete-frame.png",
            TP_GUI_TRACE_TEST_DIR) > 0);
    FILE *source = fopen(source_path, "wb");
    TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_EQUAL_size_t(
        1U, fwrite("x", 1U, 1U, source));
    TEST_ASSERT_EQUAL_INT(0, fclose(source));
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            source_path, TP_SOURCE_KIND_FILE));

    snapshot = gui_project_snapshot();
    const tp_snapshot_source *source_record =
        tp_session_snapshot_source_at(
            snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source_record);
    tp_op_sprite_ref frame = {
        source_record->id,
        "delete-frame.png",
    };
    const gui_project_create_result created =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "delete-frame", &frame, 1);
    TEST_ASSERT_TRUE(created.committed);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(
            0, created.visible_index,
            &animation));
    s_sel_anim = created.visible_index;
    s_sel_anim_frame = 0;

    gui_edit_anim_frame_remove(&animation, 0);
    TEST_ASSERT_EQUAL_INT(0, s_sel_anim_frame);
    apply_pending();
    TEST_ASSERT_EQUAL_INT(-1, s_sel_anim_frame);
    const tp_snapshot_animation *after =
        tp_session_snapshot_animation_at(
            gui_project_snapshot(), atlas_id,
            created.visible_index);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(0, after->frame_count);

    s_sel_anim_frame = 0;
    gui_edit_anim_frame_remove(&animation, 0);
    apply_pending();
    TEST_ASSERT_EQUAL_INT(0, s_sel_anim_frame);
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
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
    const int animation_index =
        gui_project_create_animation(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            "walk", NULL, 0)
            .visible_index;
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

    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
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
        GUI_PROJECT_LIFECYCLE_DRAINING,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_TRUE(gui_project_has_path());
    TEST_ASSERT_EQUAL_INT(
        2, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));
    gui_actions_pump_lifecycle();
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
    TEST_ASSERT_FALSE(s_pending_open);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
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
    apply_pending();
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_SAVE;
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        new_padding, atlas_at(0)->padding);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    s_modal_action = MODAL_CANCEL;
    apply_pending();
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

    request_new();
    apply_pending();
    s_modal_action = MODAL_DISCARD;
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_DRAINING,
        gui_project_lifecycle_state_query());
    gui_actions_pump_lifecycle();
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
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
        gui_project_target_ref_at(0, 0, &target));
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
    s_pending_add_atlas = true;

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
        0, s_actions.target_intent_count);
    TEST_ASSERT_FALSE(s_pending_add_atlas);
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
    TEST_ASSERT_EQUAL_INT(
        0, (gui_project_create_animation(
               atlas_id,
               tp_session_snapshot_revision(snapshot),
               "dependent", NULL, 0))
               .visible_index);

    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_by_id(
        snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    gui_animation_ref animation = {0};
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(
            0, 0, &animation));
    TEST_ASSERT_TRUE(
        gui_project_target_ref_at(0, 0, &target));
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

    TEST_ASSERT_TRUE(
        gui_project_animation_ref_at(
            0, 0, &animation));
    gui_edit_anim_fps(
        &animation, new_fps);
    gui_request_gesture_commit();
    apply_pending();

    TEST_ASSERT_TRUE(
        gui_project_target_ref_at(0, 0, &target));
    gui_edit_target_enabled(
        &target, new_enabled);
    apply_pending();

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

void test_prepare_failure_preserves_buffered_edit(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int padding_before = atlas->padding;
    const int64_t revision_before =
        tp_session_snapshot_revision(snapshot);

    gui_edit_atlas_setting(
        atlas_id, revision_before,
        GUI_ATLAS_PADDING,
        padding_before + 3, 0.0F);
    apply_pending();
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(
            gui_project_snapshot()));

    gui_project__test_fail_next_observe();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_lifecycle_begin_new(
            &error));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(
            gui_project_snapshot()));

    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT64(
        revision_before + 1,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    atlas = tp_session_snapshot_atlas_by_id(
        gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        padding_before + 3,
        atlas->padding);
}

void test_busy_new_enters_drain_and_resets_only_after_completion(void) {
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_TRACE_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_export_async_start(
            error, sizeof error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(NULL, NULL));
    TEST_ASSERT_TRUE(
        gui_project_job_busy());
    const uint64_t old_generation =
        gui_project_session_instance_generation();
    set_status("old session remains visible");
    request_new();
    s_pending_refresh = true;
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_STRING(
        "old session remains visible", s_status);
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_DRAINING,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        old_generation,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_STRING(
        "old session remains visible", s_status);
    TEST_ASSERT_TRUE(s_pending_refresh);
    for (int attempt = 0;
         attempt < 5000 &&
         gui_project_lifecycle_state_query() ==
             GUI_PROJECT_LIFECYCLE_DRAINING;
         ++attempt) {
        gui_actions_pump_lifecycle();
        if (gui_project_lifecycle_state_query() ==
            GUI_PROJECT_LIFECYCLE_DRAINING) {
            TEST_ASSERT_EQUAL_INT(
                TP_STATUS_OK,
                gui_project_frame_begin(NULL));
            gui_actions_poll_host_completion();
            gui_project_frame_end();
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_UINT64(
        old_generation + 1U,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_STRING(
        "New project.", s_status);
    TEST_ASSERT_FALSE(s_pending_refresh);
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
        gui_project_test_open(
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
            gui_project_lifecycle_pump(NULL, NULL));
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

void test_terminal_job_completion_is_consumed_before_cutover(void) {
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
            gui_project_lifecycle_pump(NULL, NULL));
        if (!gui_project__test_host_has_staged_completion()) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_TRUE(
        gui_project__test_host_has_staged_completion());
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_DRAINING,
        gui_project_lifecycle_state_query());

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(NULL));
    gui_pack_result_info info = {0};
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_EXPORT_OK,
        gui_pack_poll(&info));
    gui_project_frame_end();
    gui_project_lifecycle_kind completed =
        GUI_PROJECT_LIFECYCLE_NONE;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(
            &completed, NULL));
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_NEW,
        completed);
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_NONE,
        gui_pack_poll(NULL));
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
    RUN_TEST(test_pack_result_slots_reject_ownerless_results);
    RUN_TEST(
        test_lifecycle_requests_are_declaration_only);
    RUN_TEST(
        test_lifecycle_requests_require_explicit_draft_choice);
    RUN_TEST(
        test_lifecycle_apply_continues_only_after_terminal_draft_submit);
    RUN_TEST(
        test_lifecycle_discard_continues_without_submitting_draft);
    RUN_TEST(
        test_failed_atlas_gesture_aborts_dependent_action_batch);
    RUN_TEST(
        test_sequential_drafts_and_dependent_intent_advance_exactly);
    RUN_TEST(
        test_prepare_failure_preserves_buffered_edit);
    RUN_TEST(
        test_busy_new_enters_drain_and_resets_only_after_completion);
    RUN_TEST(test_atlas_draft_updates_then_undo_redo_trace_is_exact);
    RUN_TEST(test_atlas_draft_maps_every_scalar_component);
    RUN_TEST(test_undo_blocks_without_submitting_active_atlas_draft);
    RUN_TEST(
        test_foreign_model_transaction_conflicts_active_atlas_draft);
    RUN_TEST(
        test_origin_apply_mine_preserves_foreign_sibling_component);
    RUN_TEST(
        test_slice9_apply_mine_preserves_newest_untouched_components);
    RUN_TEST(
        test_flip_h_apply_mine_preserves_foreign_flip_v);
    RUN_TEST(test_one_sprite_gesture_creates_one_undo_entry);
    RUN_TEST(
        test_invalid_sprite_value_is_rejected_without_losing_draft);
    RUN_TEST(
        test_deleted_source_preserves_sprite_draft_and_disables_apply);
    RUN_TEST(
        test_deleted_animation_preserves_draft_and_disables_apply);
    RUN_TEST(
        test_lifecycle_apply_mine_resolves_conflict_before_continuing);
    RUN_TEST(
        test_failed_lifecycle_apply_keeps_explicit_draft_choice_open);
    RUN_TEST(
        test_one_draft_owner_rejects_text_begin_while_atlas_scalar_is_active);
    RUN_TEST(
        test_failed_target_path_submit_blocks_dependent_actions_and_preserves_text);
    RUN_TEST(
        test_conflicted_atlas_rename_new_cancel_preserves_draft);
    RUN_TEST(
        test_conflicted_atlas_rename_new_discard_continues_without_submit);
    RUN_TEST(
        test_conflicted_atlas_rename_new_apply_mine_precedes_dirty_choice);
    RUN_TEST(
        test_text_drafts_submit_exact_atlas_animation_sprite_and_target_ops);
    RUN_TEST(
        test_target_browse_submits_typed_path_before_starting_dialog_gesture);
    RUN_TEST(test_undo_redo_preserves_selected_animation_by_stable_id);
    RUN_TEST(
        test_deferred_frame_delete_clears_selection_only_after_commit);
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
        test_terminal_job_completion_is_consumed_before_cutover);
    RUN_TEST(test_empty_export_surfaces_skipped_atlas_warning);
    return UNITY_END();
}
