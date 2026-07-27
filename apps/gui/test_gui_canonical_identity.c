#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define test_rmdir _rmdir
#else
#include <dirent.h>
#include <unistd.h>
#define test_rmdir rmdir
#endif

#include "gui_actions.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_pack_internal.h"
#include "gui_project.h"
#include "gui_project_test_driver.h"
#include "gui_recovery_indicator.h"
#include "gui_rows.h"
#include "gui_scan.h"
#include "gui_state.h"

#include "time/nt_time.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_pack_hash.h"
#include "tp_core/tp_build_worker.h"
#include "tp_journal_internal.h"
#include "tp_image_priv.h"
#include "tp_session_internal.h"
#include "tp_test_seams.h"

#include "unity.h"

static char s_left_dir[512];
static char s_right_dir[512];
static char s_left_file[512];
static char s_right_file[512];
static uint64_t s_test_submit_id;

static gui_session_submit_identity test_submit_identity(
    char transaction_id[33]) {
    gui_session_submit_identity identity = {0};
    identity.origin_view_id.bytes[0] = 1U;
    ++s_test_submit_id;
    identity.draft_instance_id.bytes[0] = 2U;
    memcpy(
        &identity.draft_instance_id.bytes[8],
        &s_test_submit_id, sizeof s_test_submit_id);
    (void)snprintf(
        transaction_id, 33, "%016llx%016llx",
        1ULL, (unsigned long long)s_test_submit_id);
    return identity;
}

static bool test_offer_atlas_draft(
    tp_id128 atlas_id, int64_t revision,
    gui_atlas_field field, int ivalue, float fvalue) {
    gui_edit_atlas_setting(
        atlas_id, revision, field, ivalue, fvalue);
    return gui_atlas_edit_value(
        atlas_id, field, NULL, NULL);
}

static bool test_submit_atlas_name(
    tp_id128 atlas_id, int64_t revision,
    const char *name) {
    char transaction_id[33];
    const gui_session_submit_identity identity =
        test_submit_identity(transaction_id);
    gui_session_submit_terminal terminal = {0};
    tp_error error = {{0}};
    const gui_text_ref ref = {
        atlas_id, tp_id128_nil(), tp_id128_nil(), NULL,
        revision};
    return gui_project_submit_text(
               TP_OP_ATLAS_RENAME, &ref, name, identity,
               transaction_id, &terminal, &error) ==
           TP_STATUS_OK;
}

static bool test_submit_animation_name(
    const gui_animation_ref *animation,
    const char *name) {
    char transaction_id[33];
    const gui_session_submit_identity identity =
        test_submit_identity(transaction_id);
    gui_session_submit_terminal terminal = {0};
    tp_error error = {{0}};
    const gui_text_ref ref = {
        animation->atlas_id, animation->animation_id,
        tp_id128_nil(), NULL,
        animation->expected_revision};
    return gui_project_submit_text(
               TP_OP_ANIMATION_RENAME, &ref, name, identity,
               transaction_id, &terminal, &error) ==
           TP_STATUS_OK;
}

static bool test_submit_sprite_name(
    const gui_sprite_ref *sprite,
    const char *name) {
    char transaction_id[33];
    const gui_session_submit_identity identity =
        test_submit_identity(transaction_id);
    gui_session_submit_terminal terminal = {0};
    tp_error error = {{0}};
    const gui_text_ref ref = {
        sprite->atlas_id, tp_id128_nil(),
        sprite->source_id, sprite->source_key,
        sprite->expected_revision};
    return gui_project_submit_text(
               TP_OP_SPRITE_NAME_SET, &ref, name, identity,
               transaction_id, &terminal, &error) ==
           TP_STATUS_OK;
}

static bool test_animation_ref_at(
    int atlas_index, int animation_index,
    gui_animation_ref *out) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot
            ? tp_session_snapshot_atlas_at(
                  snapshot, atlas_index)
            : NULL;
    const tp_snapshot_animation *animation =
        atlas
            ? tp_session_snapshot_animation_at(
                  snapshot, atlas->id, animation_index)
            : NULL;
    if (!animation || !out) {
        return false;
    }
    *out = (gui_animation_ref){
        atlas->id,
        animation->id,
        tp_session_snapshot_revision(snapshot),
    };
    return true;
}

static bool test_target_ref_at(
    int atlas_index, int target_index,
    gui_target_ref *out) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot
            ? tp_session_snapshot_atlas_at(
                  snapshot, atlas_index)
            : NULL;
    const tp_snapshot_target *target =
        atlas
            ? tp_session_snapshot_target_at(
                  snapshot, atlas->id, target_index)
            : NULL;
    if (!target || !out) {
        return false;
    }
    *out = (gui_target_ref){
        atlas->id,
        target->id,
        tp_session_snapshot_revision(snapshot),
    };
    return true;
}

static bool remove_flat_test_dir(const char *root) {
#ifdef _WIN32
    char pattern[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        pattern, sizeof pattern, "%s/*", root);
    WIN32_FIND_DATAA item;
    HANDLE find = FindFirstFileA(pattern, &item);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(item.cFileName, ".") != 0 &&
                strcmp(item.cFileName, "..") != 0) {
                char path[TP_IDENTITY_PATH_MAX];
                (void)snprintf(
                    path, sizeof path, "%s/%s",
                    root, item.cFileName);
                (void)DeleteFileA(path);
            }
        } while (FindNextFileA(find, &item));
        (void)FindClose(find);
    }
    return RemoveDirectoryA(root) != 0 ||
           GetLastError() == ERROR_PATH_NOT_FOUND;
#else
    DIR *dir = opendir(root);
    if (dir) {
        struct dirent *item;
        while ((item = readdir(dir)) != NULL) {
            if (strcmp(item->d_name, ".") != 0 &&
                strcmp(item->d_name, "..") != 0) {
                char path[TP_IDENTITY_PATH_MAX];
                (void)snprintf(
                    path, sizeof path, "%s/%s",
                    root, item->d_name);
                (void)remove(path);
            }
        }
        (void)closedir(dir);
    }
    return rmdir(root) == 0 ||
           access(root, F_OK) != 0;
#endif
}

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
        tp_session_attach_journal(gui_project__test_session(), journal,
                                  &error));
    return io;
}

/* gui_actions owns several shell-facing commands that are linked into this
 * headless target but never exercised here. */
void gui_shell_reset_shown_result(void) {}

static gui_pack_done pump_pack_frame(
    gui_pack_result_info *info) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(NULL, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    const gui_pack_done done =
        gui_pack_poll(info);
    gui_project_frame_end();
    return done;
}

static void admit_queued_job(void) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(NULL, &error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    gui_project_frame_end();
}

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
    if (!test_submit_sprite_name(&left_ref, "left-name")) {
        char error[256];
        error[0] = '\0';
        (void)gui_project_take_op_error(error, sizeof error);
        (void)fprintf(stderr, "fixture: left rename failed error=%s\n", error);
        return false;
    }
    snapshot = gui_project_snapshot();
    gui_sprite_ref right_ref = {*atlas_id, *right_id, "shared.png",
                                tp_session_snapshot_revision(snapshot)};
    if (!test_submit_sprite_name(&right_ref, "right-name")) {
        char error[256];
        error[0] = '\0';
        (void)gui_project_take_op_error(error, sizeof error);
        (void)fprintf(stderr, "fixture: right rename failed error=%s\n", error);
        return false;
    }
    gui_project_invalidate_sources();
    gui_view_select_atlas(*atlas_id);
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
    tp_job__test_reset_all();
    TEST_ASSERT_TRUE(prepare_files());
    gui_project_init();
    gui_project_set_controller_status_port(
        (gui_project_controller_status_port){
            0});
    gui_view_reset();
    gui_view_reconcile_observation(gui_project_snapshot());
    gui_view_adopt_default_atlas(gui_project_snapshot());
    multi_sel_clear();
}

void tearDown(void) {
    tp_job__test_reset_all();
    tp_journal__test_set_record_limit(0U);
    tp_journal__test_set_file_limit(0U);
    multi_sel_clear();
    gui_pack_shutdown();
    gui_project_test_shutdown(true);
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

    gui_canvas_rebind_result(&canvas, &click, &first);
    const uint64_t first_generation =
        canvas.result_generation;
    TEST_ASSERT_FALSE(
        gui_canvas_double_click_press(
            &click, first_generation, 0, false));
    TEST_ASSERT_TRUE(click.valid);
    TEST_ASSERT_EQUAL_UINT64(
        first_generation, click.result_generation);
    TEST_ASSERT_EQUAL_INT(0, click.sprite_index);

    gui_canvas_rebind_result(&canvas, &click, &second);

    TEST_ASSERT_EQUAL_PTR(&second, canvas.result);
    TEST_ASSERT_NOT_EQUAL(
        first_generation, canvas.result_generation);
    TEST_ASSERT_FALSE(click.valid);
    TEST_ASSERT_EQUAL_UINT64(0U, click.result_generation);
    TEST_ASSERT_FALSE(
        gui_canvas_double_click_press(
            &click, canvas.result_generation, 0, true));
}

void test_selected_atlas_id_survives_preceding_atlas_removal(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *first =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(first);
    const tp_id128 first_id = first->id;
    const gui_project_create_result second =
        gui_project_add_atlas();
    TEST_ASSERT_TRUE(second.committed);

    gui_view_select_atlas(second.created_id);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        1, gui_view_atlas_index(snapshot));
    TEST_ASSERT_TRUE(gui_project_remove_atlas(
        first_id,
        tp_session_snapshot_revision(snapshot)));

    snapshot = gui_project_snapshot();
    gui_view_reconcile_observation(snapshot);
    TEST_ASSERT_TRUE(tp_id128_eq(
        second.created_id, gui_view_atlas_id()));
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_atlas_index(snapshot));
}

void test_selected_animation_id_survives_preceding_animation_removal(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const gui_project_create_result first =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "first", NULL, 0);
    TEST_ASSERT_TRUE(first.committed);
    snapshot = gui_project_snapshot();
    const gui_project_create_result second =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "second", NULL, 0);
    TEST_ASSERT_TRUE(second.committed);

    gui_view_select_atlas(atlas_id);
    gui_view_select_animation(second.created_id);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_EQUAL_INT(
        1, gui_view_animation_index(snapshot));
    gui_animation_ref first_ref = {0};
    TEST_ASSERT_TRUE(
        test_animation_ref_at(0, 0, &first_ref));
    TEST_ASSERT_TRUE(
        gui_project_remove_animation(&first_ref));

    snapshot = gui_project_snapshot();
    gui_view_reconcile_observation(snapshot);
    TEST_ASSERT_TRUE(tp_id128_eq(
        second.created_id, gui_view_animation_id()));
    TEST_ASSERT_EQUAL_INT(
        0, gui_view_animation_index(snapshot));
}

void test_preview_result_rejects_source_refresh_after_job_capture(void) {
    (void)add_coin_source();
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_preview_async_start(0, "defold", error,
                                                  sizeof error));
    admit_queued_job();
    gui_project_invalidate_sources();

    gui_pack_result_info info;
    gui_pack_done done = GUI_PACK_DONE_NONE;
    for (int i = 0; i < 5000 && done == GUI_PACK_DONE_NONE; ++i) {
        done = pump_pack_frame(&info);
        if (done == GUI_PACK_DONE_NONE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_PREVIEW_OK, done);
    TEST_ASSERT_TRUE(info.input_changed);
    TEST_ASSERT_NULL(gui_pack_preview_result(0));
}

void test_preview_after_native_pack_uses_observed_runtime_generation(void) {
    (void)add_coin_source();
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));

    double elapsed_ms = 0.0;
    char error[256] = {0};
    char notice[128] = {0};
    TEST_ASSERT_TRUE(gui_pack_atlas(
        0, &elapsed_ms, error, sizeof error,
        notice, sizeof notice));
    TEST_ASSERT_NOT_NULL(gui_pack_result(0));

    error[0] = '\0';
    TEST_ASSERT_TRUE(gui_pack_preview_blocking(
        0, "defold", error, sizeof error));
    TEST_ASSERT_NOT_NULL(gui_pack_preview_result(0));
}

void test_native_pack_marks_token_change_stale_without_host_side_probe(void) {
    const tp_id128 packed_atlas_id = add_coin_source_to_atlas(0);
    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *other =
        tp_session_snapshot_atlas_at(snapshot, 1);
    TEST_ASSERT_NOT_NULL(other);
    const tp_id128 other_atlas_id = other->id;

    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_async_start(0, error, sizeof error));
    admit_queued_job();

    snapshot = gui_project_snapshot();
    TEST_ASSERT_TRUE(test_submit_atlas_name(
        other_atlas_id, tp_session_snapshot_revision(snapshot),
        "unrelated-edit"));

    gui_pack_result_info info = {0};
    gui_pack_done done = GUI_PACK_DONE_NONE;
    for (int i = 0; i < 5000 && done == GUI_PACK_DONE_NONE; ++i) {
        done = pump_pack_frame(&info);
        if (done == GUI_PACK_DONE_NONE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_PACK_OK, done);
    /* R1c removes the worker's raw-session freshness dereference. Until the
     * separate source-runtime/hash remediation, a changed composite input
     * token is intentionally conservative: the host never decodes or reads
     * files to refine this result during GUI polling. */
    TEST_ASSERT_TRUE(info.input_changed);
    snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *packed =
        tp_session_snapshot_atlas_by_id(snapshot, packed_atlas_id);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_NOT_NULL(gui_pack_result(0));
}

void test_nil_completed_pack_input_hash_is_stale(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, 0) : NULL;
    TEST_ASSERT_NOT_NULL(atlas);

    const tp_session_pack_job_result completed = {
        .atlas_id = atlas->id,
        .input_token_at_start =
            tp_session_snapshot_input_token(snapshot),
        .pack_input_hash = {{0}},
    };

    TEST_ASSERT_TRUE(
        gui_pack__test_native_pack_input_changed_since(&completed));
}

void test_failed_current_hash_probe_is_stale(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot ? tp_session_snapshot_atlas_at(snapshot, 0) : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    tp_id128 completed_hash = tp_id128_nil();
    completed_hash.bytes[0] = 0x7fU;
    const tp_session_pack_job_result completed = {
        .atlas_id = atlas->id,
        .input_token_at_start =
            tp_session_snapshot_input_token(snapshot),
        .pack_input_hash = completed_hash,
        .freshness_token = tp_session_snapshot_input_token(snapshot),
        .freshness = TP_PACK_FRESHNESS_STALE,
        .freshness_reason = TP_PACK_FRESHNESS_PROBE_ERROR,
        .freshness_status = TP_STATUS_INVALID_ARGUMENT,
    };

    TEST_ASSERT_TRUE(
        gui_pack__test_native_pack_input_changed_since(&completed));
}

void test_gui_poll_does_not_decode_sources_on_gui_thread(void) {
    const tp_id128 atlas_id = add_coin_source();
    TEST_ASSERT_TRUE(gui_pack_init(TP_GUI_IDENTITY_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(gui_pack_async_start(0, error, sizeof error));
    admit_queued_job();

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    char hero_path[1024];
    (void)snprintf(hero_path, sizeof hero_path,
                   "%s/apps/cli/testdata/sprites/hero.png",
                   TP_TEST_SOURCE_DIR);
    TEST_ASSERT_EQUAL_INT(
        GUI_ADD_ADDED,
        gui_project_add_source_kind(
            atlas_id, tp_session_snapshot_revision(snapshot), hero_path,
            TP_SOURCE_KIND_FILE));

    tp_image__test_reset_decode_count();

    gui_pack_result_info info = {0};
    gui_pack_done done = GUI_PACK_DONE_NONE;
    uint64_t decodes_before_poll = 0U;
    for (int i = 0;
         i < 5000 &&
         done == GUI_PACK_DONE_NONE; ++i) {
        decodes_before_poll =
            tp_image__test_decode_count();
        done = pump_pack_frame(&info);
        if (done != GUI_PACK_DONE_NONE) {
            break;
        }
        nt_time_sleep(0.001);
    }

    TEST_ASSERT_EQUAL_INT(GUI_PACK_DONE_PACK_OK, done);
    TEST_ASSERT_TRUE(info.input_changed);
    TEST_ASSERT_EQUAL_UINT64(
        decodes_before_poll, tp_image__test_decode_count());
}

static bool controller_attached(
    void *context) {
    return context &&
           *(const bool *)context;
}

void test_controller_guard_rejects_identity_change_before_flush_or_write(void) {
    char first_path[TP_IDENTITY_PATH_MAX];
    char cross_path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        first_path, sizeof first_path,
        "%s/controller-first.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(
        cross_path, sizeof cross_path,
        "%s/controller-cross.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(first_path);
    (void)remove(cross_path);

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const int undo_depth =
        gui_project_undo_depth();
    TEST_ASSERT_TRUE(
        test_offer_atlas_draft(
            atlas->id, revision,
            GUI_ATLAS_PADDING,
            atlas->padding + 1, 0.0F));

    bool attached = true;
    gui_project_set_controller_status_port(
        (gui_project_controller_status_port){
            .attached =
                controller_attached,
            .context = &attached,
        });
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_UNSUPPORTED_CAPABILITY,
        gui_project_save_as_preflight(
            first_path, error,
            sizeof error));
    TEST_ASSERT_EQUAL_INT(
        revision,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        undo_depth,
        gui_project_undo_depth());
    TEST_ASSERT_FALSE(
        gui_scan_exists(first_path));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING,
        gui_draft_phase());
    int preserved_padding = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas->id, GUI_ATLAS_PADDING,
            &preserved_padding, NULL));
    TEST_ASSERT_EQUAL_INT(
        atlas->padding + 1,
        preserved_padding);

    attached = false;
    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            first_path, error,
            sizeof error));
    TEST_ASSERT_EQUAL_INT(
        revision + 1,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        undo_depth + 1,
        gui_project_undo_depth());
    attached = true;
    /* No intervening frame observation: the guard must refresh through the
     * session client and recognize this as the same identity. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as_preflight(
            first_path, error,
            sizeof error));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            first_path, error,
            sizeof error));
    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_at(
        snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const int64_t cross_revision =
        tp_session_revision(
            gui_project__test_session());
    const int cross_undo_depth =
        gui_project_undo_depth();
    TEST_ASSERT_TRUE(
        test_offer_atlas_draft(
            atlas->id,
            tp_session_snapshot_revision(
                snapshot),
            GUI_ATLAS_PADDING,
            atlas->padding + 1, 0.0F));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_UNSUPPORTED_CAPABILITY,
        gui_project_save_as_preflight(
            cross_path, error,
            sizeof error));
    TEST_ASSERT_EQUAL_INT64(
        cross_revision,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        cross_undo_depth,
        gui_project_undo_depth());
    TEST_ASSERT_EQUAL_STRING(
        first_path, gui_project_path());
    TEST_ASSERT_FALSE(
        gui_scan_exists(cross_path));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING,
        gui_draft_phase());

    attached = false;
    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            cross_path, error,
            sizeof error));
    TEST_ASSERT_EQUAL_INT64(
        cross_revision + 1,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        cross_undo_depth + 1,
        gui_project_undo_depth());
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(NULL));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_STRING(
        cross_path, gui_project_path());

    gui_project_set_controller_status_port(
        (gui_project_controller_status_port){
            0});
    TEST_ASSERT_TRUE(gui_project_test_new());
    (void)remove(first_path);
    (void)remove(cross_path);
}

void test_save_as_preflight_success_does_not_submit_or_write(void) {
    char path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        path, sizeof path,
        "%s/preflight-read-only.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(path);

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int value = atlas->padding + 1;
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const int undo_depth =
        gui_project_undo_depth();
    TEST_ASSERT_TRUE(
        test_offer_atlas_draft(
            atlas_id, revision,
            GUI_ATLAS_PADDING, value, 0.0F));

    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as_preflight(
            path, error, sizeof error));
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        undo_depth,
        gui_project_undo_depth());
    TEST_ASSERT_FALSE(gui_scan_exists(path));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING,
        gui_draft_phase());
    int preserved = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas_id, GUI_ATLAS_PADDING,
            &preserved, NULL));
    TEST_ASSERT_EQUAL_INT(value, preserved);

    gui_draft_discard();
    (void)remove(path);
}

void test_save_as_preflight_observe_failure_preserves_draft_and_model(void) {
    char path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        path, sizeof path,
        "%s/preflight-observe-failure.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(path);

    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    const int value = atlas->padding + 1;
    const int64_t revision =
        tp_session_snapshot_revision(snapshot);
    const int undo_depth =
        gui_project_undo_depth();
    TEST_ASSERT_TRUE(
        test_offer_atlas_draft(
            atlas_id, revision,
            GUI_ATLAS_PADDING, value, 0.0F));

    gui_project__test_fail_next_observe();
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_save_as_preflight(
            path, error, sizeof error));
    TEST_ASSERT_NOT_NULL(
        strstr(error, "observation test allocation failure"));
    TEST_ASSERT_EQUAL_INT64(
        revision,
        tp_session_revision(
            gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        undo_depth,
        gui_project_undo_depth());
    TEST_ASSERT_FALSE(gui_scan_exists(path));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING,
        gui_draft_phase());
    int preserved = 0;
    TEST_ASSERT_TRUE(
        gui_atlas_edit_value(
            atlas_id, GUI_ATLAS_PADDING,
            &preserved, NULL));
    TEST_ASSERT_EQUAL_INT(value, preserved);

    gui_draft_discard();
    (void)remove(path);
}

void test_open_current_canonical_identity_rejects_before_replacement(void) {
    char path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        path, sizeof path,
        "%s/already-open.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(path);
    char error_text[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            path, error_text,
            sizeof error_text));
    tp_session *active =
        gui_project__test_session();
    const uint64_t generation =
        gui_project_session_instance_generation();
    const uint64_t open_calls =
        gui_project__test_open_call_count();
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_PROJECT_LIVE,
        gui_project_lifecycle_begin_open(
            path, &error));
    TEST_ASSERT_NOT_NULL(
        strstr(error.msg, path));
    TEST_ASSERT_EQUAL_UINT64(
        open_calls,
        gui_project__test_open_call_count());
    TEST_ASSERT_EQUAL_PTR(
        active,
        gui_project__test_session());
    TEST_ASSERT_EQUAL_UINT64(
        generation,
        gui_project_session_instance_generation());
    TEST_ASSERT_EQUAL_INT(
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE,
        gui_project_lifecycle_state_query());
    TEST_ASSERT_TRUE(gui_project_test_new());
    (void)remove(path);
}

void test_deleted_pack_target_is_typed_failure_not_cancelled(void) {
    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
    const tp_id128 target_atlas_id =
        add_coin_source_to_atlas(1);
    TEST_ASSERT_TRUE(
        gui_pack_init(
            TP_GUI_IDENTITY_TEST_DIR));
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_pack_async_start(
            1, error, sizeof error));

    /* Host admission captures the target before the model mutation, but
     * observation classification intentionally happens after deletion. */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_pump(NULL, NULL));
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    TEST_ASSERT_TRUE(
        gui_project_remove_atlas(
            target_atlas_id,
            tp_session_snapshot_revision(
                snapshot)));

    gui_pack_result_info info = {0};
    gui_pack_done done =
        GUI_PACK_DONE_NONE;
    for (int attempt = 0;
         attempt < 5000 &&
         done == GUI_PACK_DONE_NONE;
         ++attempt) {
        done = pump_pack_frame(&info);
        if (done == GUI_PACK_DONE_NONE) {
            nt_time_sleep(0.001);
        }
    }
    TEST_ASSERT_EQUAL_INT(
        GUI_PACK_DONE_PACK_FAIL, done);
    TEST_ASSERT_EQUAL_INT(
        TP_SESSION_JOB_REJECTION_TARGET_DELETED,
        info.rejection);
    TEST_ASSERT_NOT_NULL(
        strstr(info.err, "target was deleted"));
}

void test_long_sprite_keys_with_shared_prefix_keep_distinct_draft_identity(void) {
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

    gui_sprite_ref first_ref = {
        atlas_id, source_id, first,
        tp_session_snapshot_revision(snapshot)
    };
    gui_edit_sprite_override(
        &first_ref, GUI_SPRITE_OV_MARGIN, 3);
    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, gui_draft_phase());

    snapshot = gui_project_snapshot();
    gui_sprite_ref second_ref = {
        atlas_id, source_id, second,
        tp_session_snapshot_revision(snapshot)
    };
    gui_edit_sprite_override(
        &second_ref, GUI_SPRITE_OV_MARGIN, 7);
    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(GUI_EDIT_IDLE, gui_draft_phase());

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
    TEST_ASSERT_TRUE(test_submit_atlas_name(
        atlas_id, tp_session_snapshot_revision(snapshot), oversized));
    snapshot = gui_project_snapshot();
    cancel_edit();
    start_atlas_edit_ref(atlas_id, tp_session_snapshot_revision(snapshot));
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());

    const int animation_index =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "long-name-fixture", NULL, 0)
            .visible_index;
    TEST_ASSERT_EQUAL_INT(0, animation_index);
    snapshot = gui_project_snapshot();
    const tp_snapshot_animation *animation =
        tp_session_snapshot_animation_at(snapshot, atlas_id, animation_index);
    TEST_ASSERT_NOT_NULL(animation);
    gui_animation_ref animation_ref = {
        atlas_id, animation->id, tp_session_snapshot_revision(snapshot)};
    TEST_ASSERT_TRUE(test_submit_animation_name(&animation_ref, oversized));
    snapshot = gui_project_snapshot();
    animation_ref.expected_revision = tp_session_snapshot_revision(snapshot);
    cancel_edit();
    start_anim_edit_ref(&animation_ref);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());

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
    TEST_ASSERT_TRUE(test_submit_sprite_name(&sprite, oversized));
    snapshot = gui_project_snapshot();
    const gui_sprite_ref renamed = {
        atlas_id, source_id, "coin.png",
        tp_session_snapshot_revision(snapshot)};
    cancel_edit();
    start_sprite_edit_ref(&renamed, oversized);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_IDLE, gui_draft_phase());
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
    TEST_ASSERT_TRUE(test_submit_atlas_name(
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
    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
    const tp_id128 packed_atlas_id = add_coin_source_to_atlas(1);

    gui_view_select_atlas(packed_atlas_id);
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
    TEST_ASSERT_TRUE(test_offer_atlas_draft(
        packed_atlas_id, tp_session_snapshot_revision(snapshot),
        GUI_ATLAS_PIXELS_PER_UNIT, 0, 2.0F));
    gui_view_select_atlas(packed_atlas_id);
    do_pack_blocking();
    packed = gui_pack_result(0);
    TEST_ASSERT_NOT_NULL(packed);
    TEST_ASSERT_TRUE(packed->pixels_per_unit == 2.0F);

    TEST_ASSERT_EQUAL_INT(
        1, gui_project_add_atlas().visible_index);
    const tp_id128 other_atlas_id =
        add_coin_source_to_atlas(1);
    gui_view_select_atlas(other_atlas_id);
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
    gui_view_select_atlas(tp_id128_nil());
    apply_pending();
    gui_view_select_atlas(atlas_id);

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
    const int animation_index =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            "picked", NULL, 0)
            .visible_index;
    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, animation_index);
    const tp_snapshot_animation *created =
        tp_session_snapshot_animation_at(
            gui_project_snapshot(), atlas_id,
            animation_index);
    TEST_ASSERT_NOT_NULL(created);
    const gui_animation_ref animation_ref = {
        atlas_id, created->id,
        tp_session_snapshot_revision(
            gui_project_snapshot())};
    multi_sel_add_ref(left->source_id, left->source_key);
    multi_sel_add_ref(right->source_id, right->source_key);
    add_selection_frames_to_animation(
        &animation_ref);
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

    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_EDITING, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        "right-name", gui_text_edit_value());
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
    char changed_atlas[] = "changed-atlas";
    tp_operation operation = {
        .kind = TP_OP_ATLAS_RENAME,
        .atlas_id = {{0}},
        .u.atlas_rename.name = changed_atlas,
    };
    operation.atlas_id = atlas_id;
    tp_txn_request request = {
        .schema = TP_TXN_SCHEMA,
        .expected_revision =
            tp_session_snapshot_revision(snapshot),
        .label = "atlas.rename",
        .author = "test",
        .ops = &operation,
        .op_count = 1,
    };
    (void)snprintf(
        request.id_hex, sizeof request.id_hex,
        "%s", "abababababababababababababababab");
    tp_txn_result result = {0};
    tp_error apply_error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_session_apply(
            gui_project__test_session(), &request,
            &result, &apply_error));
    tp_txn_result_free(&result);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&apply_error));
    gui_project_frame_end();
    TEST_ASSERT_TRUE(
        gui_text_edit_update("must-not-land"));
    gui_request_gesture_commit();
    apply_pending();

    snapshot = gui_project_snapshot();
    const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_by_key(
        snapshot, atlas_id, right_id, "shared.png");
    TEST_ASSERT_NOT_NULL(sprite);
    TEST_ASSERT_EQUAL_STRING("right-name", sprite->rename);
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED, gui_draft_phase());
    TEST_ASSERT_EQUAL_STRING(
        "must-not-land", gui_text_edit_value());
    gui_draft_discard();
}

void test_delayed_animation_context_ref_never_retargets_after_index_shift(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                         ? tp_session_snapshot_atlas_at(snapshot, 0)
                                         : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, (gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "a", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, (gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "b", NULL,
               0)).visible_index);
    snapshot = gui_project_snapshot();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(
        0, (gui_project_create_animation(
               atlas_id, tp_session_snapshot_revision(snapshot), "c", NULL,
               0)).visible_index);

    gui_animation_ref first;
    gui_animation_ref captured_second;
    TEST_ASSERT_TRUE(test_animation_ref_at(0, 0, &first));
    TEST_ASSERT_TRUE(test_animation_ref_at(0, 1, &captured_second));
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

void test_delayed_target_context_ref_never_retargets_after_index_shift(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        snapshot
            ? tp_session_snapshot_atlas_at(snapshot, 0)
            : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(1, atlas->target_count);
    const tp_id128 atlas_id = atlas->id;

    const gui_project_create_result second =
        gui_project_add_target(
            atlas_id,
            tp_session_snapshot_revision(snapshot));
    TEST_ASSERT_TRUE(second.committed);
    snapshot = gui_project_snapshot();
    const gui_project_create_result third =
        gui_project_add_target(
            atlas_id,
            tp_session_snapshot_revision(snapshot));
    TEST_ASSERT_TRUE(third.committed);

    gui_target_ref first;
    gui_target_ref captured_second;
    TEST_ASSERT_TRUE(test_target_ref_at(0, 0, &first));
    TEST_ASSERT_TRUE(test_target_ref_at(0, 1, &captured_second));
    TEST_ASSERT_TRUE(tp_id128_eq(
        second.created_id, captured_second.target_id));
    TEST_ASSERT_TRUE(gui_project_remove_target(&first));

    gui_request_remove_target_ref(&captured_second);
    apply_pending();

    snapshot = gui_project_snapshot();
    TEST_ASSERT_NOT_NULL(tp_session_snapshot_target_by_id(
        snapshot, atlas_id, second.created_id));
    TEST_ASSERT_NOT_NULL(tp_session_snapshot_target_by_id(
        snapshot, atlas_id, third.created_id));
    atlas = tp_session_snapshot_atlas_by_id(snapshot, atlas_id);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(2, atlas->target_count);
    char error[256] = {0};
    TEST_ASSERT_TRUE(
        gui_project_take_op_error(error, sizeof error));
    TEST_ASSERT_NOT_NULL(strstr(error, "revision"));
}

void test_required_recovery_without_root_warns_but_allows_edit_undo_redo(void) {
    gui_project_require_recovery();
    tp_error lifecycle_error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_lifecycle_begin_new(
            &lifecycle_error));
    char warning[256] = {0};
    TEST_ASSERT_FALSE(
        gui_project_take_recovery_setup_notice(
            warning, sizeof warning));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_test_finish(
            GUI_PROJECT_LIFECYCLE_NEW,
            &lifecycle_error));

    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = snapshot
                                          ? tp_session_snapshot_atlas_at(snapshot, 0)
                                          : NULL;
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_FALSE(
        tp_session_recovery_available(gui_project__test_session()));
    const tp_id128 atlas_id = atlas->id;
    const int padding_before = atlas->padding;
    TEST_ASSERT_TRUE(test_offer_atlas_draft(
        atlas_id, tp_session_snapshot_revision(snapshot), GUI_ATLAS_PADDING,
        padding_before + 1, 0.0F));
    TEST_ASSERT_FALSE(gui_project_can_undo());
    gui_request_gesture_commit();
    apply_pending();
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
        tp_session_recovery_available(gui_project__test_session()));

    TEST_ASSERT_TRUE(
        gui_project_take_recovery_setup_notice(warning, sizeof warning));
    TEST_ASSERT_NOT_NULL(strstr(warning, "Crash recovery is unavailable"));
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
    TEST_ASSERT_TRUE(test_submit_atlas_name(
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
        tp_session_recovery_health_query(gui_project__test_session());
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
    TEST_ASSERT_TRUE(test_offer_atlas_draft(
        atlas_id, tp_session_snapshot_revision(snapshot) - 1,
        GUI_ATLAS_PADDING, atlas->padding + 1, 0.0F));
    gui_request_gesture_commit();
    apply_pending();
    TEST_ASSERT_EQUAL_INT(
        GUI_EDIT_CONFLICTED,
        gui_draft_phase());
    gui_draft_discard();
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
        tp_session_recovery_health_query(gui_project__test_session()).degraded);
    (void)remove(save_path);
}

void test_save_as_recovery_rebind_uses_post_save_identity(void) {
    char recovery_root[512];
    char save_path[512];
    char canonical_path[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        recovery_root, sizeof recovery_root,
        "%s/rebind-recovery",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(
        save_path, sizeof save_path,
        "%s/rebind.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)remove(save_path);
    (void)remove_flat_test_dir(recovery_root);
    tp_mkdirs(recovery_root);

    gui_project_test_shutdown(true);
    gui_project_enable_recovery(recovery_root);
    gui_project_init();

    char error[256] = {0};
    const tp_session_snapshot *before_save =
        gui_project_snapshot();
    const tp_snapshot_atlas *before_save_atlas =
        tp_session_snapshot_atlas_at(
            before_save, 0);
    TEST_ASSERT_NOT_NULL(before_save_atlas);
    const char *attached_journal =
        tp_session__recovery_journal_path(
            gui_project__test_session());
    TEST_ASSERT_NOT_NULL(attached_journal);
    char pre_save_journal[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        pre_save_journal, sizeof pre_save_journal,
        "%s", attached_journal);
    tp_journal__test_set_record_limit(1U);
    TEST_ASSERT_TRUE(
        test_submit_atlas_name(
            before_save_atlas->id,
            tp_session_snapshot_revision(before_save),
            "rebind-before-save"));
    TEST_ASSERT_TRUE(
        tp_session_recovery_health_query(
            gui_project__test_session()).degraded);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_session_recovery_health_query(
            gui_project__test_session()).first_cause);
    tp_journal__test_set_record_limit(0U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            save_path, error, sizeof error));
    const char *rebound_journal =
        tp_session__recovery_journal_path(
            gui_project__test_session());
    TEST_ASSERT_NOT_NULL(rebound_journal);
    TEST_ASSERT_NOT_EQUAL(
        0, strcmp(pre_save_journal, rebound_journal));
    char post_save_journal[TP_IDENTITY_PATH_MAX];
    (void)snprintf(
        post_save_journal, sizeof post_save_journal,
        "%s", rebound_journal);
    TEST_ASSERT_FALSE(
        tp_session_recovery_health_query(
            gui_project__test_session()).degraded);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_identity_project_path_canonical(
            save_path, canonical_path,
            sizeof canonical_path, NULL));
    tp_id128 fingerprint = tp_id128_nil();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_identity_file_fingerprint(
            canonical_path, &fingerprint, NULL));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(NULL));
    gui_project_frame_end();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(
            snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(
        test_submit_atlas_name(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            "dirty-after-save"));
    gui_project_test_shutdown(false);

    gui_project_enable_recovery(recovery_root);
    gui_project_init();
    gui_recovery_list candidates = {0};
    TEST_ASSERT_EQUAL_INT(
        1, gui_recovery_collect(&candidates));
    const gui_recovery_entry *entry =
        &candidates.items[0];
    TEST_ASSERT_EQUAL_STRING(
        canonical_path, entry->original_path);
    TEST_ASSERT_EQUAL_STRING(
        "rebind.ntpacker_project",
        entry->name);
    TEST_ASSERT_EQUAL_STRING(
        post_save_journal,
        entry->journal_path);
    TEST_ASSERT_NOT_EQUAL(
        0, strcmp(pre_save_journal,
                  entry->journal_path));
    TEST_ASSERT_TRUE(
        entry->has_file_fingerprint);
    TEST_ASSERT_TRUE(
        tp_id128_eq(
            fingerprint,
            entry->file_fingerprint));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_recovery_resolve_entry(
            entry, GUI_RECOVERY_DISCARD,
            NULL, error, sizeof error));

    gui_project_test_shutdown(true);
    gui_project_enable_recovery(NULL);
    TEST_ASSERT_TRUE(
        remove_flat_test_dir(recovery_root));
    TEST_ASSERT_EQUAL_INT(0, remove(save_path));
    gui_project_init();
}

void test_save_as_projects_clean_identity_only_at_common_frame_observation(void) {
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_TRUE(
        test_submit_atlas_name(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            "save-observation-boundary"));
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_FALSE(gui_project_has_path());

    char save_path[512];
    char lock_path[544];
    (void)snprintf(
        save_path, sizeof save_path,
        "%s/save-observation.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(
        lock_path, sizeof lock_path,
        "%s.ntpacker.lock", save_path);
    (void)remove(save_path);
    (void)remove(lock_path);
    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            save_path, error, sizeof error));

    /* The Save receipt is exact and synchronous, while presentation state
     * remains the prior immutable observation until the next frame cut. */
    TEST_ASSERT_TRUE(gui_project_is_dirty());
    TEST_ASSERT_FALSE(gui_project_has_path());
    tp_error frame_error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(
            &frame_error));
    gui_project_frame_end();
    TEST_ASSERT_FALSE(gui_project_is_dirty());
    TEST_ASSERT_TRUE(gui_project_has_path());

    TEST_ASSERT_TRUE(gui_project_test_new());
    TEST_ASSERT_EQUAL_INT(0, remove(save_path));
    TEST_ASSERT_EQUAL_INT(0, remove(lock_path));
}

void test_save_as_roundtrip_releases_writer_before_open_and_keeps_external_guard(void) {
    char save_path[512];
    char rebound_path[512];
    char save_lock_path[544];
    char rebound_lock_path[544];
    (void)snprintf(
        save_path, sizeof save_path,
        "%s/roundtrip.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(
        rebound_path, sizeof rebound_path,
        "%s/roundtrip-rebound.ntpacker_project",
        TP_GUI_IDENTITY_TEST_DIR);
    (void)snprintf(
        save_lock_path, sizeof save_lock_path,
        "%s.ntpacker.lock", save_path);
    (void)snprintf(
        rebound_lock_path, sizeof rebound_lock_path,
        "%s.ntpacker.lock", rebound_path);
    (void)remove(save_path);
    (void)remove(rebound_path);
    (void)remove(save_lock_path);
    (void)remove(rebound_lock_path);

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
            s_left_file, TP_SOURCE_KIND_FILE));

    char error[256] = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            save_path, error, sizeof error));

    /* Opening the current canonical identity would contend with this
     * session's own writer lease. A real round-trip releases that owner
     * before Open; in-place reload remains an explicit R2d concern. */
    TEST_ASSERT_TRUE(gui_project_test_new());
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_test_open(
            save_path, error, sizeof error));
    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_at(snapshot, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    TEST_ASSERT_EQUAL_INT(1, atlas->source_count);
    TEST_ASSERT_FALSE(gui_project_is_dirty());

    static const char external_sentinel[] =
        "external-edit-sentinel";
    FILE *file = fopen(save_path, "wb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(
        sizeof external_sentinel,
        fwrite(
            external_sentinel, 1U,
            sizeof external_sentinel, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_FILE_CHANGED_EXTERNALLY,
        gui_project_save(error, sizeof error));

    char readback[sizeof external_sentinel] = {0};
    file = fopen(save_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_size_t(
        sizeof readback,
        fread(readback, 1U, sizeof readback, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    TEST_ASSERT_EQUAL_MEMORY(
        external_sentinel, readback,
        sizeof external_sentinel);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_save_as(
            rebound_path, error, sizeof error));

    TEST_ASSERT_TRUE(gui_project_test_new());
    TEST_ASSERT_EQUAL_INT(0, remove(save_path));
    TEST_ASSERT_EQUAL_INT(0, remove(rebound_path));
    TEST_ASSERT_EQUAL_INT(0, remove(save_lock_path));
    TEST_ASSERT_EQUAL_INT(0, remove(rebound_lock_path));
}

static void exhaust_two_observation_failures(void) {
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_frame_begin(&error));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OOM,
        gui_project_frame_begin(&error));
    gui_project_frame_end();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        gui_project_frame_begin(&error));
    gui_project_frame_end();
}

void test_committed_atlas_create_stays_successful_until_echo_reconciles(void) {
    gui_project__test_fail_observes(3);
    const gui_project_create_result created =
        gui_project_add_atlas();
    TEST_ASSERT_TRUE(created.committed);
    TEST_ASSERT_TRUE(created.observation_pending);
    TEST_ASSERT_EQUAL_INT(-1, created.visible_index);
    TEST_ASSERT_FALSE(tp_id128_is_nil(created.created_id));
    TEST_ASSERT_EQUAL_INT64(
        1, tp_session_revision(
               gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        1, tp_session_snapshot_atlas_count(
               gui_project_snapshot()));

    exhaust_two_observation_failures();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    TEST_ASSERT_NOT_NULL(
        tp_session_snapshot_atlas_by_id(
            snapshot, created.created_id));
}

void test_committed_target_create_stays_successful_until_echo_reconciles(void) {
    const tp_session_snapshot *before =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    gui_project__test_fail_observes(3);
    const gui_project_create_result created =
        gui_project_add_target(
            atlas_id,
            tp_session_snapshot_revision(before));
    TEST_ASSERT_TRUE(created.committed);
    TEST_ASSERT_TRUE(created.observation_pending);
    TEST_ASSERT_EQUAL_INT(-1, created.visible_index);
    TEST_ASSERT_FALSE(tp_id128_is_nil(created.created_id));
    TEST_ASSERT_EQUAL_INT64(
        1, tp_session_revision(
               gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        1, atlas->target_count);

    exhaust_two_observation_failures();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *observed =
        tp_session_snapshot_atlas_by_id(
            snapshot, atlas_id);
    bool found = false;
    for (int index = 0;
         observed && index < observed->target_count;
         ++index) {
        const tp_snapshot_target *target =
            tp_session_snapshot_target_at(
                snapshot, observed->id, index);
        found = found ||
                (target &&
                 tp_id128_eq(
                     target->id,
                     created.created_id));
    }
    TEST_ASSERT_TRUE(found);
}

void test_committed_animation_create_stays_successful_until_echo_reconciles(void) {
    const tp_session_snapshot *before =
        gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(before, 0);
    TEST_ASSERT_NOT_NULL(atlas);
    const tp_id128 atlas_id = atlas->id;
    gui_project__test_fail_observes(3);
    const gui_project_create_result created =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(before),
            "pending-echo", NULL, 0);
    TEST_ASSERT_TRUE(created.committed);
    TEST_ASSERT_TRUE(created.observation_pending);
    TEST_ASSERT_EQUAL_INT(-1, created.visible_index);
    TEST_ASSERT_FALSE(tp_id128_is_nil(created.created_id));
    TEST_ASSERT_EQUAL_INT64(
        1, tp_session_revision(
               gui_project__test_session()));
    TEST_ASSERT_EQUAL_INT(
        0, atlas->animation_count);

    exhaust_two_observation_failures();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const tp_snapshot_atlas *observed =
        tp_session_snapshot_atlas_by_id(
            snapshot, atlas_id);
    bool found = false;
    for (int index = 0;
         observed && index < observed->animation_count;
         ++index) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(
                snapshot, observed->id, index);
        found = found ||
                (animation &&
                 tp_id128_eq(
                     animation->id,
                     created.created_id));
    }
    TEST_ASSERT_TRUE(found);
}

int main(int argc, char **argv) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    UNITY_BEGIN();
    RUN_TEST(test_arbitrary_result_lookup_follows_displayed_sprite_order);
    RUN_TEST(test_canvas_result_rebind_resets_double_click_identity);
    RUN_TEST(
        test_selected_atlas_id_survives_preceding_atlas_removal);
    RUN_TEST(
        test_selected_animation_id_survives_preceding_animation_removal);
    RUN_TEST(test_rows_apply_renames_by_canonical_source_and_key);
    RUN_TEST(test_create_animation_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_add_frames_preserves_both_canonical_selected_sprites);
    RUN_TEST(test_sprite_edit_state_uses_canonical_duplicate_identity);
    RUN_TEST(test_sprite_edit_rejects_genuinely_stale_captured_revision);
    RUN_TEST(test_delayed_animation_context_ref_never_retargets_after_index_shift);
    RUN_TEST(test_delayed_target_context_ref_never_retargets_after_index_shift);
    RUN_TEST(test_preview_result_rejects_source_refresh_after_job_capture);
    RUN_TEST(
        test_preview_after_native_pack_uses_observed_runtime_generation);
    RUN_TEST(
        test_native_pack_marks_token_change_stale_without_host_side_probe);
    RUN_TEST(test_nil_completed_pack_input_hash_is_stale);
    RUN_TEST(test_failed_current_hash_probe_is_stale);
    RUN_TEST(test_gui_poll_does_not_decode_sources_on_gui_thread);
    RUN_TEST(
        test_deleted_pack_target_is_typed_failure_not_cancelled);
    RUN_TEST(test_long_sprite_keys_with_shared_prefix_keep_distinct_draft_identity);
    RUN_TEST(test_oversized_names_cannot_enter_a_truncating_editor);
    RUN_TEST(test_recovery_entry_keeps_core_path_capacity);
    RUN_TEST(test_canvas_cache_key_keeps_core_path_capacity);
    RUN_TEST(test_undo_redo_keep_last_successful_pack_result);
    RUN_TEST(test_pack_result_follows_stable_atlas_across_index_shift);
    RUN_TEST(test_recovery_notice_is_sticky_exact_and_clears_after_save_heals);
    RUN_TEST(
        test_save_as_recovery_rebind_uses_post_save_identity);
    RUN_TEST(
        test_save_as_roundtrip_releases_writer_before_open_and_keeps_external_guard);
    RUN_TEST(
        test_save_as_projects_clean_identity_only_at_common_frame_observation);
    RUN_TEST(
        test_controller_guard_rejects_identity_change_before_flush_or_write);
    RUN_TEST(
        test_save_as_preflight_success_does_not_submit_or_write);
    RUN_TEST(
        test_save_as_preflight_observe_failure_preserves_draft_and_model);
    RUN_TEST(
        test_open_current_canonical_identity_rejects_before_replacement);
    RUN_TEST(test_required_recovery_without_root_warns_but_allows_edit_undo_redo);
    RUN_TEST(
        test_committed_atlas_create_stays_successful_until_echo_reconciles);
    RUN_TEST(
        test_committed_target_create_stays_successful_until_echo_reconciles);
    RUN_TEST(
        test_committed_animation_create_stays_successful_until_echo_reconciles);
    return UNITY_END();
}
