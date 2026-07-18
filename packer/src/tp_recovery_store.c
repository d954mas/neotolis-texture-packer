#include "tp_recovery_state_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_fs_internal.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"

static bool s_test_fail_next_live_retire_cleanup;

bool tp_recovery__has_journal_suffix(const char *name) {
    static const char suffix[] = ".ntpjournal";
    const size_t len = name ? strlen(name) : 0U;
    return len >= sizeof suffix - 1U &&
           strcmp(name + len - (sizeof suffix - 1U), suffix) == 0;
}

const char *tp_recovery__path_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *p = base; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

tp_status tp_recovery__store_journal_path(const tp_recovery_store *store, const char *input,
                                    char *journal, size_t journal_cap, tp_error *err) {
    if (!store || !input || input[0] == '\0' || !journal || journal_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal path is required");
    }
    const char *base = tp_recovery__path_basename(input);
    if (base == input || base[0] == '\0' || !tp_recovery__has_journal_suffix(base)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal must be an .ntpjournal file under the store root");
    }
    size_t parent_len = (size_t)(base - input);
    while (parent_len > 0U && (input[parent_len - 1U] == '/' || input[parent_len - 1U] == '\\')) {
        parent_len--;
    }
    if (parent_len == 0U || parent_len >= TP_IDENTITY_PATH_MAX) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal parent path is invalid");
    }
    char parent[TP_IDENTITY_PATH_MAX];
    memcpy(parent, input, parent_len);
    parent[parent_len] = '\0';
    char canonical_parent[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_identity_path_canonical(parent, canonical_parent,
                                                  sizeof canonical_parent, err);
    if (status != TP_STATUS_OK || !tp_identity_path_equal(canonical_parent, store->root)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery journal is outside the store root");
    }
    const int written = snprintf(journal, journal_cap, "%s/%s", store->root, base);
    if (written < 0 || (size_t)written >= journal_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery journal path is too long");
    }
    return TP_STATUS_OK;
}

tp_status tp_recovery__lock_path_for(const tp_recovery_store *store, const char *journal_input,
                               char *journal, size_t journal_cap,
                               char *lock, size_t lock_cap, tp_error *err) {
    tp_status status = tp_recovery__store_journal_path(store, journal_input, journal, journal_cap, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const int written = snprintf(lock, lock_cap, "%s%s", journal, TP_RECOVERY_LOCK_SUFFIX);
    if (written < 0 || (size_t)written >= lock_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery claim lock path is too long");
    }
    return TP_STATUS_OK;
}

static bool recovery_root_is_dir(const char *path) {
    tp_fs_info info;
    return tp_fs_stat(path, &info) && info.kind == TP_FS_KIND_DIRECTORY &&
           !info.reparse;
}

// #region store & live slot
tp_status tp_recovery_store_create(const char *root, tp_id128 journal_key,
                                   tp_recovery_store **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery store output is required");
    }
    *out = NULL;
    tp_recovery_store *store = (tp_recovery_store *)calloc(1, sizeof *store);
    if (!store) {
        return tp_error_set(err, TP_STATUS_OOM, "recovery store allocation failed");
    }
    tp_status status = tp_identity_path_canonical(root, store->root,
                                                  sizeof store->root, err);
    if (status != TP_STATUS_OK || !recovery_root_is_dir(store->root)) {
        free(store);
        return status != TP_STATUS_OK
                   ? status
                   : tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                                  "recovery root is not a directory");
    }
    store->journal_key = journal_key;
    *out = store;
    return TP_STATUS_OK;
}

void tp_recovery_store_destroy(tp_recovery_store *store) {
    free(store);
}

tp_status tp_recovery_root_validate(const char *root, tp_id128 journal_key,
                                    tp_error *err) {
    tp_recovery_store *store = NULL;
    const tp_status status =
        tp_recovery_store_create(root, journal_key, &store, err);
    tp_recovery_store_destroy(store);
    return status;
}

tp_status tp_recovery__live_slot_generate(const tp_recovery_store *store,
                                             const tp_rng *rng, char *out,
                                             size_t out_cap, tp_error *err) {
    if (!store || !rng || !rng->fill || !out || out_cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live slot requires store, RNG, and output");
    }
    tp_id128 slot_id = tp_id128_nil();
    tp_status status = tp_id128_generate(rng, &slot_id, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    char hex[33];
    for (size_t i = 0U; i < sizeof slot_id.bytes; ++i) {
        (void)snprintf(hex + i * 2U, 3U, "%02x", slot_id.bytes[i]);
    }
    const int written = snprintf(out, out_cap, "%s/%s.ntpjournal",
                                 store->root, hex);
    if (written < 0 || (size_t)written >= out_cap) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "recovery live-slot path is too long");
    }
    return TP_STATUS_OK;
}

tp_status tp_recovery_store_create_live(tp_recovery_store *store,
                                        const char *journal_path,
                                        tp_recovery_live **out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live output is required");
    }
    *out = NULL;
    tp_recovery_live *live = (tp_recovery_live *)calloc(1, sizeof *live);
    if (!live) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery live allocation failed");
    }
    live->lock = TP_RECOVERY_LOCK_PIN_INIT;
    live->journal_pin = TP_RECOVERY_FILE_PIN_INIT;
    tp_status status = tp_recovery__lock_path_for(store, journal_path, live->journal_path,
                                     sizeof live->journal_path,
                                     live->lock_path,
                                     sizeof live->lock_path, err);
    if (status == TP_STATUS_OK) {
        status = tp_recovery_backend_lock_open(&live->lock, live->lock_path, err);
    }
    if (status != TP_STATUS_OK) {
        free(live);
        return status;
    }
    live->journal_key = store->journal_key;
    live->healthy = true;
    *out = live;
    return TP_STATUS_OK;
}

static tp_status live_set_metadata(tp_recovery_live *live,
                                   const tp_recovery_metadata *metadata,
                                   tp_error *err) {
    const char *path = metadata->project_path ? metadata->project_path : "";
    const char *name = metadata->project_name ? metadata->project_name : "";
    tp_status status = tp_model_set_recovery_metadata_ex(
        live->attached_model, metadata->timestamp, path, name,
        metadata->file_fingerprint, err);
    if (status != TP_STATUS_OK) {
        tp_model_detach_journal(live->attached_model);
        live->attached_model = NULL;
        live->healthy = false;
    } else {
        live->metadata_timestamp = metadata->timestamp;
        (void)snprintf(live->metadata_name, sizeof live->metadata_name,
                       "%s", name);
    }
    return status;
}

tp_status tp_recovery_live__update_saved_identity(
    tp_recovery_live *live, const char *canonical_path,
    const tp_id128 *file_fingerprint, tp_error *err) {
    if (!live || !canonical_path || canonical_path[0] == '\0' ||
        !file_fingerprint) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "saved recovery identity requires live owner, path, and fingerprint");
    }
    const tp_recovery_metadata metadata = {
        .timestamp = live->metadata_timestamp,
        .project_path = canonical_path,
        .project_name = tp_recovery__path_basename(canonical_path),
        .file_fingerprint = file_fingerprint,
    };
    return tp_recovery_live_update_metadata(live, &metadata, err);
}

tp_status tp_recovery_live_attach(tp_recovery_live *live, tp_model *model,
                                  const tp_recovery_metadata *metadata,
                                  tp_error *err) {
    if (!live || !model || !metadata || live->finished || live->attached_model) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live attach requires an open unattached live handle, model, and metadata");
    }
    tp_status io_status = TP_STATUS_JOURNAL_FAILED;
    tp_journal_io io = tp_recovery_backend_live_create(live->journal_path,
                                          &live->journal_pin,
                                          &io_status, err);
    if (!io.ctx) {
        live->healthy = false;
        return io_status;
    }
    if (!io.length || io.length(io.ctx) != 0U) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        live->healthy = false;
        return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                            "recovery live slot is not empty after reset");
    }
    tp_journal *journal = tp_journal_create(io, live->journal_key);
    if (!journal) {
        live->healthy = false;
        return tp_error_set(err, TP_STATUS_OOM,
                            "recovery live journal allocation failed");
    }
    tp_status status = tp_model_attach_journal(model, journal, err);
    if (status != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        live->healthy = false;
        return status;
    }
    live->attached_model = model;
    return live_set_metadata(live, metadata, err);
}

tp_status tp_recovery_live_update_metadata(tp_recovery_live *live,
                                           const tp_recovery_metadata *metadata,
                                           tp_error *err) {
    if (!live || !metadata || live->finished || !live->attached_model ||
        !live->healthy) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery metadata update requires a healthy attached live handle");
    }
    return live_set_metadata(live, metadata, err);
}

bool tp_recovery_live_healthy(const tp_recovery_live *live) {
    return live && live->healthy && !live->finished;
}

void tp_recovery_live__mark_degraded(tp_recovery_live *live) {
    if (live && !live->finished) {
        live->healthy = false;
    }
}

const char *tp_recovery_live_journal_path(const tp_recovery_live *live) {
    return live ? live->journal_path : NULL;
}

static tp_status live_finish(tp_recovery_live *live, bool preserve_journal,
                             bool retire_unhealthy, tp_error *err) {
    if (!live) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "recovery live handle is required");
    }
    if (live->finished) {
        return live->terminal_status;
    }
    if (live->attached_model) {
        tp_model_detach_journal(live->attached_model);
        live->attached_model = NULL;
    }
    tp_status status = TP_STATUS_OK;
    if (!preserve_journal && (live->healthy || retire_unhealthy)) {
        if (retire_unhealthy && s_test_fail_next_live_retire_cleanup) {
            s_test_fail_next_live_retire_cleanup = false;
            status = tp_error_set(err, TP_STATUS_RECOVERY_CLEANUP_FAILED,
                                  "injected recovery live-retire cleanup failure");
        } else {
            status = tp_recovery_backend_live_delete(live->journal_path,
                                     &live->journal_pin, err);
        }
    }
    tp_recovery_backend_live_close(&live->journal_pin);
    tp_recovery_backend_lock_release(&live->lock);
    live->finished = true;
    live->terminal_status = status;
    return status;
}

tp_status tp_recovery_live_finish(tp_recovery_live *live,
                                  bool preserve_journal, tp_error *err) {
    return live_finish(live, preserve_journal, false, err);
}

tp_status tp_recovery_live_retire(tp_recovery_live *live, tp_error *err) {
    return live_finish(live, false, true, err);
}

void tp_recovery_live_destroy(tp_recovery_live *live) {
    if (!live) {
        return;
    }
    (void)tp_recovery_live_finish(live, true, NULL);
    free(live);
}
// #endregion
void tp_recovery__live_test_fail_next_retire_cleanup(void) {
    s_test_fail_next_live_retire_cleanup = true;
}
