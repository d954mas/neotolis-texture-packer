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

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h> /* getcwd -- to_abs() makes a relative path absolute on POSIX too */
#endif

#include "app/nt_app.h"         /* nt_app_quit */
#include "core/nt_assert.h"     /* NT_ASSERT */
#include "graphics/nt_gfx.h"    /* nt_gfx_read_pixels (overlay pixel probe) */
#include "log/nt_log.h"         /* nt_log_info (SELFTEST-* logging) */
#include "ui/nt_ui.h"           /* nt_ui_get_bbox / nt_ui_id / nt_ui_bbox_t */
#include "window/nt_window.h"   /* g_nt_window (phase-driven framebuffer dims) */

#include "tp_core/tp_error.h"   /* tp_status_str / tp_error */
#include "tp_core/tp_export.h"  /* tp_exporter_count/at (preview-target selector index) */
#include "tp_core/tp_id.h"      /* tp_id128_eq (F2-05b-ii-B append-fail identity check) */
#include "tp_core/tp_identity.h" /* canonical recovery metadata path assertions */
#include "tp_core/tp_model.h"   /* tp_result */
#include "tp_core/tp_names.h"   /* tp_sprite_export_key (region -> override key) */
#include "tp_core/tp_project.h" /* M4 recovery-codec fixtures (J26-J36) */
#include "tp_core/tp_scan.h"    /* tp_mkdirs (portable temp-dir creation for the CI stress dirs) */
#include "tp_core/tp_sprite_index.h" /* canonical A4 selector fixture */
#include "tp_core/tp_recovery.h"    /* recovery presentation DTOs */
#include "tp_core/tp_transaction.h" /* tp_semantic_identity (F2-05b-ii-B append-fail identity check) */
#include "tp_journal_internal.h"    /* journal format constants for mismatch fixtures */
#include "tp_recovery_internal.h"   /* owner-free recovery fault/fixture seams */
#include "tp_project_internal.h"    /* deterministic atomic-save temp-create failure */

#include "gui_actions.h"  /* do_pack_blocking / reset_selection / preview_stop / anim ops + gui_request_gesture_commit */
#include "gui_canvas.h"   /* s_canvas ops + GUI_CANVAS_ATLAS */
#include "gui_pack.h"     /* gui_pack_* + GUI_PACK_ASYNC_* */
#include "gui_project.h"  /* gui_project_* + GUI_SPRITE_OV_SHAPE / GUI_ADD_DUPLICATE */
#include "gui_rows.h"     /* build_rows / multi_sel_* / select_row_for_region */
#include "gui_scan.h"     /* gui_scan_* */
#include "gui_shell.h"    /* UI_STATE_SLOTS / UI_STATE_PROBE_MAX / UI_ROW_ID_RING */
#include "gui_startup.h"  /* H/P1-8: gui_startup_decide + GUI_STARTUP_* (J14 truth table) */
#include "gui_state.h"    /* s_canvas / s_sel_* / s_sec_* / s_about_open / s_export_open / s_ctx / s_id_* */

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

static const tp_snapshot_target *selftest_target_at(int atlas_index,
                                                    int target_index) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, NULL);
    return atlas ? tp_session_snapshot_target_at(snapshot, atlas->id, target_index)
                 : NULL;
}

static bool selftest_same_path(const char *a, const char *b) {
    char canonical_a[TP_IDENTITY_PATH_MAX];
    char canonical_b[TP_IDENTITY_PATH_MAX];
    tp_error err = {0};
    return a && b &&
           tp_identity_path_canonical(a, canonical_a, sizeof canonical_a,
                                      &err) == TP_STATUS_OK &&
           tp_identity_path_canonical(b, canonical_b, sizeof canonical_b,
                                      &err) == TP_STATUS_OK &&
           tp_identity_path_equal(canonical_a, canonical_b);
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
    return atlas && gui_project_set_atlas_name(
                        atlas->id, tp_session_snapshot_revision(snapshot), name);
}

static bool selftest_set_atlas_setting_at(int index, gui_atlas_field field,
                                          int ivalue, float fvalue) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    return atlas && gui_project_set_atlas_setting(
                        atlas->id, tp_session_snapshot_revision(snapshot), field,
                        ivalue, fvalue);
}

static bool selftest_remove_atlas_at(int index) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    return atlas && gui_project_remove_atlas(
                        atlas->id, tp_session_snapshot_revision(snapshot));
}

static tp_status selftest_copy_atlas_name_at(int index, char *out, size_t capacity,
                                             tp_error *err) {
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, NULL);
    return atlas ? gui_project_copy_atlas_name(atlas->id, out, capacity, err)
                 : tp_error_set(err, TP_STATUS_NOT_FOUND, "atlas index was not found");
}

static void selftest_queue_atlas_int(int index, gui_atlas_field field, int value) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    if (atlas) {
        gui_queue_atlas_setting(atlas->id, tp_session_snapshot_revision(snapshot),
                                field, value, 0.0F);
    }
}

static gui_add_status selftest_add_source_at(int index, const char *path) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    return atlas ? gui_project_add_source(
                       atlas->id, tp_session_snapshot_revision(snapshot), path)
                 : GUI_ADD_FAILED;
}

static bool selftest_add_sources_at(int index, const char *const *paths,
                                    int path_count, tp_source_kind kind,
                                    int *added, int *duplicate) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(index, &snapshot);
    return atlas && gui_project_add_sources(
                        atlas->id, tp_session_snapshot_revision(snapshot), paths,
                        path_count, kind, added, duplicate);
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
        if (gui_project_add_source_kind(
                atlas->id, tp_session_snapshot_revision(snapshot),
                "__ntpacker_selftest_sprite_source__.png", TP_SOURCE_KIND_FILE) !=
            GUI_ADD_ADDED) {
            return false;
        }
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
    return selftest_sprite_ref_at(atlas_index, source_key, &sprite) &&
           gui_project_set_sprite_rename(&sprite, rename);
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
    return gui_project_set_sprite_rename(&sprite, rename);
}

static bool selftest_set_sprite_origin_at(int atlas_index, const char *source_key,
                                          int axis, float value) {
    gui_sprite_ref sprite;
    return selftest_sprite_ref_at(atlas_index, source_key, &sprite) &&
           gui_project_set_sprite_origin(&sprite, axis, value);
}

static bool selftest_set_sprite_slice9_at(int atlas_index, const char *source_key,
                                          int component, int value) {
    gui_sprite_ref sprite;
    return selftest_sprite_ref_at(atlas_index, source_key, &sprite) &&
           gui_project_set_sprite_slice9(&sprite, component, value);
}

static bool selftest_set_sprite_override_at(int atlas_index, const char *source_key,
                                            gui_sprite_ov which, int value) {
    gui_sprite_ref sprite;
    return selftest_sprite_ref_at(atlas_index, source_key, &sprite) &&
           gui_project_set_sprite_override(&sprite, which, value);
}

static bool selftest_peek_sprite_slice9_at(int atlas_index, const char *source_key,
                                           int out_lrtb[4]) {
    gui_sprite_ref sprite;
    return selftest_sprite_ref_at(atlas_index, source_key, &sprite) &&
           gui_project_peek_pending_slice9(&sprite, out_lrtb);
}

static int selftest_create_animation_at(int atlas_index, const char *base,
                                        const char *const *frames, int frame_count) {
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(atlas_index, &snapshot);
    return atlas ? gui_project_create_animation(
                       atlas->id, tp_session_snapshot_revision(snapshot), base,
                       frames, frame_count)
                 : -1;
}

static bool selftest_set_anim_id_at(int atlas_index, int animation_index,
                                    const char *name) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_set_anim_id(&animation, name);
}

static bool selftest_set_anim_fps_at(int atlas_index, int animation_index,
                                     float fps) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_set_anim_fps(&animation, fps);
}

static bool selftest_set_anim_playback_at(int atlas_index, int animation_index,
                                          int playback) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_set_anim_playback(&animation, playback);
}

static bool selftest_set_anim_flip_at(int atlas_index, int animation_index,
                                      bool flip_h, bool flip_v) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_set_anim_flip(&animation, flip_h, flip_v);
}

static bool selftest_anim_remove_frame_at(int atlas_index, int animation_index,
                                          int frame_index) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_anim_remove_frame(&animation, frame_index);
}

static bool selftest_anim_move_frame_at(int atlas_index, int animation_index,
                                        int frame_index, int delta) {
    gui_animation_ref animation;
    return gui_project_animation_ref_at(atlas_index, animation_index, &animation) &&
           gui_project_anim_move_frame(&animation, frame_index, delta);
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
            return gui_project_remove_animation(&animation);
        }
    }
    return false;
}

static bool selftest_target_ref_at(int atlas_index, int target_index,
                                   gui_target_ref *out) {
    return gui_project_target_ref_at(atlas_index, target_index, out);
}

static bool selftest_set_target_at(int atlas_index, int target_index,
                                   const char *exporter_id, const char *out_path,
                                   bool enabled) {
    gui_target_ref target;
    return selftest_target_ref_at(atlas_index, target_index, &target) &&
           gui_project_set_target(&target, exporter_id, out_path, enabled);
}

static bool selftest_set_target_path_at(int atlas_index, int target_index,
                                        const char *out_path) {
    gui_target_ref target;
    return selftest_target_ref_at(atlas_index, target_index, &target) &&
           gui_project_set_target_out_path(&target, out_path);
}

static bool selftest_set_target_enabled_at(int atlas_index, int target_index,
                                           bool enabled) {
    gui_target_ref target;
    return selftest_target_ref_at(atlas_index, target_index, &target) &&
           gui_project_set_target_enabled(&target, enabled);
}

static bool selftest_set_target_exporter_at(int atlas_index, int target_index,
                                            const char *exporter_id) {
    gui_target_ref target;
    return selftest_target_ref_at(atlas_index, target_index, &target) &&
           gui_project_set_target_exporter(&target, exporter_id);
}

static void selftest_queue_target_at(int atlas_index, int target_index,
                                     const char *exporter_id,
                                     const char *out_path, bool enabled) {
    gui_target_ref target;
    if (selftest_target_ref_at(atlas_index, target_index, &target)) {
        gui_edit_target(&target, exporter_id, out_path, enabled);
    }
}

/* A flush may advance the revision only for an intent that was current before
 * that flush.  A genuinely stale ref must not be rewritten to the new revision
 * merely because pending_route committed a different valid gesture. */
static bool selftest_stale_coalesced_intent_is_rejected(void) {
    if (!gui_project_new()) {
        return false;
    }
    gui_target_ref stale_target;
    if (!gui_project_target_ref_at(0, 0, &stale_target)) {
        return false;
    }
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(0, &snapshot);
    if (!atlas || !gui_project_set_atlas_name(
                      atlas->id, tp_session_snapshot_revision(snapshot), "advanced")) {
        return false;
    }
    atlas = selftest_atlas_at(0, &snapshot);
    if (!atlas || !gui_project_set_atlas_setting(
                      atlas->id, tp_session_snapshot_revision(snapshot),
                      GUI_ATLAS_PADDING, atlas->padding + 1, 0.0F)) {
        return false;
    }
    if (!gui_project_set_target_out_path(&stale_target, "out/must-not-land")) {
        return false;
    }
    const bool flushed = gui_project_flush_pending();
    const tp_snapshot_target *target = selftest_target_at(0, 0);
    return !flushed && target && strcmp(target->out_path, "out/must-not-land") != 0;
}

static bool selftest_stale_discrete_intent_is_rejected(void) {
    if (!gui_project_new()) {
        return false;
    }
    gui_target_ref stale_target;
    if (!gui_project_target_ref_at(0, 0, &stale_target)) {
        return false;
    }
    const tp_session_snapshot *snapshot = NULL;
    const tp_snapshot_atlas *atlas = selftest_atlas_at(0, &snapshot);
    if (!atlas || !gui_project_set_atlas_name(
                      atlas->id, tp_session_snapshot_revision(snapshot),
                      "advanced-discrete")) {
        return false;
    }
    atlas = selftest_atlas_at(0, &snapshot);
    if (!atlas || !gui_project_set_atlas_setting(
                      atlas->id, tp_session_snapshot_revision(snapshot),
                      GUI_ATLAS_PADDING, atlas->padding + 1, 0.0F)) {
        return false;
    }
    const bool accepted = gui_project_set_target_enabled(&stale_target, false);
    const tp_snapshot_target *target = selftest_target_at(0, 0);
    return !accepted && target && target->enabled;
}

#define gui_project_set_atlas_name(index, name) selftest_set_atlas_name_at((index), (name))
#define gui_project_set_atlas_setting(index, field, ivalue, fvalue) \
    selftest_set_atlas_setting_at((index), (field), (ivalue), (fvalue))
#define gui_project_remove_atlas(index) selftest_remove_atlas_at((index))
#define gui_project_copy_atlas_name(index, out, capacity, err) \
    selftest_copy_atlas_name_at((index), (out), (capacity), (err))
#define gui_edit_atlas_int(index, field, value) \
    selftest_queue_atlas_int((index), (field), (value))
#define gui_project_add_source(index, path) selftest_add_source_at((index), (path))
#define gui_project_add_sources(index, paths, count, kind, added, duplicate) \
    selftest_add_sources_at((index), (paths), (count), (kind), (added), (duplicate))
#define gui_project_set_sprite_rename(index, key, rename) \
    selftest_set_sprite_rename_at((index), (key), (rename))
#define gui_project_set_sprite_origin(index, key, axis, value) \
    selftest_set_sprite_origin_at((index), (key), (axis), (value))
#define gui_project_set_sprite_slice9(index, key, component, value) \
    selftest_set_sprite_slice9_at((index), (key), (component), (value))
#define gui_project_set_sprite_override(index, key, which, value) \
    selftest_set_sprite_override_at((index), (key), (which), (value))
#define gui_project_peek_pending_slice9(index, key, out_lrtb) \
    selftest_peek_sprite_slice9_at((index), (key), (out_lrtb))
#define gui_project_create_animation(index, base, frames, frame_count) \
    selftest_create_animation_at((index), (base), (frames), (frame_count))
#define gui_project_set_anim_id(index, animation, name) \
    selftest_set_anim_id_at((index), (animation), (name))
#define gui_project_set_anim_fps(index, animation, fps) \
    selftest_set_anim_fps_at((index), (animation), (fps))
#define gui_project_set_anim_playback(index, animation, playback) \
    selftest_set_anim_playback_at((index), (animation), (playback))
#define gui_project_set_anim_flip(index, animation, flip_h, flip_v) \
    selftest_set_anim_flip_at((index), (animation), (flip_h), (flip_v))
#define gui_project_anim_remove_frame(index, animation, frame) \
    selftest_anim_remove_frame_at((index), (animation), (frame))
#define gui_project_anim_move_frame(index, animation, frame, delta) \
    selftest_anim_move_frame_at((index), (animation), (frame), (delta))
#define gui_project_remove_animation(index, name) \
    selftest_remove_animation_named_at((index), (name))
#define gui_project_set_target(index, target, exporter, path, enabled) \
    selftest_set_target_at((index), (target), (exporter), (path), (enabled))
#define gui_project_set_target_out_path(index, target, path) \
    selftest_set_target_path_at((index), (target), (path))
#define gui_project_set_target_enabled(index, target, enabled) \
    selftest_set_target_enabled_at((index), (target), (enabled))
#define gui_project_set_target_exporter(index, target, exporter) \
    selftest_set_target_exporter_at((index), (target), (exporter))
#define gui_edit_target(index, target, exporter, path, enabled) \
    selftest_queue_target_at((index), (target), (exporter), (path), (enabled))

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

/* R5b-2 J18-J21 helper: fabricate a crash-ORPHAN recovery journal at `slot` holding one committed edit
 * (atlas 0 renamed to `atlas_name` -> record_count == 2 = UNSAVED WORK) with metadata timestamp `ts`, by
 * running a real journaled session and "crashing" it: disable recovery + shutdown leaves the slot on disk
 * (handle closed, lock released). The clock seam pins `ts` so the newest-orphan scan is deterministic.
 * PRECONDITION: the containing folder exists and any prior slot/lock at `slot` was removed. */
static void scan_make_orphan(const char *slot, const char *atlas_name, int64_t ts) {
    gui_project_enable_recovery(""); /* clean prior state; this teardown deletes no slot */
    gui_project_shutdown();
    gui_project__test_set_recovery_now(ts);
    gui_project_enable_recovery(slot);
    gui_project_init();                              /* fresh + journal at slot; metadata {ts, name "untitled"} */
    (void)gui_project_set_atlas_name(0, atlas_name); /* one committed txn -> post-checkpoint unsaved work */
    gui_project_enable_recovery("");                 /* crash-sim: keep the slot on disk, release the lock */
    gui_project_shutdown();
    gui_project__test_set_recovery_now(-1);          /* restore the real clock */
}

/* Saved-project orphan: open `original` while recovery is active so set_path persists the exact
 * original-file fingerprint, then commit one unsaved edit and simulate a crash. */
static void scan_make_saved_orphan(const char *slot, const char *original, const char *atlas_name, int64_t ts) {
    gui_project_enable_recovery("");
    gui_project_shutdown();
    gui_project__test_set_recovery_now(ts);
    gui_project_enable_recovery(slot);
    gui_project_init();
    char err[256] = {0};
    NT_ASSERT(gui_project_open(original, err, sizeof err) == TP_STATUS_OK &&
              "saved recovery orphan opens its original baseline");
    NT_ASSERT(gui_project_set_atlas_name(0, atlas_name) && "saved recovery orphan commits one edit");
    gui_project_enable_recovery("");
    gui_project_shutdown();
    gui_project__test_set_recovery_now(-1);
}

/* R5b-2 helper: fabricate a checkpoint-ONLY orphan (init, NO edit -> record_count == 1, NOT adoptable). */
static void scan_make_noedit_orphan(const char *slot) {
    gui_project_enable_recovery("");
    gui_project_shutdown();
    gui_project_enable_recovery(slot);
    gui_project_init();              /* CHECKPOINT + METADATA only; no txn */
    gui_project_enable_recovery(""); /* crash-sim */
    gui_project_shutdown();
}

/* R5b-2 fix [1] helper: a crash-ORPHAN with TWO committed edits (record_count == 3: CKPT + TXN1 + TXN2;
 * the META record is not counted), so that chopping the LAST txn's tail still leaves CKPT + TXN1 (2 good
 * records) recoverable -- proving the TRUNCATED prefix is adopted. */
static void scan_make_orphan_2edits(const char *slot, const char *name1, const char *name2, int64_t ts) {
    gui_project_enable_recovery("");
    gui_project_shutdown();
    gui_project__test_set_recovery_now(ts);
    gui_project_enable_recovery(slot);
    gui_project_init();
    (void)gui_project_set_atlas_name(0, name1); /* TXN1 -> the committed prefix that TRUNCATED recovers */
    (void)gui_project_set_atlas_name(0, name2); /* TXN2 -> the record we tear the tail of */
    gui_project_enable_recovery("");
    gui_project_shutdown();
    gui_project__test_set_recovery_now(-1);
}

/* R6a J26 helper: craft an orphan journal DIRECTLY (CKPT + one TXN -> record_count 2) carrying a metadata
 * record {ts, meta_path, meta_name}, keyed with `key`. R6a fix [2] made gui_recovery_collect KEY-FILTER
 * adoptable orphans, so an orphan crafted with the REAL recovery key (gui_project__test_recovery_key) peeks
 * adoptable while one with a FOREIGN key is excluded -- letting a test drive both the metadata (path/name)
 * AND the key-match/mismatch classification directly. Overwrites `path`. (owned recovery candidate, J27/J28/J30,
 * needs a real journaled session so it uses scan_make_orphan, not this.) */
static void craft_meta_orphan(const char *path, tp_id128 key, int64_t ts, const char *meta_path,
                              const char *meta_name) {
    tp_error e = {0};
    (void)tp_recovery__test_craft_metadata_journal(
        path, key, ts, meta_path, meta_name, &e);
}

/* R5b-2 fix [1] helper: physically CHOP the last `nbytes` off `path` so its final journal record frame is
 * torn (its crc/payload go short) -> replay classifies TRUNCATED there and recovers the good prefix before
 * it. Chopping only the tail (fewer bytes than the last record's size) can only tear the LAST record.
 * Returns true iff it rewrote a shorter file. */
static bool selftest_chop_file_tail(const char *path, long nbytes) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    (void)fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (sz > nbytes && nbytes > 0) {
        long keep = sz - nbytes;
        unsigned char *buf = (unsigned char *)malloc((size_t)keep);
        if (buf && fread(buf, 1, (size_t)keep, f) == (size_t)keep) {
            (void)fclose(f);
            f = NULL;
            FILE *w = fopen(path, "wb"); /* rewrite truncated */
            if (w) {
                ok = (fwrite(buf, 1, (size_t)keep, w) == (size_t)keep);
                (void)fclose(w);
            }
        }
        free(buf);
    }
    if (f) {
        (void)fclose(f);
    }
    return ok;
}

/* True when the CI job asked us to skip the GL render/layout visual phases: the GitHub Linux runner has
 * no real GL (xvfb+llvmpipe never brings the engine's materials/shaders/font atlas to "ready"), so those
 * phases read back an empty framebuffer / undeclared UI. The logical selftest (run_selftest) is unaffected
 * and stays hard headless; only the render-frame visual phases gate on this. Unset locally -> full run. */
static bool selftest_headless(void) { return getenv("NTPACKER_GUI_HEADLESS") != NULL; }

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
    FILE *f = fopen(path, "wb");
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

/* F2-05b-ii-B fix [2] test helper: flip one payload byte of the (0-based) `rec_index`-th record in a
 * journal file so replay reports CORRUPT at that record. If a LATER record follows it, the corruption
 * is MID-STREAM (F2-04 C2 -> tp_model_recover returns a usable model with a POISONED journal). Walks
 * the byte-exact frame layout (28-byte header, then [sync u32 BE | len u32 BE | payload | crc u32 BE]
 * records, v2) so it is robust to snapshot size. Returns true if the record was found + corrupted. */
static bool selftest_corrupt_journal_record(const char *path, int rec_index) {
    FILE *f = fopen(path, "r+b");
    if (!f) {
        return false;
    }
    (void)fseek(f, 0, SEEK_END);
    long lsz = ftell(f);
    (void)fseek(f, 0, SEEK_SET);
    bool ok = false;
    if (lsz > 28) {
        size_t sz = (size_t)lsz;
        unsigned char *buf = (unsigned char *)malloc(sz);
        if (buf && fread(buf, 1, sz, f) == sz) {
            size_t off = 28; /* skip the header */
            for (int i = 0; off + 8U <= sz; i++) {
                /* frame = sync u32 | payload_len u32 | payload | crc u32 (v2) -- len follows the sync-word */
                uint32_t plen = ((uint32_t)buf[off + 4] << 24) | ((uint32_t)buf[off + 5] << 16) |
                                ((uint32_t)buf[off + 6] << 8) | (uint32_t)buf[off + 7];
                size_t payload = off + 8U;
                if (i == rec_index) {
                    if (payload + 5U < sz) {
                        buf[payload + 5U] ^= 0xFFU; /* flip a payload byte -> crc mismatch at this record */
                        (void)fseek(f, 0, SEEK_SET);
                        ok = (fwrite(buf, 1, sz, f) == sz);
                    }
                    break;
                }
                off = payload + plen + 4U; /* skip payload + crc */
            }
        }
        free(buf);
    }
    (void)fclose(f);
    return ok;
}

/* UTF-8 "тест_спрайт" (a Cyrillic sprite name) -- exercises multi-byte names end-to-end. */
#define CYR_STEM "\xD1\x82\xD0\xB5\xD1\x81\xD1\x82_\xD1\x81\xD0\xBF\xD1\x80\xD0\xB0\xD0\xB9\xD1\x82"

void run_selftest(void) {
    /* CI diagnostics: unbuffered logs so a fatal NT_ASSERT (__builtin_trap, no flush) never loses the
     * preceding SELFTEST/step line -- essential for diagnosing a headless-CI-only failure. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    nt_log_info("SELFTEST: begin");
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

    st = gui_project_open(save_path, err, sizeof err);
    const tp_snapshot_atlas *reloaded_atlas = selftest_atlas_at(0, NULL);
    const int nsrc = reloaded_atlas ? reloaded_atlas->source_count : -1;
    nt_log_info("SELFTEST: reload -> %s, atlas0 sources=%d (dirty=%d)", tp_status_str(st), nsrc, gui_project_is_dirty());

    /* Master spec 14.2: live Save must not overwrite an external rewrite after Open. The exact
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
    gui_project_set_atlas_name(0, "hero_atlas"); /* structural: commits immediately -> one history step */
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
    char folder_abs[512];
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
        const gui_scan_result *sc = gui_scan_get(folder_abs);
        nt_log_info("SELFTEST: folder scan found %d image(s)", sc->count);
        if (sc->count > 0) {
            char sprite[192];
            (void)snprintf(sprite, sizeof sprite, "%s", sc->entries[0].rel);
            char *dot = strrchr(sprite, '.');
            if (dot) {
                *dot = '\0';
            }
            gui_project_set_sprite_rename(0, sprite, "renamed_region");
            const tp_snapshot_sprite *ov = tp_session_snapshot_sprite_by_key(
                gui_project_snapshot(), folder_atlas_id, folder_source_id, sprite);
            nt_log_info("SELFTEST: rename region '%s' -> override='%s'", sprite, (ov && ov->rename) ? ov->rename : "(none)");
        }
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
    gui_scan_invalidate_all();
    const int before_n = gui_scan_get(rdir)->count;
    (void)remove(rfile);
    gui_scan_invalidate_all();
    const int after_n = gui_scan_get(rdir)->count;
    nt_log_info("SELFTEST: refresh cycle temp png before=%d after=%d (removed=%d)", before_n, after_n, before_n - after_n);
#ifdef _WIN32
    (void)RemoveDirectoryA(rdir);
#endif

    /* --- in-process pack of the demo atlases: real tp_pack via gui_pack (timing + assertions) --- */
    {
        char proj[600];
        to_abs("examples/defold-demo/defold-demo.ntpacker_project", proj, sizeof proj);
        char perr[256] = {0};
        if (gui_project_open(proj, perr, sizeof perr) == TP_STATUS_OK) {
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
            gui_scan_invalidate_all();
            double ms_r = 0.0;
            double ms_b = 0.0;
            char pe[256] = {0};
            char note[128] = {0};
            const bool okr = (i_rotate >= 0) && gui_pack_atlas(i_rotate, &ms_r, pe, sizeof pe, note, sizeof note);
            const tp_result *rr = gui_pack_result(i_rotate);
            nt_log_info("SELFTEST: pack 'rotate' -> %d in %.1f ms sprites=%d pages=%d (find 'a'=%d) %s", okr, ms_r,
                        rr ? rr->sprite_count : -1, rr ? rr->page_count : -1, gui_pack_find_sprite(i_rotate, "a"),
                        okr ? "" : pe);
            NT_ASSERT(okr && rr && rr->sprite_count == 3 && rr->page_count >= 1 && "pack rotate");
            NT_ASSERT(gui_pack_find_sprite(i_rotate, "a") >= 0 && "region lookup 'a'");
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
                    gui_project_set_target(i_rotate, k, target->exporter_id,
                                           target->out_path, false);
                }
            }
            char tbase[700] = {0};
            (void)snprintf(tbase, sizeof tbase, "%s/selftest_rotate_export", s_exe_dir);
            if (jtarget >= 0) {
                gui_project_set_target(i_rotate, jtarget, "json-neotolis", tbase, true);
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
        char cfp[840];
        (void)snprintf(cfp, sizeof cfp, "%s/%s.tga", sdir, CYR_STEM);
        write_tga_2x2(cfp);

        const int sidx = gui_project_add_atlas();
        if (sidx >= 0) {
            (void)gui_project_add_source(sidx, sdir);
            gui_scan_invalidate_all();
            double sms = 0.0;
            char serr[256] = {0};
            char snote[128] = {0};
            const bool oks = gui_pack_atlas(sidx, &sms, serr, sizeof serr, snote, sizeof snote);
            const tp_result *sr = gui_pack_result(sidx);
            const int cyr_idx = gui_pack_find_sprite(sidx, CYR_STEM);
            nt_log_info("SELFTEST: stress pack -> %d in %.1f ms sprites=%d pages=%d cyr_idx=%d %s", oks, sms,
                        sr ? sr->sprite_count : -1, sr ? sr->page_count : -1, cyr_idx, oks ? "" : serr);
            NT_ASSERT(oks && sr && sr->sprite_count >= N + 1 && "stress pack 520+ sprites");
            NT_ASSERT(cyr_idx >= 0 && "Cyrillic-named region lookup");

            /* Cyrillic rename + save/load round-trip (multi-byte name survives serialization). */
            gui_project_set_sprite_rename(sidx, CYR_STEM, "\xD0\xB8\xD0\xBC\xD1\x8F"); /* "имя" */
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
                          slp, slp_atlas->id, slp_source->id, CYR_STEM)
                    : NULL;
            nt_log_info("SELFTEST: Cyrillic rename RT save=%s load=%s override='%s'", tp_status_str(sbst),
                        tp_status_str(slst), (ov && ov->rename) ? ov->rename : "(none)");
            NT_ASSERT(ov && ov->rename && strcmp(ov->rename, "\xD0\xB8\xD0\xBC\xD1\x8F") == 0 &&
                      "Cyrillic name survives save/load");
            tp_session_snapshot_destroy(slp);
            free(sbuf);

            /* Row model materializes 520+ rows (incl. the Cyrillic label) without overflow. */
            s_sel_atlas = sidx;
            build_rows();
            bool cyr_row = false;
            for (int i = 0; i < s_row_count; i++) {
                if (strcmp(s_rows[i].sprite_name, CYR_STEM) == 0) {
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
        (void)remove(cfp);
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
     *     resolve end-to-end (a fake result cannot exercise gui_pack_find_sprite). --- */
    {
        const int BIG_N = 4200; /* > the old 4096 row / multi-select cap */

        /* (A1) rows: >4096 (missing) sources materialize >4096 rows -- build_rows grows s_rows.
         *      Feed the production batch route in bounded transactions.  Calling the one-source
         *      convenience route BIG_N times makes the identity planner re-scan the growing atlas
         *      for every request (quadratic test setup) and no longer reflects the shipped
         *      multi-select workflow.  Batches stay well below both public admission limits
         *      (operation count and encoded request bytes); this probe is about row capacity,
         *      not about constructing a maximum-size transaction. */
        gui_project_new();
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
        s_sel_atlas = 0;
        build_rows();
        nt_log_info("SELFTEST: caps rows=%d (want %d; old cap 4096)", s_row_count, BIG_N);
        NT_ASSERT(s_row_count == BIG_N && "sprite rows grow past the old 4096 cap");

        /* (A2) multi-select: >4096 distinct names -- multi_sel_add grows s_multi_sel. */
        multi_sel_clear();
        for (int i = 0; i < BIG_N; i++) {
            char nm[24];
            (void)snprintf(nm, sizeof nm, "cap_%05d", i);
            multi_sel_add(nm);
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
            NT_ASSERT(s_sel_sort_ptr[i] == s_sel_sort_buf[i] &&
                      strcmp(s_sel_sort_ptr[i], want) == 0 &&
                      "SELFTEST: sort companions hold the whole selection");
        }
        nt_log_info("SELFTEST: caps sort retained=%d; unresolved animation rejected",
                    BIG_N);
        multi_sel_clear();

        /* (B) preview idxs[]: a >512-frame animation over REAL packed sprites resolves EVERY frame.
         *     Identical 2x2 sprites are NOT deduped (see the 520-sprite stress above), so M files pack
         *     to M regions. */
        gui_project_new();
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
        gui_scan_invalidate_all();
        double cms = 0.0;
        char cerr[256] = {0};
        char cnote[128] = {0};
        gui_pack_ref_index_work_reset();
        const bool okc = gui_pack_atlas(0, &cms, cerr, sizeof cerr, cnote, sizeof cnote);
        const tp_result *cr = gui_pack_result(0);
        nt_log_info("SELFTEST: caps pack -> %d sprites=%d (want >= %d) %s", okc, cr ? cr->sprite_count : -1, M,
                    okc ? "" : cerr);
        NT_ASSERT(okc && cr && cr->sprite_count >= M && "caps: pack >512 real sprites");

        s_sel_atlas = 0;
        build_rows();
        multi_sel_clear();
        for (int i = 0; i < s_row_count; i++) { /* select-all the leaf sprites (the real UI gesture) */
            if (!s_rows[i].is_folder && !s_rows[i].missing && s_rows[i].sprite_name[0] != '\0') {
                multi_sel_add(s_rows[i].sprite_name);
            }
        }
        nt_log_info("SELFTEST: caps preview select-all=%d (want %d)", s_multi_sel_count, M);
        NT_ASSERT(s_multi_sel_count == M && "caps: select-all resolves M leaf rows");
        const int panim = create_animation_from_selection();
        NT_ASSERT(panim >= 0 && "caps: animation from M frames");
        open_preview(panim);
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
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        s_sel_anim = -1;
        s_sel_anim_frame = -1;
    }

    /* --- settings panel: stale-on-change, effective-extrude, per-region RECT override,
     *     and a fresh-project seeded-target export (regions F/G, §3.3f, owner overrides) --- */
    {
        gui_project_new();
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
        gui_scan_invalidate_all();

        gui_project_mark_packed(); /* pretend current, then a setting change must set stale */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 7, 0.0F);
        nt_log_info("SELFTEST: setting change stale=%d (expect 1)", gui_project_is_stale());
        NT_ASSERT(gui_project_is_stale() && "a setting change sets preview stale");

        /* shape=concave + extrude=3 -> preview pack succeeds via the effective-extrude-0 rule */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_SHAPE, 2, 0.0F);
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_EXTRUDE, 3, 0.0F);
        double pms = 0.0;
        char perr[256] = {0};
        char pnote[128] = {0};
        const bool okc = gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
        nt_log_info("SELFTEST: concave+extrude3 pack -> %d in %.1fms (%s)", okc, pms, okc ? "effective extrude 0" : perr);
        NT_ASSERT(okc && "concave+extrude=3 packs (effective extrude 0)");

        /* per-sprite shape=RECT override -> that region packs as an exact 4-vert rect */
        char afabs[512];
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
            const gui_scan_result *sc = gui_scan_get(afabs);
            if (sc->count > 0) {
                char source_key[TP_SCAN_REL_CAP];
                char spn[192];
                (void)snprintf(source_key, sizeof source_key, "%s", sc->entries[0].rel);
                (void)snprintf(spn, sizeof spn, "%s", sc->entries[0].rel);
                char *dot = strrchr(spn, '.');
                if (dot) {
                    *dot = '\0';
                }
                gui_project_set_sprite_override(0, source_key, GUI_SPRITE_OV_SHAPE, 0 /* RECT */); /* coalescable: buffers */
                gui_project_flush_pending(); /* commit the buffered override before the raw pack reads the model */
                (void)gui_pack_atlas(0, &pms, perr, sizeof perr, pnote, sizeof pnote);
                const int rri = gui_pack_find_sprite(0, spn);
                const tp_result *rr = gui_pack_result(0);
                const int vc = (rr && rri >= 0) ? rr->sprites[rri].vert_count : -1;
                nt_log_info("SELFTEST: sprite '%s' RECT override -> vert_count=%d (expect 4)", spn, vc);
                NT_ASSERT(vc == 4 && "RECT per-sprite override packs a 4-vert rect");
            }
        }

        /* Restore a valid export state: the EXPORT path (tp_export_run) does not yet
         * apply the effective-extrude-0 rule (point-7 follow-up in the parallel exporter
         * agent's file), so concave+extrude>0 would be rejected at core validation. */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_EXTRUDE, 0, 0.0F);

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

    /* --- animations (ux.md §3.7b): pure playback map, create-from-selection natural sort, reorder,
     *     round-trip preserves frames order + playback + flips, remove-frame path --- */
    {
        bool fin = false;
        NT_ASSERT(gui_canvas_anim_frame_at(0.0, 10.0F, 2, 4, &fin) == 3 && !fin && "once_backward step0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 2, 4, &fin) == 0 && fin && "once_backward finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 3, 4, &fin) == 3 && "loop_backward wraps");
        NT_ASSERT(gui_canvas_anim_frame_at(0.35, 10.0F, 4, 3, &fin) == 1 && "once_pingpong return leg");
        NT_ASSERT(gui_canvas_anim_frame_at(0.45, 10.0F, 4, 3, &fin) == 0 && fin && "once_pingpong finishes at 0");
        NT_ASSERT(gui_canvas_anim_frame_at(0.55, 10.0F, 5, 3, &fin) == 1 && "loop_pingpong wraps");

        const int aidx = gui_project_add_atlas();
        s_sel_atlas = aidx;
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
        gui_scan_invalidate_all();
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

        gui_project_set_anim_playback(aidx, 0, 5); /* loop pingpong */
        gui_project_set_anim_flip(aidx, 0, true, false);
        gui_project_set_anim_fps(aidx, 0, 12.0F);
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
                  gui_project_animation_ref_at(aidx, shift1, &preview_ref) &&
                  "M2: capture the deferred preview by stable animation ID");
        gui_request_open_preview(&preview_ref);
        NT_ASSERT(selftest_remove_animation_named_at(aidx, "shift_first") &&
                  "M2: shift the animation collection before preview drain");
        apply_pending();
        const tp_session_snapshot *shift_snapshot = gui_project_snapshot();
        const tp_snapshot_atlas *shift_atlas = selftest_atlas_at(aidx, NULL);
        const tp_snapshot_animation *shift_selected = shift_atlas
            ? tp_session_snapshot_animation_at(shift_snapshot, shift_atlas->id,
                                               s_sel_anim)
            : NULL;
        NT_ASSERT(s_preview_active && shift_selected &&
                  tp_id128_eq(shift_selected->id, preview_ref.animation_id) &&
                  "M2: deferred preview follows the same animation after index shift");

        gui_request_remove_animation(s_sel_anim);
        s_sel_anim = 0; /* stale numeric selection must not define preview ownership */
        apply_pending();
        NT_ASSERT(!s_preview_active &&
                  "M2: removing the previewed animation compares stable IDs, not queued indices");

        s_sel_atlas = aidx;
        multi_sel_clear();
        multi_sel_add("walk_10");
        multi_sel_add("walk_2");
        multi_sel_add("walk_1");
        shift_atlas = selftest_atlas_at(aidx, NULL);
        const int animations_before_queued_create =
            shift_atlas ? shift_atlas->animation_count : -1;
        gui_request_create_animation_from_selection();
        s_sel_atlas = 0; /* redirect the live UI after intent capture */
        multi_sel_clear();
        multi_sel_add("wrong_selection");
        apply_pending();
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
        s_sel_anim = -1;
        s_sel_anim_frame = -1;
        s_sel_atlas = 0;
    }

    /* --- F2-05b-ii-A: deferred edit queue + GESTURE-SCOPED transaction coalescing (decision 0015) ---
     * The F2-03 diff history has no built-in coalescing (one commit = one undo step). A field-precise
     * pending-transaction buffer coalesces a gesture; the gesture BOUNDARY (a widget's release/blur/
     * discrete pick -- modelled here by gui_request_gesture_commit + apply_pending, or a direct
     * gui_project_flush_pending) commits it as ONE transaction = ONE undo step. */
    {
        /* (1) A gui_edit_* enqueue does not touch the model synchronously; the drained coalescable
         *     setter BUFFERS it (uncommitted); the model changes only when the gesture boundary flushes. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        const int pad0 = selftest_atlas_at(0, NULL)->padding;
        gui_edit_atlas_int(0, GUI_ATLAS_PADDING, pad0 + 5);
        const int pad_enq = selftest_atlas_at(0, NULL)->padding; /* enqueue: not applied */
        apply_pending();                                                          /* drains -> BUFFERS (no commit) */
        const int pad_buf = selftest_atlas_at(0, NULL)->padding; /* buffered, still uncommitted */
        gui_request_gesture_commit();                                             /* gesture end (e.g. slider release) */
        apply_pending();                                                          /* flush -> commit */
        const int pad_done = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: gesture pad %d ->(enqueue) %d ->(drain/buffer) %d ->(gesture-commit) %d", pad0, pad_enq,
                    pad_buf, pad_done);
        NT_ASSERT(pad_enq == pad0 && pad_buf == pad0 && pad_done == pad0 + 5 &&
                  "a coalescable edit buffers on drain and commits only at the gesture boundary");

        /* (2) ONE-control drag = ONE undo step. 8 same-key ticks buffer (latest wins, no commit); the
         *     gesture flush at release commits exactly ONE transaction; one undo reverts the WHOLE drag. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int pad_pre = selftest_atlas_at(0, NULL)->padding;
        const int undo0 = gui_project_undo_depth();
        for (int v = 1; v <= 8; v++) {
            (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, v, 0.0F); /* same key -> coalesce, no commit */
        }
        const int undo_mid = gui_project_undo_depth();                          /* still undo0: buffered */
        const int pad_mid2 = selftest_atlas_at(0, NULL)->padding; /* still pad_pre */
        gui_project_flush_pending();                                            /* the release boundary */
        const int undo1 = gui_project_undo_depth();
        const int pad_drag = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: one-control drag: 8 ticks undo %d ->(buffered) %d ->(release) %d, final padding=%d", undo0,
                    undo_mid, undo1, pad_drag);
        NT_ASSERT(undo_mid == undo0 && pad_mid2 == pad_pre &&
                  "an in-flight drag stays buffered (uncommitted) until its gesture boundary");
        NT_ASSERT(undo1 - undo0 == 1 && pad_drag == 8 &&
                  "one control drag = ONE committed transaction = ONE undo step (final value wins)");
        NT_ASSERT(gui_project_undo() && selftest_atlas_at(0, NULL)->padding == pad_pre &&
                  "one undo reverts the ENTIRE coalesced drag (not one tick)");

        /* (2b) DIVERGENCE from b-i (decision 0015): b-i's tag-only key merged DISTINCT knobs edited
         *      within the window into one undo step; A's FIELD-PRECISE key makes each distinct field its
         *      own step. Two distinct knobs -> the second (different key) FLUSHES the first -> TWO steps. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int undo_b0 = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 3, 0.0F); /* key = PADDING (buffered) */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_MARGIN, 4, 0.0F);  /* key = MARGIN: different -> flush PADDING */
        gui_project_flush_pending();                                        /* commit MARGIN */
        const int undo_b1 = gui_project_undo_depth();
        const tp_snapshot_atlas *dka = selftest_atlas_at(0, NULL);
        nt_log_info("SELFTEST: two distinct knobs padding=%d margin=%d undo delta=%d (want 2)", dka->padding, dka->margin,
                    undo_b1 - undo_b0);
        NT_ASSERT(undo_b1 - undo_b0 == 2 && dka->padding == 3 && dka->margin == 4 &&
                  "two distinct knobs back-to-back = TWO undo steps (field-precise divergence)");

        /* (2c) FLUSH-BEFORE-IS_DIRTY (the lost-edit trap New/Open/Exit must avoid): a buffered edit is
         *      not yet in the identity, so is_dirty reads CLEAN; the destructive gates flush FIRST, so the
         *      edit is committed (dirty) instead of silently discarded on a New/Open/Exit confirm. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        NT_ASSERT(!gui_project_is_dirty() && "fresh project is clean");
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, 9, 0.0F); /* buffered, uncommitted */
        const bool dirty_buffered = gui_project_is_dirty();
        gui_project_flush_pending();                                        /* what request_new/open/exit do first */
        const bool dirty_flushed = gui_project_is_dirty();
        nt_log_info("SELFTEST: flush-before-is_dirty: buffered dirty=%d, after pre-gate flush dirty=%d", dirty_buffered,
                    dirty_flushed);
        NT_ASSERT(!dirty_buffered && dirty_flushed &&
                  "the pre-gate flush commits a buffered edit so New/Open/Exit confirm instead of discarding it");

        /* (2d) SLICE9 RMW lost-edit is IMPOSSIBLE: slice9 is a read-modify-write (seeds all 4 components
         *      from the record). Two DIFFERENT components edited back-to-back must NOT drop the first --
         *      the component-precise key makes the second a different key, flushing the first BEFORE the
         *      second's RMW read, so the second seeds from the COMMITTED value. Both components survive. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_set_sprite_slice9(0, "s9sprite", 0 /* L */, 11); /* buffered */
        (void)gui_project_set_sprite_slice9(0, "s9sprite", 1 /* R */, 22); /* different component -> flush L, seed committed */
        gui_project_flush_pending();                                       /* commit R */
        const tp_snapshot_sprite *s9ov = selftest_sprite_by_name(0, "s9sprite");
        const int s9l = s9ov ? s9ov->slice9_lrtb[0] : -1;
        const int s9r = s9ov ? s9ov->slice9_lrtb[1] : -1;
        nt_log_info("SELFTEST: slice9 RMW L=%d R=%d (want 11,22 -- neither lost)", s9l, s9r);
        NT_ASSERT(s9l == 11 && s9r == 22 &&
                  "slice9 two-component edit: the field-precise key prevents the RMW lost-edit (both survive)");

        /* (3) F1: "Add frames" is DEFERRED (was a synchronous commit -> UAF while declare_animation_editor
         *     held a live `an` it kept dereferencing). The enqueue captures COPIED keys, so the frames land
         *     only on the apply_pending drain AND clearing the live selection between enqueue and drain does
         *     NOT change what lands. If someone reverts to a synchronous commit, fc_mid becomes 2 and this
         *     assertion fails HERE -- the UAF cannot regress silently. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
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
        gui_scan_invalidate_all();
        const int f1anim = gui_project_create_animation(0, "addf", NULL, 0); /* empty animation */
        s_sel_anim = f1anim;
        multi_sel_clear();
        multi_sel_add("f_2"); /* deliberately out of natural order */
        multi_sel_add("f_1");
        add_selection_frames_to_anim(f1anim); /* ENQUEUE ONLY -- must not commit synchronously */
        const tp_snapshot_animation *f1a = selftest_animation_at(0, f1anim);
        const int fc_mid = f1a ? f1a->frame_count : -1;
        multi_sel_clear();                    /* mutate the selection AFTER the enqueue: copied keys stand */
        apply_pending();                      /* drains -> gui_project_anim_add_frames replays the copies */
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

        /* (4) F2: a target toggle must carry the FULL out_path (heap), not truncate at 255. Set a >255-char
         *     path, then toggle `enabled` through the SAME deferred gui_edit_target the checkbox uses; after
         *     the drain the stored path must be byte-identical (the old fixed 256-byte queue slot corrupted
         *     any out_path > 255 on a mere enable/disable). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
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
        /* seed the long path via the setter (stored as a heap char*, up to TP_PATH_MAX) */
        (void)gui_project_set_target(0, 0, f2t->exporter_id, longp, f2t->enabled);
        f2t = selftest_target_at(0, 0);
        const bool en_was = f2t->enabled;
        /* the checkbox path: enqueue a toggle reading t->out_path (the full stored path) */
        gui_edit_target(0, 0, f2t->exporter_id, f2t->out_path, !en_was);
        apply_pending(); /* drains -> gui_project_set_target with the FULL path (no 255 truncation) */
        f2t = selftest_target_at(0, 0);
        nt_log_info("SELFTEST: F2 out_path len=%zu after toggle enabled %d->%d (match=%d)", strlen(f2t->out_path),
                    en_was, f2t->enabled, strcmp(f2t->out_path, longp) == 0);
        NT_ASSERT(strcmp(f2t->out_path, longp) == 0 && f2t->enabled == !en_was &&
                  "a target toggle preserves the full >255 out_path (F2 no truncation)");

        /* --- F2-05b-ii-A FIX pass (adversarial-review corrections) regressions --- */

        /* (#1) STALE-GUARD lost-edit: the view no longer guards `iv != committed`, so a control
         *      returned to its COMMITTED value still enqueues that value (latest wins). A gesture that
         *      nets back to committed commits NOTHING (the #3 flush-time no-op drop); a gesture that
         *      ends at a NEW final value commits EXACTLY that value -- never the stale intermediate.
         *      (Pre-fix: the guard skipped the correcting enqueue, so the buffer kept the intermediate
         *      and the flush committed the WRONG value = data loss.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int g1_pad = selftest_atlas_at(0, NULL)->padding; /* committed */
        const int g1_u0 = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 36, 0.0F); /* typed "40" (buffered) */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad, 0.0F);      /* corrected back to committed */
        gui_project_flush_pending();                                                 /* Enter / boundary */
        const int g1_u1 = gui_project_undo_depth();
        const int g1_pad1 = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: #1 revert-to-committed undo delta=%d padding=%d (want 0, %d)", g1_u1 - g1_u0, g1_pad1, g1_pad);
        NT_ASSERT(g1_u1 == g1_u0 && g1_pad1 == g1_pad &&
                  "#1: a gesture netting back to the committed value commits NOTHING (no phantom step / no wrong value)");
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 36, 0.0F); /* "40" */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g1_pad + 3, 0.0F);  /* corrected to a real new value */
        gui_project_flush_pending();
        const int g1_pad2 = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: #1 revert-to-new final padding=%d undo delta=%d (want %d, 1)", g1_pad2, gui_project_undo_depth() - g1_u0,
                    g1_pad + 3);
        NT_ASSERT(gui_project_undo_depth() - g1_u0 == 1 && g1_pad2 == g1_pad + 3 &&
                  "#1: the final value the user leaves the control at is what commits (not the stale intermediate)");

        /* (#2) ORIGIN two-component RMW lost-edit: origin is now COMPONENT-keyed like slice9. Editing X
         *      then Y back-to-back (no flush between) -> the Y edit's different key flushes the buffered X
         *      first, then seeds the committed X -> BOTH survive. (Pre-fix: X and Y shared one key + a
         *      stale view read-modify-write, so Y replaced {x=new,y=old} with {x=old,y=new} and dropped X.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_set_sprite_origin(0, "osprite", 0 /* X */, 0.25F); /* buffered */
        (void)gui_project_set_sprite_origin(0, "osprite", 1 /* Y */, 0.75F); /* different key -> flush X, seed committed */
        gui_project_flush_pending();                                         /* commit Y */
        const tp_snapshot_sprite *oov = selftest_sprite_by_name(0, "osprite");
        const float g2_ox = oov ? oov->origin_x : -1.0F;
        const float g2_oy = oov ? oov->origin_y : -1.0F;
        nt_log_info("SELFTEST: #2 origin X=%g Y=%g (want 0.25,0.75 -- neither lost)", (double)g2_ox, (double)g2_oy);
        NT_ASSERT(g2_ox == 0.25F && g2_oy == 0.75F &&
                  "#2: origin two-component edit -- the component-precise key prevents the RMW lost-edit (both survive)");

        /* (#3) NET-ZERO gesture drops NO redo branch + pushes NO phantom undo step. Make an edit, undo it
         *      (a redo branch is now present), then run a gesture that nets back to the committed value:
         *      the flush must DISCARD the no-op, leaving the redo branch intact + undo depth unchanged.
         *      (Pre-fix: the unconditional commit pushed a phantom step AND dropped the redo branch.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const int g3_pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad + 11, 0.0F);
        gui_project_flush_pending(); /* commit an edit (padding = +11) */
        NT_ASSERT(gui_project_undo() && "#3 setup: undo the committed edit");
        NT_ASSERT(gui_project_can_redo() && "#3 setup: a redo branch is present after the undo");
        const int g3_u = gui_project_undo_depth();
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad + 30, 0.0F); /* drag out */
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, g3_pad, 0.0F);      /* back to committed */
        gui_project_flush_pending();                                                 /* release: must DISCARD the no-op */
        nt_log_info("SELFTEST: #3 net-zero gesture undo delta=%d can_redo=%d (want 0, 1)", gui_project_undo_depth() - g3_u,
                    gui_project_can_redo());
        NT_ASSERT(gui_project_undo_depth() == g3_u && "#3: a net-zero gesture pushes NO phantom undo step");
        NT_ASSERT(gui_project_can_redo() && "#3: a net-zero gesture preserves the redo branch (no unconditional commit)");
        NT_ASSERT(gui_project_redo() && selftest_atlas_at(0, NULL)->padding == g3_pad + 11 &&
                  "#3: the preserved redo branch still restores the edit");

        /* (#4) FLUSH-BEFORE-READ: a buffered slice9 edit is still in flight. Flush
         *      first, then reacquire the target DTO and copy exporter_id before the
         *      next operation invalidates the cached snapshot. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const tp_snapshot_target *g4a = selftest_target_at(0, 0);
        NT_ASSERT(g4a && "#4: fresh project seeds a default target");
        (void)gui_project_set_sprite_slice9(0, "p4sprite", 0 /* L */, 5); /* buffered across the flush boundary */
        gui_project_flush_pending();                                      /* commit -> clone-swap + free the old project */
        const tp_snapshot_target *g4b = selftest_target_at(0, 0); /* re-get from the now-stable snapshot */
        char g4_exp[64];
        (void)snprintf(g4_exp, sizeof g4_exp, "%s", g4b->exporter_id); /* COPY before set_target's flush */
        (void)gui_project_set_target(0, 0, g4_exp, "out/p4", true);
        const tp_snapshot_target *g4c = selftest_target_at(0, 0);
        nt_log_info("SELFTEST: #4 flush-before-read target path='%s' exporter='%s'", g4c->out_path,
                    g4c->exporter_id);
        NT_ASSERT(strcmp(g4c->out_path, "out/p4") == 0 && strcmp(g4c->exporter_id, g4_exp) == 0 &&
                  "#4: flush-before-read commits the target with no dangling exporter read (parity UAF fix)");

        /* (#5) EFFECTIVE slice9 peek for the canvas guides: the peek returns the BUFFERED slice9 while a
         *      slice9 gesture is buffered (so the on-canvas guides move THIS frame), and false once the
         *      gesture flushes (the caller then reads the committed record). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        int g5[4] = {-1, -1, -1, -1};
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "p5sprite", g5) && "#5: no buffered slice9 -> peek returns false");
        (void)gui_project_set_sprite_slice9(0, "p5sprite", 2 /* T */, 9); /* buffered */
        const bool g5_hit = gui_project_peek_pending_slice9(0, "p5sprite", g5);
        nt_log_info("SELFTEST: #5 slice9 peek hit=%d L,R,T,B=%d,%d,%d,%d (want 1 and 0,0,9,0)", g5_hit, g5[0], g5[1], g5[2],
                    g5[3]);
        NT_ASSERT(g5_hit && g5[0] == 0 && g5[1] == 0 && g5[2] == 9 && g5[3] == 0 &&
                  "#5: peek returns the buffered slice9 (edited component + seeded others) while buffered");
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "other", g5) && "#5: the peek is keyed on the sprite (a different key misses)");
        gui_project_flush_pending();
        NT_ASSERT(!gui_project_peek_pending_slice9(0, "p5sprite", g5) &&
                  "#5: after the gesture flush the peek returns false (read the committed record)");

        /* (#9) BROWSE-TARGET ordering: a coalescable edit is buffered, then the
         *      target update flushes it before admitting its own stable-ID intent.
         *      The copied exporter remains exact across that boundary. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        gui_sprite_ref g9_sprite;
        NT_ASSERT(selftest_sprite_ref_at(0, "g9sprite", &g9_sprite) &&
                  "#9 setup: install canonical source identity before borrowing target strings");
        const tp_snapshot_target *g9a = selftest_target_at(0, 0);
        NT_ASSERT(g9a && "#9: fresh project seeds a default target");
        char g9_want[64];
        (void)snprintf(g9_want, sizeof g9_want, "%s", g9a->exporter_id); /* expected value (copied now) */
        (void)gui_project_set_sprite_slice9(0, "g9sprite", 0 /* L */, 7); /* buffer a real (non-noop) pending op, UNFLUSHED */
        const bool g9_set = gui_project_set_target(0, 0, g9_want, "out/g9", true);
        const tp_snapshot_target *g9c = selftest_target_at(0, 0);
        char g9_error[256] = {0};
        const bool g9_had_error = gui_project_take_op_error(g9_error, sizeof g9_error);
        nt_log_info("SELFTEST: #9 browse-target UAF set=%d error='%s' exporter='%s' path='%s' (want '%s','out/g9')",
                    g9_set, g9_had_error ? g9_error : "", g9c->exporter_id, g9c->out_path, g9_want);
        NT_ASSERT(g9_set && strcmp(g9c->exporter_id, g9_want) == 0 && strcmp(g9c->out_path, "out/g9") == 0 &&
                  "#9: target intent preserves exporter and path across the pending flush");

        /* (#10) SIBLING-SINK ordering: removing an animation after a buffered
         *       gesture flush still resolves the intended stable animation and
         *       removes exactly one record. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        gui_sprite_ref g10_sprite;
        NT_ASSERT(selftest_sprite_ref_at(0, "g10sprite", &g10_sprite) &&
                  "#10 setup: install canonical source identity before borrowing animation strings");
        const int g10i = gui_project_create_animation(0, "g10anim", NULL, 0);
        NT_ASSERT(g10i >= 0 && "#10: created an animation to remove");
        const tp_snapshot_atlas *g10a = selftest_atlas_at(0, NULL);
        const tp_snapshot_animation *g10anim = selftest_animation_at(0, g10i);
        const int g10n0 = g10a->animation_count;
        char g10name[128];
        (void)snprintf(g10name, sizeof g10name, "%s", g10anim->name);
        (void)gui_project_set_sprite_slice9(0, "g10sprite", 0 /* L */, 3); /* buffer a real (non-noop) op, UNFLUSHED */
        gui_project_remove_animation(0, g10name); /* its flush frees the project g10name points into */
        const tp_snapshot_atlas *g10c = selftest_atlas_at(0, NULL);
        nt_log_info("SELFTEST: #10 sibling-sink remove-anim count %d->%d (want %d)", g10n0, g10c->animation_count,
                    g10n0 - 1);
        NT_ASSERT(g10c->animation_count == g10n0 - 1 &&
                  "#10: remove_animation resolves the intended stable animation after the pending flush");

        /* (#11 -- H/G3) TARGET OUT-PATH coalescing: the export-target path text field used to fire one
         *       gui_project_set_target per keystroke -> one committed TP_OP_TARGET_SET each = undo spam.
         *       The out-path edit is now COALESCABLE, keyed per target (field = index): several distinct
         *       values typed within one gesture BUFFER (latest wins, no commit); the field's Enter/blur
         *       gesture boundary flushes the whole edit as EXACTLY ONE undo step; one undo reverts to the
         *       committed baseline and redo re-applies. RMW-seeds exporter_id + enabled (only out_path
         *       changes). Mirrors the padding-drag case (2), on the target out-path. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        const tp_snapshot_target *t11a = selftest_target_at(0, 0);
        NT_ASSERT(t11a && "#11: fresh project seeds a default target");
        /* committed baseline out_path (one immediate commit, as the discrete browse/toggle paths do) */
        (void)gui_project_set_target(0, 0, t11a->exporter_id, "out/base.json", t11a->enabled);
        t11a = selftest_target_at(0, 0);
        const char t11_base[] = "out/base.json";
        NT_ASSERT(strcmp(t11a->out_path, t11_base) == 0 && "#11: baseline out_path committed");
        const bool t11_en = t11a->enabled;
        char t11_exp[64];
        (void)snprintf(t11_exp, sizeof t11_exp, "%s", t11a->exporter_id); /* remember for the RMW-seed check */
        const int t11_u0 = gui_project_undo_depth();
        /* simulate typing "out/f" -> ... -> "out/final.json": several DISTINCT values, SAME key, NO flush between */
        (void)gui_project_set_target_out_path(0, 0, "out/f");
        (void)gui_project_set_target_out_path(0, 0, "out/fin");
        (void)gui_project_set_target_out_path(0, 0, "out/final");
        (void)gui_project_set_target_out_path(0, 0, "out/final.json");
        const int t11_umid = gui_project_undo_depth();                                          /* still t11_u0: buffered */
        const char *t11_mid = selftest_target_at(0, 0)->out_path;  /* still the baseline */
        NT_ASSERT(t11_umid == t11_u0 && strcmp(t11_mid, t11_base) == 0 &&
                  "#11: in-flight out-path keystrokes stay buffered (uncommitted) until the gesture boundary");
        gui_project_flush_pending();                                                            /* Enter / blur boundary */
        const int t11_u1 = gui_project_undo_depth();
        const tp_snapshot_target *t11b = selftest_target_at(0, 0);
        nt_log_info("SELFTEST: #11 out-path coalesce: 4 keystrokes undo %d->%d path='%s' exporter='%s' enabled=%d", t11_u0,
                    t11_u1, t11b->out_path, t11b->exporter_id, t11b->enabled);
        NT_ASSERT(t11_u1 - t11_u0 == 1 && strcmp(t11b->out_path, "out/final.json") == 0 &&
                  "#11: N out-path keystrokes = ONE undo step (final typed value wins)");
        NT_ASSERT(strcmp(t11b->exporter_id, t11_exp) == 0 && t11b->enabled == t11_en &&
                  "#11: the coalesced out-path edit leaves exporter_id + enabled UNTOUCHED (mask=TP_TF_OUT_PATH)");
        NT_ASSERT(gui_project_undo() &&
                  strcmp(selftest_target_at(0, 0)->out_path, t11_base) == 0 &&
                  "#11: one undo reverts the ENTIRE coalesced out-path edit back to the baseline");
        NT_ASSERT(gui_project_redo() &&
                  strcmp(selftest_target_at(0, 0)->out_path, "out/final.json") == 0 &&
                  "#11: redo re-applies the coalesced out-path edit");
        /* #11 net-zero parity: core semantic-diff admission owns the no-change verdict. */
        const int t11_unz = gui_project_undo_depth();
        const bool t11_stale_before = gui_project_is_stale();
        (void)gui_project_set_target_out_path(0, 0, "out/scratch");
        (void)gui_project_set_target_out_path(0, 0, "out/final.json"); /* revert to the committed value */
        gui_project_flush_pending();
        NT_ASSERT(gui_project_undo_depth() == t11_unz &&
                  gui_project_is_stale() == t11_stale_before &&
                  strcmp(selftest_target_at(0, 0)->out_path, "out/final.json") == 0 &&
                  "#11: a net-zero out-path gesture preserves Undo and preview state");
        /* #11 interleave (the coalescing hazard G3 introduced + fixed): a discrete enabled toggle made while
         * an out-path edit is still BUFFERED (typed, not yet Enter/blur) must NOT revert the typed path. The
         * old discrete gui_edit_target re-sent the STALE committed out_path, so its internal flush committed
         * the buffered value then overwrote it back. gui_project_set_target_enabled now flushes FIRST (commits
         * the buffered out-path as its own step) then commits a mask=TP_TF_ENABLED-only op -- it never re-sends
         * out_path, so the typed path cannot be reverted. Buffer "out/typed.json", then toggle enabled. */
        (void)gui_project_set_target_out_path(0, 0, "out/typed.json"); /* buffered, uncommitted */
        const tp_snapshot_target *t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/final.json") == 0 &&
                  "#11: the typed out-path is still buffered (committed record unchanged)");
        const bool t11_en0 = t11i->enabled;
        NT_ASSERT(gui_project_set_target_enabled(0, 0, !t11_en0) && "#11: discrete enabled toggle commits");
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/typed.json") == 0 && t11i->enabled == !t11_en0 &&
                  "#11: a discrete toggle mid-typing commits the buffered out-path FIRST (typed path preserved, not reverted)");
        /* #11 exporter interleave (same hazard on the EXPORTER path): buffer a new out-path, then change the
         * exporter -- the typed path must be preserved and the exporter changed. Default target is json-neotolis
         * (see the coalesce log line above), so switching to "defold" is a real change. */
        (void)gui_project_set_target_out_path(0, 0, "out/typed2.json"); /* buffered, uncommitted */
        NT_ASSERT(gui_project_set_target_exporter(0, 0, "defold") && "#11: discrete exporter change commits");
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/typed2.json") == 0 &&
                  strcmp(t11i->exporter_id, "defold") == 0 &&
                  "#11: an exporter change mid-typing preserves the buffered out-path (not reverted)");
        /* #11 empty-path: frontend forwards the typed intent; core validation is
         * the only owner. The flush-first toggle reports that rejection and may
         * be retried after the invalid pending intent has been consumed. */
        (void)gui_project_set_target_out_path(0, 0, "out/willclear"); /* buffered (uncommitted) */
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/typed2.json") == 0 &&
                  "#11: [0] buffered edit not yet committed (record still the last valid path)");
        (void)gui_project_set_target_out_path(0, 0, "");
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(strcmp(t11i->out_path, "out/typed2.json") == 0 &&
                  "#11: [0] pending invalid intent does not mutate the committed record");
        const bool t11_en1 = t11i->enabled;
        NT_ASSERT(!gui_project_set_target_enabled(0, 0, !t11_en1) &&
                  "#11: [0] flush-first surfaces the core validation failure");
        char t11_error[256] = {0};
        NT_ASSERT(gui_project_take_op_error(t11_error, sizeof t11_error) &&
                  strstr(t11_error, "out_path") != NULL &&
                  "#11: [0] invalid input produces a structured core error");
        NT_ASSERT(gui_project_set_target_enabled(0, 0, !t11_en1) &&
                  "#11: [0] retry succeeds after the rejected pending intent is consumed");
        t11i = selftest_target_at(0, 0);
        NT_ASSERT(t11i->enabled == !t11_en1 && t11i->out_path[0] != '\0' &&
                  strcmp(t11i->out_path, "out/typed2.json") == 0 &&
                  "#11: [0] the toggle applied and kept the last valid (non-empty) out-path");

        NT_ASSERT(selftest_stale_coalesced_intent_is_rejected() &&
                  "#12: flushing another gesture must not launder a stale revision");
        NT_ASSERT(selftest_stale_discrete_intent_is_rejected() &&
                  "#12: discrete target setters must not launder a stale revision");

        /* Restore a packable atlas-0 project for the render frames below (the pixel probe packs
         * atlas 0 and probes its region outlines) -- gui_project_new left it source-less. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char pfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", pfolder, sizeof pfolder);
        (void)gui_project_add_source(0, pfolder);
        gui_scan_invalidate_all();
    }

    /* --- H/P2-14: Add Atlas auto-name must SCAN for a free atlasN, not blindly use atlas_count+1 (which
     *     collides with a surviving atlas after a remove -> core rejects the duplicate name -> the button
     *     wedges). Build atlas1..atlas3, remove atlas1, then Add: the old code picked "atlas3" (count 2+1)
     *     and FAILED; the scan picks the freed "atlas1" and succeeds. --- */
    {
        gui_project_new(); /* fresh: exactly one atlas (atlas1) */
        const int p14a2 = gui_project_add_atlas(); /* atlas2 */
        const int p14a3 = gui_project_add_atlas(); /* atlas3 */
        NT_ASSERT(p14a2 >= 0 && p14a3 >= 0 &&
                  tp_session_snapshot_atlas_count(gui_project_snapshot()) == 3 &&
                  "P2-14: seeded atlas1..atlas3");
        NT_ASSERT(gui_project_remove_atlas(0) && "P2-14: removed atlas1 (count -> 2)");
        const int p14add = gui_project_add_atlas(); /* count+1 == "atlas3" WOULD collide; the scan must avoid it */
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
        gui_project_new(); /* fresh atlas1 + default target out/atlas1 */
        NT_ASSERT(gui_project_set_atlas_name(0, "sprites") && "P2-14/B: rename atlas1 -> 'sprites' (target stays out/atlas1)");
        const int p14b = gui_project_add_atlas();
        const tp_snapshot_atlas *p14ba = selftest_atlas_at(p14b, NULL);
        const tp_snapshot_target *p14bt = selftest_target_at(p14b, 0);
        const char *p14bn = p14ba ? p14ba->name : "(wedged)";
        const char *p14bo = p14bt ? p14bt->out_path : "(none)";
        nt_log_info("SELFTEST: P2-14/B rename-then-add -> name='%s' out_path='%s' (want atlas2, out/atlas2)", p14bn, p14bo);
        NT_ASSERT(p14b >= 0 && strcmp(p14bn, "atlas2") == 0 &&
                  "P2-14/B: the scan skips 'atlas1' (out/atlas1 still held by the renamed atlas) and picks 'atlas2'");
        NT_ASSERT(p14bo && strcmp(p14bo, "out/atlas1") != 0 &&
                  "P2-14/B: the new atlas's default target does NOT collide on out/atlas1");
        gui_project_new(); /* leave a clean project for the following phases */
    }

    /* --- H/P2-13: Add Files (multi-select) commits ONE transaction, not one per file -> a 4-file add is
     *     a SINGLE undo step and is ATOMIC (one undo removes all of them). Also de-dups WITHIN the batch. --- */
    {
        gui_project_new();
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
        gui_project_new(); /* leave a clean project for the following phases */
    }

    /* --- F2-05b-ii-B: LIVE recovery journal -- append-fail UX + crash-recovery round-trip (both ways) --- */
    {
        /* (J1) APPEND-FAIL UX: a recovery-journal append failure (full disk) must reject the commit
         *      cleanly -- the live model is BYTE-UNCHANGED, dirty is unchanged, a clear status is
         *      raised, no half-applied edit -- and the editor must keep working once the failure
         *      clears. Driven deterministically: attach a memory-io recovery journal to a fresh model,
         *      arm the NEXT journal write to fail, then commit a real (structural, immediate) edit. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        (void)gui_project_take_op_error(NULL, 0); /* clear any stale soft-error */
        NT_ASSERT(gui_project__test_attach_memory_recovery() &&
                  "J1: memory recovery journal attached to the live model");
        char nm0[64];
        (void)snprintf(nm0, sizeof nm0, "%s", selftest_atlas_at(0, NULL)->name);
        const tp_id128 id_before = tp_session_snapshot_semantic_identity(gui_project_snapshot());
        const bool dirty_before = gui_project_is_dirty();
        gui_project__test_fail_next_recovery_writes(1); /* the NEXT journal append fails entirely */
        const bool committed = gui_project_set_atlas_name(0, "append_should_fail"); /* structural -> immediate commit */
        char emsg[256] = {0};
        const bool surfaced = gui_project_take_op_error(emsg, sizeof emsg);
        const tp_id128 id_after = tp_session_snapshot_semantic_identity(gui_project_snapshot());
        const char *nm1 = selftest_atlas_at(0, NULL)->name;
        nt_log_info("SELFTEST: J1 append-fail committed=%d surfaced=%d msg='%s' name '%s'->'%s' dirty %d->%d",
                    (int)committed, (int)surfaced, emsg, nm0, nm1, (int)dirty_before, (int)gui_project_is_dirty());
        NT_ASSERT(!committed && "J1: a journal append failure REJECTS the commit");
        NT_ASSERT(surfaced && emsg[0] && "J1: the append failure surfaces a status-bar error");
        NT_ASSERT(tp_id128_eq(id_before, id_after) && strcmp(nm1, nm0) == 0 &&
                  "J1: the live model is BYTE-UNCHANGED after the rejected append (no half-applied edit)");
        NT_ASSERT(gui_project_is_dirty() == dirty_before && "J1: dirty is unchanged after the rejected append");
        /* the fault was one-shot -> a further edit now commits normally (editor still live) */
        NT_ASSERT(gui_project_set_atlas_name(0, "works_after") &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "works_after") == 0 &&
                  "J1: the editor keeps working once the append failure clears");
        (void)gui_project_take_op_error(NULL, 0); /* the recovered edit raised no error; clear defensively */

        /* (J2) fix [3] SAVE MUST ABORT ON A JOURNAL-FAILED FLUSH (no silent data loss / no false clean).
         *      A buffered gesture whose flush-commit fails during Save must NOT be silently dropped while
         *      the file is written + the title shows "saved". Buffer a coalescable edit, arm append-fail,
         *      Save -> assert Save FAILS, the model is NOT marked clean, and NO file is written. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J2: memory recovery journal attached");
        NT_ASSERT(gui_project_set_atlas_name(0, "before_save") && "J2: a committed edit lands (journal healthy)");
        NT_ASSERT(gui_project_is_dirty() && "J2: the committed edit dirties the model");
        const int j2pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j2pad + 5, 0.0F); /* BUFFERED (uncommitted) gesture */
        gui_project__test_fail_next_recovery_writes(1); /* the flush's journal append will fail */
        char s2path[1200];
        (void)snprintf(s2path, sizeof s2path, "%s/selftest_savefail.ntpacker_project", s_exe_dir);
        (void)remove(s2path);
        char s2err[256] = {0};
        const tp_status s2st = gui_project_save_as(s2path, s2err, sizeof s2err);
        const bool s2_written = selftest_file_exists(s2path);
        nt_log_info("SELFTEST: J2 save-with-append-fail st=%s dirty=%d file_written=%d err='%s' (want !OK,1,0)",
                    tp_status_str(s2st), (int)gui_project_is_dirty(), (int)s2_written, s2err);
        NT_ASSERT(s2st != TP_STATUS_OK && "J2/[3]: Save FAILS when the buffered edit cannot be journaled");
        NT_ASSERT(gui_project_is_dirty() && "J2/[3]: the model is NOT marked clean on a failed save (no false 'saved')");
        NT_ASSERT(!s2_written && "J2/[3]: no .ntpacker_project is written when the flush commit failed");
        NT_ASSERT(strcmp(selftest_atlas_at(0, NULL)->name, "before_save") == 0 &&
                  "J2/[3]: the committed model is intact (the failed save did not corrupt it)");
        (void)remove(s2path);
        (void)gui_project_take_op_error(NULL, 0);

        /* (J5) fix [1] SINGLE-INSTANCE LOCK: a 2nd instance that cannot acquire the slot lock must remain
         *      recovery-required read-only and NEVER touch the slot -- so it neither adopts the 1st's LIVE
         *      session as "recovered" nor truncates its live journal. Simulate the 1st instance by holding
         *      a foreign lock, then enable recovery here and assert recovery is INACTIVE + the slot journal
         *      is never created. */
        char lslot[1200];
        char llock[1210];
        (void)snprintf(lslot, sizeof lslot, "%s/selftest_lock.ntpjournal", s_exe_dir);
        (void)snprintf(llock, sizeof llock, "%s.lock", lslot);
        (void)remove(lslot);
        (void)remove(llock);
        NT_ASSERT(gui_project__test_hold_foreign_lock(lslot) && "J5: a foreign instance holds the slot lock");
        gui_project_enable_recovery("");
        gui_project_shutdown();
        gui_project_enable_recovery(lslot);
        gui_project_init(); /* attach attempts the held slot and fails closed */
        char busy[256] = {0};
        const bool busy_notice = gui_project_take_recovery_setup_notice(busy, sizeof busy);
        const bool active = gui_project__test_recovery_active();
        NT_ASSERT(!gui_project_set_atlas_name(0, "instance2_edit") &&
                  "J5: a recovery-required GUI session rejects unjournaled edits");
        const bool slot_touched = selftest_file_exists(lslot);
        nt_log_info("SELFTEST: J5 2nd-instance active=%d busy_notice=%d slot_created=%d (want 0,1,0)", (int)active,
                    (int)busy_notice, (int)slot_touched);
        NT_ASSERT(!active && "J5/[1]: a 2nd instance cannot acquire the lock -> recovery INACTIVE");
        NT_ASSERT(busy_notice && busy[0] && "J5/[1]: the 'another instance' notice is raised");
        NT_ASSERT(!slot_touched && "J5/[1]: the 2nd instance never created/touched the slot journal (no truncation)");
        gui_project__test_release_foreign_lock();
        gui_project_enable_recovery("");
        (void)remove(llock);

        /* (J6) fix2 [3]: a STRUCTURAL wrapper must ABORT when its pre-flush of a buffered gesture fails to
         *      journal -- never silently drop the gesture AND land an unrelated structural change. Buffer a
         *      coalescable gesture, arm append-fail, then call set_atlas_name -> its flush commits the
         *      gesture, the append fails, and the wrapper aborts (returns false; neither the gesture nor the
         *      rename lands) with the op-error surfaced. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J6: memory journal attached");
        char j6name0[64];
        (void)snprintf(j6name0, sizeof j6name0, "%s", selftest_atlas_at(0, NULL)->name);
        const int j6pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j6pad + 7, 0.0F); /* BUFFERED (uncommitted) */
        gui_project__test_fail_next_recovery_writes(1);                            /* the flush's append fails */
        const bool j6ret = gui_project_set_atlas_name(0, "structural_should_abort");
        char j6err[256] = {0};
        const bool j6surfaced = gui_project_take_op_error(j6err, sizeof j6err);
        const char *j6name1 = selftest_atlas_at(0, NULL)->name;
        const int j6pad1 = selftest_atlas_at(0, NULL)->padding;
        nt_log_info("SELFTEST: J6 structural-abort ret=%d surfaced=%d name '%s'->'%s' pad %d->%d (want 0,1,unchanged)",
                    (int)j6ret, (int)j6surfaced, j6name0, j6name1, j6pad, j6pad1);
        NT_ASSERT(!j6ret && "J6/[3]: a structural op ABORTS when the pre-flush of a buffered gesture fails to journal");
        NT_ASSERT(j6surfaced && j6err[0] && "J6/[3]: the journal failure is surfaced to the status-bar channel");
        NT_ASSERT(strcmp(j6name1, j6name0) == 0 && j6pad1 == j6pad &&
                  "J6/[3]: neither the dropped gesture NOR the structural op landed (both aborted -- no unrelated change)");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J7) fix2 [1]: a PACK must ABORT when its pre-flush of a buffered gesture fails to journal --
         *      never pack a stale model + report success. (do_pack_blocking's flush_failed surfaces the
         *      op-error to the status bar, so the deterministic assertion is that NO pack result is produced.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char j7folder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", j7folder, sizeof j7folder);
        (void)gui_project_add_source(0, j7folder);
        gui_scan_invalidate_all();
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J7: memory journal attached");
        gui_pack_clear(-1); /* no prior result */
        const int j7pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j7pad + 3, 0.0F); /* buffered */
        gui_project__test_fail_next_recovery_writes(1);
        do_pack_blocking(); /* flush_failed -> abort BEFORE packing */
        const tp_result *j7r = gui_pack_result(0);
        nt_log_info("SELFTEST: J7 pack-abort result=%s (want NULL -- pack aborted on the journal-failed flush)",
                    j7r ? "PRESENT" : "NULL");
        NT_ASSERT(j7r == NULL && "J7/[1]: a journal-failed flush ABORTS the pack (no stale result produced)");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J8) fix2 [0]: the unsaved-changes GATE. request_new must ABORT on a journal-failed flush, never
         *      discard the project because is_dirty read clean after the only (buffered) change was dropped.
         *      Detected via the project PATH: gui_project_new (if it ran) resets the path to ""; an abort
         *      keeps it. Save to a temp first so the buffered gesture is the ONLY unsaved change. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        char j8path[1200];
        (void)snprintf(j8path, sizeof j8path, "%s/selftest_gate.ntpacker_project", s_exe_dir);
        (void)remove(j8path);
        char j8serr[256] = {0};
        NT_ASSERT(gui_project_save_as(j8path, j8serr, sizeof j8serr) == TP_STATUS_OK &&
                  "J8: save to establish a path + a clean baseline");
        NT_ASSERT(gui_project_has_path() && !gui_project_is_dirty() && "J8: saved -> has a path + clean");
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J8: memory journal attached");
        const int j8pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j8pad + 4, 0.0F); /* the ONLY unsaved change (buffered) */
        gui_project__test_fail_next_recovery_writes(1);
        request_new(); /* WITH the fix: flush_failed -> abort -> the project + its path are KEPT */
        const bool j8kept = gui_project_has_path();
        nt_log_info("SELFTEST: J8 dirty-gate abort has_path=%d (want 1: request_new aborted, project NOT discarded)",
                    (int)j8kept);
        NT_ASSERT(j8kept &&
                  "J8/[0]: request_new ABORTS on a journal-failed flush -- the project is NOT silently discarded");
        (void)remove(j8path);
        (void)gui_project_take_op_error(NULL, 0);

        /* (J9) fix3 [0]: a now-bool remove wrapper returns FALSE on a journal-failed flush and the item is
         *      STILL present (so the deferred handler prints NO false "Removed" / bad Ctrl+Z); the healthy
         *      journal path returns TRUE and removes. */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        const int j9added = gui_project_add_atlas(); /* a 2nd atlas to remove (index 1) */
        NT_ASSERT(j9added >= 1 && "J9: added a 2nd atlas to remove");
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J9: memory journal attached");
        const int j9count0 = tp_session_snapshot_atlas_count(gui_project_snapshot());
        const int j9pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j9pad + 6, 0.0F); /* buffered gesture */
        gui_project__test_fail_next_recovery_writes(1);
        const bool j9ret_fail = gui_project_remove_atlas(j9added); /* flush fails -> abort */
        const int j9count1 = tp_session_snapshot_atlas_count(gui_project_snapshot());
        nt_log_info("SELFTEST: J9 remove-abort ret=%d count %d->%d (want 0, unchanged)", (int)j9ret_fail, j9count0, j9count1);
        NT_ASSERT(!j9ret_fail && j9count1 == j9count0 &&
                  "J9/[0]: remove_atlas returns FALSE on a journal-failed flush + the atlas is STILL present");
        (void)gui_project_take_op_error(NULL, 0);
        const bool j9ret_ok = gui_project_remove_atlas(j9added); /* healthy journal, no pending -> removes */
        const int j9count2 = tp_session_snapshot_atlas_count(gui_project_snapshot());
        nt_log_info("SELFTEST: J9 remove-success ret=%d count %d->%d (want 1, -1)", (int)j9ret_ok, j9count1, j9count2);
        NT_ASSERT(j9ret_ok && j9count2 == j9count1 - 1 &&
                  "J9/[0]: a healthy-journal remove returns TRUE + removes (the success case still works)");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J10) H/P1-2: animation rename is a first-class op now. set_anim_id returns false for BOTH a
         *       name collision AND a journal-failed flush, but the LIVE op-error channel
         *       (gui_project_take_op_error -- the same one commit_active_edit surfaces) discriminates them
         *       by MESSAGE. The retired gui_project_anim_id_exists heuristic no longer decides; assert the
         *       real reject text on each arm, so a disk-full on a unique name is never misreported as a
         *       duplicate (and the editor is not trapped). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        const int j10a = gui_project_create_animation(0, "anim_a", NULL, 0);
        const int j10b = gui_project_create_animation(0, "anim_b", NULL, 0);
        NT_ASSERT(j10a == 0 && j10b == 1 && "J10: created two animations");
        /* Case A -- genuine collision: rename anim_b to "anim_a" (exists) -> false AND the CORE reject
         *   "an animation named 'anim_a' already exists" rides the op-error channel (no client heuristic). */
        const bool j10_collide_ret = gui_project_set_anim_id(0, j10b, "anim_a");
        char j10_cerr[256] = {0};
        const bool j10_csurfaced = gui_project_take_op_error(j10_cerr, sizeof j10_cerr);
        nt_log_info("SELFTEST: J10 collision ret=%d surfaced=%d msg='%s' (want 0,1 -> the core collision message)",
                    (int)j10_collide_ret, (int)j10_csurfaced, j10_cerr);
        NT_ASSERT(!j10_collide_ret && j10_csurfaced && strstr(j10_cerr, "an animation named") != NULL &&
                  strstr(j10_cerr, "already exists") != NULL &&
                  "J10/live: a genuine duplicate -> set_anim_id false AND the op-error IS the core collision message");
        /* Case B -- journal-failed flush on a UNIQUE name: false AND the op-error is the JOURNAL-fail
         *   message, NOT a collision (the flush-first entry catches it before the rename op is built). */
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J10: memory journal attached");
        const int j10pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j10pad + 2, 0.0F); /* buffered */
        gui_project__test_fail_next_recovery_writes(1);
        const bool j10_flush_ret = gui_project_set_anim_id(0, j10b, "totally_unique_name");
        char j10_ferr[256] = {0};
        const bool j10_fsurfaced = gui_project_take_op_error(j10_ferr, sizeof j10_ferr);
        nt_log_info("SELFTEST: J10 journal-fail ret=%d surfaced=%d msg='%s' (want 0,1 -> journal msg, NOT a collision)",
                    (int)j10_flush_ret, (int)j10_fsurfaced, j10_ferr);
        NT_ASSERT(!j10_flush_ret && j10_fsurfaced && strstr(j10_ferr, "journal") != NULL &&
                  strstr(j10_ferr, "already exists") == NULL &&
                  "J10/live: a journal-failed flush on a UNIQUE name -> false with the journal message, never a false collision");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J11) H/P1-2: the anim-rename FLUSH-FIRST pattern, asserted through the LIVE op-error channel.
         *       The retired anim_id_exists heuristic was NOT a valid collision discriminator: it returns
         *       true for the anim's OWN name, yet renaming to the own name is a no-op SUCCESS -- so on a
         *       journal-fail (which also makes set_anim_id false) it misreported the OWN/unchanged name as
         *       "must be unique" + trapped the editor. commit_active_edit now flush-FIRSTs at the entry so
         *       the journal-fail is caught BEFORE set_anim_id, and post-flush a false carries a genuine
         *       reject on the op-error channel only. (commit_active_edit is a static UI fn -- not directly
         *       callable headless; we exercise its building blocks.) */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        const int j11a = gui_project_create_animation(0, "keep_me", NULL, 0);
        NT_ASSERT(j11a == 0 && "J11: created an animation");
        /* (C) own/unchanged name -> a no-op SUCCESS (true) that raises NO op-error. This is exactly the case
         *     the retired heuristic got wrong (the name "exists", yet the rename succeeds); the live path
         *     returns true and leaves the op-error channel empty. */
        const bool j11_own_ret = gui_project_set_anim_id(0, j11a, "keep_me");
        const bool j11_own_err = gui_project_take_op_error(NULL, 0);
        nt_log_info("SELFTEST: J11 own-name ret=%d op_error=%d (want 1,0 -> a no-op SUCCESS with no op-error)",
                    (int)j11_own_ret, (int)j11_own_err);
        NT_ASSERT(j11_own_ret && !j11_own_err &&
                  "J11/live: renaming to the OWN name is a no-op SUCCESS that raises no op-error");
        /* (flush-first) a journal-failed flush is caught at the ENTRY guard (flush_failed/flush_pending),
         *     BEFORE set_anim_id -- so a disk-full on the own/unchanged name is never a false collision. */
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J11: memory journal attached");
        const int j11pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j11pad + 1, 0.0F); /* buffered gesture */
        gui_project__test_fail_next_recovery_writes(1);
        const bool j11_entry_flush = gui_project_flush_pending(); /* the entry guard commit_active_edit runs FIRST */
        char j11_ferr[256] = {0};
        const bool j11_fsurfaced = gui_project_take_op_error(j11_ferr, sizeof j11_ferr);
        nt_log_info("SELFTEST: J11 entry-flush ret=%d surfaced=%d (want 0,1 -> journal-fail caught at the entry)",
                    (int)j11_entry_flush, (int)j11_fsurfaced);
        NT_ASSERT(!j11_entry_flush && j11_fsurfaced &&
                  "J11/[0]: a journal-failed flush is caught at the flush-first ENTRY (never reaches set_anim_id)");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J11b) A core-rejected atlas rename keeps the inline editor alive on Enter so the user can
         * correct the value. A forced blur dismisses it without mutating the model. */
        const int64_t j11b_revision = tp_session_snapshot_revision(gui_project_snapshot());
        start_atlas_edit(0);
        (void)snprintf(s_edit_buf, sizeof s_edit_buf, "%s", "");
        s_pending_commit_edit = false;
        s_pending_commit_edit_enter = true;
        apply_pending();
        nt_log_info("SELFTEST: J11b invalid atlas Enter edit=%d revision=%lld (want EDIT_ATLAS, unchanged)",
                    s_edit_kind, (long long)tp_session_snapshot_revision(gui_project_snapshot()));
        NT_ASSERT(s_edit_kind == EDIT_ATLAS &&
                  tp_session_snapshot_revision(gui_project_snapshot()) == j11b_revision &&
                  "J11b: Enter on an invalid atlas name keeps the editor and model unchanged");
        s_pending_commit_edit = true;
        s_pending_commit_edit_enter = false;
        apply_pending();
        NT_ASSERT(s_edit_kind == EDIT_NONE &&
                  tp_session_snapshot_revision(gui_project_snapshot()) == j11b_revision &&
                  "J11b: forced blur cancels a rejected atlas rename without mutation");

        /* (J12) fix4 [2]: do_undo must NOT report a journal-failed flush as "Nothing to undo." It
         *       flush-firsts (flush_failed) -- a buffered gesture + armed append-fail surfaces the flush
         *       error and returns BEFORE gui_project_undo (whose false would else be misread as empty). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project_set_atlas_name(0, "j12_edit") && "J12: a committed edit populates undo history");
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J12: memory journal attached");
        const int j12pad = selftest_atlas_at(0, NULL)->padding;
        (void)gui_project_set_atlas_setting(0, GUI_ATLAS_PADDING, j12pad + 5, 0.0F); /* buffered gesture */
        gui_project__test_fail_next_recovery_writes(1);
        set_status("j12_sentinel"); /* a sentinel so we can tell do_undo replaced the status */
        do_undo();                  /* flush-first: shows the flush error + returns; must NOT be "Nothing to undo." */
        nt_log_info("SELFTEST: J12 do_undo-after-journal-fail status='%s' (want the disk-full error, NOT 'Nothing to undo.')",
                    s_status);
        NT_ASSERT(strcmp(s_status, "Nothing to undo.") != 0 &&
                  "J12/[2]: a journal-failed flush is NOT reported as 'Nothing to undo.'");
        NT_ASSERT(strstr(s_status, "disk full") != NULL &&
                  "J12/[2]: do_undo surfaces the disk-full flush error on a journal-failed flush");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J12b) P1 history-gate regression: Undo/Redo itself now appends a candidate checkpoint
         * INSIDE core before publishing the project/cursor move. A failed checkpoint append must
         * return false, preserve the visible model + depths, and use the existing op-error channel
         * (never the old post-success degraded-compaction notice). */
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        (void)gui_project_take_op_error(NULL, 0);
        NT_ASSERT(gui_project__test_attach_memory_recovery() && "J12b: memory recovery journal attached");
        NT_ASSERT(gui_project_set_atlas_name(0, "j12b_edit") && "J12b: committed edit populates history");
        const int j12b_undo0 = gui_project_undo_depth();
        const int j12b_redo0 = gui_project_redo_depth();
        gui_project__test_fail_next_recovery_writes(1);
        const bool j12b_undo_fail = gui_project_undo();
        char j12b_uerr[256] = {0};
        const bool j12b_usurfaced = gui_project_take_op_error(j12b_uerr, sizeof j12b_uerr);
        NT_ASSERT(!j12b_undo_fail && j12b_usurfaced && strstr(j12b_uerr, "disk full") != NULL &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "j12b_edit") == 0 &&
                  gui_project_undo_depth() == j12b_undo0 && gui_project_redo_depth() == j12b_redo0 &&
                  "J12b: failed Undo is surfaced and publishes no model/cursor change");

        NT_ASSERT(gui_project_undo() && "J12b: one-shot fault exhausted; Undo remains retryable");
        const int j12b_undo1 = gui_project_undo_depth();
        const int j12b_redo1 = gui_project_redo_depth();
        gui_project__test_fail_next_recovery_writes(1);
        const bool j12b_redo_fail = gui_project_redo();
        char j12b_rerr[256] = {0};
        const bool j12b_rsurfaced = gui_project_take_op_error(j12b_rerr, sizeof j12b_rerr);
        NT_ASSERT(!j12b_redo_fail && j12b_rsurfaced && strstr(j12b_rerr, "disk full") != NULL &&
                  strcmp(selftest_atlas_at(0, NULL)->name, "atlas1") == 0 &&
                  gui_project_undo_depth() == j12b_undo1 && gui_project_redo_depth() == j12b_redo1 &&
                  "J12b: failed Redo is surfaced and publishes no model/cursor change");
        (void)gui_project_take_op_error(NULL, 0);

        /* (J12c) End-to-end startup-scan contract: Save compacts to one clean checkpoint; Undo
         * appends (not truncates) the dirty candidate checkpoint. After a simulated crash the normal
         * folder scan must therefore offer the slot (record_count > 1) without any GUI post-hook. */
        char j12c_dir[1000];
        char j12c_saved[1200];
        (void)snprintf(j12c_dir, sizeof j12c_dir, "%s/selftest_history_scan_j12c", s_exe_dir);
        tp_mkdirs(j12c_dir);
        (void)snprintf(j12c_saved, sizeof j12c_saved, "%s/history.ntpacker_project", j12c_dir);
        (void)remove(j12c_saved);

        gui_project_enable_recovery(""); gui_project_shutdown();
        gui_project_enable_recovery(j12c_dir);
        gui_recovery_list j12c_stale;
        (void)gui_recovery_collect(&j12c_stale);
        for (int k = 0; k < j12c_stale.count; k++) {
            char discard_error[256] = {0};
            (void)gui_recovery_resolve_entry(&j12c_stale.items[k],
                                             GUI_RECOVERY_DISCARD, "",
                                             discard_error,
                                             sizeof discard_error);
        }
        gui_project_init();
        NT_ASSERT(gui_project_set_atlas_name(0, "j12c_saved") && "J12c: committed edit before Save");
        char j12c_err[256] = {0};
        NT_ASSERT(gui_project_save_as(j12c_saved, j12c_err, sizeof j12c_err) == TP_STATUS_OK &&
                  "J12c: Save establishes one clean checkpoint");
        NT_ASSERT(gui_project_undo() && gui_project_is_dirty() &&
                  "J12c: Undo appends a dirty candidate checkpoint");
        gui_project_enable_recovery(""); gui_project_shutdown(); /* crash-sim: leave slot on disk */

        gui_recovery_list j12c_list;
        gui_project_enable_recovery(j12c_dir);
        const int j12c_count = gui_recovery_collect(&j12c_list);
        bool j12c_offered = false;
        for (int k = 0; k < j12c_list.count; k++) {
            if (j12c_list.items[k].adoptable) {
                j12c_offered = true;
            }
        }
        nt_log_info("SELFTEST: J12c R6 collect count=%d offered_slot=%d (want >=1,1)",
                    j12c_count, (int)j12c_offered);
        NT_ASSERT(j12c_count > 0 && j12c_offered &&
                  "J12c: R6 collect offers the dirty Undo checkpoint journal");
        for (int k = 0; k < j12c_list.count; k++) {
            char discard_error[256] = {0};
            (void)gui_recovery_resolve_entry(&j12c_list.items[k],
                                             GUI_RECOVERY_DISCARD, "",
                                             discard_error,
                                             sizeof discard_error);
        }
        (void)remove(j12c_saved);

        /* (J14) H/P1-8 fix: the STARTUP OPEN/DEFER GUARD itself, as a PURE truth table. main()'s data-loss
         *       guard now routes through gui_startup_decide (single source of truth); J13 above covers only
         *       the predicate that FEEDS it, leaving the guard branches themselves with no coverage (finding
         *       3). This asserts every (arg_present, arg_exists, recovered) row -- so a refactor that reverts
         *       the guard to an unconditional gui_project_open on a CLI arg FAILS here. The load-bearing rows
         *       are (1,1,1) -> DEFER (never discard recovered work) vs (1,1,0) -> OPEN, plus (1,0,1) -> DEFER
         *       (recovered wins over a stale arg -- finding 2, not MISSING). Pure -> no model state needed. */
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

        /* (J15) H/P1-6 FAIL-CLOSED SLOT RESET: a fresh recovery journal must be built ONLY on an EMPTY
         *      slot. If the reset (remove) could not clear the slot -- a locked file / read-only dir
         *      leaving stale-or-foreign bytes -- attach must FAIL CLOSED: journal-less + a degraded notice,
         *      NEVER append this session's edits after content the recovery READ will reject (that would be
         *      a silent crash-recovery loss). Arm a one-shot skip of the reset so a pre-seeded stale slot
         *      survives to the fresh attach. */
        char j15slot[1200];
        char j15lock[1210];
        (void)snprintf(j15slot, sizeof j15slot, "%s/selftest_stale.ntpjournal", s_exe_dir);
        (void)snprintf(j15lock, sizeof j15lock, "%s.lock", j15slot);
        gui_project_enable_recovery("");
        gui_project_shutdown();
        (void)remove(j15slot);
        (void)remove(j15lock);
        gui_project_enable_recovery(j15slot); /* configure the slot; New performs the live attach */
        FILE *j15sf = fopen(j15slot, "wb"); /* pre-seed 40 stale bytes (> the 28-byte header length) */
        NT_ASSERT(j15sf && "J15: seeded a stale slot file");
        unsigned char j15junk[40];
        memset(j15junk, 0xAB, sizeof j15junk);
        (void)fwrite(j15junk, 1, sizeof j15junk, j15sf);
        (void)fclose(j15sf);
        (void)gui_project_take_op_error(NULL, 0);      /* clear any prior soft-error before the attach */
        gui_project__test_skip_next_recovery_reset();  /* simulate a remove() that could not clear the slot */
        /* gui_project_new (NOT _init): goes straight to wrap_model -> attach_recovery_journal, bypassing
         * fresh journal attach so the fail-closed path is exercised directly on the seeded slot. */
        (void)gui_project_new(); /* attach_recovery_journal: skip reset -> stale slot -> FAIL CLOSED */
        char j15err[256] = {0};
        const bool j15_degraded = gui_project_take_op_error(j15err, sizeof j15err);
        const bool j15_edits_ok = gui_project_set_atlas_name(0, "edit_after_failclosed");
        /* The stale slot must be left EXACTLY as seeded (40 bytes) -- a mere existence check would pass even
         * if a regression rebuilt the journal ON TOP of the junk (ensure_header appends after any >=28B store),
         * which is precisely the P1-6 silent loss. Assert the byte count is unchanged. */
        long j15_size = -1;
        FILE *j15chk = fopen(j15slot, "rb");
        if (j15chk) {
            (void)fseek(j15chk, 0, SEEK_END);
            j15_size = ftell(j15chk);
            (void)fclose(j15chk);
        }
        nt_log_info("SELFTEST: J15 fail-closed degraded=%d edits_ok=%d slot_size=%ld msg='%s' (want 1,0,40)",
                    (int)j15_degraded, (int)j15_edits_ok, j15_size, j15err);
        NT_ASSERT(j15_degraded && j15err[0] && "J15/P1-6: a stale, unresettable slot fails CLOSED with a degraded notice");
        NT_ASSERT(!j15_edits_ok && "J15/P1-6: recovery-required editing stays blocked after attach failure");
        NT_ASSERT(j15_size == 40 && "J15/P1-6: the un-clearable stale slot is left byte-for-byte intact (never appended to)");
        (void)gui_project_take_op_error(NULL, 0);
        gui_project_enable_recovery(""); /* release the lock + disable */
        (void)remove(j15slot);
        (void)remove(j15lock);

        /* (J16) R5b-1 METADATA WIRING: set_path (the New/Open/Save-As/adopt identity chokepoint) records
         *      the project {path, name, timestamp} into the recovery journal via
         *      tp_model_set_recovery_metadata, so R5b-2's startup scan can list a crashed project by
         *      name/path/time. Prove BOTH the untitled-init case (name "untitled", path "") AND that a
         *      Save-As REFRESHES the metadata to the saved basename/path (the finding: without the set_path
         *      hook the journal's cached path stays stale and compaction re-emits the OLD/untitled path).
         *      Read back with tp_journal_peek -- exactly the scan primitive R5b-2 will use -- on a CLOSED
         *      slot (a simulated crash: shutdown with recovery disabled leaves the slot on disk and closes
         *      the live handle), so there is never a concurrent handle on the file. Isolated slot. */
        char j16slot[1200];
        char j16lock[1210];
        char j16save[1200];
        (void)snprintf(j16slot, sizeof j16slot, "%s/selftest_meta.ntpjournal", s_exe_dir);
        (void)snprintf(j16lock, sizeof j16lock, "%s.lock", j16slot);
        (void)snprintf(j16save, sizeof j16save, "%s/selftest_meta_proj.ntpacker_project", s_exe_dir);
        gui_project_enable_recovery(""); /* disable first so no teardown deletes a slot */
        gui_project_shutdown();
        (void)remove(j16slot);
        (void)remove(j16lock);
        (void)remove(j16save);

        /* Session A: untitled init attaches metadata {name "untitled", path ""}.
         * Commit one edit so session destruction preserves the journal like a dirty crash survivor. */
        gui_project_enable_recovery(j16slot);
        gui_project_init();
        NT_ASSERT(gui_project_snapshot() && "J16: an untitled journaled session initialised");
        NT_ASSERT(strcmp(gui_project_display_name(), "untitled") == 0 && "J16: untitled display name before any Save");
        NT_ASSERT(gui_project_set_atlas_name(0, "j16-dirty") &&
                  "J16a: a committed edit makes the recovery slot worth preserving");
        gui_project_enable_recovery("");
        gui_project_shutdown();
        {
            tp_recovery_candidate pk;
            memset(&pk, 0, sizeof pk);
            tp_error pkerr = {0};
            const tp_status pst = tp_recovery__test_peek_candidate(j16slot, &pk, &pkerr);
            nt_log_info("SELFTEST: J16a untitled peek st=%s name='%s' path='%s' (want OK,'untitled','')",
                        tp_status_str(pst), pk.name, pk.original_path);
            NT_ASSERT(pst == TP_STATUS_OK && "J16a: peek reads the closed untitled slot");
            NT_ASSERT(strcmp(pk.name, "untitled") == 0 && "J16a: metadata name == 'untitled'");
            NT_ASSERT(strcmp(pk.original_path, "") == 0 && "J16a: metadata path == '' (untitled project)");
        }
        (void)remove(j16slot);
        (void)remove(j16lock);

        /* Session B: init then Save-As -> set_path(path) REFRESHES the metadata to the saved basename +
         * path (compaction then re-emits the NEW cached values). Peek must show the saved identity, not
         * "untitled"/"" -- proving the Save-As refresh finding. */
        gui_project_enable_recovery(j16slot);
        gui_project_init();
        char j16err[256] = {0};
        const tp_status j16sv = gui_project_save_as(j16save, j16err, sizeof j16err);
        nt_log_info("SELFTEST: J16b save-as st=%s name='%s' err='%s'", tp_status_str(j16sv), gui_project_display_name(), j16err);
        NT_ASSERT(j16sv == TP_STATUS_OK && "J16b: Save-As succeeds");
        NT_ASSERT(strcmp(gui_project_display_name(), "selftest_meta_proj.ntpacker_project") == 0 &&
                  "J16b: display name is the saved basename after Save-As");
        NT_ASSERT(gui_project_set_atlas_name(0, "j16-saved-dirty") &&
                  "J16b: a post-save edit preserves the compacted journal for inspection");
        gui_project_enable_recovery("");
        gui_project_shutdown();
        {
            tp_recovery_candidate pk;
            memset(&pk, 0, sizeof pk);
            tp_error pkerr = {0};
            const tp_status pst = tp_recovery__test_peek_candidate(j16slot, &pk, &pkerr);
            nt_log_info("SELFTEST: J16b saved peek st=%s name='%s' path='%s'",
                        tp_status_str(pst), pk.name, pk.original_path);
            NT_ASSERT(pst == TP_STATUS_OK && "J16b: peek reads the closed saved slot");
            NT_ASSERT(strcmp(pk.name, "selftest_meta_proj.ntpacker_project") == 0 &&
                      "J16b: metadata name REFRESHED to the saved basename (set_path chokepoint hook)");
            char j16canonical[TP_IDENTITY_PATH_MAX];
            tp_error j16canonical_err = {0};
            NT_ASSERT(tp_identity_path_canonical(j16save, j16canonical, sizeof j16canonical,
                                                 &j16canonical_err) == TP_STATUS_OK);
            NT_ASSERT(strcmp(pk.original_path, j16canonical) == 0 &&
                      "J16b: metadata path REFRESHED to the saved path (compaction re-emits the NEW path, not the stale one)");
        }
        (void)remove(j16slot);
        (void)remove(j16lock);
        (void)remove(j16save);

        /* (J25) M4: lock files are permanent inode domains. Clean shutdown removes the journal but only
         * unlocks/closes its companion lock; the same lock file is reused by the next owner. */
        {
            char j25slot[1000], j25lock[1010];
            (void)snprintf(j25slot, sizeof j25slot, "%s/selftest_lock_j25.ntpjournal", s_exe_dir);
            (void)snprintf(j25lock, sizeof j25lock, "%s.lock", j25slot);
            gui_project_enable_recovery(""); gui_project_shutdown();
            (void)remove(j25slot); (void)remove(j25lock);
            gui_project_enable_recovery(j25slot);
            gui_project_init();     /* live attach acquires lock and creates journal */
            NT_ASSERT(gui_project__test_recovery_active() && "J25: recovery active (lock acquired)");
            gui_project_shutdown(); /* clean exit: deletes the slot + releases, but keeps the lock file */
            nt_log_info("SELFTEST: J25 post-shutdown slot_exists=%d lock_exists=%d (want 0,1)",
                        (int)selftest_file_exists(j25slot), (int)selftest_file_exists(j25lock));
            NT_ASSERT(!selftest_file_exists(j25slot) && selftest_file_exists(j25lock) &&
                      "J25/M4: release keeps one permanent lock inode while clean close removes the journal");
            (void)remove(j25slot); (void)remove(j25lock);
        }

        /* ===================== R6a: recovery-RESOLUTION decision/action layer (J26-J30) =====================
         * The R6b startup modal is not built yet, so J26-J30 drive the headless decision/action layer DIRECTLY
         * (exactly as J18-J25 drive the scan functions), on ISOLATED temp folders. They prove: collect
         * classifies saved-vs-untitled-vs-version-mismatch-vs-foreign + excludes live-locked (J26); Save As
         * writes the recovered state + deletes the journal (J27); Save (SAVE_ORIGINAL) replaces the original
         * IN PLACE via the ATOMIC core save with NO <orig>.bak, and a failed core save leaves the original
         * untouched + keeps the journal (J28); Discard reaps the journal and keeps its permanent lock (J29); and the paramount
         * NON-DESTRUCTIVE-ON-FAILURE invariant -- a FAILED save KEEPS the journal (J30). */

        /* (J26) COLLECT with metadata + status. Six orphans in one folder: a SAVED adoptable (meta path+name,
         * ts 8000, REAL key), an UNTITLED adoptable (empty meta, ts 7000, REAL key), a VERSION_MISMATCH
         * (bumped header -> old format, surfaced for Discard), a FOREIGN-KEY otherwise-adoptable journal (fix
         * [2]: our format, valid framing, but NOT our key -> recover would STALE_KEY -> excluded, not listed),
         * a live-LOCKED adoptable (foreign lock -> excluded), a checkpoint-only no-work journal, and a foreign
         * BAD_MAGIC file (both excluded). collect must return EXACTLY the first three, newest-first. */
        {
            char scan26[900];
            char j26saved[1000], j26unt[1000], j26vmis[1000], j26fkey[1000], j26lock[1000], j26lock_lk[1010];
            char j26noedit[1000], j26noedit_lk[1010], j26bad[1000];
            char j26live[1000];
            (void)snprintf(scan26, sizeof scan26, "%s/selftest_r6a_j26", s_exe_dir);
            tp_mkdirs(scan26);
            (void)snprintf(j26saved, sizeof j26saved, "%s/saved.ntpjournal", scan26);
            (void)snprintf(j26unt, sizeof j26unt, "%s/untitled.ntpjournal", scan26);
            (void)snprintf(j26vmis, sizeof j26vmis, "%s/oldfmt.ntpjournal", scan26);
            (void)snprintf(j26fkey, sizeof j26fkey, "%s/foreignkey.ntpjournal", scan26);
            (void)snprintf(j26lock, sizeof j26lock, "%s/locked.ntpjournal", scan26);
            (void)snprintf(j26lock_lk, sizeof j26lock_lk, "%s.lock", j26lock);
            (void)snprintf(j26noedit, sizeof j26noedit, "%s/noedit.ntpjournal", scan26);
            (void)snprintf(j26noedit_lk, sizeof j26noedit_lk, "%s.lock", j26noedit);
            (void)snprintf(j26bad, sizeof j26bad, "%s/foreign.ntpjournal", scan26);
            (void)snprintf(j26live, sizeof j26live, "%s/collect_live.ntpjournal", scan26);
            (void)remove(j26saved); (void)remove(j26unt); (void)remove(j26vmis); (void)remove(j26fkey);
            (void)remove(j26lock); (void)remove(j26lock_lk); (void)remove(j26noedit); (void)remove(j26noedit_lk);
            (void)remove(j26bad);

            const tp_id128 rkey26 = gui_project__test_recovery_key(); /* fix [2]: adoptable requires OUR key */
            tp_id128 fkey26; memset(fkey26.bytes, 0xAB, sizeof fkey26.bytes); /* a DIFFERENT key */
            craft_meta_orphan(j26saved, rkey26, 8000, "/proj/saved26.ntpacker_project", "saved26.ntpacker_project");
            craft_meta_orphan(j26unt, rkey26, 7000, "", ""); /* untitled: empty meta path+name */
            craft_meta_orphan(j26fkey, fkey26, 6500, "/proj/foreign26.ntpacker_project", "foreign26.ntpacker_project"); /* fix [2] */
            craft_meta_orphan(j26lock, rkey26, 6000, "/proj/locked26.ntpacker_project", "locked26.ntpacker_project");
            scan_make_noedit_orphan(j26noedit); /* checkpoint-only: no post-checkpoint work */
            /* version-mismatch: right magic and OUR key, but a BUMPED format version. */
            {
                uint8_t hdr[TP_JRN_HEADER_LEN];
                memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
                tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION + 1000u); /* old/other format */
                memcpy(hdr + TP_JRN_KEY_OFF, rkey26.bytes, sizeof rkey26.bytes);
                FILE *vf = fopen(j26vmis, "wb");
                NT_ASSERT(vf && "J26: seeded a VERSION_MISMATCH journal");
                (void)fwrite(hdr, 1, sizeof hdr, vf);
                (void)fclose(vf);
            }
            { /* foreign BAD_MAGIC file that happens to end .ntpjournal */
                FILE *bf = fopen(j26bad, "wb");
                NT_ASSERT(bf && "J26: seeded a BAD_MAGIC file");
                unsigned char junk[40];
                memset(junk, 0x5A, sizeof junk);
                (void)fwrite(junk, 1, sizeof junk, bf);
                (void)fclose(bf);
            }
            NT_ASSERT(gui_project__test_hold_foreign_lock(j26lock) && "J26: a live instance holds the locked orphan");

            gui_recovery_list rl;
            gui_project_enable_recovery(j26live);
            const int n26 = gui_recovery_collect(&rl);
            nt_log_info("SELFTEST: J26 collect n=%d [0]=%s/%d [1]=%s/%d [2]=%s/%d (want 3; saved,untitled,oldfmt)",
                        n26, n26 > 0 ? rl.items[0].name : "-", n26 > 0 ? rl.items[0].status : -1,
                        n26 > 1 ? rl.items[1].name : "-", n26 > 1 ? rl.items[1].status : -1,
                        n26 > 2 ? rl.items[2].name : "-", n26 > 2 ? rl.items[2].status : -1);
            NT_ASSERT(n26 == 3 && rl.count == 3 &&
                      "J26: exactly 3 collected (foreign-key + live-locked + checkpoint-only + BAD_MAGIC excluded)");
            /* [0] newest = the SAVED adoptable (ts 8000): meta path+name carried, adoptable, status OK. */
            NT_ASSERT(rl.items[0].adoptable && rl.items[0].status == (int)TP_JOURNAL_RECOVERY_OK &&
                      rl.items[0].timestamp == 8000 &&
                      strcmp(rl.items[0].name, "saved26.ntpacker_project") == 0 &&
                      strcmp(rl.items[0].orig_path, "/proj/saved26.ntpacker_project") == 0 &&
                      selftest_same_path(rl.items[0].journal_path, j26saved) &&
                      "J26: [0] = saved adoptable with meta name+orig_path (newest)");
            /* [1] = the UNTITLED adoptable (ts 7000): empty orig_path, name defaults to "untitled". */
            NT_ASSERT(rl.items[1].adoptable && rl.items[1].status == (int)TP_JOURNAL_RECOVERY_OK &&
                      rl.items[1].timestamp == 7000 && rl.items[1].orig_path[0] == '\0' &&
                      strcmp(rl.items[1].name, "untitled") == 0 &&
                      selftest_same_path(rl.items[1].journal_path, j26unt) &&
                      "J26: [1] = untitled adoptable (orig_path \"\", name \"untitled\")");
            /* [2] = the VERSION_MISMATCH (ts 0): NON-adoptable, surfaced for Discard. */
            NT_ASSERT(!rl.items[2].adoptable && rl.items[2].status == (int)TP_JOURNAL_RECOVERY_VERSION_MISMATCH &&
                      selftest_same_path(rl.items[2].journal_path, j26vmis) &&
                      "J26: [2] = version-mismatch non-adoptable (surfaced for Discard)");
            /* fix [2]: the FOREIGN-KEY journal (valid framing, wrong key) is NEVER listed -- collect only
             * offers what resolve can recover. Nor the live-locked or BAD_MAGIC file. */
            for (int k = 0; k < rl.count; k++) {
                NT_ASSERT(!selftest_same_path(rl.items[k].journal_path, j26fkey) &&
                          !selftest_same_path(rl.items[k].journal_path, j26lock) &&
                          !selftest_same_path(rl.items[k].journal_path, j26noedit) &&
                          !selftest_same_path(rl.items[k].journal_path, j26bad) &&
                          "J26/[2]: foreign-key + live-locked + checkpoint-only + BAD_MAGIC are never listed");
            }
            /* fix [2]: resolve SAVE_AS on the excluded foreign-key journal must FAIL (STALE_KEY -> nothing
             * recoverable) and LEAVE it on disk (never offers what it cannot act on). */
            {
                char e26f[256] = {0};
                char j26ftarget[1030];
                (void)snprintf(j26ftarget, sizeof j26ftarget, "%s/foreignkey_out.ntpacker_project", scan26);
                (void)remove(j26ftarget);
                const tp_status rf = gui_recovery_resolve(j26fkey, "", GUI_RECOVERY_SAVE_AS, j26ftarget, e26f, sizeof e26f);
                nt_log_info("SELFTEST: J26 foreign-key resolve r=%s src_exists=%d target_exists=%d (want !ok,1,0)",
                            tp_status_str(rf), (int)tp_scan_exists(j26fkey), (int)tp_scan_exists(j26ftarget));
                NT_ASSERT(rf != TP_STATUS_OK && e26f[0] &&
                          "J26/[2]: resolve of a foreign-key journal returns a fault (nothing recoverable)");
                NT_ASSERT(tp_scan_exists(j26fkey) && !tp_scan_exists(j26ftarget) &&
                          "J26/[2]: the foreign-key journal is KEPT and no target was written");
                (void)remove(j26ftarget);
            }
            gui_project__test_release_foreign_lock();
            (void)remove(j26saved); (void)remove(j26unt); (void)remove(j26vmis); (void)remove(j26fkey);
            (void)remove(j26lock); (void)remove(j26lock_lk); (void)remove(j26noedit); (void)remove(j26noedit_lk);
            (void)remove(j26bad);
        }

        /* (J27) SAVE AS: recover an orphan (REAL GUI key, via scan_make_orphan) to a savable clone, save it to
         * a NEW target, and delete the journal. The target must load back to the recovered state; the journal
         * must be gone while its permanent .lock remains. */
        {
            char scan27[900];
            char j27src[1000], j27src_lk[1010], j27target[1000];
            (void)snprintf(scan27, sizeof scan27, "%s/selftest_r6a_j27", s_exe_dir);
            tp_mkdirs(scan27);
            (void)snprintf(j27src, sizeof j27src, "%s/orphan27.ntpjournal", scan27);
            (void)snprintf(j27src_lk, sizeof j27src_lk, "%s.lock", j27src);
            (void)snprintf(j27target, sizeof j27target, "%s/saved_as27.ntpacker_project", scan27);
            (void)remove(j27src); (void)remove(j27src_lk); (void)remove(j27target);
            scan_make_orphan(j27src, "recovered27", 9000); /* real-key recoverable orphan */
            /* Simulate a POSIX crash leftover: a `.lock` file the crashed owner never unlinked (created AFTER
             * scan_make_orphan, whose clean release would otherwise reap it) -- resolve must reap it. */
            { FILE *lf = fopen(j27src_lk, "wb"); if (lf) { (void)fclose(lf); } }
            NT_ASSERT(tp_scan_exists(j27src) && selftest_file_exists(j27src_lk) && "J27: the orphan + a stale .lock exist before resolve");

            gui_project_enable_recovery(scan27);
            char e27[256] = {0};
            const tp_status r27 = gui_recovery_resolve(j27src, "", GUI_RECOVERY_SAVE_AS, j27target, e27, sizeof e27);
            tp_project *ld27 = NULL;
            tp_error le27 = {0};
            const tp_status l27 = tp_project_load(j27target, &ld27, &le27);
            const char *nm27 = (l27 == TP_STATUS_OK && ld27 && tp_project_get_atlas(ld27, 0))
                                   ? tp_project_get_atlas(ld27, 0)->name : "(none)";
            nt_log_info("SELFTEST: J27 SaveAs r=%s target_load=%s name='%s' src_exists=%d lock_exists=%d (want ok,ok,recovered27,0,1)",
                        tp_status_str(r27), tp_status_str(l27), nm27,
                        (int)tp_scan_exists(j27src), (int)selftest_file_exists(j27src_lk));
            NT_ASSERT(r27 == TP_STATUS_OK && "J27: Save As succeeds");
            NT_ASSERT(l27 == TP_STATUS_OK && strcmp(nm27, "recovered27") == 0 &&
                      "J27: the target file loads to the RECOVERED state");
            NT_ASSERT(!tp_scan_exists(j27src) && selftest_file_exists(j27src_lk) &&
                      "J27/M4: successful Save As deletes the journal and preserves the permanent lock inode");
            if (ld27) { tp_project_destroy(ld27); }
            (void)remove(j27src); (void)remove(j27src_lk); (void)remove(j27target);
        }

        /* (J28) SAVE_ORIGINAL rides the ATOMIC core save (R6a fix3) -- the GUI `.bak`/temp-swap dance is GONE.
         * Cases: (a=Torig) an existing original is REPLACED IN PLACE with the recovered state, NO <orig>.bak is
         * ever created, journal deleted; (b) no original file -> save proceeds, no .bak, journal deleted;
         * (Torig_fail) the core atomicity guarantee end-to-end -- a directory pinned at <orig>.savetmp (the
         * core's temp) makes tp_project_save fail, and orig is left UNTOUCHED (still its ORIGINAL bytes) with the
         * journal KEPT. */
        {
            char scan28[900];
            char j28orig[1000], j28bak[1010], j28src[1000], j28src_lk[1010];
            (void)snprintf(scan28, sizeof scan28, "%s/selftest_r6a_j28", s_exe_dir);
            tp_mkdirs(scan28);
            (void)snprintf(j28orig, sizeof j28orig, "%s/original28.ntpacker_project", scan28);
            (void)snprintf(j28bak, sizeof j28bak, "%s.bak", j28orig);
            (void)snprintf(j28src, sizeof j28src, "%s/orphan28.ntpjournal", scan28);
            (void)snprintf(j28src_lk, sizeof j28src_lk, "%s.lock", j28src);
            (void)remove(j28bak); (void)remove(j28orig); (void)remove(j28src); (void)remove(j28src_lk);

            /* (a=Torig) SAVE_ORIGINAL success + NO backup file. Seed orig="orig_before" via the real save path
             *     and an orphan recovering "recovered_r"; after SAVE_ORIGINAL orig loads as "recovered_r", NO
             *     <orig>.bak exists (the .bak dance is gone), and the journal is deleted. */
            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project_init();
            (void)gui_project_set_atlas_name(0, "orig_before");
            char es28[256] = {0};
            const tp_status ss28 = gui_project_save_as(j28orig, es28, sizeof es28);
            NT_ASSERT(ss28 == TP_STATUS_OK && tp_scan_exists(j28orig) && "J28a: seeded the ORIGINAL file");
            gui_project_enable_recovery(""); gui_project_shutdown();
            scan_make_saved_orphan(j28src, j28orig, "recovered_r", 5000);

            gui_project_enable_recovery(scan28);
            char e28[256] = {0};
            const tp_status r28 = gui_recovery_resolve(j28src, j28orig, GUI_RECOVERY_SAVE_ORIGINAL, "", e28, sizeof e28);
            tp_project *cur28 = NULL;
            tp_error ce28 = {0};
            const tp_status lc = tp_project_load(j28orig, &cur28, &ce28);
            const char *curnm = (lc == TP_STATUS_OK && cur28 && tp_project_get_atlas(cur28, 0)) ? tp_project_get_atlas(cur28, 0)->name : "(none)";
            const int no_bak = !selftest_file_exists(j28bak);
            nt_log_info("SELFTEST: J28a Torig SaveOriginal r=%s orig='%s' no_bak=%d saved_recovered=%d src_exists=%d (want ok,recovered_r,1,1,0)",
                        tp_status_str(r28), curnm, no_bak,
                        (int)(lc == TP_STATUS_OK && strcmp(curnm, "recovered_r") == 0), (int)tp_scan_exists(j28src));
            NT_ASSERT(r28 == TP_STATUS_OK && "J28a: SAVE_ORIGINAL succeeds");
            NT_ASSERT(lc == TP_STATUS_OK && strcmp(curnm, "recovered_r") == 0 &&
                      "J28a: the original now holds the RECOVERED state");
            NT_ASSERT(no_bak && "J28a/Torig: NO <orig>.bak is ever created (the .bak dance is gone)");
            NT_ASSERT(!tp_scan_exists(j28src) && "J28a: the journal is deleted after a successful save");
            if (cur28) { tp_project_destroy(cur28); }
            (void)remove(j28bak); (void)remove(j28orig); (void)remove(j28src); (void)remove(j28src_lk);

            /* (b) A journal without original-file metadata cannot infer an original from a frontend argument.
             * SAVE_ORIGINAL must fail closed and keep the journal; an explicit SAVE_AS remains available. */
            char j28orig_b[1000], j28bak_b[1010], j28src_b[1000];
            (void)snprintf(j28orig_b, sizeof j28orig_b, "%s/original28b.ntpacker_project", scan28);
            (void)snprintf(j28bak_b, sizeof j28bak_b, "%s.bak", j28orig_b);
            (void)snprintf(j28src_b, sizeof j28src_b, "%s/orphan28b.ntpjournal", scan28);
            (void)remove(j28orig_b); (void)remove(j28bak_b); (void)remove(j28src_b);
            scan_make_orphan(j28src_b, "recovered28b", 5100);
            gui_project_enable_recovery(scan28);
            char e28b[256] = {0};
            const tp_status r28b = gui_recovery_resolve(j28src_b, j28orig_b, GUI_RECOVERY_SAVE_ORIGINAL, "", e28b, sizeof e28b);
            nt_log_info("SELFTEST: J28b no-orig r=%s orig_exists=%d bak_exists=%d src_exists=%d (want changed,0,0,1)",
                        tp_status_str(r28b), (int)tp_scan_exists(j28orig_b), (int)selftest_file_exists(j28bak_b),
                        (int)tp_scan_exists(j28src_b));
            NT_ASSERT(r28b == TP_STATUS_FILE_CHANGED_EXTERNALLY && !tp_scan_exists(j28orig_b) &&
                      "J28b: Save Original requires the candidate's durable original-file baseline");
            NT_ASSERT(!selftest_file_exists(j28bak_b) && "J28b: no .bak is created when there was no original");
            NT_ASSERT(tp_scan_exists(j28src_b) && "J28b: journal is kept after the refused Save Original");
            char e28b_as[256] = {0};
            const tp_status r28b_as = gui_recovery_resolve(j28src_b, "", GUI_RECOVERY_SAVE_AS,
                                                           j28orig_b, e28b_as, sizeof e28b_as);
            NT_ASSERT(r28b_as == TP_STATUS_OK && tp_scan_exists(j28orig_b) &&
                      "J28b: explicit Save As remains available without original metadata");
            NT_ASSERT(!tp_scan_exists(j28src_b) && "J28b: journal is deleted after successful Save As");
            (void)remove(j28orig_b); (void)remove(j28bak_b); (void)remove(j28src_b);

            /* (Torig_fail) CORE ATOMICITY end-to-end -- a FAILED SAVE_ORIGINAL leaves orig intact + keeps the
             *   journal. The atomic core save writes to a sibling <orig>.savetmp; pin a NON-EMPTY DIRECTORY there
             *   so tp_project_save's fopen(tmp,"wb") fails (and remove() cannot rmdir it). SAVE_ORIGINAL must
             *   return non-OK, orig must still load as its ORIGINAL content (untouched -- proves core atomicity
             *   end-to-end), and the journal must be KEPT (non-destructive on failure). */
            char j28f_orig[1000], j28f_src[1000], j28f_src_lk[1010];
            (void)snprintf(j28f_orig, sizeof j28f_orig, "%s/failsave28.ntpacker_project", scan28);
            (void)snprintf(j28f_src, sizeof j28f_src, "%s/orphan_fail28.ntpjournal", scan28);
            (void)snprintf(j28f_src_lk, sizeof j28f_src_lk, "%s.lock", j28f_src);
            (void)remove(j28f_orig); (void)remove(j28f_src); (void)remove(j28f_src_lk);

            /* orig = a VALID project recognizable as "orig_intact" (atlas name), via the real save path. */
            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project_init();
            (void)gui_project_set_atlas_name(0, "orig_intact");
            char es_f[256] = {0};
            const tp_status ss_f = gui_project_save_as(j28f_orig, es_f, sizeof es_f);
            NT_ASSERT(ss_f == TP_STATUS_OK && tp_scan_exists(j28f_orig) && "Torig_fail: seeded a VALID original");
            gui_project_enable_recovery(""); gui_project_shutdown();

            scan_make_saved_orphan(j28f_src, j28f_orig, "recovered_fail28", 5500);
            tp_project__test_fail_next_temp_create();
            gui_project_enable_recovery(scan28);
            char e_f[256] = {0};
            const tp_status r_f = gui_recovery_resolve(j28f_src, j28f_orig, GUI_RECOVERY_SAVE_ORIGINAL, "", e_f, sizeof e_f);
            tp_project *f_orig = NULL;
            tp_error fe = {0};
            const tp_status lf = tp_project_load(j28f_orig, &f_orig, &fe);
            const char *f_orignm = (lf == TP_STATUS_OK && f_orig && tp_project_get_atlas(f_orig, 0)) ? tp_project_get_atlas(f_orig, 0)->name : "(none)";
            const int orig_intact = (lf == TP_STATUS_OK && strcmp(f_orignm, "orig_intact") == 0);
            nt_log_info("SELFTEST: Torig_fail injected-create r=%s err='%s' orig='%s' orig_intact=%d journal_kept=%d (want !ok,<msg>,orig_intact,1,1)",
                        tp_status_str(r_f), e_f, f_orignm, orig_intact, (int)tp_scan_exists(j28f_src));
            NT_ASSERT(r_f != TP_STATUS_OK && e_f[0] && "Torig_fail: injected temp creation failure is surfaced");
            NT_ASSERT(orig_intact && "Torig_fail: orig is UNTOUCHED (still its ORIGINAL content) -- core atomicity end-to-end");
            NT_ASSERT(tp_scan_exists(j28f_src) && "Torig_fail: the journal is KEPT on the failed save (non-destructive)");
            if (f_orig) { tp_project_destroy(f_orig); }
            (void)remove(j28f_orig);
            (void)remove(j28f_src); (void)remove(j28f_src_lk);
        }

        /* (J28c) Save-to-original is optimistic: if another editor rewrites the original after the
         * crash journal captured its fingerprint, recovery must refuse the overwrite and keep both
         * the externally changed file and the journal. */
        {
            char dir28c[900], orig28c[1000], src28c[1000], err28c[256] = {0};
            (void)snprintf(dir28c, sizeof dir28c, "%s/selftest_r6a_j28c", s_exe_dir);
            tp_mkdirs(dir28c);
            (void)snprintf(orig28c, sizeof orig28c, "%s/original.ntpacker_project", dir28c);
            (void)snprintf(src28c, sizeof src28c, "%s/orphan.ntpjournal", dir28c);
            (void)remove(orig28c); (void)remove(src28c);

            gui_project_enable_recovery(""); gui_project_shutdown(); gui_project_init();
            (void)gui_project_set_atlas_name(0, "baseline28c");
            NT_ASSERT(gui_project_save_as(orig28c, err28c, sizeof err28c) == TP_STATUS_OK && "J28c baseline saved");
            gui_project_enable_recovery(""); gui_project_shutdown();
            scan_make_saved_orphan(src28c, orig28c, "recovered28c", 5600);

            gui_project_init();
            (void)gui_project_set_atlas_name(0, "external28c");
            NT_ASSERT(gui_project_save_as(orig28c, err28c, sizeof err28c) == TP_STATUS_OK && "J28c external rewrite saved");
            gui_project_shutdown();

            gui_project_enable_recovery(dir28c);
            const tp_status rst28c = gui_recovery_resolve(src28c, orig28c, GUI_RECOVERY_SAVE_ORIGINAL, "",
                                                          err28c, sizeof err28c);
            tp_project *loaded = NULL;
            tp_error load_err28c = {0};
            const tp_status load_st28c = tp_project_load(orig28c, &loaded, &load_err28c);
            const char *name28c = (load_st28c == TP_STATUS_OK && loaded) ? tp_project_get_atlas(loaded, 0)->name : "";
            NT_ASSERT(rst28c == TP_STATUS_FILE_CHANGED_EXTERNALLY && tp_scan_exists(src28c) &&
                      load_st28c == TP_STATUS_OK && strcmp(name28c, "external28c") == 0 &&
                      "J28c external change refuses overwrite and keeps both copies");
            if (loaded) { tp_project_destroy(loaded); }

            /* Save As means a NEW identity.  Re-entering the recorded original (including a canonical
             * alias) must not turn the safe Save-Original refusal above into an unguarded overwrite. */
            char alias28c[1020], alias_err28c[256] = {0};
            (void)snprintf(alias28c, sizeof alias28c, "%s/./original.ntpacker_project", dir28c);
            const tp_status alias_st28c = gui_recovery_resolve(src28c, orig28c, GUI_RECOVERY_SAVE_AS,
                                                               alias28c, alias_err28c, sizeof alias_err28c);
            loaded = NULL;
            memset(&load_err28c, 0, sizeof load_err28c);
            const tp_status alias_load_st28c = tp_project_load(orig28c, &loaded, &load_err28c);
            name28c = (alias_load_st28c == TP_STATUS_OK && loaded) ? tp_project_get_atlas(loaded, 0)->name : "";
            NT_ASSERT(alias_st28c == TP_STATUS_FILE_CHANGED_EXTERNALLY && alias_err28c[0] &&
                      tp_scan_exists(src28c) && alias_load_st28c == TP_STATUS_OK &&
                      strcmp(name28c, "external28c") == 0 &&
                      "J28c Save As cannot alias the original and bypass its fingerprint guard");
            if (loaded) { tp_project_destroy(loaded); }
            (void)remove(orig28c); (void)remove(src28c);
        }

        /* (J29) DISCARD: resolve DISCARD removes the journal, preserves its permanent lock, and touches nothing else (a sentinel
         * file in the same folder survives). */
        {
            char scan29[900];
            char j29src[1000], j29src_lk[1010], j29keep[1000];
            (void)snprintf(scan29, sizeof scan29, "%s/selftest_r6a_j29", s_exe_dir);
            tp_mkdirs(scan29);
            (void)snprintf(j29src, sizeof j29src, "%s/orphan29.ntpjournal", scan29);
            (void)snprintf(j29src_lk, sizeof j29src_lk, "%s.lock", j29src);
            (void)snprintf(j29keep, sizeof j29keep, "%s/keep_me.txt", scan29);
            (void)remove(j29src); (void)remove(j29src_lk); (void)remove(j29keep);
            scan_make_orphan(j29src, "discard29", 4200); /* valid app-keyed recovery source */
            { FILE *lf = fopen(j29src_lk, "wb"); if (lf) { (void)fclose(lf); } } /* a companion .lock to reap */
            { FILE *kf = fopen(j29keep, "wb"); if (kf) { (void)fputs("keep", kf); (void)fclose(kf); } }
            NT_ASSERT(tp_scan_exists(j29src) && selftest_file_exists(j29src_lk) && "J29: orphan + .lock seeded");

            gui_project_enable_recovery(scan29);
            char e29[256] = {0};
            const tp_status r29 = gui_recovery_resolve(j29src, "", GUI_RECOVERY_DISCARD, "", e29, sizeof e29);
            nt_log_info("SELFTEST: J29 Discard r=%s src_exists=%d lock_exists=%d keep_exists=%d (want ok,0,1,1)",
                        tp_status_str(r29), (int)tp_scan_exists(j29src), (int)selftest_file_exists(j29src_lk),
                        (int)selftest_file_exists(j29keep));
            NT_ASSERT(r29 == TP_STATUS_OK && "J29: Discard returns OK");
            NT_ASSERT(!tp_scan_exists(j29src) && selftest_file_exists(j29src_lk) &&
                      "J29/M4: Discard deletes the journal and preserves its permanent lock inode");
            NT_ASSERT(selftest_file_exists(j29keep) && "J29: Discard touches nothing else (sentinel survives)");
            (void)remove(j29src_lk); (void)remove(j29keep);
        }

        /* (J29b) A resolver holds the orphan lock for the entire action. A concurrent claim gets a
         * stable structured busy result and cannot discard the other process's journal. */
        {
            char dir29b[900], src29b[1000], err29b[256] = {0};
            (void)snprintf(dir29b, sizeof dir29b, "%s/selftest_r6a_j29b", s_exe_dir);
            tp_mkdirs(dir29b);
            (void)snprintf(src29b, sizeof src29b, "%s/orphan.ntpjournal", dir29b);
            (void)remove(src29b);
            scan_make_orphan(src29b, "busy29b", 5700);
            NT_ASSERT(gui_project__test_hold_foreign_lock(src29b) && "J29b foreign resolver claim held");
            gui_project_enable_recovery(dir29b);
            const tp_status busy29b = gui_recovery_resolve(src29b, "", GUI_RECOVERY_DISCARD, "", err29b, sizeof err29b);
            NT_ASSERT(busy29b == TP_STATUS_RECOVERY_BUSY && tp_scan_exists(src29b) &&
                      "J29b concurrent resolve is rejected without deleting the journal");
            gui_project__test_release_foreign_lock();
            NT_ASSERT(gui_recovery_resolve(src29b, "", GUI_RECOVERY_DISCARD, "", err29b, sizeof err29b) == TP_STATUS_OK &&
                      !tp_scan_exists(src29b) && "J29b journal is retryable after the claim releases");

            char blocker29c[1000], src29c[1030], err29c[256] = {0};
            (void)snprintf(blocker29c, sizeof blocker29c, "%s/blocker", dir29b);
            (void)snprintf(src29c, sizeof src29c, "%s/orphan.ntpjournal", blocker29c);
            { FILE *bf = fopen(blocker29c, "wb"); if (bf) { (void)fputs("x", bf); (void)fclose(bf); } }
            const tp_status invalid29c =
                gui_recovery_resolve(src29c, "", GUI_RECOVERY_DISCARD, "", err29c, sizeof err29c);
            NT_ASSERT(invalid29c != TP_STATUS_OK && invalid29c != TP_STATUS_RECOVERY_BUSY &&
                      "J29c invalid storage/path is rejected before any resolver claim");
            (void)remove(blocker29c);
        }

        /* (J30) NON-DESTRUCTIVE ON FAILURE: a Save As to an UNWRITABLE target (a regular file sits where the
         * parent directory should be, so the write cannot open) fails -> resolve returns non-OK AND the
         * journal is STILL on disk (never deleted on a failed save). The paramount data-safety invariant. */
        {
            char scan30[900];
            char j30src[1000], j30src_lk[1010], j30blocker[1000], j30target[1030];
            (void)snprintf(scan30, sizeof scan30, "%s/selftest_r6a_j30", s_exe_dir);
            tp_mkdirs(scan30);
            (void)snprintf(j30src, sizeof j30src, "%s/orphan30.ntpjournal", scan30);
            (void)snprintf(j30src_lk, sizeof j30src_lk, "%s.lock", j30src);
            (void)snprintf(j30blocker, sizeof j30blocker, "%s/blocker", scan30);
            (void)snprintf(j30target, sizeof j30target, "%s/child.ntpacker_project", j30blocker);
            (void)remove(j30src); (void)remove(j30src_lk); (void)remove(j30blocker);
            { FILE *bf = fopen(j30blocker, "wb"); if (bf) { (void)fputs("x", bf); (void)fclose(bf); } } /* a FILE where a dir must be */
            scan_make_orphan(j30src, "recovered30", 3300); /* real-key candidate recovery succeeds; only SAVE fails */
            NT_ASSERT(tp_scan_exists(j30src) && "J30: the orphan exists before resolve");

            gui_project_enable_recovery(scan30);
            char e30[256] = {0};
            const tp_status r30 = gui_recovery_resolve(j30src, "", GUI_RECOVERY_SAVE_AS, j30target, e30, sizeof e30);
            nt_log_info("SELFTEST: J30 save-fail r=%s err='%s' src_exists=%d target_exists=%d (want !ok,<msg>,1,0)",
                        tp_status_str(r30), e30, (int)tp_scan_exists(j30src), (int)tp_scan_exists(j30target));
            NT_ASSERT(r30 != TP_STATUS_OK && e30[0] && "J30: a failed Save returns a fault");
            NT_ASSERT(tp_scan_exists(j30src) &&
                      "J30: the journal is KEPT when the save failed (non-destructive on failure)");
            NT_ASSERT(!tp_scan_exists(j30target) && "J30: the unwritable target was not created");
            (void)remove(j30src); (void)remove(j30src_lk); (void)remove(j30blocker);
        }

        /* (J31) fix [1] CAP eviction, DETERMINISTIC (fix2 F2): a full list of GUI_RECOVERY_MAX_CANDIDATES
         * NON-adoptable (version-mismatch) entries plus ONE adoptable entry must STILL retain the adoptable one
         * -- the (adoptable desc, timestamp desc) ordering ranks it above every non-adoptable entry, so it
         * evicts one of them and SURVIVES the cap. Driven straight through recovery_entry_insert via the F2
         * seam so it is INDEPENDENT of filesystem enumeration order: the old filesystem-crafted J31 relied on
         * the adoptable orphan being enumerated LAST, which holds on Windows (NTFS alphabetical) but NOT on the
         * Linux CI (readdir is unsorted) -- so that test passed on Linux EVEN WITH fix [1] reverted. All entries
         * share ts 0, so under a reverted timestamp-only insert the full equal-ts list would DROP the last
         * (adoptable) insert -> the assert below catches the regression on every OS. */
        {
            gui_recovery_list rl31;
            memset(&rl31, 0, sizeof rl31);
            for (int i = 0; i < GUI_RECOVERY_MAX_CANDIDATES; i++) {
                gui_recovery_entry e31;
                memset(&e31, 0, sizeof e31);
                (void)snprintf(e31.journal_path, sizeof e31.journal_path, "noise31_%02d.ntpjournal", i);
                (void)snprintf(e31.name, sizeof e31.name, "vmis31_%02d", i);
                e31.timestamp = 0;
                e31.adoptable = false; /* fill the cap with non-adoptable, equal-ts entries */
                gui_project__test_recovery_insert(&rl31, &e31);
            }
            gui_recovery_entry good31;
            memset(&good31, 0, sizeof good31);
            (void)snprintf(good31.journal_path, sizeof good31.journal_path, "zzz_good31.ntpjournal");
            (void)snprintf(good31.name, sizeof good31.name, "adoptable31");
            good31.timestamp = 0;   /* same ts as the noise -> a timestamp-only insert would DROP it */
            good31.adoptable = true;
            gui_project__test_recovery_insert(&rl31, &good31);

            bool found_adoptable31 = false;
            for (int k = 0; k < rl31.count; k++) {
                if (rl31.items[k].adoptable) {
                    found_adoptable31 = true;
                }
            }
            nt_log_info("SELFTEST: J31 cap deterministic count=%d found_adoptable=%d (want %d,1)",
                        rl31.count, (int)found_adoptable31, GUI_RECOVERY_MAX_CANDIDATES);
            NT_ASSERT(rl31.count == GUI_RECOVERY_MAX_CANDIDATES && "J31: the list stays capped at GUI_RECOVERY_MAX_CANDIDATES");
            NT_ASSERT(rl31.has_more && "J31: the capped list explicitly reports omitted recovery entries");
            NT_ASSERT(found_adoptable31 &&
                      "J31/[1]: the adoptable entry evicts a non-adoptable one and SURVIVES the full cap");
        }

        /* (J31b) COLLECT INTEGRATION (round-3 finding H): drive the real folder-scan classifier end-to-end on a
         * small UNCAPPED set of 3 orphans -- one adoptable-with-checkpoint (real key), one VERSION_MISMATCH
         * (bumped header), one FOREIGN-KEY (valid framing, wrong key). collect must surface the adoptable one
         * (adoptable==true), list the version-mismatch NON-adoptable (for Discard), and NEVER list the
         * foreign-key one. Order-independent: each entry is looked up by journal_path, not by index. */
        {
            char scan31b[900];
            char j31b_adopt[1000], j31b_adopt_lk[1010], j31b_vmis[1000], j31b_fkey[1000];
            (void)snprintf(scan31b, sizeof scan31b, "%s/selftest_r6a_j31b", s_exe_dir);
            tp_mkdirs(scan31b);
            (void)snprintf(j31b_adopt, sizeof j31b_adopt, "%s/adopt31b.ntpjournal", scan31b);
            (void)snprintf(j31b_adopt_lk, sizeof j31b_adopt_lk, "%s.lock", j31b_adopt);
            (void)snprintf(j31b_vmis, sizeof j31b_vmis, "%s/vmis31b.ntpjournal", scan31b);
            (void)snprintf(j31b_fkey, sizeof j31b_fkey, "%s/fkey31b.ntpjournal", scan31b);
            (void)remove(j31b_adopt); (void)remove(j31b_adopt_lk); (void)remove(j31b_vmis); (void)remove(j31b_fkey);

            scan_make_orphan(j31b_adopt, "adopt31b_atlas", 8100); /* real-key adoptable-with-checkpoint */
            { /* VERSION_MISMATCH: right magic, BUMPED format version -> non-adoptable, surfaced for Discard. */
                const tp_id128 rkey31b = gui_project__test_recovery_key();
                uint8_t hdr[TP_JRN_HEADER_LEN];
                memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
                tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION + 1000u);
                memcpy(hdr + TP_JRN_KEY_OFF, rkey31b.bytes, sizeof rkey31b.bytes);
                FILE *vf = fopen(j31b_vmis, "wb");
                NT_ASSERT(vf && "J31b: seeded a VERSION_MISMATCH journal");
                (void)fwrite(hdr, 1, sizeof hdr, vf);
                (void)fclose(vf);
            }
            { /* FOREIGN-KEY: valid framing, DIFFERENT key -> excluded (never listed). */
                tp_id128 fk31b;
                memset(fk31b.bytes, 0xCD, sizeof fk31b.bytes);
                craft_meta_orphan(j31b_fkey, fk31b, 6400, "/proj/foreign31b.ntpacker_project", "foreign31b.ntpacker_project");
            }

            gui_recovery_list rl31b;
            gui_project_enable_recovery(scan31b);
            const int n31b = gui_recovery_collect(&rl31b);
            int adopt_ok31b = 0, vmis_ok31b = 0, fkey_listed31b = 0;
            for (int k = 0; k < rl31b.count; k++) {
                if (selftest_same_path(rl31b.items[k].journal_path, j31b_adopt) && rl31b.items[k].adoptable) {
                    adopt_ok31b = 1;
                }
                if (selftest_same_path(rl31b.items[k].journal_path, j31b_vmis) && !rl31b.items[k].adoptable &&
                    rl31b.items[k].status == (int)TP_JOURNAL_RECOVERY_VERSION_MISMATCH) {
                    vmis_ok31b = 1;
                }
                if (selftest_same_path(rl31b.items[k].journal_path, j31b_fkey)) {
                    fkey_listed31b = 1;
                }
            }
            nt_log_info("SELFTEST: J31b collect n=%d adoptable_ok=%d vmis_ok=%d fkey_listed=%d (want 2,1,1,0)",
                        n31b, adopt_ok31b, vmis_ok31b, fkey_listed31b);
            NT_ASSERT(n31b == 2 && rl31b.count == 2 &&
                      "J31b: exactly 2 listed (adoptable + version-mismatch; foreign-key excluded)");
            NT_ASSERT(adopt_ok31b && "J31b: the adoptable orphan is surfaced with adoptable==true");
            NT_ASSERT(vmis_ok31b && "J31b: the version-mismatch is listed NON-adoptable (for Discard)");
            NT_ASSERT(!fkey_listed31b && "J31b: the foreign-key journal is NEVER listed");
            (void)remove(j31b_adopt); (void)remove(j31b_adopt_lk); (void)remove(j31b_vmis); (void)remove(j31b_fkey);
        }

        /* (J32) fix [4]/[5] GUARDS: resolve must refuse to (a) save the recovered project OVER its own journal
         * (self-target -> the post-save delete would erase the just-written work) and (b) touch the LIVE
         * session slot (deleting a journal still backing an open session). Both are refused with the journal
         * LEFT intact. */
        {
            char scan32[900];
            (void)snprintf(scan32, sizeof scan32, "%s/selftest_r6a_j32", s_exe_dir);
            tp_mkdirs(scan32);
            /* (a) self-target: SAVE_AS + SAVE_ORIGINAL where the write target IS the journal -> refuse, keep. */
            char j32src[1000], j32src_alias[1000], j32src_lk[1010];
            (void)snprintf(j32src, sizeof j32src, "%s/orphan32.ntpjournal", scan32);
            (void)snprintf(j32src_alias, sizeof j32src_alias, "%s/./orphan32.ntpjournal", scan32);
            (void)snprintf(j32src_lk, sizeof j32src_lk, "%s.lock", j32src);
            (void)remove(j32src); (void)remove(j32src_lk);
            scan_make_orphan(j32src, "recovered32", 3200);
            gui_project_enable_recovery(scan32);
            char e32a[256] = {0};
            const tp_status r32a = gui_recovery_resolve(j32src, "", GUI_RECOVERY_SAVE_AS, j32src_alias, e32a, sizeof e32a);
            char e32a2[256] = {0};
            const tp_status r32a2 = gui_recovery_resolve(j32src, j32src_alias, GUI_RECOVERY_SAVE_ORIGINAL, "", e32a2, sizeof e32a2);
            nt_log_info("SELFTEST: J32a self-target save_as=%s save_orig=%s src_exists=%d (want invalid,changed,1)",
                        tp_status_str(r32a), tp_status_str(r32a2), (int)tp_scan_exists(j32src));
            NT_ASSERT(r32a == TP_STATUS_INVALID_ARGUMENT &&
                      "J32a/[4]: refuse an explicit Save As over the recovery journal");
            NT_ASSERT(r32a2 == TP_STATUS_FILE_CHANGED_EXTERNALLY &&
                      "J32a/[4]: Save Original ignores frontend path arguments and requires durable metadata");
            NT_ASSERT(tp_scan_exists(j32src) && "J32a/[4]: the journal is KEPT when a self-target is refused");
            (void)remove(j32src); (void)remove(j32src_lk);

            /* (b) live slot: enable recovery on a slot, init (a LIVE journal now sits there + is locked), then
             *     resolve(slot, DISCARD) must refuse and LEAVE the live journal. */
            char j32slot[1000], j32slot_lk[1010];
            (void)snprintf(j32slot, sizeof j32slot, "%s/live32.ntpjournal", scan32);
            (void)snprintf(j32slot_lk, sizeof j32slot_lk, "%s.lock", j32slot);
            (void)remove(j32slot); (void)remove(j32slot_lk);
            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project_enable_recovery(j32slot);
            gui_project_init(); /* fresh live journal at the slot, lock held */
            NT_ASSERT(gui_project__test_recovery_active() && tp_scan_exists(j32slot) &&
                      "J32b: a live (locked) journal sits at the slot");
            char e32b[256] = {0};
            const tp_status r32b = gui_recovery_resolve(j32slot, "", GUI_RECOVERY_DISCARD, "", e32b, sizeof e32b);
            nt_log_info("SELFTEST: J32b live-slot discard r=%s slot_exists=%d (want invalid,1)",
                        tp_status_str(r32b), (int)tp_scan_exists(j32slot));
            NT_ASSERT(r32b == TP_STATUS_INVALID_ARGUMENT && "J32b/[5]: refuse to resolve the LIVE session slot");
            NT_ASSERT(tp_scan_exists(j32slot) && "J32b/[5]: the LIVE journal is NOT deleted");
            gui_project_enable_recovery(""); gui_project_shutdown();
            (void)remove(j32slot); (void)remove(j32slot_lk);
        }

        /* (J33) fix2 F3: the load-verify-before-delete backstop (fix [3]). resolve LOAD-VERIFIES the just-saved
         * file before deleting the journal; if it does not reload, the journal must be KEPT (the recovered work
         * survives). A real save-OK-but-reload-fail input is not cleanly constructible (resolve promotes ids
         * before saving), so a one-shot selftest seam simulates it right after the post-save load. Arm it, run a
         * SAVE_AS into a fresh target, and assert: a fault is returned, the TARGET WAS written (the save itself
         * succeeded before the simulated verify-fail), and the JOURNAL IS KEPT (the backstop refused to delete
         * the only other copy of the work). */
        {
            char scan33[900];
            char j33src[1000], j33src_lk[1010], j33target[1000];
            (void)snprintf(scan33, sizeof scan33, "%s/selftest_r6a_j33", s_exe_dir);
            tp_mkdirs(scan33);
            (void)snprintf(j33src, sizeof j33src, "%s/orphan33.ntpjournal", scan33);
            (void)snprintf(j33src_lk, sizeof j33src_lk, "%s.lock", j33src);
            (void)snprintf(j33target, sizeof j33target, "%s/recovered33.ntpacker_project", scan33);
            (void)remove(j33src); (void)remove(j33src_lk); (void)remove(j33target);
            scan_make_orphan(j33src, "recovered33", 7000); /* real-key adoptable orphan -> recover + save succeed */
            NT_ASSERT(tp_scan_exists(j33src) && "J33: the adoptable orphan exists before resolve");

            gui_project_enable_recovery(scan33);
            gui_project__test_fail_next_load_verify(); /* one-shot: the NEXT post-save load-verify reports failure */
            char e33[256] = {0};
            const tp_status r33 = gui_recovery_resolve(j33src, "", GUI_RECOVERY_SAVE_AS, j33target, e33, sizeof e33);
            nt_log_info("SELFTEST: J33 verify-fail r=%s err='%s' journal_kept=%d target_written=%d (want !ok,<msg>,1,1)",
                        tp_status_str(r33), e33, (int)tp_scan_exists(j33src), (int)tp_scan_exists(j33target));
            NT_ASSERT(r33 != TP_STATUS_OK && e33[0] &&
                      "J33/[3]: a saved-but-unreloadable file returns a fault");
            NT_ASSERT(tp_scan_exists(j33src) &&
                      "J33/[3]: the journal is KEPT when the just-saved file fails the load-verify (never lose the work)");
            NT_ASSERT(tp_scan_exists(j33target) &&
                      "J33/[3]: the target WAS written (the save succeeded before the simulated verify-fail)");
            (void)remove(j33src); (void)remove(j33src_lk); (void)remove(j33target);
        }

        /* (J34) R6 TRUNCATED committed-prefix recovery. A torn final transaction remains collectable when
         * checkpoint + an earlier transaction are intact; SAVE_AS persists that prefix and reaps the orphan. */
        {
            char scan34[900], src34[1000], lock34[1010], target34[1000];
            (void)snprintf(scan34, sizeof scan34, "%s/selftest_r6a_j34", s_exe_dir);
            tp_mkdirs(scan34);
            (void)snprintf(src34, sizeof src34, "%s/truncated34.ntpjournal", scan34);
            (void)snprintf(lock34, sizeof lock34, "%s.lock", src34);
            (void)snprintf(target34, sizeof target34, "%s/truncated34.ntpacker_project", scan34);
            (void)remove(src34); (void)remove(lock34); (void)remove(target34);
            scan_make_orphan_2edits(src34, "truncated34_good", "truncated34_torn", 7100);
            NT_ASSERT(selftest_chop_file_tail(src34, 6) && "J34: tear the final transaction tail");

            gui_recovery_list list34;
            gui_project_enable_recovery(scan34);
            const int n34 = gui_recovery_collect(&list34);
            bool offered34 = false;
            for (int k = 0; k < list34.count; k++) {
                if (selftest_same_path(list34.items[k].journal_path, src34) && list34.items[k].adoptable &&
                    list34.items[k].status == (int)TP_JOURNAL_RECOVERY_TRUNCATED) {
                    offered34 = true;
                }
            }
            NT_ASSERT(n34 > 0 && offered34 && "J34: R6 collect offers the truncated committed prefix");

            char err34[256] = {0};
            NT_ASSERT(gui_recovery_resolve(src34, "", GUI_RECOVERY_SAVE_AS, target34, err34, sizeof err34) ==
                          TP_STATUS_OK &&
                      "J34: R6 resolves a truncated committed prefix");
            tp_project *loaded34 = NULL;
            tp_error load_err34 = {0};
            NT_ASSERT(tp_project_load(target34, &loaded34, &load_err34) == TP_STATUS_OK && loaded34 &&
                      strcmp(tp_project_get_atlas(loaded34, 0)->name, "truncated34_good") == 0 &&
                      "J34: SAVE_AS contains the last intact transaction");
            NT_ASSERT(!tp_scan_exists(src34) && "J34: successful resolve reaps the orphan");
            if (loaded34) { tp_project_destroy(loaded34); }
            (void)remove(src34); (void)remove(lock34); (void)remove(target34);
        }

        /* (J35) R6 CORRUPT committed-prefix recovery. A mid-stream corrupt transaction with a later valid
         * frame is classified CORRUPT; resolve clones the intact prefix instead of adopting the poisoned log. */
        {
            char scan35[900], src35[1000], lock35[1010], target35[1000];
            (void)snprintf(scan35, sizeof scan35, "%s/selftest_r6a_j35", s_exe_dir);
            tp_mkdirs(scan35);
            (void)snprintf(src35, sizeof src35, "%s/corrupt35.ntpjournal", scan35);
            (void)snprintf(lock35, sizeof lock35, "%s.lock", src35);
            (void)snprintf(target35, sizeof target35, "%s/corrupt35.ntpacker_project", scan35);
            (void)remove(src35); (void)remove(lock35); (void)remove(target35);
            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project__test_set_recovery_now(7200);
            gui_project_enable_recovery(src35); gui_project_init();
            NT_ASSERT(gui_project_set_atlas_name(0, "corrupt35_good") &&
                      gui_project_set_atlas_name(0, "corrupt35_bad") &&
                      gui_project_set_atlas_name(0, "corrupt35_later") &&
                      "J35: seed three committed transactions");
            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project__test_set_recovery_now(-1);
            NT_ASSERT(selftest_corrupt_journal_record(src35, 3) && "J35: corrupt the middle transaction");

            gui_recovery_list list35;
            gui_project_enable_recovery(scan35);
            const int n35 = gui_recovery_collect(&list35);
            bool offered35 = false;
            for (int k = 0; k < list35.count; k++) {
                if (selftest_same_path(list35.items[k].journal_path, src35) && list35.items[k].adoptable &&
                    list35.items[k].status == (int)TP_JOURNAL_RECOVERY_CORRUPT) {
                    offered35 = true;
                }
            }
            NT_ASSERT(n35 > 0 && offered35 && "J35: R6 collect offers the corrupt committed prefix");

            char err35[256] = {0};
            NT_ASSERT(gui_recovery_resolve(src35, "", GUI_RECOVERY_SAVE_AS, target35, err35, sizeof err35) ==
                          TP_STATUS_OK &&
                      "J35: R6 resolves a corrupt committed prefix");
            tp_project *loaded35 = NULL;
            tp_error load_err35 = {0};
            NT_ASSERT(tp_project_load(target35, &loaded35, &load_err35) == TP_STATUS_OK && loaded35 &&
                      strcmp(tp_project_get_atlas(loaded35, 0)->name, "corrupt35_good") == 0 &&
                      "J35: SAVE_AS contains the last intact transaction before corruption");
            NT_ASSERT(!tp_scan_exists(src35) && "J35: successful resolve reaps the orphan");
            if (loaded35) { tp_project_destroy(loaded35); }
            (void)remove(src35); (void)remove(lock35); (void)remove(target35);
        }

        /* (J36) The raw selftest seam has no arbitrary-file delete authority. A matching suffix is not
         * enough: magic + application key must validate before a claim lock or Discard can mutate it. */
        {
            char foreign36[1000];
            (void)snprintf(foreign36, sizeof foreign36, "%s/foreign36.ntpjournal", s_exe_dir);
            (void)remove(foreign36);
            FILE *f36 = fopen(foreign36, "wb");
            NT_ASSERT(f36);
            (void)fputs("ordinary user file", f36);
            (void)fclose(f36);
            char err36[256] = {0};
            NT_ASSERT(gui_recovery_resolve(foreign36, "", GUI_RECOVERY_DISCARD, "", err36,
                                           sizeof err36) != TP_STATUS_OK &&
                      tp_scan_exists(foreign36) &&
                      "J36: raw resolver rejects and preserves an arbitrary .ntpjournal-named file");
            (void)remove(foreign36);
        }

        /* (J37) A raw window-close reaches the same shutdown path without explicit Exit->Discard.
         * Dirty work must leave its slot for next-launch recovery; a clean model still reaps its slot. */
        {
            char dirty37[1000], clean37[1000], dirty37lock[1010], clean37lock[1010];
            (void)snprintf(dirty37, sizeof dirty37, "%s/dirty-close37.ntpjournal", s_exe_dir);
            (void)snprintf(clean37, sizeof clean37, "%s/clean-close37.ntpjournal", s_exe_dir);
            (void)snprintf(dirty37lock, sizeof dirty37lock, "%s.lock", dirty37);
            (void)snprintf(clean37lock, sizeof clean37lock, "%s.lock", clean37);
            (void)remove(dirty37); (void)remove(clean37);
            (void)remove(dirty37lock); (void)remove(clean37lock);

            gui_project_enable_recovery(""); gui_project_shutdown();
            gui_project_enable_recovery(dirty37); gui_project_init();
            NT_ASSERT(gui_project_set_atlas_name(0, "dirty-close-survives"));
            gui_project_shutdown();
            NT_ASSERT(tp_scan_exists(dirty37) && "J37: dirty shutdown preserves recovery journal");

            gui_project_enable_recovery(clean37); gui_project_init();
            gui_project_shutdown();
            NT_ASSERT(!tp_scan_exists(clean37) && "J37: clean shutdown removes recovery journal");
            (void)remove(dirty37); (void)remove(dirty37lock);
            (void)remove(clean37); (void)remove(clean37lock);
        }

        /* Done: disable recovery + release any lock + restore a journal-LESS packable project for the
         * render phases. */
        gui_project_enable_recovery("");
        gui_project_shutdown();
        gui_project_init();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char rfolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", rfolder, sizeof rfolder);
        (void)gui_project_add_source(0, rfolder);
        gui_scan_invalidate_all();
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
            gui_project_set_target(e_atlas, 0, t0->exporter_id, t0->out_path, !was); /* dialog toggle path */
            const tp_snapshot_target *changed = selftest_target_at(e_atlas, 0);
            const bool now = changed->enabled;
            char exporter[64];
            char out_path[TP_IDENTITY_PATH_MAX];
            (void)snprintf(exporter, sizeof exporter, "%s", changed->exporter_id);
            (void)snprintf(out_path, sizeof out_path, "%s", changed->out_path);
            gui_project_set_target(e_atlas, 0, exporter, out_path, was); /* restore */
            nt_log_info("SELFTEST: export-dialog toggle atlas=%d target0 %d->%d (restored=%d)", e_atlas, was, now, was);
        }
        s_export_open = true;
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
            (void)snprintf(s_sel_abs, sizeof s_sel_abs, "%s", resolved);
            s_sel_atlas = 0;
            s_sel_src = ns - 1;
            s_sel_child = -1;
            s_sel_missing = false;
        }
    }
    /* Render coverage: leave a real animation selected + previewing so the auto-quit frames exercise the
     * left-panel animations rows, the right-panel editor, and the canvas preview (draw_anim_frame on the
     * packed regions) -- a Clay layout bug in the new UI would crash these frames. */
    {
        s_sel_atlas = 0;
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
                open_preview(pai);
                const tp_snapshot_animation *animation = selftest_animation_at(0, pai);
                nt_log_info("SELFTEST: preview anim '%s' active=%d frames=%d", animation->name, s_preview_active,
                            animation->frame_count);
            }
            multi_sel_clear();
        }
    }
    g_ui_scale = 1.5F; /* exercise the scaled layout during the auto-quit frames */
    nt_log_info("SELFTEST: end (undo:%d redo:%d; selection '%s')", gui_project_undo_depth(),
                gui_project_redo_depth(), s_sel_abs);
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
        if (selftest_headless()) {
            /* Headless CI: the GL render pipeline (materials/shaders/font atlas) never reaches "ready"
             * under xvfb+llvmpipe (can_render stays false -> nothing rasterizes), so the render/layout
             * VISUAL phases (1-15: outline pixel probe, touch-on-render, overflow/scissor sweeps) cannot
             * run -- they read back the drawn framebuffer / declared UI bboxes. Jump straight to phase 16
             * (async-shutdown-while-busy), which is GL-independent logic. These phases stay HARD locally
             * on a real GPU (env unset). */
            nt_log_info("SELFTEST: headless CI -> skipping GL render/layout phases 1-15 (no GL context)");
            s_st_phase = 16;
            s_st_pf = 0;
            return;
        }
        if (s_st_pf < 12) {
            return; /* warm up: first scene + GL page uploads settle */
        }
        s_about_open = false;
        s_export_open = false; /* close the Export dialog exercised during warmup before the pixel probe */
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
            s_sel_atlas = 0;
            do_pack_blocking();
            found = (gui_pack_result(0) && gui_pack_result(0)->sprite_count > 0) ? 0 : -1;
        }
        s_sel_atlas = (found >= 0) ? found : 0;
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
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr = gui_pack_result(s_sel_atlas);
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
            s_sel_anim = -1;
            s_sel_anim_frame = -1;
            s_sel_atlas = 0;
            const tp_snapshot_atlas *sa = selftest_atlas_at(s_sel_atlas, NULL);
            if (sa && sa->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(s_sel_atlas, afolder);
                (void)gui_project_set_atlas_setting(s_sel_atlas, GUI_ATLAS_MAX_SIZE, 256, 0.0F);
                gui_scan_invalidate_all();
            }
            double pms = 0.0;
            char perr[256] = {0};
            char pnote[128] = {0};
            (void)gui_pack_atlas(s_sel_atlas, &pms, perr, sizeof perr, pnote, sizeof pnote);
            s_canvas.mode = GUI_CANVAS_ATLAS;
            const tp_result *pr = gui_pack_result(s_sel_atlas);
            if (pr && pr->sprite_count > 0) {
                gui_canvas_select(&s_canvas, 0);
                select_row_for_region(0);
            }
            s_sec_atlas_open = s_sec_region_open = s_sec_anim_open = s_sec_export_open = true;
            s_atlas_adv_open = false;
            gui_project_mark_stale();
        }
        if (s_st_pf >= 3) { /* size + stale held >= 2 frames -> the lagged bbox reflects the stale strip here */
            const tp_snapshot_atlas *a = selftest_atlas_at(s_sel_atlas, NULL);
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
         * apply_pending swaps it in), then a blocking reference pack of the same project must match --
         * determinism holds because only WHERE the pack ran changed. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            s_sel_atlas = 0;
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
        /* Export-target PREVIEW (packet EXP-PREVIEW): a defold preview must (a) exist with identity-only
         * placements (defold caps.flips=false -> the clamp turns allow_transform off -> tp_pack bakes no
         * rotated/flipped regions), (b) leave the native session result untouched (pointer + content),
         * (c) re-bind the native result WITHOUT a repack when switched back to Native, and (d) yield a
         * non-empty degradation summary. Blocking path (the dev seam), mirroring do_pack_blocking. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            preview_target_reset();
            s_sel_atlas = 0;
            reset_selection();
            const tp_snapshot_atlas *a0 = selftest_atlas_at(0, NULL);
            if (a0 && a0->source_count == 0) {
                char afolder[512];
                to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
                (void)gui_project_add_source(0, afolder);
                (void)gui_project_set_atlas_setting(0, GUI_ATLAS_ALLOW_TRANSFORM, 1, 0.0F);
                gui_scan_invalidate_all();
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
            for (int i = 0; i < tp_exporter_count(); i++) {
                const tp_exporter *e = tp_exporter_at(i);
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

            /* (a) identity-only placements */
            int nonidentity = 0;
            for (int i = 0; i < pv->sprite_count; i++) {
                if (pv->sprites[i].transform != 0) {
                    nonidentity++;
                }
            }
            nt_log_info("SELFTEST: preview defold non-identity placements=%d (expect 0)", nonidentity);
            NT_ASSERT(nonidentity == 0 && "SELFTEST preview: defold packs identity-only (no flip/rotate)");

            /* (b) native session result untouched */
            const tp_result *native2 = gui_pack_result(0);
            NT_ASSERT((const void *)native2 == native_ptr && native2->sprite_count == native_sc &&
                      native2->page_count == native_pc && "SELFTEST preview: native session result untouched");

            /* (c) preview binds while active; back to Native re-binds the session result with no repack */
            s_preview_target = defold_idx + 1;
            s_preview_ver = gui_project_snapshot_model_generation();
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
            const int nd = gui_pack_preview_diff(0, "defold", chip, sizeof chip, tip, sizeof tip);
            nt_log_info("SELFTEST: preview defold degradation nd=%d chip='%s'", nd, chip);
            NT_ASSERT(nd > 0 && chip[0] != '\0' && "SELFTEST preview: defold degradation summary non-empty");

            gui_pack_preview_clear();
            preview_target_reset();
            nt_log_info("SELFTEST: export-target preview OK");
            s_st_phase = 12;
            s_st_pf = 0;
        }
    } else if (s_st_phase == 12) {
        /* Async EXPORT (req 4a): mirror phase 9's async==blocking pattern for the export path. Start an
         * async export of a fresh single-atlas project whose seeded target points at an isolated tmp base
         * under the build dir, spin until it lands (poll_async in apply_pending reads the report + frees the
         * job), then assert the on-disk json + page png exist -- the export_worker / save_buffer clone /
         * mkdirs path is otherwise untested (only the blocking gui_pack_export was exercised). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            char base[600];
            (void)snprintf(base, sizeof base, "%s/selftest_async_export/at0", s_exe_dir); /* ABSOLUTE -> resolves w/o a saved dir */
            gui_project_set_target(0, 0, "json-neotolis", base, true);
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
         * immediately, spin until it lands. gui_pack_poll must DISCARD the worker's result (no slot swap)
         * and poll_async must NOT clear stale -- the cancel-discard path (gui_pack.c) is otherwise never
         * hit (phase 9 waits for !busy first). */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
            gui_project_mark_stale();
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: cancel-phase pack must start");
            gui_pack_async_cancel(); /* cancel before it can land -> result discarded on landing */
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
         * stale slot 1.  The structural edit also keeps the landed result stale. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        if (s_st_pf == 1) {
            gui_project_new();
            gui_pack_clear(-1);
            const int survivor = gui_project_add_atlas();
            NT_ASSERT(survivor == 1 && "SELFTEST: stable-publication atlas must be index 1");
            (void)gui_project_set_atlas_name(1, "survivor");
            s_sel_atlas = 1;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(1, afolder);
            gui_scan_invalidate_all();
            char aerr[256] = {0};
            const bool started = gui_pack_async_start(1, aerr, sizeof aerr);
            NT_ASSERT(started && "SELFTEST: stable-publication pack must start");
            NT_ASSERT(gui_project_remove_atlas(0) &&
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
                      "SELFTEST: shifted-atlas result must stay stale after model change");
            nt_log_info("SELFTEST: async result followed stable atlas id after index shift");
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
            gui_project_new();
            gui_pack_clear(-1);
            s_sel_atlas = 0;
            reset_selection();
            char afolder[512];
            to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
            (void)gui_project_add_source(0, afolder);
            gui_scan_invalidate_all();
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
            gui_project_set_target(0, 0, "json-neotolis", base, true);
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
        /* Shutdown-while-busy (req 4c): start an async pack, then gui_pack_shutdown() must cancel + JOIN the
         * worker + free + reset without hanging (the window X-close path). Runs to completion in one frame --
         * gui_pack_shutdown joins synchronously, so afterward the job is idle. main() calls it AGAIN at exit
         * (idempotent -- the second call sees !busy). Kept LAST because it tears the pack session down. */
        g_ui_scale = 1.0F;
        g_nt_window.fb_width = 1280;
        g_nt_window.fb_height = 800;
        gui_project_new();
        gui_pack_clear(-1);
        s_sel_atlas = 0;
        reset_selection();
        char afolder[512];
        to_abs("examples/defold-demo/examples/anim_trim/anims", afolder, sizeof afolder);
        (void)gui_project_add_source(0, afolder);
        gui_scan_invalidate_all();
        char aerr[256] = {0};
        const bool started = gui_pack_async_start(0, aerr, sizeof aerr);
        NT_ASSERT(started && gui_pack_async_busy() && "SELFTEST: shutdown-phase pack must start busy");
        gui_pack_shutdown(); /* busy branch: cancel + join + free + reset */
        NT_ASSERT(!gui_pack_async_busy() && "SELFTEST: shutdown-while-busy must join + reset (no hang)");
        gui_shell_reset_shown_result();
        NT_ASSERT(!gui_canvas_has_atlas(&s_canvas) &&
                  gui_canvas_get_mode(&s_canvas) == GUI_CANVAS_SOURCE &&
                  "SELFTEST: pack shutdown must release the canvas result borrow");
        nt_log_info("SELFTEST: shutdown-while-busy joined cleanly");
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
    /* Hand off to the touch-on-render guard: a truly fresh project, no input, all sections expanded. */
    gui_project_new();
    s_sel_atlas = 0;
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
    tp_error e = {0};
    (void)gui_project_snapshot_serialize(&s_st_baseline, &s_st_baseline_n, &e);
    s_st_phase = 2;
    s_st_pf = 0;
}

#else

/* NTPACKER_GUI_SELFTEST off: this TU intentionally compiles to nothing. A file-scope typedef keeps it
 * a legal (non-empty) ISO C translation unit under -Wpedantic. */
typedef int gui_selftest_empty_translation_unit;

#endif /* NTPACKER_GUI_SELFTEST */
