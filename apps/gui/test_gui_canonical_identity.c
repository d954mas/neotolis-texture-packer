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

#include "gui_actions.h"
#include "gui_project.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "tp_core/tp_scan.h"

#include "unity.h"

static char s_left_dir[512];
static char s_right_dir[512];
static char s_left_file[512];
static char s_right_file[512];

/* gui_actions owns several shell-facing commands that are linked into this
 * headless target but never exercised here. */
void gui_shell_reset_shown_result(void) {}

static void remove_fixture_files(void) {
    (void)remove(s_left_file);
    (void)remove(s_right_file);
    (void)test_rmdir(s_left_dir);
    (void)test_rmdir(s_right_dir);
    (void)test_rmdir(TP_GUI_IDENTITY_TEST_DIR);
}

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
    (void)snprintf(s_left_dir, sizeof s_left_dir, "%s/left",
                   TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(s_right_dir, sizeof s_right_dir, "%s/right",
                   TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(s_left_file, sizeof s_left_file, "%s/shared.png",
                   s_left_dir);
    (void)snprintf(s_right_file, sizeof s_right_file, "%s/shared.png",
                   s_right_dir);
    remove_fixture_files();
    tp_mkdirs(s_left_dir);
    tp_mkdirs(s_right_dir);
    return write_fixture_file(s_left_file) && write_fixture_file(s_right_file);
}

static bool prepare_two_source_project(tp_id128 *left_id,
                                       tp_id128 *right_id,
                                       tp_id128 *atlas_id) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    if (!atlas) {
        (void)fprintf(stderr, "fixture: missing initial atlas\n");
        return false;
    }
    *atlas_id = atlas->id;
    const char *paths[2] = {s_left_file, s_right_file};
    int added = 0;
    int duplicates = 0;
    if (!gui_project_add_sources(atlas->id,
                                 tp_session_snapshot_revision(snapshot), paths,
                                 2, TP_SOURCE_KIND_FILE, &added, &duplicates) ||
        added != 2 || duplicates != 0) {
        char error[256];
        error[0] = '\0';
        (void)gui_project_take_op_error(error, sizeof error);
        (void)fprintf(stderr,
                      "fixture: add sources failed added=%d duplicates=%d error=%s\n",
                      added, duplicates, error);
        return false;
    }

    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_at(snapshot, 0) : NULL;
    const tp_snapshot_source *left = atlas
                                         ? tp_session_snapshot_source_at(
                                               snapshot, atlas->id, 0)
                                         : NULL;
    const tp_snapshot_source *right = atlas
                                          ? tp_session_snapshot_source_at(
                                                snapshot, atlas->id, 1)
                                          : NULL;
    if (!left || !right) {
        (void)fprintf(stderr, "fixture: missing added source DTOs\n");
        return false;
    }
    *left_id = left->id;
    *right_id = right->id;

    gui_sprite_ref left_ref = {atlas->id, left->id, "shared.png",
                               tp_session_snapshot_revision(snapshot)};
    if (!gui_project_set_sprite_rename(&left_ref, "left-name")) {
        char error[256];
        error[0] = '\0';
        (void)gui_project_take_op_error(error, sizeof error);
        (void)fprintf(stderr, "fixture: left rename failed error=%s\n", error);
        return false;
    }
    snapshot = gui_project_snapshot();
    gui_sprite_ref right_ref = {*atlas_id, *right_id, "shared.png",
                                tp_session_snapshot_revision(snapshot)};
    if (!gui_project_set_sprite_rename(&right_ref, "right-name")) {
        char error[256];
        error[0] = '\0';
        (void)gui_project_take_op_error(error, sizeof error);
        (void)fprintf(stderr, "fixture: right rename failed error=%s\n", error);
        return false;
    }
    gui_project_invalidate_sources();
    s_sel_atlas = 0;
    build_rows();
    return true;
}

static const sprite_row *row_for_source(tp_id128 source_id) {
    for (int i = 0; i < s_row_count; ++i) {
        if (!s_rows[i].is_folder &&
            tp_id128_eq(s_rows[i].source_id, source_id)) {
            return &s_rows[i];
        }
    }
    return NULL;
}

static bool animation_has_frame(const tp_session_snapshot *snapshot,
                                tp_id128 atlas_id,
                                const tp_snapshot_animation *animation,
                                tp_id128 source_id) {
    for (int i = 0; i < animation->frame_count; ++i) {
        const tp_snapshot_frame *frame = tp_session_snapshot_animation_frame_at(
            snapshot, atlas_id, animation->id, i);
        if (frame && tp_id128_eq(frame->source_id, source_id) &&
            frame->source_key &&
            strcmp(frame->source_key, "shared.png") == 0) {
            return true;
        }
    }
    return false;
}

void setUp(void) {
    TEST_ASSERT_TRUE(prepare_files());
    gui_project_init();
    s_sel_atlas = 0;
    multi_sel_clear();
}

void tearDown(void) {
    multi_sel_clear();
    gui_project_shutdown();
    gui_scan_shutdown();
    remove_fixture_files();
}

void test_rows_apply_renames_by_canonical_source_and_key(void) {
    tp_id128 left_id = {{0}};
    tp_id128 right_id = {{0}};
    tp_id128 atlas_id = {{0}};
    TEST_ASSERT_TRUE(
        prepare_two_source_project(&left_id, &right_id, &atlas_id));

    const sprite_row *left = row_for_source(left_id);
    const sprite_row *right = row_for_source(right_id);
    TEST_ASSERT_NOT_NULL(left);
    TEST_ASSERT_NOT_NULL(right);
    TEST_ASSERT_EQUAL_STRING("left-name (shared.png)", left->label);
    TEST_ASSERT_EQUAL_STRING("right-name (shared.png)", right->label);
}

void test_create_animation_preserves_both_canonical_selected_sprites(void) {
    tp_id128 left_id = {{0}};
    tp_id128 right_id = {{0}};
    tp_id128 atlas_id = {{0}};
    TEST_ASSERT_TRUE(
        prepare_two_source_project(&left_id, &right_id, &atlas_id));
    const sprite_row *left = row_for_source(left_id);
    const sprite_row *right = row_for_source(right_id);
    TEST_ASSERT_NOT_NULL(left);
    TEST_ASSERT_NOT_NULL(right);

    multi_sel_add_ref(left->source_id, left->source_key);
    multi_sel_add_ref(right->source_id, right->source_key);
    gui_request_create_animation_from_selection();
    multi_sel_clear();
    s_sel_atlas = -1;
    apply_pending();
    s_sel_atlas = 0;

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_at(snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(animation);
    TEST_ASSERT_EQUAL_INT(2, animation->frame_count);
    TEST_ASSERT_TRUE(animation_has_frame(snapshot, atlas_id, animation, left_id));
    TEST_ASSERT_TRUE(animation_has_frame(snapshot, atlas_id, animation, right_id));
}

void test_add_frames_preserves_both_canonical_selected_sprites(void) {
    tp_id128 left_id = {{0}};
    tp_id128 right_id = {{0}};
    tp_id128 atlas_id = {{0}};
    TEST_ASSERT_TRUE(
        prepare_two_source_project(&left_id, &right_id, &atlas_id));
    const sprite_row *left = row_for_source(left_id);
    const sprite_row *right = row_for_source(right_id);
    TEST_ASSERT_NOT_NULL(left);
    TEST_ASSERT_NOT_NULL(right);

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int animation_index = gui_project_create_animation(
        atlas_id, tp_session_snapshot_revision(snapshot), "picked", NULL, 0);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, animation_index);
    multi_sel_add_ref(left->source_id, left->source_key);
    multi_sel_add_ref(right->source_id, right->source_key);
    add_selection_frames_to_anim(animation_index);
    apply_pending();

    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_at(snapshot, atlas_id, animation_index);
    TEST_ASSERT_NOT_NULL(animation);
    TEST_ASSERT_EQUAL_INT(2, animation->frame_count);
    TEST_ASSERT_TRUE(animation_has_frame(snapshot, atlas_id, animation, left_id));
    TEST_ASSERT_TRUE(animation_has_frame(snapshot, atlas_id, animation, right_id));
}

void test_sprite_edit_state_uses_canonical_duplicate_identity(void) {
    tp_id128 left_id = {{0}};
    tp_id128 right_id = {{0}};
    tp_id128 atlas_id = {{0}};
    TEST_ASSERT_TRUE(
        prepare_two_source_project(&left_id, &right_id, &atlas_id));
    const sprite_row *right = row_for_source(right_id);
    TEST_ASSERT_NOT_NULL(right);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const gui_sprite_ref ref = {atlas_id, right->source_id, right->source_key,
                                tp_session_snapshot_revision(snapshot)};

    start_sprite_edit_ref(&ref, right->sprite_name);

    TEST_ASSERT_EQUAL_INT(EDIT_SPRITE, s_edit_kind);
    TEST_ASSERT_EQUAL_STRING("right-name", s_edit_buf);
    TEST_ASSERT_TRUE(gui_sprite_edit_matches(right));
    TEST_ASSERT_FALSE(gui_sprite_edit_matches(row_for_source(left_id)));
}

void test_sprite_edit_rejects_genuinely_stale_captured_revision(void) {
    tp_id128 left_id = {{0}};
    tp_id128 right_id = {{0}};
    tp_id128 atlas_id = {{0}};
    TEST_ASSERT_TRUE(
        prepare_two_source_project(&left_id, &right_id, &atlas_id));
    const sprite_row *right = row_for_source(right_id);
    TEST_ASSERT_NOT_NULL(right);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const gui_sprite_ref ref = {atlas_id, right_id, right->source_key,
                                tp_session_snapshot_revision(snapshot)};
    start_sprite_edit_ref(&ref, right->sprite_name);
    TEST_ASSERT_TRUE(gui_project_set_atlas_name(
        atlas_id, tp_session_snapshot_revision(snapshot), "changed-atlas"));
    (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", "must-not-land");

    commit_sprite_rename();

    snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_by_key(
        snapshot, atlas_id, right_id, "shared.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_EQUAL_STRING("right-name", sprite->rename);
    char error[256];
    error[0] = '\0';
    TEST_ASSERT_TRUE(gui_project_take_op_error(error, sizeof error));
    TEST_ASSERT_NOT_NULL(strstr(error, "revision"));
}

void test_delayed_animation_context_ref_never_retargets_after_index_shift(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "a", NULL,
               0));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "b", NULL,
               0));
    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "c", NULL,
               0));

    gui_animation_ref first;
    gui_animation_ref captured_second;
    TEST_ASSERT_TRUE(gui_project_animation_ref_at(0, 0, &first));
    TEST_ASSERT_TRUE(gui_project_animation_ref_at(0, 1, &captured_second));
    const tp_snapshot_animation *third_before =
        tp_session_snapshot_animation_at(gui_project_snapshot(), atlas_id, 2);
    TEST_ASSERT_NOT_NULL(third_before);
    const tp_id128 third_id = third_before->id;
    TEST_ASSERT_TRUE(gui_project_remove_animation(&first));

    gui_request_remove_animation_ref(&captured_second);
    apply_pending();

    snapshot = gui_project_snapshot();
    TEST_ASSERT_NOT_NULL(tp_session_snapshot_animation_by_id(
        snapshot, atlas_id, captured_second.animation_id));
    TEST_ASSERT_NOT_NULL(tp_session_snapshot_animation_by_id(
        snapshot, atlas_id, third_id));
    const tp_snapshot_atlas *after =
        tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(after);
    TEST_ASSERT_EQUAL_INT(2, after->animation_count);
    char error[256];
    error[0] = '\0';
    TEST_ASSERT_TRUE(gui_project_take_op_error(error, sizeof error));
    TEST_ASSERT_NOT_NULL(strstr(error, "revision"));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_rows_apply_renames_by_canonical_source_and_key);
    RUN_TEST(test_create_animation_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_add_frames_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_sprite_edit_state_uses_canonical_duplicate_identity);
    RUN_TEST(test_sprite_edit_rejects_genuinely_stale_captured_revision);
    RUN_TEST(test_delayed_animation_context_ref_never_retargets_after_index_shift);
    return UNITY_END();
}
