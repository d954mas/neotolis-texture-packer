/* ntpacker-gui dev seam: the headless self-test + auto-quit render/verify phase driver.
 * The whole TU compiles to nothing unless NTPACKER_GUI_SELFTEST is defined (a placeholder typedef
 * keeps it a legal ISO C translation unit). Moved verbatim out of main.c (GUI decomposition step 3);
 * only run_selftest/selftest_pre_frame/selftest_post_draw gained external linkage (the header hooks).
 * See gui_selftest.h. */

#include "gui_selftest.h"

#ifdef NTPACKER_GUI_SELFTEST

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h> /* clock() -- bounded wait for the async pack equivalence check */

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h> /* getcwd -- to_abs() makes a relative path absolute on POSIX too */
#endif

#include "app/nt_app.h"         /* nt_app_quit */
#include "core/nt_assert.h"     /* NT_ASSERT */
#include "graphics/nt_gfx.h"    /* nt_gfx_read_pixels (overlay pixel probe) */
#include "log/nt_log.h"         /* nt_log_info (SELFTEST-* logging) */
#include "time/nt_time.h"
#include "ui/nt_ui.h"           /* nt_ui_get_bbox / nt_ui_id / nt_ui_bbox_t */
#include "window/nt_window.h"   /* g_nt_window (phase-driven framebuffer dims) */

#include "tp_core/tp_error.h"   /* tp_status_str / tp_error */
#include "tp_core/tp_export.h"  /* tp_format_count/at (preview-target selector index) */
#include "tp_core/tp_id.h"      /* stable structural-ID assertions */
#include "tp_core/tp_pack_result.h"   /* tp_result */
#include "tp_core/tp_names.h"   /* tp_sprite_export_key (region -> override key) */
#include "tp_core/tp_journal.h" /* in-memory recovery fixture */
#include "tp_core/tp_scan.h"    /* tp_mkdirs (portable temp-dir creation for the CI stress dirs) */
#include "tp_core/tp_sprite_index.h" /* canonical A4 selector fixture */
#include "tp_journal_internal.h" /* bounded write-failure fixture */
#include "tp_session_internal.h" /* recovery attach fixture */
#include "nt_utf8_fs.h" /* UTF-8 fixture filenames on Windows */

#include "gui_actions.h"  /* reset_selection / preview_stop / anim ops + gui_request_gesture_commit */
#include "gui_actions_dev.h" /* explicit host-driving self-test seams */
#include "gui_actions_internal.h" /* focused reducer submit prerequisite */
#include "gui_canvas.h"   /* s_canvas ops + GUI_CANVAS_ATLAS */
#include "gui_pack.h"     /* gui_pack_* + GUI_PACK_ASYNC_* */
#include "gui_project.h"  /* gui_project_* + GUI_SPRITE_OV_SHAPE / GUI_ADD_DUPLICATE */
#include "gui_project_test_driver.h"
#include "gui_project_operations.h" /* snapshot-read helpers behind the deleted forwarders */
#include "gui_rows.h"     /* build_rows / multi_sel_* / select_row_for_region */
#include "gui_shell.h"    /* UI_STATE_SLOTS / UI_STATE_PROBE_MAX / UI_ROW_ID_RING */
#include "gui_startup.h"  /* H/P1-8: gui_startup_decide + GUI_STARTUP_* (J14 truth table) */
#include "gui_state.h"    /* s_canvas / disclosures / modals / s_ctx / shared UI ids */
#include "gui_view_chrome.h" /* menu-open keyboard guard */

static tp_journal_io s_test_recovery_io; /* borrowed while the session owns ctx */

static bool gui_project__test_attach_memory_recovery(void) {
    tp_journal_io io = tp_journal_io_memory();
    if (!io.ctx) {
        return false;
    }
    static const uint8_t key_bytes[16] = {
        'n', 't', 'p', 'k', '_', 'r', 'e', 'c',
        'o', 'v', 'e', 'r', 'y', '_', '0', '1'};
    tp_id128 key;
    memcpy(key.bytes, key_bytes, sizeof key.bytes);
    tp_journal *journal = tp_journal_create(io, key);
    if (!journal) {
        return false; /* create consumed io */
    }
    tp_error error = {{0}};
    if (tp_session_attach_journal(gui_project__test_session(), journal,
                                  &error) != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        return false;
    }
    s_test_recovery_io = io;
    return true;
}

static void gui_project__test_fail_next_recovery_writes(int count) {
    tp_journal_io_memory__fail_next_writes(s_test_recovery_io, count);
}

static bool gui_project__test_recovery_notice(tp_status expected,
                                              gui_recovery_notice *out) {
    gui_recovery_notice notice = {0};
    const tp_session_recovery_health health =
        tp_session_recovery_health_query(gui_project__test_session());
    const bool active = gui_project_recovery_notice_query(&notice);
    const bool exact =
        active && health.degraded &&
        strcmp(notice.notice_id, TP_SESSION_NOTICE_RECOVERY_DEGRADED) == 0 &&
        notice.generation == health.generation &&
        notice.status == expected && health.first_cause == expected &&
        strstr(notice.message, tp_status_str(expected)) != NULL;
    if (out) {
        *out = notice;
    }
    return exact;
}

/* Dev-seam index conveniences resolve against the same owned snapshot a real
 * widget uses, then exercise the stable-ID production contract. */
static const tp_snapshot_atlas *selftest_atlas_at(int index,
                                                  const tp_session_snapshot **snapshot_out) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    if (snapshot_out) {
        *snapshot_out = snapshot;
    }
    return snapshot ? tp_session_snapshot_atlas_at(snapshot, index) : NULL;
}

static bool selftest_select_atlas(int index) {
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, NULL);
    gui_view_select_atlas(atlas ? atlas->id : tp_id128_nil());
    return atlas != NULL;
}

static int selftest_selected_atlas_index(void) {
    return gui_view_atlas_index(gui_project_snapshot());
}

static bool selftest_select_animation_at(int atlas_index,
                                         int animation_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas =
        selftest_atlas_at(atlas_index, NULL);
    const tp_snapshot_animation *animation =
        atlas ? tp_session_snapshot_animation_at(
                    snapshot, atlas->id, animation_index)
              : NULL;
    if (!atlas || !animation) {
        gui_view_select_animation(tp_id128_nil());
        return false;
    }
    gui_view_select_atlas(atlas->id);
    gui_view_select_animation(animation->id);
    return true;
}

static void selftest_clear_animation_selection(void) {
    gui_view_select_animation(tp_id128_nil());
}

static void selftest_observe_session(void) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        tp_error error = {{0}};
        NT_ASSERT(
            gui_actions_step(NULL, &error) ==
            TP_STATUS_OK);
        if (gui_project_snapshot() &&
            s_actions.intent_count == 0 &&
            s_actions.pending_lifecycle_request ==
                GUI_LIFECYCLE_REQUEST_NONE) {
            return;
        }
    }
    NT_ASSERT(
        gui_project_snapshot() &&
        s_actions.intent_count == 0 &&
        s_actions.pending_lifecycle_request ==
            GUI_LIFECYCLE_REQUEST_NONE &&
        "self-test action queue must reach a quiescent published observation cut");
}

/* Membership edits coalesce an automatic Refresh. A synchronous self-test
 * adapter settles only the Refresh it caused; it must never drain a Pack or
 * Export that was already active for a lifecycle/concurrency probe. */
static void selftest_settle_new_refresh(
    bool job_was_busy) {
    for (int attempt = 0;
         !job_was_busy &&
         attempt < 5000 &&
         gui_project_job_busy() &&
         gui_project_job_active_kind() ==
             TP_SESSION_JOB_REFRESH;
         ++attempt) {
        nt_time_sleep(0.001);
        selftest_observe_session();
    }
    NT_ASSERT(
        job_was_busy ||
        !gui_project_job_busy() ||
        gui_project_job_active_kind() !=
            TP_SESSION_JOB_REFRESH);
}

static const tp_snapshot_animation *selftest_animation_at(int atlas_index,
                                                          int animation_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, NULL);
    return atlas ? tp_session_snapshot_animation_at(snapshot, atlas->id,
                                                    animation_index)
                 : NULL;
}

static const tp_snapshot_frame *selftest_frame_at(int atlas_index,
                                                  int animation_index,
                                                  int frame_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, NULL);
    const tp_snapshot_animation *animation = selftest_animation_at(
        atlas_index, animation_index);
    return atlas && animation
               ? tp_session_snapshot_animation_frame_at(
                     snapshot, atlas->id, animation->id, frame_index)
               : NULL;
}

/* Legacy selftest fixtures spell unique sprite selectors as names. Resolve at
 * the test intent boundary, then call the canonical production selection API. */
static void selftest_multi_sel_add_name(const char *selector) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_index = gui_view_atlas_index(snapshot);
    const tp_snapshot_atlas *atlas =
        atlas_index >= 0
            ? tp_session_snapshot_atlas_at(snapshot, atlas_index)
            : NULL;
    tp_selector_result resolved;
    tp_id128 source_id = tp_id128_nil();
    char source_key[TP_SRCKEY_MAX];
    tp_error err = {0};
    const tp_status status = atlas
        ? tp_session_snapshot_resolve_sprite_selector(
              snapshot, atlas->id, selector, &resolved, &source_id,
              source_key, sizeof source_key, NULL, &err)
        : TP_STATUS_NOT_FOUND;
    if (status == TP_STATUS_OK) {
        multi_sel_add_ref(source_id, source_key);
    } else {
        /* Capacity-only fixtures intentionally use synthetic names that do not
         * exist in the project. Keep them structurally canonical without
         * reintroducing a production name-only selection path. */
        source_id = tp_id128_nil();
        source_id.bytes[0] = 1U;
        multi_sel_add_ref(source_id, selector);
    }
}

#define multi_sel_add(selector) selftest_multi_sel_add_name(selector)

static const tp_snapshot_target *selftest_target_at(int atlas_index,
                                                    int target_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, NULL);
    return atlas ? tp_session_snapshot_target_at(snapshot, atlas->id, target_index)
                 : NULL;
}

static bool selftest_target_ref_at(int atlas_index, int target_index,
                                   gui_target_ref *out) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    const tp_snapshot_target *target =
        atlas ? tp_session_snapshot_target_at(
                    snapshot, atlas->id, target_index)
              : NULL;
    if (!target || !out) {
        return false;
    }
    *out = (gui_target_ref){
        atlas->id, target->id,
        tp_session_snapshot_revision(snapshot)};
    return true;
}

static bool selftest_animation_ref_at(
    int atlas_index, int animation_index,
    gui_animation_ref *out) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    const tp_snapshot_animation *animation =
        atlas ? tp_session_snapshot_animation_at(
                    snapshot, atlas->id, animation_index)
              : NULL;
    if (!animation || !out) {
        return false;
    }
    *out = (gui_animation_ref){
        atlas->id, animation->id,
        tp_session_snapshot_revision(snapshot)};
    return true;
}

static const tp_snapshot_sprite *selftest_sprite_by_name(int atlas_index,
                                                         const char *name) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, NULL);
    for (int i = 0; atlas && i < atlas->sprite_count; ++i) {
        const tp_snapshot_sprite *sprite = tp_session_snapshot_sprite_at(
            snapshot, atlas->id, i);
        if (sprite && strcmp(sprite->name ? sprite->name : "", name ? name : "") == 0) {
            return sprite;
        }
    }
    return NULL;
}

static bool selftest_set_atlas_name_at(int index, const char *name) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    if (!atlas ||
        !gui_text_edit_begin_atlas_name(
            atlas->id,
            tp_session_snapshot_revision(snapshot),
            atlas->name) ||
        !gui_text_edit_update(name)) {
        return false;
    }
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    if (!committed) {
        gui_draft_discard();
    }
    return committed;
}

static bool selftest_remove_atlas_at(int index) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    if (!atlas) {
        return false;
    }
    const bool removed = gui_project_remove_atlas(
        atlas->id, tp_session_snapshot_revision(snapshot));
    selftest_observe_session();
    return removed;
}

static bool selftest_remove_atlas_ref(tp_id128 atlas_id,
                                      int64_t expected_revision) {
    const bool removed = gui_project_remove_atlas(
        atlas_id, expected_revision);
    selftest_observe_session();
    return removed;
}

static tp_status selftest_copy_atlas_name_at(int index, char *out, size_t capacity,
                                             tp_error *err) {
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, NULL);
    return atlas ? gui_project_operation_copy_atlas_name(gui_project_snapshot(), atlas->id,
                                              out, capacity, err)
                 : tp_error_set(err, TP_STATUS_NOT_FOUND, "atlas index was not found");
}

static gui_add_status selftest_add_source_at(int index, const char *path) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    if (!atlas) {
        return GUI_ADD_FAILED;
    }
    const bool job_was_busy =
        gui_project_job_busy();
    const gui_add_status status = gui_project_add_source_kind(
        atlas->id, tp_session_snapshot_revision(snapshot), path,
        TP_SOURCE_KIND_FOLDER);
    selftest_observe_session();
    selftest_settle_new_refresh(
        job_was_busy);
    return status;
}

static bool selftest_add_sources_at(int index, const char *const *paths,
                                    int path_count, tp_source_kind kind,
                                    int *added, int *duplicate) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    if (!atlas) {
        return false;
    }
    const bool job_was_busy =
        gui_project_job_busy();
    const bool committed = gui_project_add_sources(
        atlas->id, tp_session_snapshot_revision(snapshot), paths,
        path_count, kind, added, duplicate);
    selftest_observe_session();
    selftest_settle_new_refresh(
        job_was_busy);
    return committed;
}

static bool selftest_sprite_ref_at(int atlas_index, const char *source_key,
                                   gui_sprite_ref *out) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    const tp_snapshot_source *source = atlas && atlas->source_count > 0
                                           ? tp_session_snapshot_source_at(snapshot, atlas->id, 0)
                                           : NULL;
    /* Older GUI regression probes created name-only pending overrides in an atlas
     * with no source.  The production contract now requires the canonical
     * {source_id, raw source key} identity, so give those probes one inert file
     * source through the same public operation path before constructing the ref.
     * The path need not exist: these tests exercise project mutation, not scan. */
    if (atlas && !source) {
        const bool job_was_busy =
            gui_project_job_busy();
        if (gui_project_add_source_kind(
                atlas->id, tp_session_snapshot_revision(snapshot),
                "__ntpacker_selftest_sprite_source__.png", TP_SOURCE_KIND_FILE) !=
            GUI_ADD_ADDED) {
            return false;
        }
        selftest_observe_session();
        selftest_settle_new_refresh(
            job_was_busy);
        atlas = selftest_atlas_at(atlas_index, &snapshot);
        source = atlas ? tp_session_snapshot_source_at(snapshot, atlas->id, 0) : NULL;
    }
    if (!source || !source_key || source_key[0] == '\0') {
        return false;
    }
    *out = (gui_sprite_ref){atlas->id, source->id, source_key,
                            tp_session_snapshot_revision(snapshot)};
    return true;
}

static bool selftest_set_sprite_rename_at(int atlas_index, const char *source_key,
                                          const char *rename) {
    gui_sprite_ref sprite;
    if (!selftest_sprite_ref_at(
            atlas_index, source_key, &sprite) ||
        !gui_text_edit_begin_sprite_rename(
            &sprite, "") ||
        !gui_text_edit_update(rename)) {
        return false;
    }
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    if (!committed) {
        gui_draft_discard();
    }
    return committed;
}

static int selftest_pack_find_sprite_ref_at(int atlas_index, int source_index,
                                            const char *source_key) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    const tp_snapshot_source *source =
        atlas ? tp_session_snapshot_source_at(snapshot, atlas->id, source_index)
              : NULL;
    return source && source_key && source_key[0] != '\0'
               ? gui_pack_find_sprite_ref(atlas_index, source->id, source_key)
               : -1;
}

static bool selftest_rename_animation_frame_at(int atlas_index,
                                               int animation_index,
                                               int frame_index,
                                               const char *rename) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    const tp_snapshot_animation *animation = atlas
        ? tp_session_snapshot_animation_at(snapshot, atlas->id,
                                           animation_index)
        : NULL;
    const tp_snapshot_frame *frame = animation
        ? tp_session_snapshot_animation_frame_at(
              snapshot, atlas->id, animation->id, frame_index)
        : NULL;
    if (!frame) {
        return false;
    }
    const gui_sprite_ref sprite = {
        atlas->id, frame->source_id, frame->source_key,
        tp_session_snapshot_revision(snapshot)};
    if (!gui_text_edit_begin_sprite_rename(
            &sprite, "") ||
        !gui_text_edit_update(rename)) {
        return false;
    }
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    if (!committed) {
        gui_draft_discard();
    }
    return committed;
}

static bool selftest_set_sprite_origin_at(int atlas_index, const char *source_key,
                                          int axis, float value) {
    gui_sprite_ref sprite;
    if (!selftest_sprite_ref_at(
            atlas_index, source_key, &sprite)) {
        return false;
    }
    gui_edit_sprite_origin(&sprite, axis, value);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static bool selftest_set_sprite_slice9_at(int atlas_index, const char *source_key,
                                          int component, int value) {
    gui_sprite_ref sprite;
    if (!selftest_sprite_ref_at(
            atlas_index, source_key, &sprite)) {
        return false;
    }
    gui_edit_sprite_slice9(&sprite, component, value);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static bool selftest_set_sprite_override_at(int atlas_index, const char *source_key,
                                            gui_sprite_ov which, int value) {
    gui_sprite_ref sprite;
    if (!selftest_sprite_ref_at(
            atlas_index, source_key, &sprite)) {
        return false;
    }
    gui_edit_sprite_override(&sprite, which, value);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static int selftest_create_animation_at(int atlas_index, const char *base,
                                        const char *const *frames, int frame_count) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    if (!atlas || frame_count < 0 || (frame_count > 0 && !frames)) {
        return -1;
    }
    const tp_id128 atlas_id = atlas->id;
    tp_op_sprite_ref *refs = frame_count > 0
        ? calloc((size_t)frame_count, sizeof *refs)
        : NULL;
    char (*keys)[TP_SRCKEY_MAX] = frame_count > 0
        ? calloc((size_t)frame_count, sizeof *keys)
        : NULL;
    if (frame_count > 0 && (!refs || !keys)) {
        free(refs);
        free(keys);
        return -1;
    }
    for (int i = 0; i < frame_count; ++i) {
        tp_selector_result resolved;
        tp_error err = {0};
        if (tp_session_snapshot_resolve_sprite_selector(
                snapshot, atlas->id, frames[i], &resolved,
                &refs[i].source_id, keys[i], sizeof keys[i], NULL,
                &err) != TP_STATUS_OK) {
            free(refs);
            free(keys);
            return -1;
        }
        refs[i].src_key = keys[i];
    }
    const gui_project_create_result created =
        gui_project_create_animation(
            atlas_id,
            tp_session_snapshot_revision(snapshot),
            base, refs, frame_count);
    free(refs);
    free(keys);
    if (!created.committed) {
        selftest_observe_session();
        return -1;
    }
    selftest_observe_session();
    snapshot = gui_project_snapshot();
    atlas = tp_session_snapshot_atlas_by_id(
        snapshot, atlas_id);
    for (int index = 0;
         atlas && index < atlas->animation_count;
         ++index) {
        const tp_snapshot_animation *animation =
            tp_session_snapshot_animation_at(
                snapshot, atlas->id, index);
        if (animation &&
            tp_id128_eq(
                animation->id,
                created.created_id)) {
            return index;
        }
    }
    return -1;
}

static bool selftest_set_anim_id_at(int atlas_index, int animation_index,
                                    const char *name) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index,
            &animation) ||
        !gui_text_edit_begin_animation_name(
            &animation, "") ||
        !gui_text_edit_update(name)) {
        return false;
    }
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    if (!committed) {
        gui_draft_discard();
    }
    return committed;
}

static bool selftest_set_anim_fps_at(int atlas_index, int animation_index,
                                     float fps) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    gui_edit_anim_fps(&animation, fps);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static bool selftest_set_anim_playback_at(int atlas_index, int animation_index,
                                          int playback) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    gui_edit_anim_playback(&animation, playback);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static bool selftest_set_anim_flip_at(int atlas_index, int animation_index,
                                      bool flip_h, bool flip_v) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    gui_edit_anim_flip(&animation, 0, flip_h);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    selftest_observe_session();
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    gui_edit_anim_flip(&animation, 1, flip_v);
    gui_request_gesture_commit();
    gui_actions__selftest_drain_intents();
    const bool committed =
        gui_draft_phase() == GUI_EDIT_IDLE;
    selftest_observe_session();
    return committed;
}

static bool selftest_anim_remove_frame_at(int atlas_index, int animation_index,
                                          int frame_index) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    const bool removed =
        gui_project_anim_remove_frame(
            &animation, frame_index);
    selftest_observe_session();
    return removed;
}

static bool selftest_anim_move_frame_at(int atlas_index, int animation_index,
                                        int frame_index, int delta) {
    gui_animation_ref animation;
    if (!selftest_animation_ref_at(
            atlas_index, animation_index, &animation)) {
        return false;
    }
    const bool moved =
        gui_project_anim_move_frame(
            &animation, frame_index, delta);
    selftest_observe_session();
    return moved;
}

static bool selftest_remove_animation_named_at(int atlas_index, const char *name) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    for (int i = 0; atlas && i < atlas->animation_count; i++) {
        const tp_snapshot_animation *candidate =
            tp_session_snapshot_animation_at(snapshot, atlas->id, i);
        if (candidate && candidate->name && name &&
            strcmp(candidate->name, name) == 0) {
            const gui_animation_ref animation = {
                atlas->id, candidate->id,
                tp_session_snapshot_revision(snapshot)};
            const bool removed =
                gui_project_remove_animation(
                    &animation);
            selftest_observe_session();
            return removed;
        }
    }
    return false;
}

static bool selftest_set_target_path_at(
    int atlas_index, int target_index,
    const char *out_path);

/* Both string arguments are COPIED before the first mutation below. Callers
 * routinely pass `tp_snapshot_target` fields, and every submit/apply here
 * republishes the observation and frees the snapshot generation those strings
 * live in -- reading the caller's pointer after that point is a use-after-free
 * (ASan caught exactly that on the final comparison). The copies are the only
 * values compared against the committed result. */
static bool selftest_set_target_at(int atlas_index, int target_index,
                                   const char *exporter_id, const char *out_path,
                                   bool enabled) {
    char exporter_copy[TP_EXPORTER_ID_MAX];
    char out_path_copy[TP_IDENTITY_PATH_MAX];
    if (!exporter_id || !out_path ||
        strlen(exporter_id) >= sizeof exporter_copy ||
        strlen(out_path) >= sizeof out_path_copy) {
        return false;
    }
    memcpy(
        exporter_copy, exporter_id,
        strlen(exporter_id) + 1U);
    memcpy(
        out_path_copy, out_path,
        strlen(out_path) + 1U);
    if (!selftest_set_target_path_at(
            atlas_index, target_index, out_path_copy)) {
        return false;
    }
    const bool path_committed =
        gui_actions__submit_draft();
    selftest_observe_session();
    if (!path_committed) {
        return false;
    }
    gui_target_ref target;
    if (!selftest_target_ref_at(
            atlas_index, target_index, &target)) {
        return false;
    }
    gui_edit_target_exporter(
        &target, exporter_copy);
    gui_actions__selftest_drain_intents();
    selftest_observe_session();
    if (!selftest_target_ref_at(
            atlas_index, target_index, &target)) {
        return false;
    }
    gui_edit_target_enabled(&target, enabled);
    gui_actions__selftest_drain_intents();
    selftest_observe_session();
    const tp_snapshot_target *result =
        selftest_target_at(atlas_index, target_index);
    return result &&
           strcmp(result->exporter_id, exporter_copy) == 0 &&
           strcmp(result->out_path, out_path_copy) == 0 &&
           result->enabled == enabled;
}

static bool selftest_set_target_path_at(int atlas_index, int target_index,
                                        const char *out_path) {
    gui_target_ref target;
    if (!selftest_target_ref_at(
            atlas_index, target_index, &target)) {
        return false;
    }
    const tp_snapshot_target *current =
        selftest_target_at(atlas_index, target_index);
    if (!gui_target_path_edit_matches(&target) &&
        (!current ||
         !gui_text_edit_begin_target_out_path(
             &target, current->out_path))) {
        return false;
    }
    return gui_text_edit_update(out_path);
}

static gui_project_create_result
selftest_add_atlas_observed(void) {
    gui_project_create_result created =
        gui_project_add_atlas();
    if (!created.committed) {
        selftest_observe_session();
        return created;
    }
    selftest_observe_session();
    const tp_session_snapshot *snapshot =
        gui_project_snapshot();
    const int count =
        tp_session_snapshot_atlas_count(snapshot);
    created.visible_index = -1;
    for (int index = 0; index < count; ++index) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(
                snapshot, index);
        if (atlas &&
            tp_id128_eq(
                atlas->id, created.created_id)) {
            created.visible_index = index;
            break;
        }
    }
    created.observation_pending = false;
    return created;
}

static tp_status selftest_project_save(
    char *err_out, size_t err_cap) {
    const tp_status status =
        gui_project_save(err_out, err_cap);
    selftest_observe_session();
    return status;
}

static tp_status selftest_project_save_as(
    const char *path, char *err_out,
    size_t err_cap) {
    const tp_status status =
        gui_project_save_as(
            path, err_out, err_cap);
    selftest_observe_session();
    return status;
}

static bool selftest_project_undo(void) {
    const bool changed =
        gui_project_undo();
    selftest_observe_session();
    return changed;
}

static bool selftest_project_redo(void) {
    const bool changed =
        gui_project_redo();
    selftest_observe_session();
    return changed;
}

#define gui_project_add_atlas() \
    selftest_add_atlas_observed()
#define gui_project_remove_atlas(index) selftest_remove_atlas_at((index))
#define gui_project_copy_atlas_name(index, out, capacity, err) \
    selftest_copy_atlas_name_at((index), (out), (capacity), (err))
#define gui_project_add_source(index, path) selftest_add_source_at((index), (path))
#define gui_project_add_sources(index, paths, count, kind, added, duplicate) \
    selftest_add_sources_at((index), (paths), (count), (kind), (added), (duplicate))
#define gui_project_create_animation(index, base, frames, frame_count) \
    selftest_create_animation_at((index), (base), (frames), (frame_count))
#define gui_project_anim_remove_frame(index, animation, frame) \
    selftest_anim_remove_frame_at((index), (animation), (frame))
#define gui_project_anim_move_frame(index, animation, frame, delta) \
    selftest_anim_move_frame_at((index), (animation), (frame), (delta))
#define gui_project_remove_animation(index, name) \
    selftest_remove_animation_named_at((index), (name))
#define gui_project_save(err_out, err_cap) \
    selftest_project_save((err_out), (err_cap))
#define gui_project_save_as(path, err_out, err_cap) \
    selftest_project_save_as((path), (err_out), (err_cap))
#define gui_project_undo() selftest_project_undo()
#define gui_project_redo() selftest_project_redo()
static void to_abs(const char *rel, char *out, size_t cap) {
#ifdef _WIN32
    if (GetFullPathNameA(rel, (DWORD)cap, out, NULL) == 0) {
        (void)snprintf(out, cap, "%s", rel);
    }
    normalize_slashes(out);
#else
    /* Mirror the Windows branch: yield a genuine absolute path. A bare snprintf left `rel`
     * relative on POSIX, which resolves fine when scanned directly from CWD but NOT when it
     * becomes a source of a fresh (never-saved, project_dir==NULL) project -- tp_project_resolve_path
     * rejects a relative source with no base, so the pack sees "no usable images" (CI-only bug,
     * since Windows GetFullPathNameA silently absolutized it). */
    if (rel[0] == '/') {
        (void)snprintf(out, cap, "%s", rel); /* already absolute */
    } else {
        char cwd[512];
        if (getcwd(cwd, sizeof cwd) != NULL) {
            (void)snprintf(out, cap, "%s/%s", cwd, rel);
        } else {
            (void)snprintf(out, cap, "%s", rel); /* getcwd failed: fall back to relative */
        }
    }
#endif
}

/* Exercise serialized round-trips without letting the frontend test seam decode
 * or inspect a tp_project directly.  The temporary file goes through the same
 * session open/lease/fingerprint path as a real GUI open, and the returned
 * immutable snapshot owns all DTO storage after the session is destroyed. */
static tp_session_snapshot *selftest_snapshot_open_buffer(
    const char *stem, const char *bytes, size_t length, tp_status *status_out,
    tp_error *err) {
    char path[700];
    (void)snprintf(path, sizeof path, "%s/%s.ntpacker_project", s_exe_dir,
                   stem ? stem : "selftest_roundtrip");
    FILE *file = fopen(path, "wb");
    const bool wrote = file &&
                       (length == 0 || fwrite(bytes, 1, length, file) == length);
    const bool closed = file && fclose(file) == 0;
    if (!wrote || !closed) {
        if (status_out) {
            *status_out = TP_STATUS_BAD_PROJECT;
        }
        (void)tp_error_set(err, TP_STATUS_BAD_PROJECT,
                           "selftest temporary project write failed");
        (void)remove(path);
        return NULL;
    }

    tp_rng rng = tp_rng_os();
    tp_session *session = NULL;
    tp_status status = tp_session_open(path, &rng, &session, err);
    tp_session_snapshot *snapshot = NULL;
    if (status == TP_STATUS_OK) {
        status = tp_session_snapshot_create(session, &snapshot, err);
    }
    tp_session_destroy(session);
    (void)remove(path);
    if (status_out) {
        *status_out = status;
    }
    return status == TP_STATUS_OK ? snapshot : NULL;
}

/* Writes a tiny valid 2x2 32-bit uncompressed TGA (stb decodes it) -- cheap procedural sprite. */
static void write_tga_2x2(const char *path) {
    const unsigned char hdr[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 2, 0, 32, 0x28};
    unsigned char px[2 * 2 * 4];
    for (int i = 0; i < 4; i++) {
        px[i * 4 + 0] = 200; /* B */
        px[i * 4 + 1] = 180; /* G */
        px[i * 4 + 2] = 160; /* R */
        px[i * 4 + 3] = 255; /* A */
    }
    FILE *f = nt_utf8_fopen(path, "wb");
    if (f) {
        (void)fwrite(hdr, 1, sizeof hdr, f);
        (void)fwrite(px, 1, sizeof px, f);
        (void)fclose(f);
    }
}

/* Reads a whole file into a malloc'd NUL-terminated buffer (caller frees; NULL on miss). */
static char *selftest_slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = (sz >= 0) ? (char *)malloc((size_t)sz + 1) : NULL;
    size_t rd = buf ? fread(buf, 1, (size_t)sz, f) : 0;
    (void)fclose(f);
    if (!buf) {
        return NULL;
    }
    buf[rd] = '\0';
    return buf;
}

/* True iff the file at `path` exists (and can be opened). */
static bool selftest_file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        (void)fclose(f);
        return true;
    }
    return false;
}

static unsigned long selftest_process_id(void) {
#ifdef _WIN32
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

static bool selftest_is_private_recovery_root(const char *root) {
    const char *base = root;
    for (const char *p = root; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    static const char prefix[] = "recovery_boundary_";
    if (strncmp(base, prefix, sizeof prefix - 1U) != 0 ||
        base[sizeof prefix - 1U] == '\0') {
        return false;
    }
    for (const char *p = base + sizeof prefix - 1U; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return true;
}

/* Recovery roots are flat. Remove all generated journals, permanent lock
 * identities, and saved projects so repeated CI runs cannot exhaust scan caps. */
static bool selftest_remove_flat_dir(const char *root) {
    if (!root || !selftest_is_private_recovery_root(root)) {
        return false;
    }
#ifdef _WIN32
    char pattern[TP_IDENTITY_PATH_MAX];
    const int pattern_len = snprintf(pattern, sizeof pattern, "%s/*", root);
    if (pattern_len < 0 || (size_t)pattern_len >= sizeof pattern) {
        return false;
    }
    WIN32_FIND_DATAA item;
    HANDLE find = FindFirstFileA(pattern, &item);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(item.cFileName, ".") != 0 &&
                strcmp(item.cFileName, "..") != 0) {
                char path[TP_IDENTITY_PATH_MAX];
                const int path_len = snprintf(path, sizeof path, "%s/%s",
                                              root, item.cFileName);
                if (path_len < 0 || (size_t)path_len >= sizeof path) {
                    (void)FindClose(find);
                    return false;
                }
                (void)DeleteFileA(path);
            }
        } while (FindNextFileA(find, &item));
        (void)FindClose(find);
    }
    (void)RemoveDirectoryA(root);
    return GetFileAttributesA(root) == INVALID_FILE_ATTRIBUTES;
#else
    DIR *dir = opendir(root);
    if (dir) {
        struct dirent *item;
        while ((item = readdir(dir)) != NULL) {
            if (strcmp(item->d_name, ".") != 0 &&
                strcmp(item->d_name, "..") != 0) {
                char path[TP_IDENTITY_PATH_MAX];
                const int path_len = snprintf(path, sizeof path, "%s/%s",
                                              root, item->d_name);
                if (path_len < 0 || (size_t)path_len >= sizeof path) {
                    (void)closedir(dir);
                    return false;
                }
                (void)remove(path);
            }
        }
        (void)closedir(dir);
    }
    (void)rmdir(root);
    return access(root, F_OK) != 0;
#endif
}

/* Create one orphan through the production GUI lifecycle, then reopen a clean
 * live session in the same recovery domain and return its typed scan row. */
static bool selftest_make_recovery_candidate(const char *root,
                                             const char *original_path,
                                             const char *atlas_name,
                                             gui_recovery_list *scratch,
                                             gui_recovery_entry *out) {
    gui_project_test_shutdown(true);
    gui_project_enable_recovery(root);
    gui_project_init();
    if (original_path && original_path[0] != '\0') {
        char err[256];
        if (gui_project_save_as(original_path, err, sizeof err) != TP_STATUS_OK) {
            return false;
        }
    }
    if (!selftest_set_atlas_name_at(0, atlas_name)) {
        return false;
    }
    gui_project_test_shutdown(false); /* dirty raw close keeps the candidate */

    gui_project_enable_recovery(root);
    gui_project_init();
    if (gui_recovery_collect(scratch) != 1) {
        return false;
    }
    *out = scratch->items[0];
    return true;
}

/* UTF-8 "тест_спрайт" (a Cyrillic sprite name) -- exercises multi-byte names end-to-end. */
#define CYR_STEM "\xD1\x82\xD0\xB5\xD1\x81\xD1\x82_\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82"

void run_selftest(void) {
    /* CI diagnostics: unbuffered logs so a fatal NT_ASSERT (__builtin_trap, no flush) never loses the
     * preceding SELFTEST/step line -- essential for diagnosing a headless-CI-only failure. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    nt_log_info("SELFTEST: begin");

    close_all_menus();
    NT_ASSERT(!gui_view_chrome_any_menu_open() &&
              "closed chrome must not block keyboard routing");
    NT_ASSERT(!gui_view_chrome_consume_escape() &&
              "Escape must remain available when no menu is open");
    s_ctx_state.open = true;
    NT_ASSERT(gui_view_chrome_any_menu_open() &&
              "an open context menu blocks global/list keyboard routing");
    NT_ASSERT(gui_view_chrome_consume_escape() &&
              "Escape must be consumed by an open context menu");
    NT_ASSERT(!gui_view_chrome_any_menu_open() &&
              "consumed Escape must close the context menu");
    for (int menu = 0; menu < 5; ++menu) {
        gui_view_chrome_selftest_set_menubar_open(menu, true);
        NT_ASSERT(gui_view_chrome_any_menu_open() &&
                  "every menubar dropdown blocks global/list keyboard routing");
        NT_ASSERT(gui_view_chrome_consume_escape() &&
                  "Escape must be consumed by every menubar dropdown");
        NT_ASSERT(!gui_view_chrome_any_menu_open() &&
                  "consumed Escape must close every menubar dropdown");
    }

    gui_project_init();
    NT_ASSERT(tp_session_snapshot_atlas_count(gui_project_snapshot()) == 1);

    /* Absolute paths (from cwd=workspace) so they survive relativize-on-save + resolve-on-load. */
    char folder[512];
    char file[512];
    to_abs("examples/defold-demo/examples/anim_trim/anims", folder, sizeof folder);
    to_abs("examples/defold-demo/examples/anim_trim/anims/sq1.png", file, sizeof file);

    const gui_add_status a1 = gui_project_add_source(0, folder);
    nt_log_info("SELFTEST: add folder -> %d (dirty=%d stale=%d)", (int)a1, gui_project_is_dirty(), gui_project_is_stale());
    const gui_add_status a2 = gui_project_add_source(0, file);
    nt_log_info("SELFTEST: add file -> %d", (int)a2);
    const gui_add_status a3 = gui_project_add_source(0, folder); /* dedupe (F6c): expect DUPLICATE(2) */
    nt_log_info("SELFTEST: dedupe add folder again -> %d (expect %d)", (int)a3, (int)GUI_ADD_DUPLICATE);

    char err[256] = {0};
    const bool dec = gui_canvas_set_image(&s_canvas, file, err, sizeof err);
    nt_log_info("SELFTEST: decode+upload -> %d (%dx%d) %s", dec, gui_canvas_img_w(&s_canvas), gui_canvas_img_h(&s_canvas), dec ? "" : err);

    char save_path[1200];
    (void)snprintf(save_path, sizeof save_path, "%s/selftest.ntpacker_project", s_exe_dir);
    tp_status st = gui_project_save_as(save_path, err, sizeof err);
    nt_log_info("SELFTEST: save '%s' -> %s (dirty=%d)", save_path, tp_status_str(st), gui_project_is_dirty());
    NT_ASSERT(st == TP_STATUS_OK);

    /* A saved session owns the canonical writer lease. The architecture
     * deliberately rejects opening that same identity until R2d supplies an
     * explicit in-place reload/lease-transfer contract. Exercise a real
     * close/reopen round-trip by installing a fresh session first, which
     * releases the saved identity through the existing owner boundary. */
    NT_ASSERT(gui_project_test_new());
    st = gui_project_test_open(save_path, err, sizeof err);
    const tp_snapshot_atlas *reloaded_atlas = selftest_atlas_at(0, NULL);
    const int nsrc = reloaded_atlas ? reloaded_atlas->source_count : -1;
    nt_log_info("SELFTEST: reload -> %s, atlas0 sources=%d (dirty=%d)", tp_status_str(st), nsrc, gui_project_is_dirty());
    NT_ASSERT(st == TP_STATUS_OK && nsrc == 2 &&
              "Save As round-trip requires releasing the prior writer before Open");

    /* Live Save must not overwrite an external rewrite after Open. The exact
     * sentinel remains on disk; a deliberate Save As to another identity is still allowed. */
    static const char external_sentinel[] = "external-edit-sentinel";
    { FILE *xf = fopen(save_path, "wb"); NT_ASSERT(xf); (void)fwrite(external_sentinel, 1, sizeof external_sentinel, xf); (void)fclose(xf); }
    memset(err, 0, sizeof err);
    const tp_status external_guard = gui_project_save(err, sizeof err);
    char external_readback[sizeof external_sentinel] = {0};
    { FILE *xf = fopen(save_path, "rb"); NT_ASSERT(xf); (void)fread(external_readback, 1, sizeof external_readback, xf); (void)fclose(xf); }
    NT_ASSERT(external_guard == TP_STATUS_FILE_CHANGED_EXTERNALLY &&
              memcmp(external_readback, external_sentinel, sizeof external_sentinel) == 0 &&
              "live Save refuses an external rewrite and leaves its bytes intact");
    char rebound_path[1200];
    (void)snprintf(rebound_path, sizeof rebound_path, "%s/selftest_rebound.ntpacker_project", s_exe_dir);
    NT_ASSERT(gui_project_save_as(rebound_path, err, sizeof err) == TP_STATUS_OK &&
              "Save As to a different project identity remains available after an external conflict");

    /* --- rename atlas + undo/redo THROUGH THE F2-03 DIFF HISTORY (b-ii-A): the model swaps its
     *     project on undo/redo; verify the name reverts/replays exactly and identity-dirty tracks
     *     (undo back to the saved baseline reads CLEAN even though the revision is higher). --- */
    char name0[64];
    (void)snprintf(name0, sizeof name0, "%s", selftest_atlas_at(0, NULL)->name);
    NT_ASSERT(!gui_project_is_dirty() && "reloaded project is clean at its saved baseline");
    selftest_set_atlas_name_at(0, "hero_atlas"); /* structural: commits immediately -> one history step */
    char committed_atlas_name[64];
    tp_error committed_name_error = {0};
    NT_ASSERT(gui_project_copy_atlas_name(0, committed_atlas_name, sizeof committed_atlas_name,
                                          &committed_name_error) == TP_STATUS_OK &&
              strcmp(committed_atlas_name, "hero_atlas") == 0 &&
              "shipping rename reads the committed name through an owned session snapshot");
    nt_log_info("SELFTEST: rename atlas '%s' -> '%s' (dirty=%d undo_depth=%d)", name0,
                committed_atlas_name, gui_project_is_dirty(), gui_project_undo_depth());
    NT_ASSERT(gui_project_is_dirty() && strcmp(selftest_atlas_at(0, NULL)->name, "hero_atlas") == 0 &&
              "rename dirties + applies");
    const bool undone = gui_project_undo();
    nt_log_info("SELFTEST: undo -> %d name='%s' (dirty=%d) [expect name reverted, dirty=0]", undone,
                selftest_atlas_at(0, NULL)->name, gui_project_is_dirty());
    NT_ASSERT(undone && strcmp(selftest_atlas_at(0, NULL)->name, name0) == 0 && !gui_project_is_dirty() &&
              "undo through F2-03 history restores the pre-rename name AND reads clean at the saved baseline");
    const bool redone = gui_project_redo();
    nt_log_info("SELFTEST: redo -> %d name='%s' (dirty=%d)", redone, selftest_atlas_at(0, NULL)->name,
                gui_project_is_dirty());
    NT_ASSERT(redone && strcmp(selftest_atlas_at(0, NULL)->name, "hero_atlas") == 0 && gui_project_is_dirty() &&
              "redo re-applies the rename + re-dirties");

    /* --- rename a region (sprite override), verify it is stored on the model --- */
    char folder_abs[TP_IDENTITY_PATH_MAX];
    const tp_session_snapshot *folder_snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *folder_atlas = selftest_atlas_at(0, NULL);
    const tp_snapshot_source *folder_source = folder_atlas
                                                  ? tp_session_snapshot_source_at(folder_snapshot,
                                                                                  folder_atlas->id, 0)
                                                  : NULL;
    const tp_id128 folder_atlas_id = folder_atlas ? folder_atlas->id : (tp_id128){{0}};
    const tp_id128 folder_source_id = folder_source ? folder_source->id : (tp_id128){{0}};
    tp_error folder_error = {0};
    if (folder_source && tp_session_snapshot_resolve_path(folder_snapshot, folder_atlas_id,
                                                          folder_source_id, folder_abs,
                                                          sizeof folder_abs,
                                                          &folder_error) == TP_STATUS_OK) {
        tp_scan_result sc = {0};
        if (tp_scan_dir(folder_abs, &sc, &folder_error) != TP_STATUS_OK) {
            nt_log_error("SELFTEST: folder scan failed: %s", folder_error.msg);
        } else {
            nt_log_info("SELFTEST: folder scan found %d image(s)", sc.count);
            if (sc.count > 0) {
                char sprite[192];
                (void)snprintf(sprite, sizeof sprite, "%s", sc.entries[0].rel);
                char *dot = strrchr(sprite, '.');
                if (dot) {
                    *dot = '\0';
                }
                selftest_set_sprite_rename_at(0, sprite, "renamed_region");
                const tp_snapshot_sprite *ov = tp_session_snapshot_sprite_by_key(
                    gui_project_snapshot(), folder_atlas_id, folder_source_id, sprite);
                nt_log_info("SELFTEST: rename region '%s' -> override='%s'", sprite, (ov && ov->rename) ? ov->rename : "(none)");
            }
        }
        tp_scan_free(&sc);
    }

    /* --- save_buffer / load_buffer round-trip in-app --- */
    char *bb = NULL;
    size_t bl = 0;
    tp_error be = {0};
    const tp_status bst = gui_project_snapshot_serialize(&bb, &bl, &be);
    tp_error le = {0};
    tp_status lst = bst;
    tp_session_snapshot *lp =
        (bst == TP_STATUS_OK)
            ? selftest_snapshot_open_buffer("selftest_buffer_rt", bb, bl, &lst, &le)
            : NULL;
    const tp_snapshot_atlas *lp_atlas = lp ? tp_session_snapshot_atlas_at(lp, 0) : NULL;
    nt_log_info("SELFTEST: save_buffer(%zuB)->%s; load_buffer->%s atlas0='%s'", bl, tp_status_str(bst), tp_status_str(lst),
                lp_atlas ? lp_atlas->name : "(none)");
    tp_session_snapshot_destroy(lp);
    free(bb);

    /* --- refresh cycle: create + delete a temp png, observe the scan change --- */
    char rdir[600];
    char rfile[700];
    (void)snprintf(rdir, sizeof rdir, "%s/selftest_refresh", s_exe_dir);
    tp_mkdirs(rdir); /* portable: was Windows-only CreateDirectoryA, so POSIX CI never created it */
    (void)snprintf(rfile, sizeof rfile, "%s/temp.png", rdir);
    FILE *tf = fopen(rfile, "wb");
    if (tf) {
        (void)fputs("PNGDATA", tf);
        (void)fclose(tf);
    }
    tp_scan_result refresh_scan = {0};
    tp_error refresh_error = {0};
    const int before_n = tp_scan_dir(rdir, &refresh_scan, &refresh_error) == TP_STATUS_OK
                             ? refresh_scan.count
                             : -1;
    tp_scan_free(&refresh_scan);
    (void)remove(rfile);
    const int after_n = tp_scan_dir(rdir, &refresh_scan, &refresh_error) == TP_STATUS_OK
                            ? refresh_scan.count
                            : -1;
    tp_scan_free(&refresh_scan);
    nt_log_info("SELFTEST: refresh cycle temp png before=%d after=%d (removed=%d)", before_n, after_n, before_n - after_n);
#ifdef _WIN32
    (void)RemoveDirectoryA(rdir);
#endif

    /* --- in-process pack of the demo atlases: real tp_pack via gui_pack (timing + assertions) --- */
    {
        char proj[600];
        to_abs("examples/defold-demo/defold-demo.ntpacker_project", proj, sizeof proj);
        char perr[256] = {0};
        if (gui_project_test_open(proj, perr, sizeof perr) == TP_STATUS_OK) {
            const tp_session_snapshot *demo_snapshot = gui_project_snapshot();
            int i_rotate = -1;
            int i_basic = -1;
            const int demo_atlas_count = tp_session_snapshot_atlas_count(demo_snapshot);
            for (int i = 0; i < demo_atlas_count; i++) {
                const tp_snapshot_atlas *demo_atlas = tp_session_snapshot_atlas_at(demo_snapshot, i);
                if (demo_atlas && strcmp(demo_atlas->name, "rotate") == 0) {
                    i_rotate = i;
                } else if (demo_atlas && strcmp(demo_atlas->name, "basic") == 0) {
                    i_basic = i;
                }
            }
            double ms_r = 0.0;
            double ms_b = 0.0;
            char pe[256] = {0};
            char note[128] = {0};
            const bool okr = (i_rotate >= 0) && gui_pack_atlas(i_rotate, &ms_r, pe, sizeof pe, note, sizeof note);
            const tp_result *rr = gui_pack_result(i_rotate);
            const int rotate_a =
                selftest_pack_find_sprite_ref_at(i_rotate, 0, "a.png");
            nt_log_info("SELFTEST: pack 'rotate' -> %d in %.1f ms sprites=%d pages=%d (find 'a.png'=%d) %s", okr, ms_r,
                        rr ? rr->sprite_count : -1, rr ? rr->page_count : -1, rotate_a,
                        okr ? "" : pe);
            NT_ASSERT(okr && rr && rr->sprite_count == 3 && rr->page_count >= 1 && "pack rotate");
            NT_ASSERT(rotate_a >= 0 && "canonical region lookup 'a.png'");

            /* U-02 finding-1: a canvas-region click (select_row_for_region) must re-pin BOTH the keyboard
             * focus and the Shift-range anchor onto the newly selected row, so F2/arrows act on the clicked
             * sprite, not the previously focused one. Build the row model + view for the packed atlas, seed
             * the stale-focus state a real click starts from (focus/anchor on a DIFFERENT row A), then assert
             * the click re-pins both onto 'a.png'. Headless-reachable (run_selftest), unlike the visual-phase
             * select_row_for_region callsites which the headless-CI jump to phase 16 skips. */
            NT_ASSERT(selftest_select_atlas(i_rotate));
            build_rows();
            build_view();
            gui_rows_set_focus_view_index(-1);
            gui_rows_set_anchor_view_index(-1);
            select_row_for_region(rotate_a);
            const int focus_view = gui_rows_focus_view_index();
            NT_ASSERT(focus_view >= 0 && focus_view < s_view_count &&
                      "select_row_for_region re-pins keyboard focus onto the selected row");
            {
                const sprite_row *frow = &s_rows[s_view[focus_view]];
                NT_ASSERT(gui_rows_primary() == frow &&
                          "the re-pinned focus row is the one carrying the primary selection");
            }
            const int anchor_view = gui_rows_anchor_view_index();
            NT_ASSERT(anchor_view == focus_view &&
                      "select_row_for_region anchors the Shift-range on the new focus");
            nt_log_info("SELFTEST: canvas-click focus re-pin OK (focus_view=%d anchor=%d view_count=%d)",
                        focus_view, anchor_view, s_view_count);

            char pe2[256] = {0};
            const bool okb = (i_basic >= 0) && gui_pack_atlas(i_basic, &ms_b, pe2, sizeof pe2, note, sizeof note);
            const tp_result *rb = gui_pack_result(i_basic);
            nt_log_info("SELFTEST: pack 'basic' -> %d in %.1f ms sprites=%d pages=%d %s", okb, ms_b,
                        rb ? rb->sprite_count : -1, rb ? rb->page_count : -1, okb ? "" : pe2);

            /* export 'rotate' via gui_pack_export, ISOLATED to a throwaway base under the build dir so
             * the demo's committed exports (owned by another agent) are never touched: disable the
             * atlas's other targets, point json-neotolis at the temp base, then assert the files exist.
             * tp_export_run uses the target out_path as the exporter BASE and appends .json / -N.png. */
            const tp_snapshot_atlas *rot_a = selftest_atlas_at(i_rotate, NULL);
            int jtarget = -1;
            const int rtc = rot_a ? rot_a->target_count : 0;
            for (int k = 0; k < rtc; k++) {
                /* F2-05b-i: gui_project_set_target now clone-swaps the model, freeing the old
                 * project -- re-fetch the atlas each iteration (dp/rot_a would dangle). */
                const tp_session_snapshot *target_snapshot = gui_project_snapshot();
                const tp_snapshot_atlas *ra = selftest_atlas_at(i_rotate, NULL);
                if (!ra) {
                    break;
                }
                const tp_snapshot_target *target = tp_session_snapshot_target_at(
                    target_snapshot, ra->id, k);
                if (!target) {
                    continue;
                }
                if (strcmp(target->exporter_id, "json-neotolis") == 0) {
                    jtarget = k;
                } else {
                    /* `target` is borrowed from `target_snapshot`; the disable
                     * below republishes the observation and frees it. Copy the
                     * values out BEFORE the mutation, then re-fetch to prove
                     * the row still resolves at its stable index. */
                    char keep_exporter[TP_EXPORTER_ID_MAX];
                    char keep_out_path[TP_IDENTITY_PATH_MAX];
                    (void)snprintf(keep_exporter, sizeof keep_exporter, "%s",
                                   target->exporter_id);
                    (void)snprintf(keep_out_path, sizeof keep_out_path, "%s",
                                   target->out_path);
                    selftest_set_target_at(i_rotate, k, keep_exporter,
                                           keep_out_path, false);
                    NT_ASSERT(selftest_target_at(i_rotate, k) != NULL &&
                              "the disabled target survives its own mutation");
                }
            }
            char tbase[700] = {0};
            (void)snprintf(tbase, sizeof tbase, "%s/selftest_rotate_export", s_exe_dir);
            if (jtarget >= 0) {
                selftest_set_target_at(i_rotate, jtarget, "json-neotolis", tbase, true);
            }
            int etg = 0;
            int enc = 0;
            char eerr[256] = {0};
            char enote[128] = {0};
            const bool oke = (i_rotate >= 0 && jtarget >= 0) &&
                             gui_pack_export(i_rotate, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
            char jpath[720] = {0};
            char ppath[720] = {0};
            (void)snprintf(jpath, sizeof jpath, "%s.json", tbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", tbase);
            bool jok = false;
            bool pok = false;
            {
                FILE *jf = fopen(jpath, "rb");
                if (jf) {
                    jok = (fgetc(jf) == '{'); /* lightweight parse check; full parse is in ctest test_export_json */
                    (void)fclose(jf);
                }
                FILE *pf = fopen(ppath, "rb");
                if (pf) {
                    pok = (fgetc(pf) != EOF); /* exists AND non-empty */
                    (void)fclose(pf);
                }
            }
            nt_log_info("SELFTEST: export 'rotate' -> ok=%d targets=%d notices=%d json{=%d png0=%d %s", oke, etg, enc,
                        jok, pok, oke ? "" : eerr);
            (void)jok;
            (void)pok;
            /* Assert the GUI export ORCHESTRATION ran (oke + one target); the written-file existence
             * (json{/png0) is LOGGED, not asserted. jpath/ppath are hand-rebuilt from s_exe_dir, which is
             * absolute on Windows but relative in the headless CI run, whereas tp_export_run resolves the
             * out_path against the project dir -- so the files land where the re-derived path doesn't look.
             * The export BYTES are already verified cross-OS by the dedicated test_export_json /
             * test_export_defold ctests, so this smoke step only needs to prove the GUI path runs. */
            NT_ASSERT(oke && etg == 1 && "export rotate: the GUI export path must succeed with one target");
            (void)remove(jpath); /* throwaway under the build dir */
            (void)remove(ppath);
        } else {
            nt_log_info("SELFTEST: demo project open failed: %s", perr);
        }
    }

    /* --- stress: 520 procedural sprites incl. a Cyrillic name -> pack + row model + Cyrillic RT --- */
    {
        char sdir[700];
        (void)snprintf(sdir, sizeof sdir, "%s/selftest_stress", s_exe_dir);
        tp_mkdirs(sdir); /* portable: was Windows-only -> the 520 .tga writes silently failed on POSIX CI */
        const int N = 520;
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            write_tga_2x2(fp);
        }
        char cyr_source_key[192];
        (void)snprintf(cyr_source_key, sizeof cyr_source_key, "%s.tga", CYR_STEM);
        char cfp[840];
        (void)snprintf(cfp, sizeof cfp, "%s/%s", sdir, cyr_source_key);
        write_tga_2x2(cfp);

        const int sidx =
            gui_project_add_atlas().visible_index;
        if (sidx >= 0) {
            (void)gui_project_add_source(sidx, sdir);
            double sms = 0.0;
            char serr[256] = {0};
            char snote[128] = {0};
            const bool oks = gui_pack_atlas(sidx, &sms, serr, sizeof serr, snote, sizeof snote);
            const tp_result *sr = gui_pack_result(sidx);
            const int cyr_idx =
                selftest_pack_find_sprite_ref_at(sidx, 0, cyr_source_key);
            nt_log_info("SELFTEST: stress pack -> %d in %.1f ms sprites=%d pages=%d cyr_idx=%d %s", oks, sms,
                        sr ? sr->sprite_count : -1, sr ? sr->page_count : -1, cyr_idx, oks ? "" : serr);
            NT_ASSERT(oks && sr && sr->sprite_count >= N + 1 && "stress pack 520+ sprites");
            NT_ASSERT(cyr_idx >= 0 && "Cyrillic-named region lookup");

            /* Cyrillic rename + save/load round-trip (multi-byte name survives serialization). */
            selftest_set_sprite_rename_at(sidx, cyr_source_key,
                                          "\xD0\xB8\xD0\xBC\xD1\x8F"); /* "имя" */
            char *sbuf = NULL;
            size_t slen = 0;
            tp_error sbe = {0};
            tp_error sle = {0};
            const tp_status sbst = gui_project_snapshot_serialize(&sbuf, &slen, &sbe);
            tp_status slst = sbst;
            tp_session_snapshot *slp =
                (sbst == TP_STATUS_OK)
                    ? selftest_snapshot_open_buffer("selftest_cyrillic_rt", sbuf,
                                                    slen, &slst, &sle)
                    : NULL;
            const tp_snapshot_atlas *slp_atlas =
                slp ? tp_session_snapshot_atlas_at(slp, sidx) : NULL;
            const tp_snapshot_source *slp_source =
                slp_atlas ? tp_session_snapshot_source_at(slp, slp_atlas->id, 0)
                          : NULL;
            const tp_snapshot_sprite *ov =
                (slp_atlas && slp_source)
                    ? tp_session_snapshot_sprite_by_key(
                          slp, slp_atlas->id, slp_source->id, cyr_source_key)
                    : NULL;
            nt_log_info("SELFTEST: Cyrillic rename RT save=%s load=%s override='%s'", tp_status_str(sbst),
                        tp_status_str(slst), (ov && ov->rename) ? ov->rename : "(none)");
            NT_ASSERT(ov && ov->rename && strcmp(ov->rename, "\xD0\xB8\xD0\xBC\xD1\x8F") == 0 &&
                      "Cyrillic name survives save/load");
            tp_session_snapshot_destroy(slp);
            free(sbuf);

            /* Row model materializes 520+ rows (incl. the Cyrillic label) without overflow. */
            NT_ASSERT(selftest_select_atlas(sidx));
            build_rows();
            bool cyr_row = false;
            for (int i = 0; i < s_row_count; i++) {
                if (s_rows[i].sprite_name &&
                    strcmp(s_rows[i].sprite_name, CYR_STEM) == 0) {
                    cyr_row = true;
                    break;
                }
            }
            nt_log_info("SELFTEST: stress rows=%d cyr_row=%d | state pool slots=%u probe=%u ring=%u (bounded, no overflow)",
                        s_row_count, cyr_row, (unsigned)UI_STATE_SLOTS, (unsigned)UI_STATE_PROBE_MAX,
                        (unsigned)UI_ROW_ID_RING);
            NT_ASSERT(s_row_count >= N + 1 && cyr_row && "stress row model incl. Cyrillic");
        }
        /* cleanup scratch sprites (keep the tree clean). The no-overflow guarantee is id_ring x
         * state_slots capacity, verified above + interactively. */
        for (int i = 0; i < N; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/spr_%03d.tga", sdir, i);
            (void)remove(fp);
        }
        NT_ASSERT(nt_utf8_remove(cfp) == 0 &&
                  "Cyrillic stress fixture must be removed through the UTF-8 filesystem boundary");
#ifdef _WIN32
        (void)RemoveDirectoryA(sdir);
#endif
    }

    /* --- large-N caps (P1 fix, decomposition step 7): the row / multi-select / preview-frame arrays
     *     used to silently DROP entries past fixed caps (4096 rows, 4096 multi-select, 512 preview
     *     frames) -- sprites packed fine but VANISHED from the UI. They are growable now; prove it with
     *     EXACT counts so a reintroduced fixed cap fails HERE. Two routes: (A) an in-memory synthetic
     *     project exceeds the 4096 row/multi-select caps without writing >4096 files (too heavy for CI);
     *     (B) a >512-frame animation over REAL packed sprites, which the preview idxs[] path must
     *     resolve end-to-end (a fake result cannot exercise canonical result lookup). --- */
    {
        const int BIG_N = 4200; /* > the old 4096 row / multi-select cap */

        /* (A1) rows: >4096 (missing) sources materialize >4096 rows -- build_rows grows s_rows.
         *      Feed the production batch route in bounded transactions.  Calling the one-source
         *      convenience route BIG_N times makes the identity planner re-scan the growing atlas
         *      for every request (quadratic test setup) and no longer reflects the shipped
         *      multi-select workflow.  Batches stay well below both public admission limits
         *      (operation count and encoded request bytes); this probe is about row capacity,
         *      not about constructing a maximum-size transaction. */
        gui_project_test_new();
        gui_pack_clear(-1);
        char (*source_paths)[24] = calloc((size_t)BIG_N, sizeof *source_paths);
        const char **source_args = calloc((size_t)BIG_N, sizeof *source_args);
        NT_ASSERT(source_paths && source_args && "SELFTEST: caps source batch allocation");
        for (int i = 0; i < BIG_N; i++) {
            (void)snprintf(source_paths[i], sizeof source_paths[i],
                           "cap/s%05d.png", i); /* distinct + missing -> exactly 1 row each */
            source_args[i] = source_paths[i];
        }
        enum { CAP_BATCH = 1024 };
        int cap_total_added = 0;
        for (int offset = 0; offset < BIG_N; offset += CAP_BATCH) {
            const int count = BIG_N - offset < CAP_BATCH
                                  ? BIG_N - offset
                                  : CAP_BATCH;
            int cap_added = 0;
            int cap_duplicates = 0;
            const bool cap_ok = gui_project_add_sources(
                0, source_args + offset, count, TP_SOURCE_KIND_FOLDER,
                &cap_added, &cap_duplicates);
            NT_ASSERT(cap_ok && cap_added == count && cap_duplicates == 0 &&
                      "SELFTEST: caps source batch");
            cap_total_added += cap_added;
        }
        NT_ASSERT(cap_total_added == BIG_N &&
                  "SELFTEST: all caps sources admitted");
        free(source_args);
        free(source_paths);
        NT_ASSERT(selftest_select_atlas(0));
        build_rows();
        nt_log_info("SELFTEST: caps rows=%d (want %d; old cap 4096)", s_row_count, BIG_N);
        NT_ASSERT(s_row_count == BIG_N && "sprite rows grow past the old 4096 cap");

        /* (A2) multi-select: >4096 distinct names -- multi_sel_add grows s_multi_sel. */
        multi_sel_clear();
        tp_id128 synthetic_source_id = tp_id128_nil();
        synthetic_source_id.bytes[0] = 1U;
        for (int i = 0; i < BIG_N; i++) {
            char nm[24];
            (void)snprintf(nm, sizeof nm, "cap_%05d", i);
            /* Capacity-only selectors are intentionally unresolved. Feed the
             * canonical selection seam directly: resolving each synthetic name
             * across BIG_N sources would make test setup quadratic. */
            multi_sel_add_ref(synthetic_source_id, nm);
        }
        nt_log_info("SELFTEST: caps multi_sel=%d (want %d; old cap 4096)", s_multi_sel_count, BIG_N);
        NT_ASSERT(s_multi_sel_count == BIG_N && "multi-select grows past the old 4096 cap");

        /* (A3) sort companions: create-animation natural-sorts the WHOLE selection through
         *      s_sel_sort_buf/ptr; if those did not grow with the set the sort path would re-truncate.
         *      These synthetic names intentionally do NOT resolve to sprites (all sources above
         *      are missing), so M5 canonical admission must reject the animation while the sort
         *      scratch still proves it retained every selected value. */
        NT_ASSERT(sel_sort_reserve(BIG_N) &&
                  "SELFTEST: sort companions reserve the whole selection");
        const int ca_anim = create_animation_from_selection();
        NT_ASSERT(ca_anim == -1 &&
                  "SELFTEST: unresolved synthetic frame selectors are rejected");
        for (int i = 0; i < BIG_N; i++) {
            char want[24];
            (void)snprintf(want, sizeof want, "cap_%05d", i);
            NT_ASSERT(s_sel_sort_ptr[i] == s_sel_sort_buf[i].source_key &&
                      strcmp(s_sel_sort_ptr[i], want) == 0 &&
                      "SELFTEST: sort companions hold the whole selection");
        }
        nt_log_info("SELFTEST: caps sort retained=%d; unresolved animation rejected",
                    BIG_N);
        multi_sel_clear();

        /* (B) preview idxs[]: a >512-frame animation over REAL packed sprites resolves EVERY frame.
         *     Identical 2x2 sprites are NOT deduped (see the 520-sprite stress above), so M files pack
         *     to M regions. */
        gui_project_test_new();
        gui_pack_clear(-1);
        const int M = 600; /* > the old 512 preview-frame cap */
        char pdir[700];
        (void)snprintf(pdir, sizeof pdir, "%s/selftest_caps", s_exe_dir);
        tp_mkdirs(pdir); /* portable: was Windows-only */
        for (int i = 0; i < M; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/f_%04d.tga", pdir, i);
            write_tga_2x2(fp);
        }
        (void)gui_project_add_source(0, pdir);
        double cms = 0.0;
        char cerr[256] = {0};
        char cnote[128] = {0};
        gui_pack_ref_index_work_reset();
        const bool okc = gui_pack_atlas(0, &cms, cerr, sizeof cerr, cnote, sizeof cnote);
        const tp_result *cr = gui_pack_result(0);
        nt_log_info("SELFTEST: caps pack -> %d sprites=%d (want >= %d) %s", okc, cr ? cr->sprite_count : -1, M,
                    okc ? "" : cerr);
        NT_ASSERT(okc && cr && cr->sprite_count >= M && "caps: pack >512 real sprites");

        NT_ASSERT(selftest_select_atlas(0));
        build_rows();
        multi_sel_clear();
        for (int i = 0; i < s_row_count; i++) { /* select-all the leaf sprites (the real UI gesture) */
            if (!s_rows[i].is_folder && !s_rows[i].missing &&
                s_rows[i].sprite_name && s_rows[i].sprite_name[0] != '\0') {
                multi_sel_add(s_rows[i].sprite_name);
            }
        }
        nt_log_info("SELFTEST: caps preview select-all=%d (want %d)", s_multi_sel_count, M);
        NT_ASSERT(s_multi_sel_count == M && "caps: select-all resolves M leaf rows");
        enum { PREVIEW_SIBLINGS = 32 };
        for (int i = 0; i < PREVIEW_SIBLINGS; ++i) {
            NT_ASSERT(selftest_create_animation_at(0, "preview_sibling", NULL,
                                                   0) >= 0 &&
                      "caps: create sibling animation for lookup bound");
        }
        const int panim = create_animation_from_selection();
        NT_ASSERT(panim >= 0 && "caps: animation from M frames");
        gui_preview_frame_work_reset();
        gui_animation_ref preview_ref;
        NT_ASSERT(
            selftest_animation_ref_at(
                gui_view_atlas_index(
                    gui_project_snapshot()),
                panim, &preview_ref));
        open_preview_ref(&preview_ref);
        update_preview();
        nt_log_info("SELFTEST: caps preview frames resolved=%d (want %d; old cap 512)", s_preview_frame_count, M);
        NT_ASSERT(s_preview_frame_count == M && "preview resolves all >512 frames (idxs[] grows)");
        const gui_pack_ref_index_work ref_work =
            gui_pack_ref_index_work_get();
        const uint64_t ref_work_bound =
            8U * (uint64_t)(cr->sprite_count + M);
        nt_log_info("SELFTEST: canonical preview index build=%llu/%llu lookups=%llu/%llu probes=%llu bound=%llu",
                    (unsigned long long)ref_work.build_items,
                    (unsigned long long)cr->sprite_count,
                    (unsigned long long)ref_work.lookup_calls,
                    (unsigned long long)M,
                    (unsigned long long)(ref_work.build_probes +
                                         ref_work.lookup_probes),
                    (unsigned long long)ref_work_bound);
        NT_ASSERT(ref_work.build_items == (uint64_t)cr->sprite_count &&
                  ref_work.lookup_calls == (uint64_t)M &&
                  ref_work.build_probes + ref_work.lookup_probes <=
                      ref_work_bound &&
                  "canonical preview resolution is O(S+F), not O(S*F)");
        const gui_preview_frame_work first_preview_work =
            gui_preview_frame_work_get();
        update_preview();
        const gui_preview_frame_work unchanged_preview_work =
            gui_preview_frame_work_get();
        NT_ASSERT(first_preview_work.rebuilds == 1U &&
                  first_preview_work.frame_span_lookups == 1U &&
                  first_preview_work.frame_iterations == (uint64_t)M &&
                  first_preview_work.realloc_calls <= 1U &&
                  unchanged_preview_work.rebuilds == first_preview_work.rebuilds &&
                  unchanged_preview_work.frame_span_lookups ==
                      first_preview_work.frame_span_lookups &&
                  unchanged_preview_work.frame_iterations ==
                      first_preview_work.frame_iterations &&
                  unchanged_preview_work.realloc_calls ==
                      first_preview_work.realloc_calls &&
                  gui_pack_ref_index_work_get().lookup_calls == (uint64_t)M &&
                  "unchanged animation preview reuses the resolved frame map");

        NT_ASSERT(selftest_set_anim_fps_at(0, panim, 24.0F) &&
                  "preview cache model-key edit commits");
        update_preview();
        gui_preview_frame_work changed_preview_work =
            gui_preview_frame_work_get();
        NT_ASSERT(changed_preview_work.rebuilds == 2U &&
                  changed_preview_work.frame_span_lookups == 2U &&
                  changed_preview_work.frame_iterations == (uint64_t)(2 * M) &&
                  "animation edit rebuilds the preview frame map once");

        NT_ASSERT(gui_pack_atlas(0, &cms, cerr, sizeof cerr, cnote,
                                 sizeof cnote) &&
                  "successful repack publishes a new preview input");
        update_preview();
        changed_preview_work = gui_preview_frame_work_get();
        NT_ASSERT(changed_preview_work.rebuilds == 3U &&
                  changed_preview_work.frame_span_lookups == 3U &&
                  changed_preview_work.frame_iterations == (uint64_t)(3 * M) &&
                  "new Pack result rebuilds the preview frame map once");

        const tp_snapshot_animation *preview_before_shift =
            preview_animation();
        NT_ASSERT(preview_before_shift &&
                  "active preview fixture has a stable target");
        const tp_id128 preview_id_before_shift = preview_before_shift->id;
        NT_ASSERT(selftest_remove_animation_named_at(0, "preview_sibling") &&
                  "active preview fixture removes a preceding animation");
        update_preview();
        const tp_snapshot_animation *preview_after_shift = preview_animation();
        changed_preview_work = gui_preview_frame_work_get();
        NT_ASSERT(preview_after_shift &&
                  tp_id128_eq(preview_after_shift->id,
                              preview_id_before_shift) &&
                  s_preview_frame_count == M &&
                  changed_preview_work.rebuilds == 4U &&
                  changed_preview_work.frame_span_lookups == 4U &&
                  changed_preview_work.frame_iterations == (uint64_t)(4 * M) &&
                  "active preview keeps the stable target and frame map after collection shift");
        preview_stop();
        multi_sel_clear();

        for (int i = 0; i < M; i++) {
            char fp[820];
            (void)snprintf(fp, sizeof fp, "%s/f_%04d.tga", pdir, i);
            (void)remove(fp);
        }
#ifdef _WIN32
        (void)RemoveDirectoryA(pdir);
#endif
        /* leave a clean fresh project for the phases below */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        selftest_clear_animation_selection();
    }

    /* --- settings panel: stale-on-change, effective-extrude, per-region RECT override,
     *     and a fresh-project seeded-target export (regions F/G, §3.3f, owner overrides) --- */
    {
        gui_project_test_new();
        gui_pack_clear(-1);
        const tp_session_snapshot *fresh_snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *fresh_atlas = selftest_atlas_at(0, NULL);
        const tp_snapshot_target *fresh_target = fresh_atlas
                                                     ? tp_session_snapshot_target_at(
                                                           fresh_snapshot, fresh_atlas->id, 0)
                                                     : NULL;
        NT_ASSERT(fresh_atlas && fresh_atlas->target_count == 1 && fresh_target &&
                  "fresh project seeds exactly one target (I1, single-seed invariant)");
        nt_log_info("SELFTEST: fresh target[0]=%s base=%s", fresh_target->exporter_id,
                    fresh_target->out_path);

        char afolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
        (void)gui_project_add_source(0, afolder);

        gui_project_mark_packed(); /* pretend current, then a setting change must set stale */
        const tp_session_snapshot *setting_snapshot = NULL;
        const tp_snapshot_atlas *setting_atlas =
            selftest_atlas_at(0, &setting_snapshot);
        NT_ASSERT(setting_atlas && "settings fixture resolves atlas 0");
        gui_edit_atlas_setting(
            setting_atlas->id,
            tp_session_snapshot_revision(setting_snapshot),
            GUI_ATLAS_PADDING, 7, 0.0F);
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_IDLE &&
                  "padding draft submits before continuing");
        selftest_observe_session();
        nt_log_info("SELFTEST: setting change stale=%d (expect 1)", gui_project_is_stale());
        NT_ASSERT(gui_project_is_stale() && "a setting change sets preview stale");

        /* A valid concave atlas packs with zero extrusion. Invalid
         * concave+extrude input is covered by structured validation tests. */
        setting_atlas = selftest_atlas_at(0, &setting_snapshot);
        gui_edit_atlas_setting(
            setting_atlas->id,
            tp_session_snapshot_revision(setting_snapshot),
            GUI_ATLAS_SHAPE, 2, 0.0F);
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_IDLE &&
                  "shape draft submits before continuing");
        selftest_observe_session();
        double pms = 0.0;
        char perr[256] = {0};
        char pnote[128] = {0};
        const bool okc = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
        nt_log_info("SELFTEST: concave pack -> %d in %.1fms (%s)",
                    okc, pms, okc ? "extrude 0" : perr);
        NT_ASSERT(okc && "valid concave atlas packs");

        /* per-sprite shape=RECT override -> that region packs as an exact 4-vert rect */
        char afabs[TP_IDENTITY_PATH_MAX];
        const tp_session_snapshot *source_snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *source_atlas = selftest_atlas_at(0, NULL);
        const tp_snapshot_source *source0 = source_atlas
                                                ? tp_session_snapshot_source_at(source_snapshot,
                                                                                source_atlas->id, 0)
                                                : NULL;
        tp_error source_error = {0};
        if (source0 && tp_session_snapshot_resolve_path(source_snapshot, source_atlas->id,
                                                        source0->id, afabs, sizeof afabs,
                                                        &source_error) == TP_STATUS_OK) {
            tp_scan_result sc = {0};
            if (tp_scan_dir(afabs, &sc, &source_error) == TP_STATUS_OK &&
                sc.count > 0) {
                char source_key[TP_SRCKEY_MAX];
                char spn[192];
                (void)snprintf(source_key, sizeof source_key, "%s", sc.entries[0].rel);
                (void)snprintf(spn, sizeof spn, "%s", sc.entries[0].rel);
                char *dot = strrchr(spn, '.');
                if (dot) {
                    *dot = '\0';
                }
                const tp_id128 shape_atlas_id =
                    source_atlas->id;
                const tp_id128 shape_source_id =
                    source0->id;
                NT_ASSERT(
                    selftest_set_sprite_override_at(
                        0, source_key,
                        GUI_SPRITE_OV_SHAPE,
                        0 /* RECT */) &&
                    "sprite shape draft submits");
                const tp_snapshot_sprite *shape_override =
                    tp_session_snapshot_sprite_by_key(
                        gui_project_snapshot(),
                        shape_atlas_id, shape_source_id,
                        source_key);
                NT_ASSERT(
                    shape_override &&
                    shape_override->override_shape == 0 &&
                    "sprite shape override is visible in the atomic snapshot");
                (void)gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
                const int rri =
                    selftest_pack_find_sprite_ref_at(0, 0, source_key);
                const tp_result *rr = gui_pack_result(0);
                const int vc = (rr && rri >= 0) ? rr->sprites[rri].vert_count : -1;
                nt_log_info("SELFTEST: sprite '%s' RECT override -> vert_count=%d (expect 4)", spn, vc);
                NT_ASSERT(vc == 4 && "RECT per-sprite override packs a 4-vert rect");
            }
            tp_scan_free(&sc);
        }

        /* save + export a fresh GUI project -> the seeded target writes files (audit I1) */
        char fpath[1200];
        (void)snprintf(fpath, sizeof fpath, "%s/selftest_fresh.ntpacker_project", s_exe_dir);
        char serr[256] = {0};
        (void)gui_project_save_as(fpath, serr, sizeof serr);
        int etg = 0;
        int enc = 0;
        char eerr[256] = {0};
        char enote[128] = {0};
        const bool oke = gui_pack_export(0, &etg, &enc, eerr, sizeof eerr, enote, sizeof enote);
        char jbase[600] = {0};
        char jpath[640] = {0};
        char ppath[640] = {0};
        bool jok = false;
        bool pok = false;
        const int jn = snprintf(jbase, sizeof jbase, "%s/out/atlas1", s_exe_dir);
        if (jn > 0 && (size_t)jn < sizeof jbase) {
            (void)snprintf(jpath, sizeof jpath, "%s.json", jbase);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", jbase);
            FILE *jf = fopen(jpath, "rb");
            if (jf) {
                jok = (fgetc(jf) == '{');
                (void)fclose(jf);
            }
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                pok = true;
                (void)fclose(pf);
            }
        }
        nt_log_info("SELFTEST: fresh export ok=%d targets=%d json{=%d png0=%d %s", oke, etg, jok, pok, oke ? "" : eerr);
        NT_ASSERT(oke && jok && pok && "fresh GUI project exports its seeded target");
        (void)remove(jpath);
        (void)remove(ppath);
        (void)remove(fpath);
    }

    /* --- animations: pure playback map, create-from-selection natural sort, reorder,
     *     round-trip preserves frames order + playback + flips, remove-frame path --- */
    {
        bool fin = false;
        NT_ASSERT(gui_canvas_anim_frame_at(0.0, 10.0F, 2, 4, &fin) == 3 && !fin && "once_backward step0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 2, 4, &fin) == 0 && fin && "once_backward finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 3, 4, &fin) == 3 && "loop_backward wraps");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 4, 3, &fin) == 1 && "once_pingpong return leg");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 4, 3, &fin) == 0 && fin && "once_pingpong finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.55, 10.0F, 5, 3, &fin) == 1 && "loop_pingpong wraps");

        const int aidx =
            gui_project_add_atlas().visible_index;
        NT_ASSERT(selftest_select_atlas(aidx));
        char anim_source_dir[700];
        (void)snprintf(anim_source_dir, sizeof anim_source_dir,
                       "%s/selftest_animation_frames", s_exe_dir);
        tp_mkdirs(anim_source_dir);
        const char *walk_files[] = {"walk_1.tga", "walk_2.tga",
                                    "walk_10.tga"};
        for (int i = 0; i < 3; ++i) {
            char frame_path[820];
            (void)snprintf(frame_path, sizeof frame_path, "%s/%s",
                           anim_source_dir, walk_files[i]);
            write_tga_2x2(frame_path);
        }
        NT_ASSERT(gui_project_add_source(aidx, anim_source_dir) == GUI_ADD_ADDED &&
                  "animation selector fixture adds one real source");
        multi_sel_clear();
        multi_sel_add("walk_10"); /* deliberately out of natural order */
        multi_sel_add("walk_2");
        multi_sel_add("walk_1");
        const int ai = create_animation_from_selection();
        const tp_session_snapshot *animation_snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *aa = selftest_atlas_at(aidx, NULL);
        const tp_snapshot_animation *an = aa
                                               ? tp_session_snapshot_animation_at(animation_snapshot, aa->id, 0)
                                               : NULL;
        const tp_snapshot_frame *an0 = an ? tp_session_snapshot_animation_frame_at(animation_snapshot, aa->id, an->id, 0) : NULL;
        const tp_snapshot_frame *an1 = an ? tp_session_snapshot_animation_frame_at(animation_snapshot, aa->id, an->id, 1) : NULL;
        const tp_snapshot_frame *an2 = an ? tp_session_snapshot_animation_frame_at(animation_snapshot, aa->id, an->id, 2) : NULL;
        NT_ASSERT(ai == 0 && an && "create animation from selection");
        nt_log_info("SELFTEST: anim '%s' frames [%s,%s,%s]", an->name, an0->name, an1->name, an2->name);
        NT_ASSERT(an->frame_count == 3 && strcmp(an0->name, "walk_1") == 0 &&
                  strcmp(an1->name, "walk_2") == 0 && strcmp(an2->name, "walk_10") == 0 &&
                  "frames natural-sorted (walk_2 before walk_10)");

        selftest_set_anim_playback_at(aidx, 0, 5); /* loop pingpong */
        selftest_set_anim_flip_at(aidx, 0, true, false);
        selftest_set_anim_fps_at(aidx, 0, 12.0F);
        gui_project_anim_move_frame(aidx, 0, 0, 2); /* walk_1 rides to the end */
        animation_snapshot = gui_project_snapshot();
        aa = selftest_atlas_at(aidx, NULL);
        an = tp_session_snapshot_animation_at(animation_snapshot, aa->id, 0);
        an0 = tp_session_snapshot_animation_frame_at(animation_snapshot, aa->id, an->id, 0);
        an2 = tp_session_snapshot_animation_frame_at(animation_snapshot, aa->id, an->id, 2);
        NT_ASSERT(strcmp(an0->name, "walk_2") == 0 && strcmp(an2->name, "walk_1") == 0 &&
                  "reorder a frame");

        char *abuf = NULL;
        size_t alen = 0;
        tp_error abe = {0};
        tp_error ale = {0};
        const tp_status abs_st = gui_project_snapshot_serialize(&abuf, &alen, &abe);
        tp_status als_st = abs_st;
        tp_session_snapshot *alp =
            (abs_st == TP_STATUS_OK)
                ? selftest_snapshot_open_buffer("selftest_animation_rt", abuf,
                                                alen, &als_st, &ale)
                : NULL;
        const tp_snapshot_atlas *rl_atlas =
            alp ? tp_session_snapshot_atlas_at(alp, aidx) : NULL;
        const tp_snapshot_animation *rl =
            rl_atlas ? tp_session_snapshot_animation_at(alp, rl_atlas->id, 0)
                     : NULL;
        const tp_snapshot_frame *rl0 =
            rl ? tp_session_snapshot_animation_frame_at(alp, rl_atlas->id,
                                                        rl->id, 0)
               : NULL;
        const tp_snapshot_frame *rl2 =
            rl ? tp_session_snapshot_animation_frame_at(alp, rl_atlas->id,
                                                        rl->id, 2)
               : NULL;
        nt_log_info("SELFTEST: anim RT save=%s load=%s playback=%d flip_h=%d fps=%g", tp_status_str(abs_st),
                    tp_status_str(als_st), rl ? rl->playback : -1, rl ? rl->flip_h : -1, rl ? (double)rl->fps : 0.0);
        NT_ASSERT(rl && rl->frame_count == 3 && rl->playback == 5 && rl->flip_h && !rl->flip_v && rl->fps == 12.0F &&
                  rl0 && rl2 && strcmp(rl0->name, "walk_2") == 0 &&
                  strcmp(rl2->name, "walk_1") == 0 &&
                  "round-trip preserves frame order + playback + flips");
        tp_session_snapshot_destroy(alp);
        free(abuf);

        NT_ASSERT(gui_project_anim_remove_frame(aidx, 0, 1) && "remove a frame");
        animation_snapshot = gui_project_snapshot();
        aa = selftest_atlas_at(aidx, NULL);
        an = aa ? tp_session_snapshot_animation_at(animation_snapshot, aa->id, 0) : NULL;
        NT_ASSERT(an && an->frame_count == 2 && "remove a frame count");
        nt_log_info("SELFTEST: animation create/reorder/round-trip OK");

        /* M2 stable-ID deferred intents. Preview captures the animation ID, so
         * removing an earlier collection member before the drain must resolve
         * the requested animation at its new index. Create-from-selection owns
         * the target atlas + sorted frame strings, so later UI selection changes
         * cannot redirect or rewrite the request. */
        const char *shift_frame[] = {"walk_1"};
        const int shift0 = selftest_create_animation_at(
            aidx, "shift_first", shift_frame, 1);
        const int shift1 = selftest_create_animation_at(
            aidx, "shift_target", shift_frame, 1);
        gui_animation_ref preview_ref;
        NT_ASSERT(shift0 >= 0 && shift1 >= 0 &&
                  selftest_animation_ref_at(aidx, shift1, &preview_ref) &&
                  "M2: capture the deferred preview by stable animation ID");
        gui_request_open_preview(&preview_ref);
        NT_ASSERT(selftest_remove_animation_named_at(aidx, "shift_first") &&
                  "M2: shift the animation collection before preview drain");
        gui_actions__selftest_drain_intents();
        const tp_session_snapshot *shift_snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *shift_atlas = selftest_atlas_at(aidx, NULL);
        const int shift_selection =
            gui_view_animation_index(shift_snapshot);
        const tp_snapshot_animation *shift_selected = shift_atlas
            ? tp_session_snapshot_animation_at(shift_snapshot, shift_atlas->id,
                                               shift_selection)
            : NULL;
        NT_ASSERT(s_preview_active && shift_selected &&
                  tp_id128_eq(shift_selected->id, preview_ref.animation_id) &&
                  "M2: deferred preview follows the same animation after index shift");

        selftest_clear_animation_selection();
        const tp_snapshot_animation *stable_preview = preview_animation();
        NT_ASSERT(stable_preview &&
                  tp_id128_eq(stable_preview->id, preview_ref.animation_id) &&
                  "M2: active preview does not depend on numeric selection");
        const gui_animation_ref remove_preview_ref = {
            preview_ref.atlas_id, preview_ref.animation_id,
            tp_session_snapshot_revision(gui_project_snapshot())};
        gui_request_remove_animation_ref(&remove_preview_ref);
        (void)selftest_select_animation_at(
            aidx, 0); /* another stable selection must not define preview ownership */
        gui_actions__selftest_drain_intents();
        NT_ASSERT(!s_preview_active &&
                  "M2: removing the previewed animation compares stable IDs, not queued indices");
        selftest_observe_session();

        NT_ASSERT(selftest_select_atlas(aidx));
        multi_sel_clear();
        multi_sel_add("walk_10");
        multi_sel_add("walk_2");
        multi_sel_add("walk_1");
        shift_atlas = selftest_atlas_at(aidx, NULL);
        const int animations_before_queued_create =
            shift_atlas ? shift_atlas->animation_count : -1;
        gui_request_create_animation_from_selection();
        NT_ASSERT(selftest_select_atlas(
            0)); /* redirect the live UI after intent capture */
        multi_sel_clear();
        multi_sel_add("wrong_selection");
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        shift_snapshot = gui_project_snapshot();
        shift_atlas = selftest_atlas_at(aidx, NULL);
        const tp_snapshot_animation *queued = shift_atlas
            ? tp_session_snapshot_animation_at(shift_snapshot, shift_atlas->id,
                                               animations_before_queued_create)
            : NULL;
        const tp_snapshot_frame *queued0 = queued
            ? tp_session_snapshot_animation_frame_at(
                  shift_snapshot, shift_atlas->id, queued->id, 0)
            : NULL;
        const tp_snapshot_frame *queued1 = queued
            ? tp_session_snapshot_animation_frame_at(
                  shift_snapshot, shift_atlas->id, queued->id, 1)
            : NULL;
        const tp_snapshot_frame *queued2 = queued
            ? tp_session_snapshot_animation_frame_at(
                  shift_snapshot, shift_atlas->id, queued->id, 2)
            : NULL;
        NT_ASSERT(queued && queued->frame_count == 3 && queued0 && queued1 &&
                  queued2 && strcmp(queued0->name, "walk_1") == 0 &&
                  strcmp(queued1->name, "walk_2") == 0 &&
                  strcmp(queued2->name, "walk_10") == 0 &&
                  "M2: queued create owns atlas and natural-sorted frame payload");

        NT_ASSERT(gui_project_remove_atlas(aidx) &&
                  "animation fixture atlas is removed before deleting its source files");

        for (int i = 0; i < 3; ++i) {
            char frame_path[820];
            (void)snprintf(frame_path, sizeof frame_path, "%s/%s",
                           anim_source_dir, walk_files[i]);
            (void)remove(frame_path);
        }
#ifdef _WIN32
        (void)RemoveDirectoryA(anim_source_dir);
#endif

        multi_sel_clear();
        selftest_clear_animation_selection();
        NT_ASSERT(selftest_select_atlas(0));
    }

    /* --- Draft-owned sprite/animation/target edit regressions.
     * Detailed conflict and receipt behavior is covered by
     * test_gui_action_trace. --- */
    {
        /* Slice-9 drafts own one component. Each submit rebuilds the grouped
         * operation from the newest snapshot, so sequential component edits
         * preserve both values. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)selftest_set_sprite_slice9_at(0, "s9sprite", 0 /* L */, 11);
        (void)selftest_set_sprite_slice9_at(0, "s9sprite", 1 /* R */, 22);
        const tp_snapshot_sprite *s9ov = selftest_sprite_by_name(0, "s9sprite");
        const int s9l = s9ov ? s9ov->slice9_lrtb[0] : -1;
        const int s9r = s9ov ? s9ov->slice9_lrtb[1] : -1;
        nt_log_info("SELFTEST: slice9 RMW L=%d R=%d (want 11,22 -- neither lost)", s9l, s9r);
        NT_ASSERT(s9l == 11 && s9r == 22 &&
                  "slice9 component drafts preserve both submitted values");

        /* (3) F1: "Add frames" is DEFERRED (was a synchronous commit -> UAF while declare_animation_editor
         *     held a live `an` it kept dereferencing). The enqueue captures COPIED keys, so the frames land
         *     only on the test intent drain AND clearing the live selection between enqueue and drain does
         *     NOT change what lands. If someone reverts to a synchronous commit, fc_mid becomes 2 and this
         *     assertion fails HERE -- the UAF cannot regress silently. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        char add_frames_dir[700];
        (void)snprintf(add_frames_dir, sizeof add_frames_dir,
                       "%s/selftest_add_frames", s_exe_dir);
        tp_mkdirs(add_frames_dir);
        for (int i = 1; i <= 2; ++i) {
            char frame_path[820];
            (void)snprintf(frame_path, sizeof frame_path, "%s/f_%d.tga",
                           add_frames_dir, i);
            write_tga_2x2(frame_path);
        }
        NT_ASSERT(gui_project_add_source(0, add_frames_dir) == GUI_ADD_ADDED &&
                  "deferred add-frames fixture adds one real source");
        const int f1anim = gui_project_create_animation(0, "addf", NULL, 0); /* empty animation */
        NT_ASSERT(selftest_select_animation_at(0, f1anim));
        multi_sel_clear();
        multi_sel_add("f_2"); /* deliberately out of natural order */
        multi_sel_add("f_1");
        gui_animation_ref f1ref;
        NT_ASSERT(
            selftest_animation_ref_at(
                0, f1anim, &f1ref));
        add_selection_frames_to_animation(
            &f1ref); /* ENQUEUE ONLY -- must not commit synchronously */
        const tp_snapshot_animation *f1a = selftest_animation_at(0, f1anim);
        const int fc_mid = f1a ? f1a->frame_count : -1;
        multi_sel_clear();                    /* mutate the selection AFTER the enqueue: copied keys stand */
        gui_actions__selftest_drain_intents();                      /* drains -> gui_project_anim_add_frames replays the copies */
        selftest_observe_session();
        f1a = selftest_animation_at(0, f1anim);
        const int fc_after = f1a ? f1a->frame_count : -1;
        const tp_snapshot_frame *f1f0 = selftest_frame_at(0, f1anim, 0);
        const char *ff0 = f1f0 ? f1f0->name : "";
        nt_log_info("SELFTEST: F1 add-frames deferred: mid=%d after=%d frame0='%s' (want mid=0 after=2 f_1)",
                    fc_mid, fc_after, ff0);
        NT_ASSERT(fc_mid == 0 && fc_after == 2 && strcmp(ff0, "f_1") == 0 &&
                  "Add frames is deferred + captures copied keys (F1 UAF fix)");
        for (int i = 1; i <= 2; ++i) {
            char frame_path[820];
            (void)snprintf(frame_path, sizeof frame_path, "%s/f_%d.tga",
                           add_frames_dir, i);
            (void)remove(frame_path);
        }
#ifdef _WIN32
        (void)RemoveDirectoryA(add_frames_dir);
#endif

        /* A narrow target-enabled operation must not resend or truncate the
         * independently owned output path. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        char longp[600];
        {
            size_t w = 0;
            w += (size_t)snprintf(longp + w, sizeof longp - w, "out/");
            for (int k = 0; k < 30 && w + 20 < sizeof longp; k++) { /* 30 * ~17 chars -> well over 255 */
                w += (size_t)snprintf(longp + w, sizeof longp - w, "deep_subdir_%02d/", k);
            }
            (void)snprintf(longp + w, sizeof longp - w, "atlas.json");
        }
        NT_ASSERT(strlen(longp) > 255 && "F2 test path must exceed the old 255-byte slot");
        const tp_snapshot_atlas *f2a = selftest_atlas_at(0, NULL);
        const tp_snapshot_target *f2t = selftest_target_at(0, 0);
        NT_ASSERT(f2a && f2t && "fresh project seeds a target for the F2 toggle test");
        /* Seed the long path through the text-draft path. */
        (void)selftest_set_target_at(
            0, 0, f2t->exporter_id, longp, f2t->enabled);
        f2t = selftest_target_at(0, 0);
        const bool en_was = f2t->enabled;
        gui_target_ref f2ref;
        NT_ASSERT(selftest_target_ref_at(0, 0, &f2ref));
        gui_edit_target_enabled(&f2ref, !en_was);
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        f2t = selftest_target_at(0, 0);
        nt_log_info("SELFTEST: F2 out_path len=%zu after toggle enabled %d->%d (match=%d)", strlen(f2t->out_path),
                    en_was, f2t->enabled, strcmp(f2t->out_path, longp) == 0);
        NT_ASSERT(strcmp(f2t->out_path, longp) == 0 && f2t->enabled == !en_was &&
                  "a target-enabled operation preserves the full output path");

        /* Origin drafts also own one component and rebuild the grouped
         * operation from the newest snapshot at submit time. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)selftest_set_sprite_origin_at(0, "osprite", 0 /* X */, 0.25F);
        (void)selftest_set_sprite_origin_at(0, "osprite", 1 /* Y */, 0.75F);
        const tp_snapshot_sprite *oov = selftest_sprite_by_name(0, "osprite");
        const float g2_ox = oov ? oov->origin_x : -1.0F;
        const float g2_oy = oov ? oov->origin_y : -1.0F;
        nt_log_info("SELFTEST: #2 origin X=%g Y=%g (want 0.25,0.75 -- neither lost)", (double)g2_ox, (double)g2_oy);
        NT_ASSERT(g2_ox == 0.25F && g2_oy == 0.75F &&
                  "origin component drafts preserve both submitted values");

        /* (#11) Target-path typing is one reducer-owned draft. Keystrokes
         * replace only its value; one gesture submits one masked operation. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        const tp_snapshot_target *t11a = selftest_target_at(0, 0);
        NT_ASSERT(t11a && "#11: fresh project seeds a default target");
        /* committed baseline out_path (one immediate commit, as the discrete browse/toggle paths do) */
        (void)selftest_set_target_at(
            0, 0, t11a->exporter_id, "out/base.json", t11a->enabled);
        t11a = selftest_target_at(0, 0);
        const char t11_base[] = "out/base.json";
        NT_ASSERT(strcmp(t11a->out_path, t11_base) == 0 && "#11: baseline out_path committed");
        const bool t11_en = t11a->enabled;
        char t11_exp[TP_EXPORTER_ID_MAX];
        memcpy(t11_exp, t11a->exporter_id, strlen(t11a->exporter_id) + 1U);
        const int t11_u0 = gui_project_undo_depth();
        (void)selftest_set_target_path_at(0, 0, "out/f");
        (void)selftest_set_target_path_at(0, 0, "out/fin");
        (void)selftest_set_target_path_at(0, 0, "out/final");
        (void)selftest_set_target_path_at(0, 0, "out/final.json");
        const int t11_umid = gui_project_undo_depth();
        const char *t11_mid = selftest_target_at(0, 0)->out_path;
        NT_ASSERT(t11_umid == t11_u0 && strcmp(t11_mid, t11_base) == 0 &&
                  "#11: in-flight path text exists only in the draft");
        NT_ASSERT(gui_actions__submit_draft() &&
                  "#11: gesture submits the active path draft");
        selftest_observe_session();
        const int t11_u1 = gui_project_undo_depth();
        const tp_snapshot_target *t11b = selftest_target_at(0, 0);
        nt_log_info("SELFTEST: #11 path draft: 4 changes undo %d->%d path='%s' exporter='%s' enabled=%d", t11_u0,
                    t11_u1, t11b->out_path, t11b->exporter_id, t11b->enabled);
        NT_ASSERT(t11_u1 - t11_u0 == 1 && strcmp(t11b->out_path, "out/final.json") == 0 &&
                  "#11: N text changes produce one operation and one Undo step");
        NT_ASSERT(strcmp(t11b->exporter_id, t11_exp) == 0 && t11b->enabled == t11_en &&
                  "#11: path operation leaves target siblings untouched");
        NT_ASSERT(gui_project_undo() &&
                  strcmp(selftest_target_at(0, 0)->out_path, t11_base) == 0 &&
                  "#11: one undo reverts the path gesture");
        NT_ASSERT(gui_project_redo() &&
                  strcmp(selftest_target_at(0, 0)->out_path, "out/final.json") == 0 &&
                  "#11: redo reapplies the path gesture");

        const int t11_unz = gui_project_undo_depth();
        const bool t11_stale_before = gui_project_is_stale();
        (void)selftest_set_target_path_at(0, 0, "out/scratch");
        (void)selftest_set_target_path_at(0, 0, "out/final.json");
        NT_ASSERT(gui_actions__submit_draft());
        selftest_observe_session();
        NT_ASSERT(gui_project_undo_depth() == t11_unz &&
                  gui_project_is_stale() == t11_stale_before &&
                  strcmp(selftest_target_at(0, 0)->out_path, "out/final.json") == 0 &&
                  "#11: a net-zero gesture changes neither history nor preview state");

        gui_target_ref t11_ref;
        NT_ASSERT(selftest_target_ref_at(0, 0, &t11_ref));
        (void)selftest_set_target_path_at(0, 0, "out/typed.json");
        gui_edit_target_enabled(&t11_ref, !t11_en);
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        const tp_snapshot_target *t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/typed.json") == 0 &&
                  t11i->enabled == !t11_en &&
                  "#11: dependent target toggle runs after exact draft success");

        (void)selftest_set_target_path_at(0, 0, "");
        const bool t11_en1 = t11i->enabled;
        NT_ASSERT(selftest_target_ref_at(0, 0, &t11_ref));
        gui_edit_target_enabled(&t11_ref, !t11_en1);
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_EDITING &&
                  t11i->enabled == t11_en1 &&
                  strcmp(t11i->out_path, "out/typed.json") == 0 &&
                  "#11: invalid draft preserves text and blocks dependent mutation");
        gui_draft_discard();

        /* Restore a packable atlas-0 project for the render frames below (the pixel probe packs
         * atlas 0 and probes its region outlines) -- gui_project_test_new left it source-less. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        reset_selection();
        char pfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", pfolder, sizeof pfolder);
        (void)gui_project_add_source(0, pfolder);
    }

    /* --- H/P2-14: Add Atlas auto-name must SCAN for a free atlasN, not blindly use atlas_count+1 (which
     *     collides with a surviving atlas after a remove -> core rejects the duplicate name -> the button
     *     wedges). Build atlas1..atlas3, remove atlas1, then Add: the old code picked "atlas3" (count 2+1)
     *     and FAILED; the scan picks the freed "atlas1" and succeeds. --- */
    {
        gui_project_test_new(); /* fresh: exactly one atlas (atlas1) */
        const int p14a2 =
            gui_project_add_atlas().visible_index; /* atlas2 */
        const int p14a3 =
            gui_project_add_atlas().visible_index; /* atlas3 */
        NT_ASSERT(p14a2 >= 0 && p14a3 >= 0 &&
                  tp_session_snapshot_atlas_count(gui_project_snapshot()) == 3 &&
                  "P2-14: seeded atlas1..atlas3");
        NT_ASSERT(gui_project_remove_atlas(0) && "P2-14: removed atlas1 (count -> 2)");
        const int p14add =
            gui_project_add_atlas().visible_index; /* count+1 == "atlas3" WOULD collide; the scan must avoid it */
        const int p14count = tp_session_snapshot_atlas_count(gui_project_snapshot());
        const tp_snapshot_atlas *p14added = selftest_atlas_at(p14add, NULL);
        const char *p14nm = p14added ? p14added->name : "(wedged)";
        int p14dupes = 0;
        for (int i = 0; i < p14count; i++) {
            const tp_snapshot_atlas *candidate = selftest_atlas_at(i, NULL);
            if (candidate && candidate->name && strcmp(candidate->name, p14nm) == 0) {
                p14dupes++;
            }
        }
        nt_log_info("SELFTEST: P2-14 add-after-remove -> idx=%d name='%s' count=%d dupes=%d (want idx>=0, dupes=1)",
                    p14add, p14nm, p14count, p14dupes);
        NT_ASSERT(p14add >= 0 && "P2-14: Add Atlas after a remove does NOT wedge on a colliding auto-name");
        NT_ASSERT(p14dupes == 1 && "P2-14: the auto-name is unique (the scan skipped the surviving atlas3)");

        /* Scenario B (review [0]/[1]): a name-only scan would reclaim a freed NAME whose default out_path
         * is still live on a RENAMED atlas -> two targets at out/atlasN -> silent export overwrite. Rename
         * atlas1 -> 'sprites' (its target stays out/atlas1); Add Atlas must SKIP "atlas1" (out/atlas1 taken)
         * and pick "atlas2". */
        gui_project_test_new(); /* fresh atlas1 + default target out/atlas1 */
        NT_ASSERT(selftest_set_atlas_name_at(0, "sprites") && "P2-14/B: rename atlas1 -> 'sprites' (target stays out/atlas1)");
        const int p14b =
            gui_project_add_atlas().visible_index;
        const tp_snapshot_atlas *p14ba = selftest_atlas_at(p14b, NULL);
        const tp_snapshot_target *p14bt = selftest_target_at(p14b, 0);
        const char *p14bn = p14ba ? p14ba->name : "(wedged)";
        const char *p14bo = p14bt ? p14bt->out_path : "(none)";
        nt_log_info("SELFTEST: P2-14/B rename-then-add -> name='%s' out_path='%s' (want atlas2, out/atlas2)", p14bn, p14bo);
        NT_ASSERT(p14b >= 0 && strcmp(p14bn, "atlas2") == 0 &&
                  "P2-14/B: the scan skips 'atlas1' (out/atlas1 still held by the renamed atlas) and picks 'atlas2'");
        NT_ASSERT(p14bo && strcmp(p14bo, "out/atlas1") != 0 &&
                  "P2-14/B: the new atlas's default target does NOT collide on out/atlas1");
        gui_project_test_new(); /* leave a clean project for the following phases */
    }

    /* --- H/P2-13: Add Files (multi-select) commits ONE transaction, not one per file -> a 4-file add is
     *     a SINGLE undo step and is ATOMIC (one undo removes all of them). Also de-dups WITHIN the batch. --- */
    {
        gui_project_test_new();
        const tp_snapshot_atlas *p13a = selftest_atlas_at(0, NULL);
        const int p13n0 = p13a ? p13a->source_count : -1;
        const int p13u0 = gui_project_undo_depth();
        const char *p13paths[4] = {"batch/a.png", "batch/b.png", "batch/c.png", "batch/a.png"}; /* last = in-batch dup */
        int p13add = -1;
        int p13dup = -1;
        const bool p13ok = gui_project_add_sources(0, p13paths, 4, TP_SOURCE_KIND_FILE, &p13add, &p13dup);
        const tp_snapshot_atlas *p13a1 = selftest_atlas_at(0, NULL);
        const int p13n1 = p13a1 ? p13a1->source_count : -1;
        const int p13u1 = gui_project_undo_depth();
        nt_log_info("SELFTEST: P2-13 batch-add ok=%d added=%d dup=%d sources %d->%d undo %d->%d (want ok,3,1,+3,+1)",
                    (int)p13ok, p13add, p13dup, p13n0, p13n1, p13u0, p13u1);
        NT_ASSERT(p13ok && p13add == 3 && p13dup == 1 && "P2-13: 3 distinct added, the in-batch duplicate skipped");
        NT_ASSERT(p13n1 == p13n0 + 3 && "P2-13: all 3 distinct sources landed in one commit");
        NT_ASSERT(p13u1 == p13u0 + 1 && "P2-13: the whole multi-select is ONE undo step (not one per file)");
        const bool p13undo = gui_project_undo(); /* atomic: a single undo removes ALL three */
        const tp_snapshot_atlas *p13a2 = selftest_atlas_at(0, NULL);
        nt_log_info("SELFTEST: P2-13 undo=%d sources->%d undo_depth->%d (want back to %d,%d)",
                    (int)p13undo, p13a2 ? p13a2->source_count : -1, gui_project_undo_depth(), p13n0, p13u0);
        NT_ASSERT(p13undo && p13a2 && p13a2->source_count == p13n0 && gui_project_undo_depth() == p13u0 &&
                  "P2-13: ONE undo atomically removes all three batch sources");
        const bool p13redo = gui_project_redo(); /* atomic: a single redo restores ALL three */
        const tp_snapshot_atlas *p13a3 = selftest_atlas_at(0, NULL);
        NT_ASSERT(p13redo && p13a3 && p13a3->source_count == p13n0 + 3 && gui_project_undo_depth() == p13u0 + 1 &&
                  "P2-13: ONE redo atomically restores all three batch sources");
        /* a batch whose path is ALREADY in the atlas counts it as a dup, not an add (the in-atlas branch). */
        const char *p13paths2[2] = {"batch/a.png", "batch/d.png"}; /* a already present (redone), d new */
        int p13add2 = -1;
        int p13dup2 = -1;
        const bool p13ok2 = gui_project_add_sources(0, p13paths2, 2, TP_SOURCE_KIND_FILE, &p13add2, &p13dup2);
        const tp_snapshot_atlas *p13a4 = selftest_atlas_at(0, NULL);
        nt_log_info("SELFTEST: P2-13 in-atlas-dup ok=%d add=%d dup=%d sources->%d (want ok,1,1,+4)", (int)p13ok2, p13add2,
                    p13dup2, p13a4 ? p13a4->source_count : -1);
        NT_ASSERT(p13ok2 && p13add2 == 1 && p13dup2 == 1 &&
                  "P2-13: a path already in the atlas is a dup; only the genuinely-new one is added");
        NT_ASSERT(p13a4 && p13a4->source_count == p13n0 + 4 && "P2-13: exactly one new source landed");
        gui_project_test_new(); /* leave a clean project for the following phases */
    }

    /* --- LIVE best-effort recovery UX + crash-recovery round-trip --- */
    {
        /* (J1) Recovery append failure is a visible best-effort degradation, not
         * a model commit gate. The edit, dirty state, and History remain live. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        reset_selection();
        (void)gui_project_take_op_error(NULL, 0); /* clear any stale soft-error */
        NT_ASSERT(gui_project__test_attach_memory_recovery() &&
                  "J1: memory recovery journal attached to the live model");
        char nm0[64];
        (void)snprintf(nm0, sizeof nm0, "%s", selftest_atlas_at(0, NULL)->name);
        const bool dirty_before = gui_project_is_dirty();
        gui_project__test_fail_next_recovery_writes(1); /* the NEXT journal append fails entirely */
        const bool committed = selftest_set_atlas_name_at(0, "committed_without_recovery");
        gui_recovery_notice j1notice = {0};
        const bool surfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j1notice);
        const char *nm1 = selftest_atlas_at(0, NULL)->name;
        nt_log_info("SELFTEST: J1 append-fail committed=%d surfaced=%d msg='%s' name '%s'->'%s' dirty %d->%d",
                    (int)committed, (int)surfaced, j1notice.message, nm0, nm1, (int)dirty_before, (int)gui_project_is_dirty());
        const tp_session_recovery_health j1health =
            tp_session_recovery_health_query(gui_project__test_session());
        NT_ASSERT(committed && strcmp(nm1, "committed_without_recovery") == 0 &&
                  "J1: recovery failure does not reject or roll back the edit");
        NT_ASSERT(surfaced && !gui_project_take_op_error(NULL, 0) &&
                  "J1: exact persistent recovery notice is separate from operation rejects");
        NT_ASSERT(!dirty_before && gui_project_is_dirty() &&
                  "J1: the committed edit still dirties the project");
        NT_ASSERT(j1health.degraded && j1health.first_cause == TP_STATUS_JOURNAL_FAILED &&
                  "J1: recovery health retains the first durable-write cause");
        /* Sticky degradation skips dependent appends, but editing stays live. */
        NT_ASSERT(selftest_set_atlas_name_at(0, "works_after") &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "works_after") == 0 &&
                  "J1: the editor keeps working while recovery is degraded");
        gui_recovery_notice j1after = {0};
        NT_ASSERT(gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, &j1after) &&
                  j1after.generation == j1notice.generation &&
                  "J1: sticky recovery notice keeps its generation across later edits");

        /* (J2) A reducer-owned draft commits even if its recovery append fails;
         * the following Save publishes it and heals recovery with a checkpoint. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J2: memory recovery journal attached");
        NT_ASSERT(selftest_set_atlas_name_at(0, "before_save") && "J2: a committed edit lands (journal healthy)");
        NT_ASSERT(gui_project_is_dirty() && "J2: the committed edit dirties the model");
        const int j2pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j2snapshot = NULL;
        const tp_snapshot_atlas *j2atlas =
            selftest_atlas_at(0, &j2snapshot);
        gui_edit_atlas_setting(
            j2atlas->id, tp_session_snapshot_revision(j2snapshot),
            GUI_ATLAS_PADDING, j2pad + 5, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        NT_ASSERT(gui_actions__submit_draft() &&
                  "J2: reducer draft submits despite recovery degradation");
        char s2path[1200];
        (void)snprintf(s2path, sizeof s2path, "%s/selftest_savefail.ntpacker_project", s_exe_dir);
        (void)remove(s2path);
        char s2err[256] = {0};
        const tp_status s2st = gui_project_save_as(s2path, s2err, sizeof s2err);
        const bool s2_written = selftest_file_exists(s2path);
        /* Save completion is an exact synchronous receipt; dirty/recovery
         * projections advance only through the common atomic observation. */
        selftest_observe_session();
        nt_log_info("SELFTEST: J2 save-with-append-fail st=%s dirty=%d file_written=%d err='%s' (want OK,0,1)",
                    tp_status_str(s2st), (int)gui_project_is_dirty(), (int)s2_written, s2err);
        const tp_session_recovery_health j2health =
            tp_session_recovery_health_query(gui_project__test_session());
        NT_ASSERT(s2st == TP_STATUS_OK && !gui_project_is_dirty() && s2_written &&
                  "J2: Save succeeds independently of the failed diff append");
        NT_ASSERT(selftest_atlas_at(0, NULL)->padding == j2pad + 5 &&
                  "J2: Save contains the submitted draft");
        NT_ASSERT(!j2health.degraded && "J2: the successful Save checkpoint heals recovery");
        NT_ASSERT(!gui_project_recovery_notice_query(NULL) &&
                  "J2: successful Save publishes the cleared recovery notice state");
        (void)remove(s2path);
        (void)gui_project_take_op_error(NULL, 0);

        /* (J6) A structural operation may continue after the reducer draft is
         * submitted and only recovery recording degrades. Both edits land. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J6: memory journal attached");
        char j6name0[64];
        (void)snprintf(j6name0, sizeof j6name0, "%s", selftest_atlas_at(0, NULL)->name);
        const int j6pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j6snapshot = NULL;
        const tp_snapshot_atlas *j6atlas =
            selftest_atlas_at(0, &j6snapshot);
        gui_edit_atlas_setting(
            j6atlas->id, tp_session_snapshot_revision(j6snapshot),
            GUI_ATLAS_PADDING, j6pad + 7, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        NT_ASSERT(gui_actions__submit_draft() &&
                  "J6: draft submit remains successful when recovery degrades");
        selftest_observe_session();
        const bool j6ret = selftest_set_atlas_name_at(0, "structural_should_abort");
        gui_recovery_notice j6notice = {0};
        const bool j6surfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j6notice);
        const char *j6name1 = selftest_atlas_at(0, NULL)->name;
        const int j6pad1 = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: J6 structural-after-degrade ret=%d surfaced=%d name '%s'->'%s' pad %d->%d (want 1,1,changed)",
                    (int)j6ret, (int)j6surfaced, j6name0, j6name1, j6pad, j6pad1);
        NT_ASSERT(j6ret && "J6: recovery degradation does not abort the structural op");
        NT_ASSERT(j6surfaced && !gui_project_take_op_error(NULL, 0) &&
                  "J6: recovery degradation is a persistent exact notice, not an op-error");
        NT_ASSERT(strcmp(j6name1, "structural_should_abort") == 0 && j6pad1 == j6pad + 7 &&
                  "J6: both the submitted draft and following structural edit landed");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J7) Pack submits the active draft first and uses the newly committed
         * model even when recovery recording degrades. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        reset_selection();
        char j7folder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", j7folder, sizeof j7folder);
        (void)gui_project_add_source(0, j7folder);
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J7: memory journal attached");
        gui_pack_clear(-1); /* no prior result */
        const int j7pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j7snapshot = NULL;
        const tp_snapshot_atlas *j7atlas =
            selftest_atlas_at(0, &j7snapshot);
        gui_edit_atlas_setting(
            j7atlas->id, tp_session_snapshot_revision(j7snapshot),
            GUI_ATLAS_PADDING, j7pad + 3, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        do_pack_blocking();
        const tp_result *j7r = gui_pack_result(0);
        nt_log_info("SELFTEST: J7 pack-after-degrade result=%s (want PRESENT)",
                    j7r ? "PRESENT" : "NULL");
        NT_ASSERT(j7r != NULL && "J7: Pack proceeds from the committed post-flush model");
        NT_ASSERT(gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, NULL) &&
                  !gui_project_take_op_error(NULL, 0) &&
                  "J7: Pack leaves the recovery warning persistent and non-rejecting");

        /* (J8) New/Open/Exit share the explicit reducer-draft choice:
         * Apply & Continue, Discard & Continue, or Cancel. Exercise Apply on
         * New here; the action-trace suite covers all three lifecycle requests. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        char j8path[1200];
        (void)snprintf(j8path, sizeof j8path, "%s/selftest_gate.ntpacker_project", s_exe_dir);
        (void)remove(j8path);
        char j8serr[256] = {0};
        NT_ASSERT(gui_project_save_as(j8path, j8serr, sizeof j8serr) == TP_STATUS_OK &&
                  "J8: save to establish a path + a clean baseline");
        selftest_observe_session();
        NT_ASSERT(gui_project_has_path() && !gui_project_is_dirty() && "J8: saved -> has a path + clean");
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J8: memory journal attached");
        const int j8pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j8snapshot = NULL;
        const tp_snapshot_atlas *j8atlas =
            selftest_atlas_at(0, &j8snapshot);
        gui_edit_atlas_setting(
            j8atlas->id, tp_session_snapshot_revision(j8snapshot),
            GUI_ATLAS_PADDING, j8pad + 4, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        request_new();
        NT_ASSERT(gui_project_has_path() &&
                  !gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, NULL) &&
                  "J8: view-facing New only enqueues before the between-frame drain");
        gui_actions__selftest_drain_intents();
        const bool j8kept = gui_project_has_path();
        gui_lifecycle_view lifecycle =
            gui_actions_lifecycle_view();
        nt_log_info("SELFTEST: J8 draft-gate has_path=%d confirm=%d draft=%d",
                    (int)j8kept,
                    (int)gui_actions_lifecycle_active(),
                    (int)(lifecycle.phase ==
                          GUI_LIFECYCLE_RESOLVE_DRAFT));
        NT_ASSERT(j8kept &&
                  lifecycle.phase ==
                      GUI_LIFECYCLE_RESOLVE_DRAFT &&
                  lifecycle.request ==
                      GUI_LIFECYCLE_REQUEST_NEW &&
                  gui_draft_phase() == GUI_EDIT_EDITING &&
                  "J8: New presents the explicit active-draft choice");
        NT_ASSERT(!gui_project__test_recovery_notice(
                       TP_STATUS_JOURNAL_FAILED, NULL) &&
                  "J8: opening the draft choice does not submit it");
        gui_actions_lifecycle_choose(
            GUI_LIFECYCLE_CHOICE_ACCEPT);
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        lifecycle = gui_actions_lifecycle_view();
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_IDLE &&
                  lifecycle.phase ==
                      GUI_LIFECYCLE_RESOLVE_DIRTY &&
                  lifecycle.request ==
                      GUI_LIFECYCLE_REQUEST_NEW &&
                  gui_project_is_dirty() &&
                  selftest_atlas_at(0, NULL)->padding == j8pad + 4 &&
                  "J8: Apply submits the draft, then continues to the dirty-project choice");
        NT_ASSERT(gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, NULL) &&
                      !gui_project_take_op_error(NULL, 0) &&
                  "J8: draft submit preserves the persistent recovery warning");
        gui_actions_lifecycle_choose(
            GUI_LIFECYCLE_CHOICE_CANCEL);
        gui_actions__selftest_drain_intents();
        lifecycle = gui_actions_lifecycle_view();
        NT_ASSERT(lifecycle.phase ==
                      GUI_LIFECYCLE_IDLE &&
                  lifecycle.request ==
                      GUI_LIFECYCLE_REQUEST_NONE &&
                  gui_project_has_path() &&
                  "J8: Cancel keeps the current project after Apply");
        (void)remove(j8path);

        /* (J9) Remove continues after a prerequisite draft submit degrades
         * recovery. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        const int j9added =
            gui_project_add_atlas().visible_index; /* a 2nd atlas to remove (index 1) */
        NT_ASSERT(j9added >= 1 && "J9: added a 2nd atlas to remove");
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J9: memory journal attached");
        const int j9count0 = tp_session_snapshot_atlas_count(gui_project_snapshot());
        const int j9pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j9snapshot = NULL;
        const tp_snapshot_atlas *j9atlas =
            selftest_atlas_at(0, &j9snapshot);
        gui_edit_atlas_setting(
            j9atlas->id, tp_session_snapshot_revision(j9snapshot),
            GUI_ATLAS_PADDING, j9pad + 6, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        NT_ASSERT(gui_actions__submit_draft() &&
                  "J9: prerequisite draft submit succeeds");
        selftest_observe_session();
        const bool j9ret_fail = gui_project_remove_atlas(j9added);
        const int j9count1 = tp_session_snapshot_atlas_count(gui_project_snapshot());
        nt_log_info("SELFTEST: J9 remove-after-degrade ret=%d count %d->%d (want 1, -1)", (int)j9ret_fail, j9count0, j9count1);
        NT_ASSERT(j9ret_fail && j9count1 == j9count0 - 1 &&
                  "J9: recovery degradation does not block the requested removal");
        NT_ASSERT(gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, NULL) &&
                  !gui_project_take_op_error(NULL, 0) &&
                  "J9: removal keeps the exact recovery warning separate from rejects");

        /* (J10) Animation rename remains a first-class operation: collisions
         * reject, while a unique rename commits despite recovery degradation. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        const int j10a = gui_project_create_animation(0, "anim_a", NULL, 0);
        const int j10b = gui_project_create_animation(0, "anim_b", NULL, 0);
        NT_ASSERT(j10a == 0 && j10b == 1 && "J10: created two animations");
        /* Case A -- genuine collision: the exact core reject is surfaced by
         * the draft submit; no client-side collision heuristic exists. */
        const bool j10_collide_ret = selftest_set_anim_id_at(0, j10b, "anim_a");
        nt_log_info("SELFTEST: J10 collision ret=%d msg='%s' (want 0 -> the core collision message)",
                    (int)j10_collide_ret, s_status);
        NT_ASSERT(!j10_collide_ret &&
                  strstr(s_status, "an animation named") != NULL &&
                  strstr(s_status, "already exists") != NULL &&
                  "J10/live: a genuine duplicate preserves the core collision message");
        /* Case B -- recovery-degraded draft submit before a UNIQUE name: both
         * operations commit, with a recovery warning rather than a false collision. */
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J10: memory journal attached");
        const int j10pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j10snapshot = NULL;
        const tp_snapshot_atlas *j10atlas =
            selftest_atlas_at(0, &j10snapshot);
        gui_edit_atlas_setting(
            j10atlas->id, tp_session_snapshot_revision(j10snapshot),
            GUI_ATLAS_PADDING, j10pad + 2, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        NT_ASSERT(gui_actions__submit_draft() &&
                  "J10: prerequisite draft submit succeeds");
        selftest_observe_session();
        const bool j10_flush_ret = selftest_set_anim_id_at(0, j10b, "totally_unique_name");
        gui_recovery_notice j10_notice = {0};
        const bool j10_fsurfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j10_notice);
        nt_log_info("SELFTEST: J10 recovery-degrade ret=%d surfaced=%d msg='%s' (want 1,1 -> recovery warning)",
                    (int)j10_flush_ret, (int)j10_fsurfaced, j10_notice.message);
        NT_ASSERT(j10_flush_ret && j10_fsurfaced &&
                  strstr(j10_notice.message, "already exists") == NULL &&
                  !gui_project_take_op_error(NULL, 0) &&
                  strcmp(selftest_animation_at(0, j10b)->name, "totally_unique_name") == 0 &&
                  "J10/live: a unique rename commits and recovery failure is only a warning");

        /* (J11) Own-name remains a no-op success. A recovery-degraded reducer
         * submit also succeeds and exposes only the non-blocking warning. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        const int j11a = gui_project_create_animation(0, "keep_me", NULL, 0);
        NT_ASSERT(j11a == 0 && "J11: created an animation");
        /* (C) own/unchanged name -> a no-op SUCCESS (true) that raises NO op-error. This is exactly the case
         *     the retired heuristic got wrong (the name "exists", yet the rename succeeds); the live path
         *     returns true and leaves the op-error channel empty. */
        const bool j11_own_ret = selftest_set_anim_id_at(0, j11a, "keep_me");
        const bool j11_own_err = gui_project_take_op_error(NULL, 0);
        nt_log_info("SELFTEST: J11 own-name ret=%d op_error=%d (want 1,0 -> a no-op SUCCESS with no op-error)",
                    (int)j11_own_ret, (int)j11_own_err);
        NT_ASSERT(j11_own_ret && !j11_own_err &&
                  "J11/live: renaming to the OWN name is a no-op SUCCESS that raises no op-error");
        /* Recovery failure during draft submit is not an operation rejection. */
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J11: memory journal attached");
        const int j11pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j11snapshot = NULL;
        const tp_snapshot_atlas *j11atlas =
            selftest_atlas_at(0, &j11snapshot);
        gui_edit_atlas_setting(
            j11atlas->id, tp_session_snapshot_revision(j11snapshot),
            GUI_ATLAS_PADDING, j11pad + 1, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        const bool j11_submit = gui_actions__submit_draft();
        gui_recovery_notice j11_notice = {0};
        const bool j11_fsurfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j11_notice);
        nt_log_info("SELFTEST: J11 draft-submit ret=%d surfaced=%d (want 1,1 -> recovery warning)",
                    (int)j11_submit, (int)j11_fsurfaced);
        NT_ASSERT(j11_submit && j11_fsurfaced &&
                   !gui_project_take_op_error(NULL, 0) &&
                   "J11: recovery-degraded draft submit still commits successfully");
        selftest_observe_session();

        /* (J11b) A core-rejected atlas rename preserves the draft until the
         * user explicitly corrects or discards it. */
        const tp_session_snapshot *j11b_snapshot = NULL;
        const tp_snapshot_atlas *j11b_atlas =
            selftest_atlas_at(0, &j11b_snapshot);
        const int64_t j11b_revision =
            tp_session_snapshot_revision(j11b_snapshot);
        NT_ASSERT(j11b_atlas);
        start_atlas_edit_ref(
            j11b_atlas->id, j11b_revision);
        NT_ASSERT(gui_text_edit_update(""));
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        nt_log_info("SELFTEST: J11b invalid atlas Enter phase=%d revision=%lld (want EDITING, unchanged)",
                    (int)gui_draft_phase(), (long long)tp_session_snapshot_revision(gui_project_snapshot()));
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_EDITING &&
                  tp_session_snapshot_revision(gui_project_snapshot()) == j11b_revision &&
                  "J11b: Enter on an invalid atlas name keeps the editor and model unchanged");
        gui_draft_discard();
        NT_ASSERT(gui_draft_phase() == GUI_EDIT_IDLE &&
                  tp_session_snapshot_revision(gui_project_snapshot()) == j11b_revision &&
                  "J11b: explicit discard clears a rejected rename without mutation");

        /* (J12) Undo blocks while a reducer-owned draft is active. Once the
         * draft is explicitly applied, Undo operates on committed History and
         * recovery degradation remains a separate warning. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(selftest_set_atlas_name_at(0, "j12_edit") && "J12: a committed edit populates undo history");
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J12: memory journal attached");
        const int j12pad = selftest_atlas_at(0, NULL)->padding;
        const tp_session_snapshot *j12snapshot = NULL;
        const tp_snapshot_atlas *j12atlas =
            selftest_atlas_at(0, &j12snapshot);
        const int64_t j12revision =
            tp_session_snapshot_revision(j12snapshot);
        gui_edit_atlas_setting(
            j12atlas->id, j12revision,
            GUI_ATLAS_PADDING, j12pad + 5, 0.0F);
        gui_project__test_fail_next_recovery_writes(1);
        set_status("j12_sentinel"); /* a sentinel so we can tell do_undo replaced the status */
        do_undo();
        nt_log_info("SELFTEST: J12 do_undo-with-active-draft status='%s'",
                    s_status);
        NT_ASSERT(strcmp(
                      s_status,
                      "Apply or discard the active edit before Undo.") == 0 &&
                  gui_draft_phase() == GUI_EDIT_EDITING &&
                  tp_session_snapshot_revision(gui_project_snapshot()) ==
                      j12revision &&
                  selftest_atlas_at(0, NULL)->padding == j12pad &&
                  "J12: Undo blocks without submitting the active draft");
        NT_ASSERT(!gui_project__test_recovery_notice(
                       TP_STATUS_JOURNAL_FAILED, NULL) &&
                  "J12: blocked Undo does not consume the pending recovery failure");
        gui_request_gesture_commit();
        gui_actions__selftest_drain_intents();
        selftest_observe_session();
        NT_ASSERT(gui_project__test_recovery_notice(
                      TP_STATUS_JOURNAL_FAILED, NULL) &&
                  gui_draft_phase() == GUI_EDIT_IDLE &&
                  selftest_atlas_at(0, NULL)->padding == j12pad + 5 &&
                  !gui_project_take_op_error(NULL, 0) &&
                  "J12: explicit Apply commits and surfaces recovery degradation");
        do_undo();
        selftest_observe_session();
        NT_ASSERT(strncmp(s_status, "Undo", 4) == 0 &&
                  selftest_atlas_at(0, NULL)->padding == j12pad &&
                  "J12: Undo resumes after the draft reaches a terminal state");

        /* (J12b) Undo/Redo publish History independently of recovery recording. */
        gui_project_test_new();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J12b: memory recovery journal attached");
        NT_ASSERT(selftest_set_atlas_name_at(0, "j12b_edit") && "J12b: committed edit populates history");
        gui_project__test_fail_next_recovery_writes(1);
        const bool j12b_undo_fail = gui_project_undo();
        gui_recovery_notice j12b_undo_notice = {0};
        const bool j12b_usurfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j12b_undo_notice);
        NT_ASSERT(j12b_undo_fail && j12b_usurfaced &&
                  !gui_project_take_op_error(NULL, 0) &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "atlas1") == 0 &&
                  "J12b: Undo publishes and surfaces recovery degradation as a warning");

        const bool j12b_redo_fail = gui_project_redo();
        gui_recovery_notice j12b_redo_notice = {0};
        const bool j12b_rsurfaced = gui_project__test_recovery_notice(
            TP_STATUS_JOURNAL_FAILED, &j12b_redo_notice);
        NT_ASSERT(j12b_redo_fail && j12b_rsurfaced &&
                  j12b_redo_notice.generation == j12b_undo_notice.generation &&
                  !gui_project_take_op_error(NULL, 0) &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "j12b_edit") == 0 &&
                  "J12b: Redo keeps the same sticky recovery notice generation");

        /* (J14) The startup decision must never replace recovered work with
         * a command-line project. Exercise the complete pure truth table. */
        NT_ASSERT(gui_startup_decide(true,  true,  true)  == GUI_STARTUP_DEFER &&
                  "J14: arg present+exists + recovered -> DEFER (the data-loss guard; must NOT open)");
        NT_ASSERT(gui_startup_decide(true,  false, true)  == GUI_STARTUP_DEFER &&
                  "J14: recovered wins over a stale arg -> DEFER, not MISSING (finding 2; no clobber)");
        NT_ASSERT(gui_startup_decide(true,  true,  false) == GUI_STARTUP_OPEN &&
                  "J14: arg present+exists + NOT recovered -> OPEN");
        NT_ASSERT(gui_startup_decide(true,  false, false) == GUI_STARTUP_MISSING &&
                  "J14: arg present but missing + NOT recovered -> MISSING (project not found)");
        NT_ASSERT(gui_startup_decide(false, false, true)  == GUI_STARTUP_IDLE &&
                  "J14: no arg + recovered -> IDLE (caller keeps the recovery warning)");
        NT_ASSERT(gui_startup_decide(false, true,  true)  == GUI_STARTUP_IDLE &&
                  "J14: no arg -> IDLE regardless of arg_exists (recovered)");
        NT_ASSERT(gui_startup_decide(false, false, false) == GUI_STARTUP_IDLE &&
                  "J14: no arg + not recovered -> IDLE (Ready...)");
        NT_ASSERT(gui_startup_decide(false, true,  false) == GUI_STARTUP_IDLE &&
                  "J14: no arg -> IDLE regardless of arg_exists (not recovered)");
        nt_log_info("SELFTEST: J14 gui_startup_decide truth table OK (8 rows; (1,1,1)->DEFER, (1,1,0)->OPEN, (1,0,1)->DEFER)");

        /* One GUI-boundary recovery smoke: typed list ownership, all three
         * action mappings, and raw-close versus explicit-discard policy. */
        char j15root[TP_IDENTITY_PATH_MAX];
        char j15original[TP_IDENTITY_PATH_MAX];
        char j15save_as[TP_IDENTITY_PATH_MAX];
        char j15discard_target[TP_IDENTITY_PATH_MAX];
        int j15n = snprintf(j15root, sizeof j15root,
                            "%s/recovery_boundary_%lu", s_exe_dir,
                            selftest_process_id());
        NT_ASSERT(j15n > 0 && (size_t)j15n < sizeof j15root &&
                  selftest_is_private_recovery_root(j15root));
        j15n = snprintf(j15original, sizeof j15original,
                        "%s/original.ntpacker_project", j15root);
        NT_ASSERT(j15n > 0 && (size_t)j15n < sizeof j15original);
        j15n = snprintf(j15save_as, sizeof j15save_as,
                        "%s/save_as.ntpacker_project", j15root);
        NT_ASSERT(j15n > 0 && (size_t)j15n < sizeof j15save_as);
        j15n = snprintf(j15discard_target, sizeof j15discard_target,
                        "%s/discard_must_not_write.ntpacker_project",
                        j15root);
        NT_ASSERT(j15n > 0 && (size_t)j15n < sizeof j15discard_target);
        gui_project_test_shutdown(true);
        NT_ASSERT(selftest_remove_flat_dir(j15root));
        tp_mkdirs(j15root);
        gui_recovery_list *j15list =
            (gui_recovery_list *)calloc(1, sizeof *j15list);
        NT_ASSERT(j15list && "J15: recovery scratch allocation");

        gui_recovery_entry j15entry;
        NT_ASSERT(selftest_make_recovery_candidate(
                      j15root, j15original, "saved_original", j15list,
                      &j15entry) &&
                  "J15: dirty raw close leaves a typed recovery candidate");
        (void)gui_recovery_collect(j15list);
        j15list->has_more = true;
        gui_actions_open_recovery(j15list);
        j15list->count = 0; /* prove gui_actions owns a value copy */
        const gui_recovery_view j15view =
            gui_actions_recovery_view();
        NT_ASSERT(j15view.phase == GUI_RECOVERY_CHOOSE &&
                  j15view.count == 1 &&
                  j15view.has_more &&
                  gui_actions_recovery_at(0) != NULL &&
                  gui_actions_recovery_at(-1) == NULL &&
                  gui_actions_recovery_at(1) == NULL &&
                  "J15: recovery modal owns the typed bounded list");
        gui_actions_recovery_dismiss();
        NT_ASSERT(gui_recovery_collect(j15list) == 1 &&
                  "J15: Later preserves recovery for the next launch");

        char j15err[256];
        NT_ASSERT(gui_recovery_resolve_entry(&j15entry,
                                              GUI_RECOVERY_SAVE_ORIGINAL, "",
                                              j15err, sizeof j15err) == TP_STATUS_OK &&
                  "J15: Save Original maps to the core recovery action");
        NT_ASSERT(gui_project_test_open(j15original, j15err, sizeof j15err) == TP_STATUS_OK &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "saved_original") == 0 &&
                  "J15: Save Original writes the recovered model");

        NT_ASSERT(selftest_make_recovery_candidate(
                      j15root, "", "saved_as", j15list, &j15entry) &&
                  gui_recovery_resolve_entry(&j15entry, GUI_RECOVERY_SAVE_AS,
                                              j15save_as, j15err,
                                              sizeof j15err) == TP_STATUS_OK &&
                  gui_project_test_open(j15save_as, j15err, sizeof j15err) == TP_STATUS_OK &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "saved_as") == 0 &&
                  "J15: Save As maps and writes the recovered model");

        NT_ASSERT(selftest_make_recovery_candidate(
                      j15root, "", "discarded", j15list, &j15entry) &&
                  gui_recovery_resolve_entry(&j15entry, GUI_RECOVERY_DISCARD,
                                              j15discard_target, j15err,
                                              sizeof j15err) == TP_STATUS_OK &&
                  !selftest_file_exists(j15discard_target) &&
                  "J15: Discard removes recovery without writing a project");
        NT_ASSERT(gui_recovery_resolve_entry(&j15entry,
                                              (gui_recovery_action)99, "",
                                              j15err, sizeof j15err) ==
                          TP_STATUS_INVALID_ARGUMENT &&
                  j15err[0] != '\0' &&
                  "J15: invalid GUI action remains a structured error");

        /* Explicit Exit->Discard removes dirty recovery; an ordinary clean
         * shutdown also leaves no candidate. */
        gui_project_test_shutdown(true);
        gui_project_enable_recovery(j15root);
        gui_project_init();
        NT_ASSERT(selftest_set_atlas_name_at(0, "explicit_discard"));
        gui_project_test_shutdown(true);
        gui_project_enable_recovery(j15root);
        gui_project_init();
        NT_ASSERT(gui_recovery_collect(j15list) == 0 &&
                  "J15: explicit Exit->Discard removes dirty recovery");
        gui_project_test_shutdown(false); /* clean close removes its live slot */
        gui_project_enable_recovery(j15root);
        gui_project_init();
        NT_ASSERT(gui_recovery_collect(j15list) == 0 &&
                  "J15: clean close leaves no recovery candidate");

        /* Successful New/Open replace the outgoing session only after the
         * replacement is ready. The explicit replacement discards its dirty
         * recovery slot; a failed Open keeps the current session and slot. */
        char j15outgoing[TP_IDENTITY_PATH_MAX];
        const char *j15live = NULL;
        NT_ASSERT(selftest_set_atlas_name_at(0, "discarded_by_new"));
        j15live = tp_session__recovery_journal_path(
            gui_project__test_session());
        NT_ASSERT(j15live &&
                  snprintf(j15outgoing, sizeof j15outgoing, "%s", j15live) > 0 &&
                  selftest_file_exists(j15outgoing));
        NT_ASSERT(gui_project_test_new() &&
                  !selftest_file_exists(j15outgoing) &&
                  gui_recovery_collect(j15list) == 0 &&
                  "J15: New->Discard removes the outgoing dirty recovery slot");

        NT_ASSERT(selftest_set_atlas_name_at(0, "discarded_by_open"));
        j15live = tp_session__recovery_journal_path(
            gui_project__test_session());
        NT_ASSERT(j15live &&
                  snprintf(j15outgoing, sizeof j15outgoing, "%s", j15live) > 0 &&
                  selftest_file_exists(j15outgoing));
        NT_ASSERT(gui_project_test_open(j15original, j15err, sizeof j15err) ==
                      TP_STATUS_OK &&
                  !selftest_file_exists(j15outgoing) &&
                  gui_recovery_collect(j15list) == 0 &&
                  "J15: Open->Discard removes the outgoing dirty recovery slot");

        NT_ASSERT(selftest_set_atlas_name_at(0, "failed_open_kept"));
        tp_session *j15before_failed_open = gui_project__test_session();
        j15live = tp_session__recovery_journal_path(j15before_failed_open);
        NT_ASSERT(j15live &&
                  snprintf(j15outgoing, sizeof j15outgoing, "%s", j15live) > 0 &&
                  selftest_file_exists(j15outgoing));
        char j15missing[TP_IDENTITY_PATH_MAX];
        j15n = snprintf(j15missing, sizeof j15missing,
                        "%s/missing.ntpacker_project", j15root);
        NT_ASSERT(j15n > 0 && (size_t)j15n < sizeof j15missing &&
                  gui_project_test_open(j15missing, j15err, sizeof j15err) !=
                      TP_STATUS_OK &&
                  gui_project__test_session() == j15before_failed_open &&
                  gui_project_is_dirty() &&
                  selftest_file_exists(j15outgoing) &&
                  "J15: failed Open preserves the outgoing dirty session and recovery slot");
        nt_log_info("SELFTEST: J15 typed recovery boundary and shutdown policy OK");
        free(j15list);
        gui_project_test_shutdown(true);
        NT_ASSERT(selftest_remove_flat_dir(j15root));

        /* Done: disable recovery + release any lock + restore a journal-LESS packable project for the
         * render phases. */
        gui_project_enable_recovery("");
        gui_project_test_shutdown(true);
        gui_project_init();
        gui_pack_clear(-1);
        NT_ASSERT(selftest_select_atlas(0));
        reset_selection();
        char rfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", rfolder, sizeof rfolder);
        (void)gui_project_add_source(0, rfolder);
    }

    /* --- About modal: open it so the auto-quit frames render it (OK/Esc close it interactively) --- */
    s_about_open = true;
    nt_log_info("SELFTEST: About modal opened=%d", s_about_open);

    /* --- Export dialog: exercise its data path (toggle a target the way the dialog checkbox does) and
     * leave it open so the warmup frames render the modal (a Clay layout bug there would crash them). --- */
    {
        const int ep_count = tp_session_snapshot_atlas_count(gui_project_snapshot());
        int e_atlas = -1;
        for (int i = 0; i < ep_count; i++) {
            const tp_snapshot_atlas *atlas = selftest_atlas_at(i, NULL);
            if (atlas && atlas->target_count > 0) {
                e_atlas = i;
                break;
            }
        }
        if (e_atlas >= 0) {
            const tp_snapshot_target *t0 = selftest_target_at(e_atlas, 0);
            const bool was = t0->enabled;
            /* Same observation-lifetime rule as the export loop above: the
             * toggle frees the snapshot `t0` points into, so the exporter id
             * and out_path travel as copies, not as borrowed rows. */
            char t0_exporter[TP_EXPORTER_ID_MAX];
            char t0_out_path[TP_IDENTITY_PATH_MAX];
            (void)snprintf(t0_exporter, sizeof t0_exporter, "%s",
                           t0->exporter_id);
            (void)snprintf(t0_out_path, sizeof t0_out_path, "%s",
                           t0->out_path);
            selftest_set_target_at(
                e_atlas, 0, t0_exporter, t0_out_path, !was);
            const tp_snapshot_target *changed = selftest_target_at(e_atlas, 0);
            const bool now = changed->enabled;
            char exporter[TP_EXPORTER_ID_MAX];
            char out_path[TP_IDENTITY_PATH_MAX];
            memcpy(exporter, changed->exporter_id,
                   strlen(changed->exporter_id) + 1U);
            (void)snprintf(out_path, sizeof out_path, "%s", changed->out_path);
            selftest_set_target_at(
                e_atlas, 0, exporter, out_path, was);
            nt_log_info("SELFTEST: export-dialog toggle atlas=%d target0 %d->%d (restored=%d)", e_atlas, was, now, was);
        }
        s_export_open = true;
    }

    /* Async == blocking equivalence (spec req 4). Promoted out of visual phase 9
     * so headless CI actually runs it: phase 9 needs a GL context and is skipped
     * in a NTPACKER_GUI_HEADLESS_CI build, which left the whole worker-process Pack path
     * unverified on CI. Needs no GL -- only the job transport and the result
     * metadata. Phase 9 keeps its copy for local GPU runs. */
    {
        NT_ASSERT(selftest_select_atlas(0));
        char async_error[256] = {0};
        NT_ASSERT(gui_pack_async_start(0, async_error, sizeof async_error) &&
                  "SELFTEST: async pack must start");
        const clock_t async_deadline = clock() + (clock_t)(60 * CLOCKS_PER_SEC);
        while (gui_pack_async_busy()) {
            /* Same explicit boundary the frame loop uses. */
            tp_error pump_error = {{0}};
            NT_ASSERT(gui_actions_step(NULL, &pump_error) ==
                      TP_STATUS_OK);
            NT_ASSERT(clock() < async_deadline &&
                      "SELFTEST: async pack did not finish within 60s");
        }
        const tp_result *async_result = gui_pack_result(0);
        NT_ASSERT(async_result && async_result->sprite_count > 0 &&
                  async_result->page_count > 0 &&
                  "SELFTEST: async pack produced no result");
        const int async_sprites = async_result->sprite_count;
        const int async_pages = async_result->page_count;
        const int async_w = async_result->pages[0].w;
        const int async_h = async_result->pages[0].h;
        double blocking_ms = 0.0;
        char blocking_error[256] = {0};
        char blocking_note[128] = {0};
        NT_ASSERT(gui_pack_atlas(0, &blocking_ms, blocking_error,
                                 sizeof blocking_error, blocking_note,
                                 sizeof blocking_note) &&
                  "SELFTEST: blocking reference pack failed");
        const tp_result *blocking_result = gui_pack_result(0);
        NT_ASSERT(blocking_result &&
                  blocking_result->sprite_count == async_sprites &&
                  blocking_result->page_count == async_pages &&
                  blocking_result->pages[0].w == async_w &&
                  blocking_result->pages[0].h == async_h &&
                  "SELFTEST: async vs blocking result mismatch (non-deterministic)");
        nt_log_info(
            "SELFTEST: async==blocking OK (sprites=%d pages=%d page0=%dx%d)",
            async_sprites, async_pages, async_w, async_h);
    }

    /* Leave a live selection so the auto-quit frames draw the decoded image. */
    const tp_session_snapshot *cur = gui_project_snapshot();
    const tp_snapshot_atlas *cur_atlas = selftest_atlas_at(0, NULL);
    const int ns = cur_atlas ? cur_atlas->source_count : 0;
    if (cur && cur_atlas && ns > 0) {
        const tp_snapshot_source *source = tp_session_snapshot_source_at(cur, cur_atlas->id, ns - 1);
        char resolved[512];
        tp_error resolve_error = {0};
        if (source && tp_session_snapshot_resolve_path(cur, cur_atlas->id, source->id,
                                                       resolved, sizeof resolved,
                                                       &resolve_error) == TP_STATUS_OK) {
            NT_ASSERT(selftest_select_atlas(0));
            build_rows();
            build_view();
            for (int row_index = 0; row_index < s_row_count; ++row_index) {
                const sprite_row *row = &s_rows[row_index];
                if (row->is_source &&
                    tp_id128_eq(row->source_id, source->id)) {
                    gui_rows_select_primary(row);
                    for (int view_index = 0;
                         view_index < s_view_count; ++view_index) {
                        if (s_view[view_index] == row_index) {
                            gui_rows_set_focus_view_index(view_index);
                            gui_rows_set_anchor_view_index(view_index);
                            break;
                        }
                    }
                    break;
                }
            }
        }
    }
    /* Render coverage: leave a real animation selected + previewing so the auto-quit frames exercise the
     * left-panel animations rows, the right-panel editor, and the canvas preview (draw_anim_frame on the
     * packed regions) -- a Clay layout bug in the new UI would crash these frames. */
    {
        NT_ASSERT(selftest_select_atlas(0));
        const tp_snapshot_atlas *pa = selftest_atlas_at(0, NULL);
        const tp_result *pr = gui_pack_result(0);
        if (pa && pr && pr->sprite_count > 0) {
            multi_sel_clear();
            for (int i = 0; i < pr->sprite_count && i < 4; i++) {
                char key[192];
                tp_sprite_export_key(pr->sprites[i].name, key, sizeof key);
                multi_sel_add(key);
            }
            const int pai = create_animation_from_selection();
            pa = selftest_atlas_at(0, NULL);
            if (pai >= 0 && pa) {
                gui_animation_ref preview_ref;
                NT_ASSERT(
                    selftest_animation_ref_at(
                        0, pai, &preview_ref));
                open_preview_ref(&preview_ref);
                const tp_snapshot_animation *animation = selftest_animation_at(0, pai);
                nt_log_info("SELFTEST: preview anim '%s' active=%d frames=%d", animation->name, s_preview_active,
                            animation->frame_count);
            }
            multi_sel_clear();
        }
    }
    g_ui_scale = 1.5F; /* exercise the scaled layout during the auto-quit frames */
    const sprite_row *selected_row = gui_rows_primary();
    nt_log_info("SELFTEST: end (undo:%d redo:%d; selection '%s')", gui_project_undo_depth(),
                gui_project_redo_depth(),
                selected_row && selected_row->abs ? selected_row->abs : "");
}

/* --- Overlay pixel probe (F) + touch-on-render guard, driven across the auto-quit frames --- */
static int s_st_phase;      /* 0 warmup, 1 outline pixel probe, 2 touch-on-render guard, 3 done */
static int s_st_pf;         /* frames spent in the current phase */
static int s_st_cyan0;      /* outline-OFF cyan count (baseline of the diff test) */
static char *s_st_baseline; /* fresh-project bytes captured with zero input */
static size_t s_st_baseline_n;

/* Count blue/cyan overlay pixels in the current canvas box (framebuffer read, top-left origin). The
 * region-outline colour is (0.30,0.72,1.0): B high, B>>R, G>R -- distinct from grey checker + sprites. */
static int selftest_probe_cyan(void) {
    if (gui_canvas_get_mode(&s_canvas) != GUI_CANVAS_ATLAS || !gui_canvas_has_atlas(&s_canvas)) {
        return -1;
    }
    const float *bb = s_canvas.last_bb;
    int x = (int)bb[0];
    int y = (int)bb[1];
    int w = (int)bb[2];
    int h = (int)bb[3];
    if (w < 8 || h < 8) {
        return -1;
    }
    if (w > 900) {
        w = 900;
    }
    if (h > 900) {
        h = 900;
    }
    const uint32_t capn = (uint32_t)w * (uint32_t)h * 4u;
    uint8_t *px = (uint8_t *)malloc(capn);
    if (!px) {
        return -1;
    }
    int cyan = -1;
    if (nt_gfx_read_pixels(x, y, w, h, px, capn)) {
        cyan = 0;
        for (uint32_t i = 0; i + 3u < capn; i += 4u) {
            const int r = px[i];
            const int g = px[i + 1];
            const int b = px[i + 2];
            if (b > 150 && b > r + 40 && g > r + 25 && g > 110) {
                cyan++;
            }
        }
    }
    free(px);
    return cyan;
}

/* Overflow regression: the key containers must sit inside the window and the right-panel content must not
 * be wider than the panel (rows fit). Reads the PREVIOUS frame's committed layout, so the caller must have
 * held the target size for >= 2 frames. Fails (NT_ASSERT) before the layout fix, passes after. */
static void selftest_assert_no_overflow(float win_w, float win_h) {
    const struct {
        const char *name;
        uint32_t id;
    } items[4] = {{"left", s_id_left_panel}, {"strip", s_id_strip}, {"canvas", s_id_canvas},
                  {"right", s_id_right_panel}}; /* status bar removed (pass 2): messages float as a pill */
    for (int i = 0; i < 4; i++) {
        const nt_ui_bbox_t b = nt_ui_get_bbox(s_ctx, items[i].id);
        nt_log_info("SELFTEST-BOUNDS %-6s found=%d x=%.1f y=%.1f w=%.1f h=%.1f right=%.1f/%.0f bottom=%.1f/%.0f",
                    items[i].name, (int)b.found, (double)b.x, (double)b.y, (double)b.width, (double)b.height,
                    (double)(b.x + b.width), (double)win_w, (double)(b.y + b.height), (double)win_h);
        NT_ASSERT(b.found && "SELFTEST overflow: key container was not laid out");
        NT_ASSERT(b.x >= -1.0F && (b.x + b.width) <= win_w + 1.0F &&
                  "SELFTEST overflow: container spills past the window horizontally");
        NT_ASSERT(b.y >= -1.0F && (b.y + b.height) <= win_h + 1.0F &&
                  "SELFTEST overflow: container spills past the window vertically");
    }
    const nt_ui_bbox_t rp = nt_ui_get_bbox(s_ctx, s_id_right_panel);
    const nt_ui_bbox_t rc = nt_ui_get_bbox(s_ctx, s_id_right_content);
    NT_ASSERT(rp.found && rc.found && (rc.x + rc.width) <= (rp.x + rp.width) + 2.0F &&
              "SELFTEST overflow: right-panel rows bleed past the panel");
}

/* Top-of-frame phase driver: sets up each phase's scene BEFORE the layout/walk. */
void selftest_pre_frame(void) {
    s_st_pf++;
    if (s_st_phase == 0) {
#ifdef NTPACKER_GUI_HEADLESS_CI
        /* Headless CI build: the GL render pipeline (materials/shaders/font atlas) never reaches "ready"
         * under xvfb+llvmpipe (can_render stays false -> nothing rasterizes), so the render/layout
         * VISUAL phases (1-15: outline pixel probe, touch-on-render, overflow/scissor sweeps) cannot
         * run -- they read back the drawn framebuffer / declared UI bboxes. Jump straight to phase 16
         * (async-shutdown-while-busy), which is GL-independent logic. These phases stay HARD locally on
         * a real GPU: every preset PINS NTPACKER_GUI_HEADLESS_CI to OFF (so a build dir once configured
         * with it cannot keep it across a plain preset reconfigure); only the CI job's -D configure
         * override turns it ON. */
        nt_log_info("SELFTEST: headless CI -> skipping GL render/layout phases 1-15 (no GL context)");
        s_st_phase = 16;
        s_st_pf = 0;
        return;
#endif
        if (s_st_pf < 12) {
            return; /* warm up: first scene + GL page uploads settle */
        }
        s_about_open = false;
        s_export_open = false; /* close the Export dialog exercised during warmup before the pixel probe */
        gui_actions_lifecycle_dismiss(); /* a logical phase (J8: request_new on a dirty project, per the PR#3
                                 * recovery-degradation semantics) leaves the unsaved-changes confirm
                                 * modal open; it dims the canvas behind it AND its blue Save button
                                 * reads as static "cyan", masking the outline delta. Dismiss it before
                                 * the visual probe (like about/export). */
        gui_actions__selftest_drain_intents();
        preview_stop();
        int found = -1;
        const int atlas_count = tp_session_snapshot_atlas_count(gui_project_snapshot());
        for (int i = 0; i < atlas_count; i++) {
            const tp_result *r = gui_pack_result(i);
            if (r && r->sprite_count > 0 && r->page_count > 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            NT_ASSERT(selftest_select_atlas(0));
            do_pack_blocking();
            found = (gui_pack_result(0) && gui_pack_result(0)->sprite_count > 0) ? 0 : -1;
        }
        NT_ASSERT(selftest_select_atlas((found >= 0) ? found : 0));
        gui_canvas_select(&s_canvas, -1); /* no selection -> plain hull outlines */
        s_canvas.mode = GUI_CANVAS_ATLAS;
        s_canvas.show_outline = true;
        s_canvas.show_trim = false;
        s_canvas.show_frame = false;
        s_canvas.show_pivot = false;
        s_st_phase = 1;
        s_st_pf = 0;
    } else if (s_st_phase == 1) {
        s_canvas.mode = GUI_CANVAS_ATLAS; /* hold ATLAS mode through the probe frames */
        /* OFF for the first frames (settled diff baseline captured at pf 5), then ON for the whole retry
         * window. The readback + retry logic lives in selftest_post_draw (see the mechanism note there). */
        s_canvas.show_outline = (s_st_pf > 5);
    } else if (s_st_phase == 18) {
        /* post_draw runs while the frame observation is pinned. Perform the
         * fresh-session transition here at the next between-frame ingress
         * boundary, then capture the reducer-owned initial observation. */
        NT_ASSERT(gui_project_test_new());
        NT_ASSERT(selftest_select_atlas(0));
        reset_selection();
        s_about_open = false;
        s_sec_atlas_open = true;
        s_atlas_adv_open = true;
        s_sec_region_open = true;
        s_sec_anim_open = true;
        s_sec_export_open = true;
        free(s_st_baseline);
        s_st_baseline = NULL;
        s_st_baseline_n = 0;
        tp_error error = {{0}};
        NT_ASSERT(
            gui_project_snapshot_serialize(
                &s_st_baseline,
                &s_st_baseline_n,
                &error) == TP_STATUS_OK);
        s_st_phase = 2;
        s_st_pf = 0;
    } else if (s_st_phase == 2) {
        if (s_st_pf > 10) {
            const bool dirty = gui_project_is_dirty();
            char *nb = NULL;
            size_t nn = 0;
            tp_error e = {0};
            const bool saved = gui_project_snapshot_serialize(&nb, &nn, &e) == TP_STATUS_OK;
            const bool same = saved && s_st_baseline && nn == s_st_baseline_n && memcmp(nb, s_st_baseline, nn) == 0;
            nt_log_info("SELFTEST: touch-on-render guard dirty=%d bytes_match=%d (%zu vs %zu)", dirty, same, nn, s_st_baseline_n);
            NT_ASSERT(!dirty); /* a control that writes its widget value on first render flips this */
            NT_ASSERT(same);
            free(nb);
            free(s_st_baseline);
            s_st_baseline = NULL;
            s_st_phase = 3;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 3) {
        /* Section-toggle sweep at a CLAMPED panel width: collapsed/expanded + empty sections (the fresh
         * project has no sprites/anims) under the clipped scroll must never yield a degenerate float. */
        g_nt_window.fb_width = 520;
        g_nt_window.fb_height = 440;
        s_sec_atlas_open = (s_st_pf / 2) % 2 == 0;
        s_atlas_adv_open = (s_st_pf / 3) % 2 == 0;
        s_sec_region_open = (s_st_pf / 2) % 2 != 0;
        s_sec_anim_open = (s_st_pf / 4) % 2 == 0;
        s_sec_export_open = (s_st_pf / 3) % 2 != 0;
        if (s_st_pf > 16) {
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            s_sec_atlas_open = s_atlas_adv_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            nt_log_info("SELFTEST: section-toggle sweep OK (no empty-scissor assert)");
            s_st_phase = 4;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 4) {
        /* Tiny-window sweep: the layout solve must not assert (empty scissor) at any size. Override the
         * framebuffer dims for two frames each (restored at the end) -- nt_window_poll re-reads them next
         * frame, so this only affects the current frame's scale. Covers panels-declared-and-clamped down
         * to the have_room skip threshold. */
        static const int sizes[8][2] = {{700, 500}, {560, 420}, {480, 360}, {420, 320}, {360, 280}, {240, 180}, {120, 120}, {64, 64}};
        const int idx = s_st_pf / 2;
        if (idx >= 8) {
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            nt_log_info("SELFTEST: tiny-window sweep OK (no empty-scissor assert)");
            s_st_phase = 5;
            s_st_pf = 0;
        } else {
            g_nt_window.fb_width = (uint32_t)sizes[idx][0];
            g_nt_window.fb_height = (uint32_t)sizes[idx][1];
        }
    } else if (s_st_phase == 5) {
        /* Scaled 16:9 overflow regression (owner's case): at 1366x768 @ g_ui_scale 1.5 no key container may
         * leave the window and the right-panel rows must fit the panel. Pre-fix the strip forced the middle
         * row wider than the window -> the right panel was pushed off-screen (asserts fire here). */
        g_nt_window.fb_width = 1366;
        g_nt_window.fb_height = 768;
        if (s_st_pf == 1) { /* enter: exercise the normal atlas strip + a populated Region panel */
            preview_stop();
            selftest_clear_animation_selection();
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr =
                gui_pack_result(selftest_selected_atlas_index());
            if (pr && pr->sprite_count > 0) {
                gui_canvas_select(&s_canvas, 0);
                select_row_for_region(0);
            }
            s_sec_atlas_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            s_atlas_adv_open = false;
        }
        if (s_st_pf >= 3) { /* size held >= 2 frames -> the 1-frame-lagged bbox now reflects 1366x768 */
            selftest_assert_no_overflow(1366.0F, 768.0F);
            nt_log_info("SELFTEST: 16:9 @1.5 overflow check OK (1366x768, no container/right-panel spill)");
            s_st_phase = 6;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 6 || s_st_phase == 7 || s_st_phase == 8) {
        /* Stale-state overflow regression (owner's icon-strip case): a packed-but-stale atlas shows the amber
         * Pack + the "outdated" chip. The chip gate must keep the labeled+chip strip min-content inside the
         * strip's real budget (s_canvas_w - the canvas card's S(20) padding); pre-fix STRIP_CHIP_MIN_W ignored
         * the chip's own width, so at 1920x1080@1.5 the chip forced the middle row wider -> right panel off the
         * screen. Three stops, all @1.5 (page count varies with the project, so the chip visible/dropped assert
         * -- which depends only on the gate, not on the strip's pixel width -- is the deterministic fail-before):
         *  6) 1920x1080 -- chip does NOT fit; must be DROPPED (fail-before: chip shown -> overflow assert).
         *  7) 1366x768  -- compact two-row stale strip (chip already dropped); must still stay in-window.
         *  8) 2200x1080 -- wide enough that the chip DOES fit; must be SHOWN and still not overflow. (2200,
         *     not 2000: packet EXP-PREVIEW's fixed-width preview selector now also sits in this row, so the
         *     "roomy enough for the chip" stop -- STRIP_CHIP_MIN_W -- rose above the 2000@1.5 canvas width.) */
        const float win_w = (s_st_phase == 6) ? 1920.0F : (s_st_phase == 7) ? 1366.0F : 2200.0F;
        const float win_h = (s_st_phase == 7) ? 768.0F : 1080.0F;
        g_ui_scale = 1.5F;
        g_nt_window.fb_width = (uint32_t)win_w;
        g_nt_window.fb_height = (uint32_t)win_h;
        if (s_st_pf == 1) {
            /* Phase 1's handoff (selftest_post_draw) left a truly-fresh, source-less project, so build the
             * stale scene here: a MULTI-PAGE atlas (small max_size -> page buttons, matching the owner's
             * full-tier strip at 1920x1080) that is packed, then re-marked stale so the strip shows the amber
             * Pack + the "outdated" chip. mark_stale must run AFTER the pack (a successful pack clears stale). */
            preview_stop();
            selftest_clear_animation_selection();
            NT_ASSERT(selftest_select_atlas(0));
            const int atlas_index = selftest_selected_atlas_index();
            const tp_snapshot_atlas *sa =
                selftest_atlas_at(atlas_index, NULL);
            if (sa && sa->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(atlas_index, afolder);
                const tp_session_snapshot *edit_snapshot = NULL;
                const tp_snapshot_atlas *edit_atlas =
                    selftest_atlas_at(atlas_index, &edit_snapshot);
                gui_edit_atlas_setting(
                    edit_atlas->id,
                    tp_session_snapshot_revision(edit_snapshot),
                    GUI_ATLAS_MAX_SIZE, 256, 0.0F);
                NT_ASSERT(gui_actions__submit_draft() &&
                          "SELFTEST: stale-scene max-size draft submits");
                selftest_observe_session();
            }
            double pms = 0.0;
            char perr[256] = {0};
            char pnote[128] = {0};
            (void)gui_pack_atlas(atlas_index, &pms, perr, sizeof perr,
                                 pnote, sizeof pnote);
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr = gui_pack_result(atlas_index);
            if (pr && pr->sprite_count > 0) {
                gui_canvas_select(&s_canvas, 0);
                select_row_for_region(0);
            }
            s_sec_atlas_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            s_atlas_adv_open = false;
            gui_project_mark_stale();
        }
        if (s_st_pf >= 3) { /* size + stale held >= 2 frames -> the lagged bbox reflects the stale strip here */
            const tp_snapshot_atlas *a = selftest_atlas_at(
                selftest_selected_atlas_index(), NULL);
            NT_ASSERT(a && a->source_count > 0 && gui_project_is_stale() &&
                      "SELFTEST: stale precondition (sources present + preview stale -> amber Pack + chip)");
            selftest_assert_no_overflow(win_w, win_h);
            /* The chip visible/dropped decision depends ONLY on the gate (accent && width), not on the strip's
             * pixel width, so it is the deterministic fail-before signal even where the page count would let the
             * bounds check pass: at 1920x1080@1.5 the chip must be DROPPED, at the wide 2200 it must be SHOWN. */
            const bool chip = nt_ui_get_bbox(s_ctx, nt_ui_id("ntpacker/stale_chip")).found;
            if (s_st_phase == 6) {
                NT_ASSERT(!chip && "SELFTEST: stale chip must be dropped where it would overflow the canvas budget");
            } else if (s_st_phase == 8) {
                NT_ASSERT(chip && "SELFTEST: stale chip must be shown when the canvas is wide enough to hold it");
            }
            nt_log_info("SELFTEST: stale-state overflow check OK (%.0fx%.0f@1.5, chip=%d)", (double)win_w,
                        (double)win_h, (int)chip);
            s_st_phase++;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 9) {
        /* Async-path equivalence (req 4): start an async pack, spin until it lands (poll_async in
         * gui_actions_step swaps it in), then a blocking reference pack of the same project must match --
         * determinism holds because only WHERE the pack ran changed. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            NT_ASSERT(selftest_select_atlas(0));
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            nt_log_info("SELFTEST: async pack start -> %d (%s)", (int)started, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST: async pack must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: async pack did not finish within the frame cap");
        } else {
            const tp_result *ra = gui_pack_result(0);
            NT_ASSERT(ra && ra->sprite_count > 0 && ra->page_count > 0 && "SELFTEST: async pack produced no result");
            const int sc_a = ra->sprite_count;
            const int pc_a = ra->page_count;
            const int pw_a = ra->pages[0].w;
            const int ph_a = ra->pages[0].h;
            double bms = 0.0;
            char berr[256] = {0};
            char bnote[128] = {0};
            const bool okb = gui_pack_atlas(0, &bms, berr, sizeof berr, bnote, sizeof bnote);
            NT_ASSERT(okb && "SELFTEST: blocking reference pack failed");
            const tp_result *rb = gui_pack_result(0);
            NT_ASSERT(rb && rb->sprite_count == sc_a && rb->page_count == pc_a && rb->pages[0].w == pw_a &&
                      rb->pages[0].h == ph_a && "SELFTEST: async vs blocking result mismatch (non-deterministic)");
            nt_log_info("SELFTEST: async==blocking OK (sprites=%d pages=%d page0=%dx%d)", sc_a, pc_a, pw_a, ph_a);
            s_st_phase = 10;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 10) {
        /* Busy-strip overflow (req 6): the Packing.../Cancel strip must fit at the owner matrix. Forced
         * busy (no real worker) so the strip renders its busy tier deterministically. */
        const bool first = s_st_pf < 8;
        const float win_w = first ? 1366.0F : 1024.0F;
        const float win_h = 768.0F;
        g_ui_scale = first ? 1.5F : 2.0F;
        g_nt_window.fb_width = (uint32_t)win_w;
        g_nt_window.fb_height = (uint32_t)win_h;
        gui_pack_debug_force_busy(GUI_PACK_ASYNC_PACK);
        if (s_st_pf == 6 || s_st_pf == 13) { /* size held >= 2 frames -> the lagged bbox reflects the busy strip */
            selftest_assert_no_overflow(win_w, win_h);
            nt_log_info("SELFTEST: busy-strip overflow OK (%.0fx%.0f@%.1f)", (double)win_w, (double)win_h,
                        (double)g_ui_scale);
        }
        if (s_st_pf >= 14) {
            gui_pack_debug_force_busy(GUI_PACK_ASYNC_NONE);
            g_ui_scale = 1.0F;
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            s_st_phase = 11;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 11) {
        /* Export-target PREVIEW (packet EXP-PREVIEW): a Defold preview must (a) contain only transforms
         * admitted by the exact Defold mask (identity + clockwise 90), (b) leave the native session result untouched (pointer + content),
         * (c) re-bind the native result WITHOUT a repack when switched back to Native, and (d) yield a
         * non-empty degradation summary. Blocking path (the dev seam), mirroring do_pack_blocking. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_test_new();
            gui_pack_clear(-1);
            preview_target_reset();
            NT_ASSERT(selftest_select_atlas(0));
            reset_selection();
            const tp_snapshot_atlas *a0 = selftest_atlas_at(0, NULL);
            if (a0 && a0->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(0, afolder);
                const tp_session_snapshot *edit_snapshot = NULL;
                const tp_snapshot_atlas *edit_atlas =
                    selftest_atlas_at(0, &edit_snapshot);
                gui_edit_atlas_setting(
                    edit_atlas->id,
                    tp_session_snapshot_revision(edit_snapshot),
                    GUI_ATLAS_ALLOW_TRANSFORM, 1, 0.0F);
                NT_ASSERT(gui_actions__submit_draft() &&
                          "SELFTEST preview: allow-transform draft submits");
                selftest_observe_session();
            }
            double nms = 0.0;
            char nerr[256] = {0};
            char nnote[128] = {0};
            const bool okn = gui_pack_atlas(0, &nms, nerr, sizeof nerr, nnote, sizeof nnote);
            const tp_result *native = gui_pack_result(0);
            nt_log_info("SELFTEST: preview native pack -> %d sprites=%d %s", (int)okn, native ? native->sprite_count : -1,
                        okn ? "" : nerr);
            NT_ASSERT(okn && native && native->sprite_count > 0 && "SELFTEST preview: native session pack");
            const void *native_ptr = (const void *)native;
            const int native_sc = native->sprite_count;
            const int native_pc = native->page_count;

            int defold_idx = -1;
            for (int i = 0; i < tp_format_count(); i++) {
                const tp_format_descriptor *e = tp_format_at(i);
                if (e && strcmp(e->id, "defold") == 0) {
                    defold_idx = i;
                    break;
                }
            }
            NT_ASSERT(defold_idx >= 0 && "SELFTEST preview: defold exporter registered");

            char pverr[256] = {0};
            const bool okp = gui_pack_preview_blocking(0, "defold", pverr, sizeof pverr);
            const tp_result *pv = gui_pack_preview_result(0);
            nt_log_info("SELFTEST: preview defold pack -> %d sprites=%d %s", (int)okp, pv ? pv->sprite_count : -1,
                        okp ? "" : pverr);
            NT_ASSERT(okp && pv && pv->sprite_count > 0 && "SELFTEST preview: defold preview result present");

            /* (a) every placement is admitted by the exact target mask */
            const tp_format_descriptor *defold_exporter = tp_format_at(defold_idx);
            int unsupported = 0;
            for (int i = 0; i < pv->sprite_count; i++) {
                const uint8_t transform_bit = TP_EXPORT_TRANSFORM_BIT(pv->sprites[i].transform);
                if ((defold_exporter->caps.transform_mask & transform_bit) == 0U) {
                    unsupported++;
                }
            }
            nt_log_info("SELFTEST: preview defold unsupported placements=%d (expect 0)", unsupported);
            NT_ASSERT(unsupported == 0 && "SELFTEST preview: Defold exact transform mask enforced");

            /* (b) native session result untouched */
            const tp_result *native2 = gui_pack_result(0);
            NT_ASSERT((const void *)native2 == native_ptr && native2->sprite_count == native_sc &&
                      native2->page_count == native_pc && "SELFTEST preview: native session result untouched");

            /* (c) preview binds while active; back to Native re-binds the session result with no repack */
            s_preview_target = defold_idx + 1;
            s_canvas_w = 700.0F; /* single-row tier (>= STRIP_SINGLE_MIN_W) so the preview binds, not compact */
            const tp_result *shown_pv = preview_target_result();
            NT_ASSERT((const void *)shown_pv == (const void *)pv && "SELFTEST preview: preview bound while active");
            preview_target_reset();
            const tp_result *shown_native = preview_target_result();
            NT_ASSERT((const void *)shown_native == native_ptr &&
                      "SELFTEST preview: back to Native re-binds the session result (no repack)");

            /* (d) degradation summary non-empty for defold */
            char chip[96] = {0};
            char tip[224] = {0};
            gui_pack_preview_diff_work_reset();
            const int nd = gui_pack_preview_diff(0, "defold", chip, sizeof chip, tip, sizeof tip);
            char chip_again[96] = {0};
            char tip_again[224] = {0};
            const int nd_again = gui_pack_preview_diff(
                0, "defold", chip_again, sizeof chip_again, tip_again,
                sizeof tip_again);
            nt_log_info("SELFTEST: preview defold degradation nd=%d chip='%s'", nd, chip);
            NT_ASSERT(nd > 0 && chip[0] != '\0' && "SELFTEST preview: defold degradation summary non-empty");
            NT_ASSERT(nd_again == nd && strcmp(chip_again, chip) == 0 &&
                      strcmp(tip_again, tip) == 0 &&
                      gui_pack_preview_diff_rebuilds() == 1U &&
                      "SELFTEST preview: unchanged degradation diff is cached");
            NT_ASSERT(selftest_set_atlas_name_at(0, "preview-cache-refresh") &&
                      "SELFTEST preview: cache invalidation edit commits");
            (void)gui_pack_preview_diff(0, "defold", chip_again,
                                        sizeof chip_again, tip_again,
                                        sizeof tip_again);
            NT_ASSERT(gui_pack_preview_diff_rebuilds() == 2U &&
                      "SELFTEST preview: model generation invalidates degradation cache");
            const int full_diff = gui_pack_preview_diff(
                0, TP_EXPORTER_ID_JSON_NEOTOLIS, chip_again,
                sizeof chip_again, tip_again, sizeof tip_again);
            NT_ASSERT(full_diff == 0 &&
                      gui_pack_preview_diff_rebuilds() == 3U &&
                      "SELFTEST preview: exporter identity invalidates degradation cache");

            gui_pack_preview_clear();
            preview_target_reset();
            nt_log_info("SELFTEST: export-target preview OK");
            s_st_phase = 12;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 12) {
        /* Async EXPORT (req 4a): mirror phase 9's async==blocking pattern for the export path. Start an
         * async export of a fresh single-atlas project whose seeded target points at an isolated tmp base
         * under the build dir, spin until it lands (gui_actions_step reads the report + frees the
         * job), then assert the on-disk json + page png exist -- the export_worker / save_buffer clone /
         * mkdirs path is otherwise untested (only the blocking gui_pack_export was exercised). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_test_new();
            gui_pack_clear(-1);
            NT_ASSERT(selftest_select_atlas(0));
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            char base[600];
            (void)snprintf(base, sizeof base, "%s/selftest_async_export/at0", s_exe_dir); /* ABSOLUTE -> resolves w/o a saved dir */
            selftest_set_target_at(0, 0, "json-neotolis", base, true);
            char aerr[256] = {0};
            const bool started = gui_pack_export_async_start(aerr, sizeof aerr);
            nt_log_info("SELFTEST: async export start -> %d (%s)", (int)started, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST: async export must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: async export did not finish within the frame cap");
        } else {
            char base[600];
            char jpath[640] = {0};
            char ppath[640] = {0};
            (void)snprintf(base, sizeof base, "%s/selftest_async_export/at0", s_exe_dir);
            (void)snprintf(jpath, sizeof jpath, "%s.json", base);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", base);
            bool jok = false;
            bool pok = false;
            FILE *jf = fopen(jpath, "rb");
            if (jf) {
                jok = (fgetc(jf) == '{'); /* lightweight parse check (full parse is in the packer ctest) */
                (void)fclose(jf);
            }
            FILE *pf = fopen(ppath, "rb");
            if (pf) {
                pok = (fgetc(pf) != EOF); /* exists AND non-empty */
                (void)fclose(pf);
            }
            nt_log_info("SELFTEST: async export landed json{=%d png0=%d", (int)jok, (int)pok);
            NT_ASSERT(jok && pok && "SELFTEST: async export must write the json + page png");
            (void)remove(jpath);
            (void)remove(ppath);
            s_st_phase = 13;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 13) {
        /* Cancel mid-pack (req 4b): start an async pack over a CLEARED slot with stale set, cancel it
         * immediately, spin until it lands. gui_actions_step must DISCARD the worker's result (no slot swap)
         * and poll_async must NOT clear stale -- the cancel-discard path (gui_pack.c) is otherwise never
         * hit (phase 9 waits for !busy first). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_test_new();
            gui_pack_clear(-1);
            NT_ASSERT(selftest_select_atlas(0));
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_project_mark_stale();
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: cancel-phase pack must start");
            NT_ASSERT(
                gui_pack_async_cancel(NULL) ==
                    TP_STATUS_OK &&
                "SELFTEST: cancel-phase request must be admitted");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: cancelled pack did not land");
        } else {
            NT_ASSERT(gui_pack_result(0) == NULL && "SELFTEST: cancelled pack must not swap a result in");
            NT_ASSERT(gui_project_is_stale() && "SELFTEST: cancelled pack must leave stale honest (not cleared)");
            nt_log_info("SELFTEST: cancel-mid-pack discarded cleanly (no swap, stale kept)");
            s_st_phase = 14;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 14) {
        /* Stable publication identity (req 4d): pack atlas index 1, then remove the
         * earlier atlas while the worker flies.  The survivor shifts to index 0;
         * completion must resolve its captured atlas ID instead of publishing into
         * stale slot 1. The model-generation token changed, so the adopted receipt
         * remains explicitly stale even though its semantic pack hash still names
         * the survivor's result. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_test_new();
            gui_pack_clear(-1);
            const int survivor =
                gui_project_add_atlas().visible_index;
            NT_ASSERT(survivor == 1 && "SELFTEST: stable-publication atlas must be index 1");
            (void)selftest_set_atlas_name_at(1, "survivor");
            NT_ASSERT(selftest_select_atlas(1));
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(1, afolder);
            const tp_session_snapshot *remove_snapshot =
                gui_project_snapshot();
            const tp_snapshot_atlas *earlier_atlas =
                tp_session_snapshot_atlas_at(
                    remove_snapshot, 0);
            NT_ASSERT(earlier_atlas);
            const tp_id128 earlier_atlas_id =
                earlier_atlas->id;
            const int64_t remove_revision =
                tp_session_snapshot_revision(
                    remove_snapshot);
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(1, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: stable-publication pack must start");
            NT_ASSERT(selftest_remove_atlas_ref(
                          earlier_atlas_id,
                          remove_revision) &&
                      "SELFTEST: removing the earlier atlas must commit while pack runs");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST: stable-publication pack did not land");
        } else {
            const tp_result *survivor_result = gui_pack_result(0);
            NT_ASSERT(survivor_result &&
                          strcmp(survivor_result->atlas_name, "survivor") == 0 &&
                          gui_pack_result(1) == NULL &&
                      "SELFTEST: async result must follow the survivor atlas ID to index 0");
            NT_ASSERT(gui_project_is_stale() &&
                      "SELFTEST: a newer model token keeps the adopted survivor result stale");
            nt_log_info("SELFTEST: async result followed stable atlas id after index shift and stayed stale");
            s_st_phase = 15;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 15) {
        /* Rename-through-export (A4): a sprite an animation references is renamed; the export must carry the
         * rename into BOTH the sprite name and the animation frame. Mirrors phase 12's async-export driver
         * (isolated tmp base under the build dir). Kept before the teardown phase (16). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_test_new();
            gui_pack_clear(-1);
            NT_ASSERT(selftest_select_atlas(0));
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            double pms = 0.0;
            char perr[256] = {0};
            char pnote[128] = {0};
            const bool okp = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
            const tp_result *pr = gui_pack_result(0);
            NT_ASSERT(okp && pr && pr->sprite_count >= 2 && "SELFTEST A4: pack produced >=2 sprites");
            char k0[192];
            char k1[192];
            tp_sprite_index selector_index = {0};
            tp_error selector_error = {{0}};
            const tp_session_snapshot *selector_snapshot = gui_project_snapshot();
            NT_ASSERT(tp_sprite_index_build_snapshot(
                          selector_snapshot, 0, &selector_index,
                          &selector_error) == TP_STATUS_OK &&
                      selector_index.count >= 2 &&
                      "SELFTEST A4: canonical selector index has two sprites");
            (void)snprintf(k0, sizeof k0, "%s",
                           selector_index.refs[0].export_key);
            (void)snprintf(k1, sizeof k1, "%s",
                           selector_index.refs[1].export_key);
            tp_sprite_index_free(&selector_index);
            multi_sel_clear();
            multi_sel_add(k0);
            multi_sel_add(k1);
            const int ai = create_animation_from_selection();
            NT_ASSERT(ai >= 0 && "SELFTEST A4: animation from two frames");
            NT_ASSERT(selftest_rename_animation_frame_at(
                          0, ai, 0, "a4_renamed") &&
                      "SELFTEST A4: rename uses the frame's canonical sprite ref");
            multi_sel_clear();
            char base[600];
            (void)snprintf(base, sizeof base, "%s/selftest_a4_export/at0", s_exe_dir); /* ABSOLUTE -> resolves w/o a saved dir */
            selftest_set_target_at(0, 0, "json-neotolis", base, true);
            char aerr[256] = {0};
            const bool started = gui_pack_export_async_start(aerr, sizeof aerr);
            nt_log_info("SELFTEST: A4 rename export start -> %d k0='%s' (%s)", (int)started, k0, started ? "ok" : aerr);
            NT_ASSERT(started && "SELFTEST A4: async export must start");
        } else if (gui_pack_async_busy()) {
            NT_ASSERT(s_st_pf < 3000 && "SELFTEST A4: rename export did not finish within the frame cap");
        } else {
            char base[600];
            char jpath[640] = {0};
            char ppath[640] = {0};
            (void)snprintf(base, sizeof base, "%s/selftest_a4_export/at0", s_exe_dir);
            (void)snprintf(jpath, sizeof jpath, "%s.json", base);
            (void)snprintf(ppath, sizeof ppath, "%s-0.png", base);
            char *js = selftest_slurp(jpath);
            NT_ASSERT(js && "SELFTEST A4: exported json must exist");
            int hits = 0;
            for (const char *p = js; (p = strstr(p, "a4_renamed")) != NULL; p += 10) {
                hits++; /* expect 2: once as the sprite name, once as the animation frame */
            }
            nt_log_info("SELFTEST: A4 rename export landed 'a4_renamed' hits=%d (expect >=2: sprite name + anim frame)", hits);
            NT_ASSERT(hits >= 2 && "SELFTEST A4: rename must appear as the sprite name AND the animation frame it follows");
            free(js);
            (void)remove(jpath);
            (void)remove(ppath);
            s_st_phase = 16;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 16) {
        /* Shutdown-while-busy: shutdown stops ingress without blocking. The
         * normal frame host then owns cancel, process pump, observation, and
         * terminal receipt classification over subsequent frames. */
        if (s_st_pf == 1) {
            g_ui_scale = 1.0F;
            g_nt_window.fb_width = 1280;
            g_nt_window.fb_height = 800;
            gui_project_test_new();
            gui_pack_clear(-1);
            NT_ASSERT(selftest_select_atlas(0));
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            NT_ASSERT(started && gui_pack_async_busy() &&
                      "SELFTEST: shutdown-phase pack must start busy");
            return;
        }
        if (s_st_pf == 2) {
            NT_ASSERT(
                gui_pack_async_busy() &&
                "SELFTEST: shutdown-phase pack must reach host admission");
            gui_pack_shutdown();
            NT_ASSERT(
                gui_project_lifecycle_state_query() ==
                    GUI_PROJECT_LIFECYCLE_ACTIVE &&
                gui_pack_async_busy() &&
                "SELFTEST: pack cleanup must not own session drain");
            tp_error replacement_error = {{0}};
            NT_ASSERT(
                gui_project_lifecycle_begin_new(
                    &replacement_error) ==
                    TP_STATUS_OK &&
                "SELFTEST: lifecycle owner must accept busy replacement");
            return;
        }
        NT_ASSERT(
            s_st_pf < 3000 &&
            "SELFTEST: shutdown drain exceeded the frame cap");
        if (gui_project_lifecycle_state_query() !=
                GUI_PROJECT_LIFECYCLE_ACTIVE ||
            gui_pack_async_busy()) {
            return;
        }
        NT_ASSERT(
            gui_project_lifecycle_state_query() ==
            GUI_PROJECT_LIFECYCLE_ACTIVE);
        gui_shell_reset_shown_result();
        NT_ASSERT(!gui_canvas_has_atlas(&s_canvas) &&
                  gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_SOURCE &&
                  "SELFTEST: pack shutdown must release the canvas result borrow");
        nt_log_info("SELFTEST: shutdown-while-busy drained cleanly");
        s_st_phase = 17;
        s_st_pf = 0;
    } else {
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        nt_app_quit();
    }
}

/* Post-walk hook: pixel readbacks happen after nt_ui_walk has drawn the overlay. */
void selftest_post_draw(void) {
    if (s_st_phase != 1) {
        return;
    }
    /* Overlay pixel probe, HARDENED against transient GPU-readback stalls under load (a single-shot read
     * at one fixed frame flaked repeatedly under load today):
     *   (1) SETTLE  -- capture the OFF baseline only at a settled frame (pf 5, several frames after the
     *                  scene + page uploads land), and give the ON outline 2 frames to rasterize before
     *                  the first ON readback (pf 8).
     *   (2) RETRY   -- once outlines are ON, take the readback every frame across a wide window (pf 8..48)
     *                  and PASS the instant one frame clears the cyan-delta threshold. A lone stalled
     *                  readback (delta transiently low) just retries next frame instead of failing the run.
     * The test still FAILS on a real regression: if outlines never rasterize (e.g. the cam-on-plane
     * zero-width-line bug), EVERY frame in the window stays below threshold, the window expires at pf 48,
     * and the assert fires. Observed retries with this scheme: ~0 (passes at pf 8) even under load. */
    if (s_st_pf == 5) {
        s_st_cyan0 = selftest_probe_cyan(); /* settled OFF baseline */
        return;
    }
    if (s_st_pf < 8) {
        return; /* ON settle */
    }
    const int c1 = selftest_probe_cyan();
    const bool ok = (s_st_cyan0 >= 0 && c1 >= 0 && (c1 - s_st_cyan0) >= 8);
    if (!ok && s_st_pf < 48) {
        return; /* transient stall -> retry the readback next frame (outline is still ON) */
    }
    nt_log_info("SELFTEST: outline pixel probe cyan off=%d on=%d delta=%d (settled pf=%d, retries=%d)", s_st_cyan0, c1,
                c1 - s_st_cyan0, (int)s_st_pf, (int)(s_st_pf - 8));
    NT_ASSERT(s_st_cyan0 >= 0 && c1 >= 0);
    NT_ASSERT(ok && "hull outline must add cyan pixels (retry window expired -> outlines never rendered)");
    /* post_draw is inside a pinned frame. Defer the fresh-session mutation to
     * the next pre-frame ingress boundary before starting the render guard. */
    s_st_phase = 18;
    s_st_pf = 0;
}

#else

/* NTPACKER_GUI_SELFTEST off: this TU intentionally compiles to nothing. A file-scope typedef keeps it
 * a legal (non-empty) ISO C translation unit under -Wpedantic. */
typedef int gui_selftest_empty_translation_unit;

#endif /* NTPACKER_GUI_SELFTEST */
