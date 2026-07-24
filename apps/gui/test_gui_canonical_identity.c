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
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_project.h"
#include "gui_recovery_indicator.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "time/nt_time.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_build_worker.h"
#include "tp_journal_internal.h"
#include "tp_session_internal.h"

#include "unity.h"

static char s_left_dir[512];
static char s_right_dir[512];
static char s_left_file[512];
static char s_right_file[512];

static tp_journal_io attach_memory_recovery(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    const tp_id128 key = {{
        'g', 'u', 'i', '_', 'r', 'e', 'c', 'o',
        'v', 'e', 'r', 'y', '_', 'f', '0', '3'}};
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_attach_journal(gui_project_session_for_jobs(), journal,
                                  &error));
    return io;
}

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

static tp_id128 add_coin_source_to_atlas(int atlas_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot,
                                                                        atlas_index)
                                         : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    char source_path[1024];
    (void)snprintf(source_path, sizeof source_path,
                   "%s/apps/cli/testdata/sprites/coin.png",
                   TP_TEST_SOURCE_DIR);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(atlas_id,
                                    tp_session_snapshot_revision(snapshot),
                                    source_path, TP_SOURCE_KIND_FILE));
    return atlas_id;
}

static tp_id128 add_coin_source(void) {
    return add_coin_source_to_atlas(0);
}

static void assert_atlas_name(tp_id128 atlas_id, const char *name) {
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_by_id(
        gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_STRING(name, atlas->name);
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
    tp_journal__test_set_record_limit(0U);
    tp_journal__test_set_file_limit(0U);
    multi_sel_clear();
    gui_pack_shutdown();
    gui_project_shutdown();
    gui_scan_shutdown();
    remove_fixture_files();
}

void test_arbitrary_result_lookup_follows_displayed_sprite_order(void) {
    tp_id128 left_id = tp_id128_nil();
    tp_id128 right_id = tp_id128_nil();
    left_id.bytes[0] = 0x11U;
    right_id.bytes[0] = 0x22U;

    char left_name[TP_PACK_INTERNAL_NAME_CAP];
    char right_name[TP_PACK_INTERNAL_NAME_CAP];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(left_id, "shared.png", left_name,
                                         sizeof left_name, NULL));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_pack_input_format_sprite_name(right_id, "shared.png", right_name,
                                         sizeof right_name, NULL));

    tp_sprite native_sprites[2] = {
        {.name = left_name},
        {.name = right_name},
    };
    tp_sprite preview_sprites[2] = {
        {.name = right_name},
        {.name = left_name},
    };
    const tp_result native = {
        .sprites = native_sprites,
        .sprite_count = 2,
    };
    const tp_result preview = {
        .sprites = preview_sprites,
        .sprite_count = 2,
    };

    TEST_ASSERT_EQUAL_INT(
        0, gui_pack_find_sprite_ref_in_result(&native, left_id, "shared.png"));
    TEST_ASSERT_EQUAL_INT(
        1, gui_pack_find_sprite_ref_in_result(&preview, left_id, "shared.png"));
    TEST_ASSERT_EQUAL_INT(
        0, gui_pack_find_sprite_ref_in_result(&preview, right_id, "shared.png"));
}

void test_canvas_result_rebind_resets_double_click_identity(void) {
    tp_result first = {0};
    tp_result second = {0};
    gui_canvas canvas = {0};
    gui_canvas_double_click_ref click = {0};

    TEST_ASSERT_FALSE(
        gui_canvas_double_click_press(&click, &first, 0, false));
    TEST_ASSERT_TRUE(click.valid);
    TEST_ASSERT_EQUAL_PTR(&first, click.result);

    gui_canvas_rebind_result(&canvas, &click, &second);

    TEST_ASSERT_EQUAL_PTR(&second, canvas.result);
    TEST_ASSERT_FALSE(click.valid);
    TEST_ASSERT_NULL(click.result);
    TEST_ASSERT_FALSE(
        gui_canvas_double_click_press(&click, &second, 0, true));
}

void test_preview_result_rejects_source_refresh_after_job_capture(void) {
    (void)add_coin_source();
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_preview_async_start(0, "defold", error,
                                                  sizeof error));
    gui_project_invalidate_sources();

    gui_pack_result_info info;
    gui_pack_done done = GUI_PACK_DONE_NONE;
    for (int i = 0; i < 5000 && done == GUI_PACK_DONE_NONE; ++i) {
        done = gui_pack_poll(&info);
        if (done == GUI_PACK_DONE_NONE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_PREVIEW_OK, done);
    TEST_ASSERT_TRUE(info.input_changed);
    TEST_ASSERT_NULL(gui_pack_preview_result(0));
}

void test_long_sprite_keys_with_shared_prefix_never_coalesce(void) {
    const tp_id128 atlas_id = add_coin_source();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_source *source =
        tp_session_snapshot_source_at(snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source);
    const tp_id128 source_id = source->id;

    char first[320];
    char second[320];
    memset(first, 'k', sizeof first - 1U);
    memcpy(second, first, sizeof first);
    first[sizeof first - 2U] = 'a';
    second[sizeof second - 2U] = 'b';
    first[sizeof first - 1U] = '\0';
    second[sizeof second - 1U] = '\0';
    TEST_ASSERT_EQUAL_MEMORY(first, second, 255U);

    const int64_t revision = tp_session_snapshot_revision(snapshot);
    const gui_sprite_ref first_ref = {atlas_id, source_id, first, revision};
    const gui_sprite_ref second_ref = {atlas_id, source_id, second, revision};
    TEST_ASSERT_TRUE(gui_project_set_sprite_override(
        &first_ref, GUI_SPRITE_OV_MARGIN, 3));
    TEST_ASSERT_TRUE(gui_project_set_sprite_override(
        &second_ref, GUI_SPRITE_OV_MARGIN, 7));
    TEST_ASSERT_TRUE(gui_project_flush_pending());

    snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *first_sprite =
        tp_session_snapshot_sprite_by_key(snapshot, atlas_id, source_id, first);
    const tp_snapshot_sprite *second_sprite =
        tp_session_snapshot_sprite_by_key(snapshot, atlas_id, source_id, second);
    TEST_ASSERT_NOT_NULL(first_sprite);
    TEST_ASSERT_NOT_NULL(second_sprite);
    TEST_ASSERT_EQUAL_INT(3, first_sprite->override_margin);
    TEST_ASSERT_EQUAL_INT(7, second_sprite->override_margin);
}

void test_oversized_names_cannot_enter_a_truncating_editor(void) {
    char oversized[TP_SRCKEY_MAX + 1U];
    memset(oversized, 'z', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    TEST_ASSERT_TRUE(gui_project_set_atlas_name(
        atlas_id, tp_session_snapshot_revision(snapshot), oversized));
    snapshot = gui_project_snapshot();
    cancel_edit();
    start_atlas_edit_ref(atlas_id, tp_session_snapshot_revision(snapshot));
    TEST_ASSERT_EQUAL_INT(EDIT_NONE, s_edit_kind);

    const int animation_index = gui_project_create_animation(
        atlas_id, tp_session_snapshot_revision(snapshot), "long-name-fixture",
        NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, animation_index);
    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_at(snapshot, atlas_id, animation_index);
    TEST_ASSERT_NOT_NULL(animation);
    gui_animation_ref animation_ref = {
        atlas_id, animation->id, tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_anim_id(&animation_ref, oversized));
    snapshot = gui_project_snapshot();
    animation_ref.expected_revision = tp_session_snapshot_revision(snapshot);
    cancel_edit();
    start_anim_edit_ref(&animation_ref);
    TEST_ASSERT_EQUAL_INT(EDIT_NONE, s_edit_kind);

    (void)add_coin_source();
    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_snapshot_source *source =
        tp_session_snapshot_source_at(snapshot, atlas_id, 0);
    TEST_ASSERT_NOT_NULL(source);
    const tp_id128 source_id = source->id;
    const gui_sprite_ref sprite = {
        atlas_id, source_id, "coin.png",
        tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(gui_project_set_sprite_rename(&sprite, oversized));
    snapshot = gui_project_snapshot();
    const gui_sprite_ref renamed = {
        atlas_id, source_id, "coin.png",
        tp_session_snapshot_revision(snapshot)};
    cancel_edit();
    start_sprite_edit_ref(&renamed, oversized);
    TEST_ASSERT_EQUAL_INT(EDIT_NONE, s_edit_kind);
}

void test_recovery_entry_keeps_core_path_capacity(void) {
    char original_path[1501];
    memset(original_path, 'p', sizeof original_path - 1U);
    original_path[0] = 'C';
    original_path[1] = ':';
    original_path[2] = '/';
    original_path[sizeof original_path - 1U] = '\0';

    gui_recovery_entry entry = {0};
    TEST_ASSERT_EQUAL_size_t(TP_IDENTITY_PATH_MAX,
                             sizeof entry.original_path);
    (void)snprintf(entry.original_path, sizeof entry.original_path, "%s",
                   original_path);
    TEST_ASSERT_EQUAL_STRING(original_path, entry.original_path);
}

void test_canvas_cache_key_keeps_core_path_capacity(void) {
    gui_canvas canvas = {0};
    TEST_ASSERT_EQUAL_size_t(TP_IDENTITY_PATH_MAX,
                             sizeof canvas.loaded_path);

    char oversized[TP_IDENTITY_PATH_MAX + 1U];
    memset(oversized, 'p', sizeof oversized - 1U);
    oversized[sizeof oversized - 1U] = '\0';
    char error[128] = {0};
    TEST_ASSERT_FALSE(
        gui_canvas_set_image(&canvas, oversized, error, sizeof error));
    TEST_ASSERT_NOT_NULL(strstr(error, "maximum"));
    TEST_ASSERT_EQUAL_CHAR('\0', canvas.loaded_path[0]);
}

void test_undo_redo_keep_last_successful_pack_result(void) {
    const tp_id128 atlas_id = add_coin_source();

    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    do_pack_blocking();
    const tp_result *packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_FALSE(gui_project_is_stale());
    const int sprite_count = packed->sprite_count;
    const int page_count = packed->page_count;

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_set_atlas_name(
        atlas_id, tp_session_snapshot_revision(snapshot), "edited"));
    assert_atlas_name(atlas_id, "edited");

    do_undo();
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);
    TEST_ASSERT_EQUAL_INT(page_count, packed->page_count);
    assert_atlas_name(atlas_id, "atlas1");
    TEST_ASSERT_TRUE(gui_project_is_stale());

    do_redo();
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);
    TEST_ASSERT_EQUAL_INT(page_count, packed->page_count);
    assert_atlas_name(atlas_id, "edited");
    TEST_ASSERT_TRUE(gui_project_is_stale());
}

void test_pack_result_follows_stable_atlas_across_index_shift(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *first = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(first);
    const tp_id128 first_id = first->id;
    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas());
    const tp_id128 packed_atlas_id = add_coin_source_to_atlas(1);

    s_sel_atlas = 1;
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    do_pack_blocking();
    const tp_result *packed = gui_pack_result(1);
    TEST_ASSERT_NOT_NULL(packed);
    const int sprite_count = packed->sprite_count;

    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_remove_atlas(
        first_id, tp_session_snapshot_revision(snapshot)));
    snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *shifted = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(shifted);
    TEST_ASSERT_TRUE(tp_id128_eq(packed_atlas_id, shifted->id));
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);

    TEST_ASSERT_TRUE(gui_project_undo());
    packed = gui_pack_result(1);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);

    TEST_ASSERT_TRUE(gui_project_redo());
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);

    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(gui_project_set_atlas_setting(
        packed_atlas_id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PIXELS_PER_UNIT, 0, 2.0F));
    s_sel_atlas = 0;
    do_pack_blocking();
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_TRUE(packed->pixels_per_unit == 2.0F);

    TEST_ASSERT_EQUAL_INT(1, gui_project_add_atlas());
    (void)add_coin_source_to_atlas(1);
    s_sel_atlas = 1;
    do_pack_blocking();
    TEST_ASSERT_NOT_NULL(gui_pack_result(1));
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_EQUAL_INT(sprite_count, packed->sprite_count);
    TEST_ASSERT_TRUE(packed->pixels_per_unit == 2.0F);
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

void test_required_recovery_without_root_warns_but_allows_edit_undo_redo(void) {
    gui_project_require_recovery();
    gui_project_shutdown();
    gui_project_init();

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                          ? tp_session_snapshot_atlas_at(snapshot, 0)
                                          : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_FALSE(
        tp_session_recovery_available(gui_project_session_for_jobs()));
    const tp_id128 atlas_id = atlas->id;
    const int padding_before = atlas->padding;
    TEST_ASSERT_TRUE(gui_project_set_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot), GUI_ATLAS_PADDING,
        padding_before + 1, 0.0F));
    TEST_ASSERT_TRUE(gui_project_can_undo());
    TEST_ASSERT_TRUE(gui_project_flush_pending());
    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(padding_before + 1, atlas->padding);
    TEST_ASSERT_TRUE(gui_project_can_undo());
    TEST_ASSERT_TRUE(gui_project_undo());
    atlas = tp_session_snapshot_atlas_by_id(gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(padding_before, atlas->padding);
    TEST_ASSERT_TRUE(gui_project_can_redo());
    TEST_ASSERT_TRUE(gui_project_redo());
    atlas = tp_session_snapshot_atlas_by_id(gui_project_snapshot(), atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(padding_before + 1, atlas->padding);
    TEST_ASSERT_FALSE(
        tp_session_recovery_available(gui_project_session_for_jobs()));

    char warning[256] = {0};
    TEST_ASSERT_TRUE(
        gui_project_take_recovery_setup_notice(warning, sizeof warning));
    TEST_ASSERT_NOT_NULL(strstr(warning, "Crash recovery is unavailable"));
    gui_project_discard_recovery_on_shutdown();
}

void test_recovery_notice_is_sticky_exact_and_clears_after_save_heals(void) {
    (void)attach_memory_recovery();
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    tp_journal__test_set_record_limit(1U);
    TEST_ASSERT_TRUE(gui_project_set_atlas_name(
        atlas_id, tp_session_snapshot_revision(snapshot), "recovery-notice"));

    char save_path[512];
    (void)snprintf(save_path, sizeof save_path, "%s/recovery-notice.ntpacker_project",
                   TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(save_path);
    tp_journal__test_set_file_limit(1U);
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          gui_project_save_as(save_path, error, sizeof error));

    const tp_session_recovery_health health =
        tp_session_recovery_health_query(gui_project_session_for_jobs());
    TEST_ASSERT_TRUE(health.degraded);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, health.first_cause);
    gui_recovery_notice notice = {0};
    TEST_ASSERT_TRUE(gui_project_recovery_notice_query(&notice));
    TEST_ASSERT_EQUAL_STRING(TP_SESSION_NOTICE_RECOVERY_DEGRADED,
                             notice.notice_id);
    TEST_ASSERT_EQUAL_UINT64(health.generation, notice.generation);
    TEST_ASSERT_EQUAL_INT(health.first_cause, notice.status);
    TEST_ASSERT_NOT_NULL(strstr(notice.message, "degraded"));
    TEST_ASSERT_NOT_NULL(strstr(notice.message, "Editing and Undo remain available"));
    const gui_recovery_indicator indicator =
        gui_recovery_indicator_present(true, &notice);
    TEST_ASSERT_TRUE(indicator.visible);
    TEST_ASSERT_EQUAL_STRING("!", indicator.glyph);
    TEST_ASSERT_EQUAL_STRING(notice.message, indicator.tooltip);
    TEST_ASSERT_FALSE(
        gui_recovery_indicator_present(false, &notice).visible);

    snapshot = gui_project_snapshot();
    atlas = snapshot ? tp_session_snapshot_atlas_by_id(snapshot, atlas_id) : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(gui_project_set_atlas_setting(
        atlas_id, tp_session_snapshot_revision(snapshot) + 1,
        GUI_ATLAS_PADDING, atlas->padding + 1, 0.0F));
    TEST_ASSERT_FALSE(gui_project_flush_pending());
    TEST_ASSERT_TRUE(gui_project_take_op_error(error, sizeof error));
    gui_recovery_notice after_drain = {0};
    TEST_ASSERT_TRUE(gui_project_recovery_notice_query(&after_drain));
    TEST_ASSERT_EQUAL_STRING(notice.notice_id, after_drain.notice_id);
    TEST_ASSERT_EQUAL_UINT64(notice.generation, after_drain.generation);
    TEST_ASSERT_EQUAL_INT(notice.status, after_drain.status);

    tp_journal__test_set_record_limit(0U);
    tp_journal__test_set_file_limit(0U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          gui_project_save(error, sizeof error));
    TEST_ASSERT_FALSE(gui_project_recovery_notice_query(&after_drain));
    TEST_ASSERT_FALSE(
        tp_session_recovery_health_query(gui_project_session_for_jobs()).degraded);
    (void)remove(save_path);
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_arbitrary_result_lookup_follows_displayed_sprite_order);
    RUN_TEST(test_canvas_result_rebind_resets_double_click_identity);
    RUN_TEST(test_rows_apply_renames_by_canonical_source_and_key);
    RUN_TEST(test_create_animation_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_add_frames_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_sprite_edit_state_uses_canonical_duplicate_identity);
    RUN_TEST(test_sprite_edit_rejects_genuinely_stale_captured_revision);
    RUN_TEST(test_delayed_animation_context_ref_never_retargets_after_index_shift);
    RUN_TEST(test_preview_result_rejects_source_refresh_after_job_capture);
    RUN_TEST(test_long_sprite_keys_with_shared_prefix_never_coalesce);
    RUN_TEST(test_oversized_names_cannot_enter_a_truncating_editor);
    RUN_TEST(test_recovery_entry_keeps_core_path_capacity);
    RUN_TEST(test_canvas_cache_key_keeps_core_path_capacity);
    RUN_TEST(test_undo_redo_keep_last_successful_pack_result);
    RUN_TEST(test_pack_result_follows_stable_atlas_across_index_shift);
    RUN_TEST(test_recovery_notice_is_sticky_exact_and_clears_after_save_heals);
    RUN_TEST(test_required_recovery_without_root_warns_but_allows_edit_undo_redo);
    return UNITY_END();
}
