/* Draft and conflict half of the action-state trace oracle.
 *
 * One draft owner, foreign-commit conflict, Apply Mine component merges, and
 * the deferred gestures that must follow a stable selection across the commit
 * that changes it. Kept as its own target so a draft regression fails a test
 * named for drafts instead of a 2900-line catch-all. */

#include "test_gui_action_trace_fixture.h"

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

    pump_action_frame();
    TEST_ASSERT_EQUAL_INT64(revision0,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_FALSE(gui_project_can_undo());
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());

    gui_request_gesture_commit();
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());
    pump_action_frame();

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

    const tp_id128 selected_source_id = {{7}};
    gui_rows_select_primary_ref(
        selected_source_id, "preserved.png", false);
    do_undo();
    TEST_ASSERT_EQUAL_STRING(
        "Undo (undo:0 redo:1)", s_status);
    settle_project_job();
    TEST_ASSERT_EQUAL_INT64(revision1 + 1,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(initial_max_size, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(0, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(1, gui_project_redo_depth());
    TEST_ASSERT_TRUE(gui_rows_primary_matches(
        selected_source_id, "preserved.png", false));

    do_redo();
    TEST_ASSERT_EQUAL_STRING(
        "Redo (undo:1 redo:0)", s_status);
    settle_project_job();
    TEST_ASSERT_EQUAL_INT64(revision1 + 2,
                            tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(1024, atlas_at(0)->max_size);
    TEST_ASSERT_EQUAL_INT(1, gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(0, gui_project_redo_depth());
    TEST_ASSERT_TRUE(gui_rows_primary_matches(
        selected_source_id, "preserved.png", false));
}

void test_undo_redo_preserves_selected_animation_by_stable_id(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;

    TEST_ASSERT_EQUAL_INT(
        0, (create_animation_observed(
               atlas_id, tp_session_snapshot_revision(snapshot), "idle", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        1, (create_animation_observed(
               atlas_id, tp_session_snapshot_revision(snapshot), "walk", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *selected =
        tp_session_snapshot_animation_at(snapshot, atlas_id, 1);
    TEST_ASSERT_NOT_NULL(selected);
    const tp_id128 selected_id = selected->id;
    gui_view_select_animation(selected_id);
    TEST_ASSERT_EQUAL_INT(1, gui_view_animation_index(snapshot));

    gui_edit_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_MAX_SIZE, 1024, 0.0F);
    gui_request_gesture_commit();
    pump_action_frame();

    do_undo();
    settle_project_job();
    const int undo_animation_index =
        gui_view_animation_index(gui_project_snapshot());
    TEST_ASSERT_GREATER_OR_EQUAL(0, undo_animation_index);
    selected = tp_session_snapshot_animation_at(
        gui_project_snapshot(), atlas_id, undo_animation_index);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_TRUE(tp_id128_eq(selected_id, selected->id));
    TEST_ASSERT_TRUE(tp_id128_eq(
        selected_id, gui_view_animation_id()));

    do_redo();
    settle_project_job();
    const int redo_animation_index =
        gui_view_animation_index(gui_project_snapshot());
    TEST_ASSERT_GREATER_OR_EQUAL(0, redo_animation_index);
    selected = tp_session_snapshot_animation_at(
        gui_project_snapshot(), atlas_id, redo_animation_index);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_TRUE(tp_id128_eq(selected_id, selected->id));
    TEST_ASSERT_TRUE(tp_id128_eq(
        selected_id, gui_view_animation_id()));
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
        pump_action_frame();
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

/* Sibling of test_atlas_draft_maps_every_scalar_component for the descriptor
 * rows the rest of the corpus never asserts: the three sprite overrides
 * ROTATE / MAXVERT / EXTRUDE, slice9 slots 2 and 3 (only slots 0 and 1 are
 * driven elsewhere), and the animation FLIP_V component as a REAL value change
 * rather than a preserved foreign sibling. Same table-driven oracle shape: every
 * row must reach IDLE in exactly one revision and land its own field. */
void test_sprite_and_animation_drafts_map_every_uncovered_component(void) {
    gui_sprite_ref sprite = add_test_sprite_ref(
        "__uncovered_component_source__.png",
        "uncovered-component-sprite.png");
    const struct {
        gui_sprite_edit_kind kind;
        int component;
        int value;
    } edits[] = {
        {GUI_SPRITE_EDIT_OVERRIDE,
         GUI_SPRITE_OV_ROTATE, 0},
        {GUI_SPRITE_EDIT_OVERRIDE,
         GUI_SPRITE_OV_MAXVERT, 7},
        {GUI_SPRITE_EDIT_OVERRIDE,
         GUI_SPRITE_OV_EXTRUDE, 2},
        {GUI_SPRITE_EDIT_SLICE9, 2, 33},
        {GUI_SPRITE_EDIT_SLICE9, 3, 44},
    };
    for (size_t index = 0U;
         index < sizeof edits / sizeof edits[0];
         ++index) {
        const int64_t revision =
            tp_session_snapshot_revision(
                gui_project_snapshot());
        sprite.expected_revision = revision;
        if (edits[index].kind ==
            GUI_SPRITE_EDIT_SLICE9) {
            gui_edit_sprite_slice9(
                &sprite, edits[index].component,
                edits[index].value);
        } else {
            gui_edit_sprite_override(
                &sprite,
                (gui_sprite_ov)edits[index].component,
                edits[index].value);
        }
        gui_request_gesture_commit();
        pump_action_frame();
        TEST_ASSERT_EQUAL_INT(
            GUI_EDIT_IDLE, gui_draft_phase());
        TEST_ASSERT_EQUAL_INT64(
            revision + 1,
            tp_session_snapshot_revision(
                gui_project_snapshot()));
    }
    const tp_snapshot_sprite *committed =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), sprite.atlas_id,
            sprite.source_id, sprite.source_key);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_EQUAL_INT(
        0, committed->override_allow_rotate);
    TEST_ASSERT_EQUAL_INT(
        7, committed->override_max_vertices);
    TEST_ASSERT_EQUAL_INT(
        2, committed->override_extrude);
    TEST_ASSERT_EQUAL_UINT16(
        33, committed->slice9_lrtb[2]);
    TEST_ASSERT_EQUAL_UINT16(
        44, committed->slice9_lrtb[3]);

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(
        0, (create_animation_observed(
               atlas->id,
               tp_session_snapshot_revision(snapshot),
               "uncovered-flip", NULL, 0))
               .visible_index);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(0, 0, &animation));
    const int64_t animation_revision =
        tp_session_snapshot_revision(
            gui_project_snapshot());
    gui_edit_anim_flip(&animation, 1, true);
    gui_request_gesture_commit();
    pump_action_frame();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT64(
        animation_revision + 1,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    const tp_snapshot_animation *committed_animation =
        tp_session_snapshot_animation_by_id(
            gui_project_snapshot(), animation.atlas_id,
            animation.animation_id);
    TEST_ASSERT_NOT_NULL(committed_animation);
    TEST_ASSERT_TRUE(committed_animation->flip_v);
    TEST_ASSERT_FALSE(committed_animation->flip_h);
}

/* The LOCAL Undo trigger is spec §12.4's blocked-with-choice row, not the
 * event-impact subject of §8.3: a blocked command never reaches the conflict
 * path, so this case deliberately carries no verification-id tag. The
 * conflicting Undo is the FOREIGN one, tagged below. */
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

/* USA-21 partial: an Undo admitted from ANOTHER view or controller is a
 * revision-changing event and conflicts an active draft. Limit: the
 * "Save/source/job state does not conflict" half lives in the sibling targets. */
void test_foreign_undo_conflicts_active_atlas_draft(void) {
    /* A committed step to undo, made before the draft exists. */
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    char foreign_name[] = "undo-me";
    tp_operation rename = {
        .kind = TP_OP_ATLAS_RENAME,
        .atlas_id = {{0}},
        .u.atlas_rename.name = foreign_name,
    };
    rename.atlas_id = atlas_id;
    apply_foreign_operation(
        &rename, "a5000000000000000000000000000001");
    TEST_ASSERT_TRUE(gui_project_can_undo());

    snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *based =
        tp_session_snapshot_atlas_by_id(
            snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(based);
    const int draft_padding = based->padding + 3;
    gui_edit_atlas_setting(
        atlas_id,
        tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PADDING, draft_padding, 0.0F);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());

    /* Not do_undo(): that is the LOCAL trigger, which §12.4 blocks. This is an
     * Undo admitted straight into the session by another owner. */
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_undo(
            gui_project__test_session(), &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    gui_project_frame_end();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    int retained = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas_id, GUI_ATLAS_PADDING,
            &retained, NULL));
    TEST_ASSERT_EQUAL_INT(draft_padding, retained);
    gui_draft_discard();
}

/* USA-16 partial: an agent commit during a numeric edit preserves the draft
 * and conflicts explicitly; the text, rename, and grouped variants are the
 * sibling cases in this file. */
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

/* USA-22 partial: gap/resync is a deterministic transition; the
 * repeated-race half is test_apply_mine_submits_once_and_later_foreign_
 * revision_conflicts in test_gui_edit_state.c. */
void test_event_gap_resync_conflicts_active_draft_and_retains_visible_value(
    void) {
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
    TEST_ASSERT_TRUE(
        gui_text_edit_update("mine-survives-resync"));

    char foreign_name[64] = {0};
    tp_operation operation = {
        .kind = TP_OP_ATLAS_RENAME,
        .atlas_id = {{0}},
        .u.atlas_rename.name = foreign_name,
    };
    operation.atlas_id = atlas_id;
    int64_t expected_revision =
        tp_session_snapshot_revision(snapshot);
    for (int index = 0; index < 70; ++index) {
        (void)snprintf(
            foreign_name, sizeof foreign_name,
            "event-gap-%d", index);
        tp_txn_request request = {
            .schema = TP_TXN_SCHEMA,
            .expected_revision = expected_revision,
            .label = "atlas.rename",
            .author = "foreign",
            .ops = &operation,
            .op_count = 1,
        };
        (void)snprintf(
            request.id_hex, sizeof request.id_hex,
            "%032x", index + 1);
        tp_txn_result result = {0};
        tp_error error = {{0}};
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_session_apply(
                gui_project__test_session(),
                &request, &result, &error));
        TEST_ASSERT_TRUE(result.committed);
        expected_revision = result.revision;
        tp_txn_result_free(&result);
    }

    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        "mine-survives-resync",
        gui_text_edit_value());
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
    pump_action_frame();
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

/* USA-20: Apply Mine never restores stale grouped sibling fields. */
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
    pump_action_frame();
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
        0, (create_animation_observed(
               atlas->id,
               tp_session_snapshot_revision(snapshot),
               "component-flip", NULL, 0))
               .visible_index);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(0, 0, &animation));
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
    pump_action_frame();
    const tp_snapshot_animation *committed =
        tp_session_snapshot_animation_by_id(
            gui_project_snapshot(), animation.atlas_id,
            animation.animation_id);
    TEST_ASSERT_NOT_NULL(committed);
    TEST_ASSERT_TRUE(committed->flip_h);
    TEST_ASSERT_TRUE(committed->flip_v);
}

/* USA-23: one gesture produces at most one transaction and Undo entry. */
void test_one_sprite_gesture_creates_one_undo_entry(void) {
    gui_sprite_ref sprite = add_test_sprite_ref(
        "__sprite_gesture_source__.png", "gesture-sprite.png");
    const int64_t revision_before =
        tp_session_snapshot_revision(gui_project_snapshot());
    const int undo_before = gui_project_undo_depth();

    gui_edit_sprite_origin(&sprite, 0, 0.25F);
    gui_edit_sprite_origin(&sprite, 0, 0.375F);
    pump_action_frame();
    TEST_ASSERT_EQUAL_INT64(
        revision_before,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(undo_before, gui_project_undo_depth());

    gui_request_gesture_commit();
    pump_action_frame();
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
    settle_project_job();
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
    pump_action_frame();

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

/* USA-19 partial: target deletion disables Apply Mine without losing the
 * drafted value; the retained value here is numeric, not copyable text. */
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
        0, (create_animation_observed(
                atlas->id,
                tp_session_snapshot_revision(snapshot),
                "deleted-animation", NULL, 0))
               .visible_index);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(trace_animation_ref_at(
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

/* Spec §12.4 "Blur": moving to a sibling field submits the active draft instead
 * of demanding an explicit Enter/Escape first. The new field then owns the one
 * draft, and the first field's value is committed -- not lost. */
void test_sibling_field_blur_submits_the_active_draft(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const int draft_padding = atlas->padding + 3;
    const int draft_margin = atlas->margin + 5;

    gui_edit_atlas_setting(
        atlas_id, revision, GUI_ATLAS_PADDING,
        draft_padding, 0.0F);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());

    /* The sibling field: no Enter, no Escape, no discard in between. */
    gui_edit_atlas_setting(
        atlas_id, tp_session_snapshot_revision(gui_project_snapshot()),
        GUI_ATLAS_MARGIN, draft_margin, 0.0F);
    publish_project_frame();

    /* Padding committed; margin is now the one active draft. */
    TEST_ASSERT_EQUAL_INT64(
        revision + 1,
        tp_session_snapshot_revision(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        draft_padding,
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id)->padding);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    int effective_margin = 0;
    TEST_ASSERT_TRUE(gui_atlas_edit_value(
        atlas_id, GUI_ATLAS_MARGIN, &effective_margin, NULL));
    TEST_ASSERT_EQUAL_INT(draft_margin, effective_margin);
    TEST_ASSERT_FALSE(gui_atlas_edit_value(
        atlas_id, GUI_ATLAS_PADDING, NULL, NULL));

    gui_request_gesture_commit();
    pump_action_frame();
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_EQUAL_INT(
        draft_margin,
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id)->margin);
}

void test_failed_target_path_submit_blocks_dependent_actions_and_preserves_text(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        trace_target_ref_at(0, 0, &target));
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
    gui_request_pack();
    gui_request_gesture_commit();
    pump_action_frame();

    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_snapshot_revision(
            gui_project_snapshot()));
    const tp_snapshot_target *after =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(), atlas_id,
            target.target_id);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(
        enabled_before, after->enabled);
    TEST_ASSERT_FALSE(gui_actions__intent_queued(GUI_INTENT_PACK));
    TEST_ASSERT_EQUAL_INT(
        0, s_actions.intent_count);
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
    pump_action_frame();
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_TRUE(s_confirm_draft);
    s_modal_action = MODAL_CANCEL;
    pump_action_frame();

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
    pump_action_frame();
    s_modal_action = MODAL_DISCARD;
    pump_action_frame();
    pump_action_frame();

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
    TEST_ASSERT_TRUE(s_confirm_open);
    TEST_ASSERT_FALSE(s_confirm_draft);
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_ACTIVE,
        gui_project_lifecycle_state_query());
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(
        "foreign-name", atlas->name);
    s_modal_action = MODAL_CANCEL;
    pump_action_frame();
}

void test_conflicted_atlas_rename_new_apply_mine_precedes_dirty_choice(void) {
    const tp_id128 atlas_id =
        begin_conflicted_atlas_name_draft(
            "mine-to-apply");

    request_new();
    pump_action_frame();
    s_modal_action = MODAL_SAVE;
    pump_action_frame();
    pump_action_frame();

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
    pump_action_frame();
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
    pump_action_frame();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_by_id(
            gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(
        "atlas-text-draft", atlas->name);

    const gui_project_create_result created =
        create_animation_observed(
            atlas_id,
            tp_session_snapshot_revision(
                gui_project_snapshot()),
            "animation-before", NULL, 0);
    TEST_ASSERT_TRUE(created.committed);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(
            0, created.visible_index, &animation));
    TEST_ASSERT_TRUE(
        gui_text_edit_begin_animation_name(
            &animation, "animation-before"));
    TEST_ASSERT_TRUE(
        gui_text_edit_update("animation-after"));
    gui_request_gesture_commit();
    pump_action_frame();
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
    settle_project_job();
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
    pump_action_frame();
    const tp_snapshot_sprite *sprite_after =
        tp_session_snapshot_sprite_by_key(
            gui_project_snapshot(), atlas_id,
            source_id, "sprite.png");
    TEST_ASSERT_NOT_NULL(sprite_after);
    TEST_ASSERT_EQUAL_STRING(
        "sprite-after", sprite_after->rename);

    gui_target_ref target = {0};
    TEST_ASSERT_TRUE(
        trace_target_ref_at(0, 0, &target));
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
    pump_action_frame();
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
        trace_target_ref_at(0, 0, &target));
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
    publish_project_frame();
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
    publish_project_frame();
    const tp_snapshot_target *selected =
        tp_session_snapshot_target_by_id(
            gui_project_snapshot(),
            target.atlas_id, target.target_id);
    TEST_ASSERT_NOT_NULL(selected);
    TEST_ASSERT_EQUAL_STRING(
        "selected-by-browse", selected->out_path);
}

void test_deferred_frame_move_follows_selection_at_post_commit_generation(void) {
    const gui_sprite_ref sprite =
        add_test_sprite_ref("__action_trace_move__.png", "first.png");
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_op_sprite_ref frames[] = {
        {sprite.source_id, "first.png"},
        {sprite.source_id, "second.png"},
    };
    const gui_project_create_result created =
        create_animation_observed(
            sprite.atlas_id,
            tp_session_snapshot_revision(snapshot),
            "move-frame", frames, 2);
    TEST_ASSERT_TRUE(created.committed);

    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(trace_animation_ref_at(
        0, created.visible_index, &animation));
    snapshot = gui_project_snapshot();
    gui_view_select_atlas(sprite.atlas_id);
    gui_view_select_animation(created.created_id);
    gui_view_select_animation_frame(snapshot, 0);
    const uint64_t selected_generation =
        gui_view_animation_frame_generation();
    TEST_ASSERT_EQUAL_UINT64(
        tp_session_snapshot_model_generation(snapshot),
        selected_generation);

    gui_edit_anim_frame_move(&animation, 0, 1);
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_animation_frame(gui_project_snapshot()));
    pump_action_frame();

    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_THAN_UINT64(
        selected_generation,
        tp_session_snapshot_model_generation(snapshot));
    TEST_ASSERT_EQUAL_INT(
        1, gui_view_animation_frame(snapshot));
    TEST_ASSERT_EQUAL_UINT64(
        tp_session_snapshot_model_generation(snapshot),
        gui_view_animation_frame_generation());
    const tp_snapshot_frame *moved =
        tp_session_snapshot_animation_frame_at(
            snapshot, sprite.atlas_id,
            created.created_id, 1);
    TEST_ASSERT_NOT_NULL(moved);
    TEST_ASSERT_EQUAL_STRING("first.png", moved->source_key);
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
    settle_project_job();

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
        create_animation_observed(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "delete-frame", &frame, 1);
    TEST_ASSERT_TRUE(created.committed);
    gui_animation_ref animation = {0};
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(
            0, created.visible_index,
            &animation));
    snapshot = gui_project_snapshot();
    gui_view_select_atlas(atlas_id);
    gui_view_select_animation(created.created_id);
    gui_view_select_animation_frame(snapshot, 0);
    const uint64_t selected_generation =
        gui_view_animation_frame_generation();
    TEST_ASSERT_EQUAL_UINT64(
        tp_session_snapshot_model_generation(snapshot),
        selected_generation);

    gui_edit_anim_frame_remove(&animation, 0);
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_animation_frame(gui_project_snapshot()));
    pump_action_frame();
    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_THAN_UINT64(
        selected_generation,
        tp_session_snapshot_model_generation(snapshot));
    TEST_ASSERT_EQUAL_INT(
        -1, gui_view_animation_frame(snapshot));
    const tp_snapshot_animation *after =
        tp_session_snapshot_animation_at(
            snapshot, atlas_id,
            created.visible_index);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(0, after->frame_count);

    gui_view_select_animation_frame(snapshot, 0);
    const uint64_t rejected_generation =
        gui_view_animation_frame_generation();
    gui_edit_anim_frame_remove(&animation, 0);
    pump_action_frame();
    TEST_ASSERT_EQUAL_UINT64(
        rejected_generation,
        tp_session_snapshot_model_generation(
            gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_animation_frame(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(0, remove(source_path));
}

void test_deferred_action_mutates_before_publishing_success_status(void) {
    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    set_status("atlas queued");
    gui_request_add_atlas();

    TEST_ASSERT_EQUAL_INT(1, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_atlas_index(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_STRING("atlas queued", s_status);

    pump_action_frame();
    TEST_ASSERT_EQUAL_INT(2, tp_session_snapshot_atlas_count(
                                 gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        1, gui_view_atlas_index(gui_project_snapshot()));
    TEST_ASSERT_FALSE(gui_actions__intent_queued(GUI_INTENT_ADD_ATLAS));
    TEST_ASSERT_EQUAL_STRING("Added atlas 'atlas2'", s_status);
}

void test_preview_request_is_deferred_and_selection_reset_stops_it(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int animation_index =
        create_animation_observed(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            "walk", NULL, 0)
            .visible_index;
    TEST_ASSERT_EQUAL_INT(0, animation_index);

    gui_animation_ref animation;
    TEST_ASSERT_TRUE(
        trace_animation_ref_at(0, animation_index, &animation));
    gui_view_select_animation(tp_id128_nil());
    s_preview_active = false;
    s_preview_playing = false;
    set_status("preview queued");

    gui_request_open_preview(&animation);
    TEST_ASSERT_EQUAL_INT(
        -1, gui_view_animation_index(gui_project_snapshot()));
    TEST_ASSERT_FALSE(s_preview_active);
    TEST_ASSERT_EQUAL_STRING("preview queued", s_status);

    pump_action_frame();
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_atlas_index(gui_project_snapshot()));
    TEST_ASSERT_EQUAL_INT(
        animation_index,
        gui_view_animation_index(gui_project_snapshot()));
    TEST_ASSERT_TRUE(s_preview_active);
    TEST_ASSERT_TRUE(s_preview_playing);
    TEST_ASSERT_EQUAL_STRING(
        "Pack (Ctrl+P) to preview the animation on packed regions.", s_status);

    const tp_id128 selected_source_id = {{4}};
    const sprite_row selected_row = {
        .source_id = selected_source_id,
        .source_key = "selected.png",
    };
    gui_rows_select_primary(&selected_row);
    TEST_ASSERT_TRUE(gui_rows_primary_is_set());
    s_preview_target = 1;
    reset_selection();
    TEST_ASSERT_FALSE(gui_rows_primary_is_set());
    TEST_ASSERT_EQUAL_INT(
        -1, gui_view_animation_index(gui_project_snapshot()));
    TEST_ASSERT_FALSE(s_preview_active);
    TEST_ASSERT_FALSE(s_preview_playing);
    TEST_ASSERT_EQUAL_INT(0, s_preview_target);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_atlas_draft_updates_then_undo_redo_trace_is_exact);
    RUN_TEST(test_atlas_draft_maps_every_scalar_component);
    RUN_TEST(
        test_sprite_and_animation_drafts_map_every_uncovered_component);
    RUN_TEST(test_undo_blocks_without_submitting_active_atlas_draft);
    RUN_TEST(test_foreign_undo_conflicts_active_atlas_draft);
    RUN_TEST(test_foreign_model_transaction_conflicts_active_atlas_draft);
    RUN_TEST(
        test_event_gap_resync_conflicts_active_draft_and_retains_visible_value);
    RUN_TEST(test_origin_apply_mine_preserves_foreign_sibling_component);
    RUN_TEST(test_slice9_apply_mine_preserves_newest_untouched_components);
    RUN_TEST(test_flip_h_apply_mine_preserves_foreign_flip_v);
    RUN_TEST(test_one_sprite_gesture_creates_one_undo_entry);
    RUN_TEST(test_invalid_sprite_value_is_rejected_without_losing_draft);
    RUN_TEST(test_deleted_source_preserves_sprite_draft_and_disables_apply);
    RUN_TEST(test_deleted_animation_preserves_draft_and_disables_apply);
    RUN_TEST(
        test_one_draft_owner_rejects_text_begin_while_atlas_scalar_is_active);
    RUN_TEST(test_sibling_field_blur_submits_the_active_draft);
    RUN_TEST(
        test_failed_target_path_submit_blocks_dependent_actions_and_preserves_text);
    RUN_TEST(test_conflicted_atlas_rename_new_cancel_preserves_draft);
    RUN_TEST(test_conflicted_atlas_rename_new_discard_continues_without_submit);
    RUN_TEST(test_conflicted_atlas_rename_new_apply_mine_precedes_dirty_choice);
    RUN_TEST(
        test_text_drafts_submit_exact_atlas_animation_sprite_and_target_ops);
    RUN_TEST(
        test_target_browse_submits_typed_path_before_starting_dialog_gesture);
    RUN_TEST(test_undo_redo_preserves_selected_animation_by_stable_id);
    RUN_TEST(
        test_deferred_frame_move_follows_selection_at_post_commit_generation);
    RUN_TEST(test_deferred_frame_delete_clears_selection_only_after_commit);
    RUN_TEST(test_deferred_action_mutates_before_publishing_success_status);
    RUN_TEST(test_preview_request_is_deferred_and_selection_reset_stops_it);
    return UNITY_END();
}
