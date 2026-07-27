/* Model/state mutation layer for the ntpacker GUI (see gui_actions.h). This TU is Clay-free AND
 * nt_ui-free: it reads/mutates the model + shared state only. */

#include "gui_actions.h"
#include "gui_actions_internal.h"
#include "gui_project.h"

#include "gui_defs.h" /* S() -- the compact-strip stop that folds the preview selector away */
#include "gui_state.h"
#include "gui_rows.h"
#include "gui_project.h"
#include "gui_scan.h"
#include "gui_canvas.h"
#include "gui_pack.h"
#include "gui_paths.h"
#include "gui_shell.h" /* reset the canvas borrow across pack/history transitions */
#include "tinyfiledialogs.h"

#include "clipboard/nt_clipboard.h"
#include "tp_core/tp_export.h" /* tp_exporter_at -> the preview selector's exporter list */
#include "tp_core/tp_id.h"
#include "tp_core/tp_names.h"  /* tp_names_common_prefix (anim id from selection) */

#include "app/nt_app.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum gui_pending_history_action {
    GUI_PENDING_HISTORY_NONE = 0,
    GUI_PENDING_HISTORY_UNDO,
    GUI_PENDING_HISTORY_REDO,
} gui_pending_history_action;

static gui_pending_history_action s_pending_history_action;

/* Presentation-only mapping from the active stable animation to one Pack result. */

gui_lifecycle_request s_after_confirm;
bool s_confirm_open;
bool s_confirm_draft;
int s_modal_action;
/* R6b: startup crash-recovery modal glue. The orphan list lives here; the modal reads it via the
 * count/at accessors and requests a per-row action, deferred to apply_pending() (below) so the
 * Save-As dialog + disk-mutating gui_recovery_resolve run outside nt_ui_begin/end, like s_pending_save_as. */
bool s_recovery_open;
double s_last_pack_ms;      /* wall-clock ms of the last successful pack (for the stats line) */
tp_id128 s_last_pack_atlas_id;

gui_actions_state s_actions = {.recovery_pending_row = -1};

bool gui_actions_copy_text_available(void) {
    return nt_clipboard_available();
}

void gui_actions_copy_text(const char *text) {
    if (text) {
        nt_clipboard_set_text(text);
    }
}

// #region undo/redo + refresh actions
static tp_id128 selected_animation_id(void) {
    return gui_view_animation_id();
}

/* After undo/redo, drop transient editor and preview state but retain canonical
 * selection refs for revalidation against rebuilt rows. */
static void undo_redo_settle(tp_id128 animation_id) {
    gui_shell_reset_shown_result();
    cancel_edit();
    gui_view_reconcile_observation(gui_project_snapshot());
    if (tp_id128_is_nil(animation_id) ||
        gui_view_animation_index(
            gui_project_snapshot()) < 0) {
        gui_view_select_animation(tp_id128_nil());
    }
    if (s_preview_active) {
        preview_stop();
    }
    preview_target_reset();
    gui_canvas_invalidate(&s_canvas);
}
void do_undo(void) {
    if (gui_draft_phase() != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "Apply or discard the active edit before Undo.");
        return;
    }
    const tp_id128 animation_id = selected_animation_id();
    if (gui_project_undo()) {
        undo_redo_settle(animation_id);
        set_statusf("Undo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to undo.");
    }
}
void do_redo(void) {
    if (gui_draft_phase() != GUI_EDIT_IDLE) {
        set_status_ex(
            STATUS_WARNING,
            "Apply or discard the active edit before Redo.");
        return;
    }
    const tp_id128 animation_id = selected_animation_id();
    if (gui_project_redo()) {
        undo_redo_settle(animation_id);
        set_statusf("Redo (undo:%d redo:%d)", gui_project_undo_depth(), gui_project_redo_depth());
    } else {
        set_status("Nothing to redo.");
    }
}

void gui_request_undo(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE) {
        return;
    }
    s_pending_history_action = GUI_PENDING_HISTORY_UNDO;
}

void gui_request_redo(void) {
    if (gui_project_lifecycle_state_query() !=
        GUI_PROJECT_LIFECYCLE_OPEN_IDLE) {
        return;
    }
    s_pending_history_action = GUI_PENDING_HISTORY_REDO;
}

static void apply_pending_history_action(void) {
    const gui_pending_history_action action =
        s_pending_history_action;
    s_pending_history_action = GUI_PENDING_HISTORY_NONE;
    if (action == GUI_PENDING_HISTORY_UNDO) {
        do_undo();
    } else if (action == GUI_PENDING_HISTORY_REDO) {
        do_redo();
    }
}

void gui_actions__clear_history_request(void) {
    s_pending_history_action = GUI_PENDING_HISTORY_NONE;
}

/* Fingerprint every source (folders expand to their scanned children, files stat
 * directly) so a Refresh can diff added/removed/changed. Missing entries carry
 * size==-1 so a vanish/restore reads as removed/added. */
typedef struct fp_entry {
    char *abs;
    tp_id128 atlas_id;
    tp_id128 source_id;
    long long size;
    long long mtime;
} fp_entry;

typedef struct fp_source {
    tp_id128 atlas_id;
    tp_id128 source_id;
    char *abs;
} fp_source;

static fp_entry *s_refresh_fingerprint;
static int s_refresh_fingerprint_count;
static fp_source *s_refresh_sources;
static int s_refresh_source_count;
static bool s_refresh_fingerprint_valid;
static uint64_t s_refresh_fingerprint_instance_generation;
static tp_id128 s_refresh_membership_hash;
static tp_status fp_collect(fp_entry **arr, int *count, int *cap,
                            fp_source **sources, int *source_count,
                            int *source_cap,
                            tp_error *error);

static void fp_free(fp_entry *entries, int count) {
    for (int i = 0; i < count; ++i) {
        free(entries[i].abs);
    }
    free(entries);
}

static void fp_sources_free(fp_source *sources, int count) {
    for (int i = 0; i < count; ++i) {
        free(sources[i].abs);
    }
    free(sources);
}

void gui_actions_refresh_fingerprint_reset(void) {
    fp_free(s_refresh_fingerprint, s_refresh_fingerprint_count);
    fp_sources_free(s_refresh_sources, s_refresh_source_count);
    s_refresh_fingerprint = NULL;
    s_refresh_fingerprint_count = 0;
    s_refresh_sources = NULL;
    s_refresh_source_count = 0;
    s_refresh_fingerprint_valid = false;
    s_refresh_fingerprint_instance_generation = 0U;
    s_refresh_membership_hash = tp_id128_nil();
}

static void fp_bind_current_session(void) {
    const uint64_t instance_generation =
        gui_project_session_instance_generation();
    if (instance_generation !=
        s_refresh_fingerprint_instance_generation) {
        gui_actions_refresh_fingerprint_reset();
        s_refresh_fingerprint_instance_generation =
            instance_generation;
    }
}

/* Exact source-membership signature: model source transactions rebase the
 * filesystem baseline, while unrelated model edits keep external deltas visible. */
static tp_id128 fp_membership_hash(void) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    tp_hasher hasher = tp_hasher_init();
    static const char tag[] = "gui-refresh-membership-v1";
    tp_hasher_update(&hasher, tag, sizeof tag);
    const int atlas_count =
        snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    tp_hasher_update(&hasher, &atlas_count, sizeof atlas_count);
    for (int ai = 0; ai < atlas_count; ++ai) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, ai);
        if (!atlas) {
            continue;
        }
        tp_hasher_update(&hasher, atlas->id.bytes, sizeof atlas->id.bytes);
        tp_hasher_update(&hasher, &atlas->source_count,
                         sizeof atlas->source_count);
        for (int si = 0; si < atlas->source_count; ++si) {
            const tp_snapshot_source *source =
                tp_session_snapshot_source_at(snapshot, atlas->id, si);
            if (!source) {
                continue;
            }
            tp_hasher_update(&hasher, source->id.bytes, sizeof source->id.bytes);
            tp_hasher_update(&hasher, &source->kind, sizeof source->kind);
            const char *path = source->path ? source->path : "";
            tp_hasher_update(&hasher, path, strlen(path) + 1U);
        }
    }
    return tp_hasher_final(hasher);
}

static tp_status fp_push(fp_entry **arr, int *count, int *cap,
                         tp_id128 atlas_id, tp_id128 source_id,
                         const char *abs, long long size, long long mtime,
                         tp_error *error) {
    if (*count == *cap) {
        if (*cap > INT_MAX / 2) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint has too many entries");
        }
        int nc = *cap ? *cap * 2 : 64;
        if ((size_t)nc > SIZE_MAX / sizeof **arr) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh fingerprint table overflows size_t");
        }
        fp_entry *ne =
            (fp_entry *)realloc(*arr, (size_t)nc * sizeof *ne);
        if (!ne) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "refresh fingerprint allocation failed");
        }
        *arr = ne;
        *cap = nc;
    }
    char *path = gui_actions__strdup(abs);
    if (!path) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "refresh fingerprint path allocation failed");
    }
    (*arr)[*count] =
        (fp_entry){path, atlas_id, source_id, size, mtime};
    (*count)++;
    return TP_STATUS_OK;
}

static tp_status fp_source_push(fp_source **arr, int *count, int *cap,
                                tp_id128 atlas_id, tp_id128 source_id,
                                const char *abs, tp_error *error) {
    if (*count == *cap) {
        if (*cap > INT_MAX / 2) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh has too many sources");
        }
        const int nc = *cap ? *cap * 2 : 16;
        if ((size_t)nc > SIZE_MAX / sizeof **arr) {
            return tp_error_set(error, TP_STATUS_OUT_OF_BOUNDS,
                                "refresh source table overflows size_t");
        }
        fp_source *grown =
            (fp_source *)realloc(*arr, (size_t)nc * sizeof *grown);
        if (!grown) {
            return tp_error_set(error, TP_STATUS_OOM,
                                "refresh source table allocation failed");
        }
        *arr = grown;
        *cap = nc;
    }
    char *path = gui_actions__strdup(abs);
    if (!path) {
        return tp_error_set(error, TP_STATUS_OOM,
                            "refresh source path allocation failed");
    }
    (*arr)[*count] = (fp_source){atlas_id, source_id, path};
    (*count)++;
    return TP_STATUS_OK;
}

static tp_status fp_collect(fp_entry **arr, int *count, int *cap,
                            fp_source **sources, int *source_count,
                            int *source_cap,
                            tp_error *error) {
    const tp_session_snapshot *snapshot = gui_project_snapshot();
    const int atlas_count = snapshot ? tp_session_snapshot_atlas_count(snapshot) : 0;
    for (int ai = 0; ai < atlas_count; ai++) {
        const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snapshot, ai);
        for (int si = 0; si < a->source_count; si++) {
            const tp_snapshot_source *source = tp_session_snapshot_source_at(snapshot, a->id, si);
            char abs[TP_IDENTITY_PATH_MAX];
            if (!source) {
                return tp_error_set(error, TP_STATUS_NOT_FOUND,
                                    "refresh source %d in atlas %d is unavailable",
                                    si, ai);
            }
            tp_status status = tp_session_snapshot_resolve_path(
                snapshot, a->id, source->id, abs, sizeof abs, error);
            if (status != TP_STATUS_OK) {
                return status;
            }
            status = fp_source_push(sources, source_count, source_cap, a->id,
                                    source->id, abs, error);
            if (status != TP_STATUS_OK) {
                return status;
            }
            tp_scan_kind kind = TP_SCAN_KIND_MISSING;
            status = tp_scan_classify_checked(abs, &kind, error);
            if (status == TP_STATUS_NOT_FOUND &&
                kind == TP_SCAN_KIND_MISSING) {
                status = fp_push(arr, count, cap, a->id, source->id, abs, -1,
                                 -1, error);
                if (status != TP_STATUS_OK) {
                    return status;
                }
                continue;
            }
            if (status != TP_STATUS_OK) {
                return status;
            }
            if (kind == TP_SCAN_KIND_DIRECTORY) {
                const gui_scan_result *sc = NULL;
                tp_status scan_status = gui_scan_get(abs, &sc, error);
                if (scan_status != TP_STATUS_OK) {
                    return scan_status;
                }
                for (int ci = 0; ci < sc->count; ci++) {
                    tp_status push_status = fp_push(
                        arr, count, cap, a->id, source->id,
                        sc->entries[ci].abs,
                        sc->entries[ci].size, sc->entries[ci].mtime, error);
                    if (push_status != TP_STATUS_OK) {
                        return push_status;
                    }
                }
            } else {
                long long sz = -1;
                long long mt = -1;
                if (!gui_scan_stat(abs, &sz, &mt)) {
                    return tp_error_set(
                        error, TP_STATUS_PATH_RESOLVE_FAILED,
                        "refresh could not stat regular source '%s'", abs);
                }
                tp_status push_status =
                    fp_push(arr, count, cap, a->id, source->id, abs, sz, mt,
                            error);
                if (push_status != TP_STATUS_OK) {
                    return push_status;
                }
            }
        }
    }
    return TP_STATUS_OK;
}

static const fp_entry *fp_find(const fp_entry *arr, int n,
                               tp_id128 atlas_id, tp_id128 source_id,
                               const char *abs) {
    for (int i = 0; i < n; i++) {
        if (tp_id128_eq(arr[i].atlas_id, atlas_id) &&
            tp_id128_eq(arr[i].source_id, source_id) &&
            strcmp(arr[i].abs, abs) == 0) {
            return &arr[i];
        }
    }
    return NULL;
}

static const fp_source *fp_source_find(const fp_source *sources, int count,
                                       tp_id128 atlas_id,
                                       tp_id128 source_id) {
    for (int i = 0; i < count; ++i) {
        if (tp_id128_eq(sources[i].atlas_id, atlas_id) &&
            tp_id128_eq(sources[i].source_id, source_id)) {
            return &sources[i];
        }
    }
    return NULL;
}

static bool fp_source_membership_contains(const fp_source *sources, int count,
                                          const fp_source *candidate) {
    const fp_source *found =
        fp_source_find(sources, count, candidate->atlas_id,
                       candidate->source_id);
    return found && strcmp(found->abs, candidate->abs) == 0;
}

/* Preserve the last successful observation for source memberships that still
 * exist with the same stable IDs and resolved paths. Entries from newly added
 * memberships are rebased to their current state so the model transaction
 * itself is not reported as an external filesystem delta. */
static tp_status fp_rebase_membership(
    const fp_entry *old_entries, int old_count,
    const fp_source *old_sources, int old_source_count,
    const fp_entry *current_entries, int current_count,
    const fp_source *current_sources, int current_source_count,
    fp_entry **out_entries, int *out_count, int *out_cap,
    tp_error *error) {
    for (int i = 0; i < old_count; ++i) {
        const fp_source *owner =
            fp_source_find(old_sources, old_source_count,
                           old_entries[i].atlas_id,
                           old_entries[i].source_id);
        if (!owner ||
            !fp_source_membership_contains(current_sources,
                                           current_source_count, owner)) {
            continue;
        }
        tp_status status =
            fp_push(out_entries, out_count, out_cap,
                    old_entries[i].atlas_id, old_entries[i].source_id,
                    old_entries[i].abs, old_entries[i].size,
                    old_entries[i].mtime, error);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    for (int i = 0; i < current_count; ++i) {
        const fp_source *owner =
            fp_source_find(current_sources, current_source_count,
                           current_entries[i].atlas_id,
                           current_entries[i].source_id);
        if (!owner ||
            fp_source_membership_contains(old_sources, old_source_count,
                                          owner)) {
            continue;
        }
        tp_status status =
            fp_push(out_entries, out_count, out_cap,
                    current_entries[i].atlas_id,
                    current_entries[i].source_id, current_entries[i].abs,
                    current_entries[i].size, current_entries[i].mtime,
                    error);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    return TP_STATUS_OK;
}

/* The synchronous cost of a refresh: fingerprint every source (fp_collect), publish the external
 * runtime refresh (invalidate), fingerprint again, and diff. This is the folder-walk/stat-heavy part,
 * with NO UI/status/canvas/preview-stale side effects -- so it stays a pure, revision/dirty-preserving
 * computation shared by do_refresh (which adds the side effects) and the --bench-perf headless seam. */
static tp_status refresh_diff_core(int *out_added, int *out_removed,
                                   int *out_changed,
                                   int *out_unavailable,
                                   bool *out_sources_invalidated,
                                   tp_error *error) {
    if (out_added) {
        *out_added = 0;
    }
    if (out_removed) {
        *out_removed = 0;
    }
    if (out_changed) {
        *out_changed = 0;
    }
    if (out_unavailable) {
        *out_unavailable = 0;
    }
    if (out_sources_invalidated) {
        *out_sources_invalidated = false;
    }
    fp_bind_current_session();
    const tp_id128 membership_hash = fp_membership_hash();

    fp_entry *before = NULL;
    int bn = 0;
    int bc = 0;
    bool before_is_retained = false;
    const bool retained_before =
        s_refresh_fingerprint_valid &&
        tp_id128_eq(s_refresh_membership_hash, membership_hash);
    if (retained_before) {
        before = s_refresh_fingerprint;
        bn = s_refresh_fingerprint_count;
        before_is_retained = true;
    } else {
        fp_entry *current = NULL;
        int current_count = 0;
        int current_cap = 0;
        fp_source *current_sources = NULL;
        int current_source_count = 0;
        int current_source_cap = 0;
        tp_status status =
            fp_collect(&current, &current_count, &current_cap,
                       &current_sources, &current_source_count,
                       &current_source_cap, error);
        if (status != TP_STATUS_OK) {
            fp_free(current, current_count);
            fp_sources_free(current_sources, current_source_count);
            gui_project_invalidate_sources();
            if (out_sources_invalidated) {
                *out_sources_invalidated = true;
            }
            return status;
        }
        if (s_refresh_fingerprint_valid) {
            status = fp_rebase_membership(
                s_refresh_fingerprint, s_refresh_fingerprint_count,
                s_refresh_sources, s_refresh_source_count, current,
                current_count, current_sources, current_source_count,
                &before, &bn, &bc, error);
            fp_free(current, current_count);
            if (status != TP_STATUS_OK) {
                fp_free(before, bn);
                fp_sources_free(current_sources, current_source_count);
                return status;
            }
        } else {
            before = current;
            bn = current_count;
            bc = current_cap;
        }
        fp_sources_free(current_sources, current_source_count);
    }

    gui_project_invalidate_sources(); /* publish the external runtime refresh */
    if (out_sources_invalidated) {
        *out_sources_invalidated = true;
    }

    fp_entry *after = NULL;
    int an = 0;
    int ac = 0;
    fp_source *after_sources = NULL;
    int after_source_count = 0;
    int after_source_cap = 0;
    tp_status status =
        fp_collect(&after, &an, &ac, &after_sources, &after_source_count,
                   &after_source_cap, error);
    if (status != TP_STATUS_OK) {
        if (!before_is_retained) {
            fp_free(before, bn);
        }
        fp_free(after, an);
        fp_sources_free(after_sources, after_source_count);
        return status;
    }

    int added = 0;
    int removed = 0;
    int changed = 0;
    int unavailable = 0;
    for (int i = 0; i < an; i++) {
        const fp_entry *b =
            fp_find(before, bn, after[i].atlas_id, after[i].source_id,
                    after[i].abs);
        if (after[i].size < 0) {
            unavailable++;
            if (b && b->size >= 0) {
                removed++;
            }
        } else if (!b || b->size < 0) {
            added++;
        } else if (b->size != after[i].size || b->mtime != after[i].mtime) {
            changed++;
        }
    }
    for (int i = 0; i < bn; i++) {
        if (before[i].size >= 0 &&
            !fp_find(after, an, before[i].atlas_id, before[i].source_id,
                     before[i].abs)) {
            removed++;
        }
    }
    if (s_refresh_fingerprint_valid) {
        fp_free(s_refresh_fingerprint, s_refresh_fingerprint_count);
        fp_sources_free(s_refresh_sources, s_refresh_source_count);
    }
    if (!before_is_retained) {
        fp_free(before, bn);
    }
    s_refresh_fingerprint = after;
    s_refresh_fingerprint_count = an;
    s_refresh_sources = after_sources;
    s_refresh_source_count = after_source_count;
    s_refresh_fingerprint_valid = true;
    s_refresh_membership_hash = membership_hash;

    if (out_added) {
        *out_added = added;
    }
    if (out_removed) {
        *out_removed = removed;
    }
    if (out_changed) {
        *out_changed = changed;
    }
    if (out_unavailable) {
        *out_unavailable = unavailable;
    }
    return TP_STATUS_OK;
}

bool gui_actions_refresh_should_mark_stale(tp_status status,
                                           bool sources_invalidated) {
    return status == TP_STATUS_OK || sources_invalidated;
}

/* Rescan sources and mark derived preview data stale without dirtying the model. */
void gui_actions__refresh(void) {
    int added = 0;
    int removed = 0;
    int changed = 0;
    int unavailable = 0;
    bool sources_invalidated = false;
    tp_error error = {0};
    const tp_status status =
        refresh_diff_core(&added, &removed, &changed, &unavailable,
                          &sources_invalidated, &error);
    if (status != TP_STATUS_OK) {
        if (gui_actions_refresh_should_mark_stale(status,
                                                  sources_invalidated)) {
            gui_project_mark_stale();
        }
        const bool runtime_source_warning =
            status == TP_STATUS_NOT_FOUND ||
            status == TP_STATUS_PATH_RESOLVE_FAILED ||
            status == TP_STATUS_INVALID_UTF8;
        set_statusf_ex(runtime_source_warning ? STATUS_WARNING : STATUS_ERROR,
                       runtime_source_warning ? "Refresh warning: %s"
                                              : "Refresh failed: %s",
                       error.msg);
        return;
    }

    gui_canvas_invalidate(&s_canvas); /* force the shown image to reload (or show missing) */
    gui_project_mark_stale();         /* disk changed -> preview stale, project NOT dirtied */
    if (unavailable > 0) {
        set_statusf_ex(
            STATUS_WARNING,
            "Refresh: +%d new, %d removed, %d changed; %d source unavailable",
            added, removed, changed, unavailable);
    } else {
        set_statusf("Refresh: +%d new, %d removed, %d changed", added, removed,
                    changed);
    }
}

/* Headless bench seam for the real refresh cost without UI or preview side
 * effects. Revision and dirty state remain unchanged. */
bool gui_actions_refresh_diff_headless(int *out_added, int *out_removed,
                                       int *out_changed) {
    tp_error error = {0};
    return refresh_diff_core(out_added, out_removed, out_changed, NULL, NULL,
                             &error) == TP_STATUS_OK;
}

// #endregion

// #region deferred side-effects (run at the top of the frame, between frames)
void apply_pending(void) {
    if (gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_DRAINING) {
        return;
    }
    const bool save_as_requires_preflight =
        gui_draft_phase() != GUI_EDIT_IDLE &&
        (gui_actions__intent_queued(GUI_INTENT_SAVE_AS) ||
         (gui_actions__intent_queued(GUI_INTENT_SAVE) &&
          !gui_project_has_path()));
    if (save_as_requires_preflight) {
        s_actions.gesture_commit = false;
        (void)gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG);
        gui_actions__clear_pending();
        return;
    }
    const tp_session_snapshot *before_atlas =
        gui_project_snapshot();
    const int64_t revision_before_atlas =
        before_atlas
            ? tp_session_snapshot_revision(before_atlas)
            : -1;
    if (s_actions.draft_apply_mine) {
        const bool applied =
            gui_actions__apply_draft_mine();
        s_actions.draft_apply_mine = false;
        if (!applied) {
            s_actions.gesture_commit = false;
            gui_actions__discard_deferred_edits();
            gui_actions__clear_pending();
            return;
        }
    } else if (s_actions.gesture_commit &&
               gui_draft_phase() !=
                   GUI_EDIT_IDLE) {
        if (!gui_actions__submit_draft()) {
            s_actions.gesture_commit = false;
            gui_actions__discard_deferred_edits();
            gui_actions__clear_pending();
            return;
        }
    }
    const tp_session_snapshot *after_atlas =
        gui_project_snapshot();
    const int64_t revision_after_atlas =
        after_atlas
            ? tp_session_snapshot_revision(after_atlas)
            : -1;
    if (revision_before_atlas >= 0 &&
        revision_after_atlas >= 0) {
        gui_actions__rebase_deferred_edits(
            revision_before_atlas,
            revision_after_atlas);
    }

    /* The atlas draft is the prerequisite for the remaining edits in this
     * frame. Only after it reaches a terminal success may dependent edit
     * queues mutate the session. */
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_EDIT);

    s_actions.gesture_commit = false;

    if (gui_actions__apply_lifecycle_request() &&
        gui_project_lifecycle_state_query() ==
            GUI_PROJECT_LIFECYCLE_DRAINING) {
        return;
    }

    apply_pending_history_action();

    gui_actions__apply_confirm();
    if (gui_project_lifecycle_state_query() ==
        GUI_PROJECT_LIFECYCLE_DRAINING) {
        return;
    }

    gui_actions__apply_recovery();

    if (gui_actions__intent_drain(GUI_INTENT_PHASE_DIALOG)) {
        gui_actions__clear_pending();
        return;
    }
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_STRUCTURAL);
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_REFRESH);
    (void)gui_actions__intent_drain(GUI_INTENT_PHASE_PACK);

    gui_actions__clear_pending();
}

void gui_actions_poll_host_completion(void) {
    gui_actions__poll_pack();
}
// #endregion
