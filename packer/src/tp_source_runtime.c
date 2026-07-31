#include "tp_source_runtime_internal.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hash/nt_hash.h"

#include "tp_core/tp_scan.h"
#include "tp_core/tp_session_snapshot_query.h"
#include "tp_core/tp_srckey.h"

struct tp_source_runtime_projection {
    tp_source_runtime_source *sources;
    int source_count;
    tp_source_runtime_entry *entries;
    int entry_count;
};

static char *runtime_strdup(const char *text) {
    if (!text) {
        return NULL;
    }
    const size_t length = strlen(text) + 1U;
    char *copy = malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static const char *runtime_basename(const char *path) {
    const char *base = path ? path : "";
    for (const char *scan = base; *scan; ++scan) {
        if (*scan == '/' || *scan == '\\') {
            base = scan + 1;
        }
    }
    return base;
}

typedef struct runtime_key_set {
    const char **slots;
    size_t capacity;
} runtime_key_set;

static tp_status runtime_key_set_init(
    runtime_key_set *set, int expected, tp_error *err) {
    if (expected <= 1) {
        return TP_STATUS_OK;
    }
    size_t required = (size_t)expected;
    if (required > SIZE_MAX / 2U) {
        return tp_error_set(
            err, TP_STATUS_OUT_OF_BOUNDS,
            "source runtime canonical-key set overflows size_t");
    }
    required *= 2U;
    size_t capacity = 2U;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(
                err, TP_STATUS_OUT_OF_BOUNDS,
                "source runtime canonical-key set is too large");
        }
        capacity *= 2U;
    }
    set->slots = calloc(capacity, sizeof *set->slots);
    if (!set->slots) {
        return tp_error_set(
            err, TP_STATUS_OOM,
            "source runtime canonical-key set allocation failed");
    }
    set->capacity = capacity;
    return TP_STATUS_OK;
}

static bool runtime_key_set_contains(
    const runtime_key_set *set, const char *key) {
    if (!set->slots) {
        return false;
    }
    const size_t mask = set->capacity - 1U;
    size_t slot =
        (size_t)nt_hash64_str(key).value & mask;
    while (set->slots[slot]) {
        if (strcmp(set->slots[slot], key) == 0) {
            return true;
        }
        slot = (slot + 1U) & mask;
    }
    return false;
}

static void runtime_key_set_insert(
    runtime_key_set *set, const char *key) {
    if (!set->slots) {
        return;
    }
    const size_t mask = set->capacity - 1U;
    size_t slot =
        (size_t)nt_hash64_str(key).value & mask;
    while (set->slots[slot]) {
        slot = (slot + 1U) & mask;
    }
    set->slots[slot] = key;
}

static void runtime_source_error(
    tp_source_runtime_source *source, tp_status status,
    const tp_error *error, const char *fallback) {
    source->status = status;
    if (error && error->msg[0] != '\0') {
        source->error = *error;
    } else {
        (void)tp_error_set(
            &source->error, status, "%s", fallback);
    }
}

void tp_source_runtime_destroy(
    tp_source_runtime_projection *projection) {
    if (!projection) {
        return;
    }
    for (int i = 0; i < projection->source_count; ++i) {
        free((void *)projection->sources[i].absolute_path);
    }
    for (int i = 0; i < projection->entry_count; ++i) {
        free((void *)projection->entries[i].source_key);
        free((void *)projection->entries[i].absolute_path);
    }
    free(projection->sources);
    free(projection->entries);
    free(projection);
}

static void runtime_truncate_entries(
    tp_source_runtime_projection *projection, int count) {
    while (projection->entry_count > count) {
        tp_source_runtime_entry *entry =
            &projection->entries[--projection->entry_count];
        free((void *)entry->source_key);
        free((void *)entry->absolute_path);
        memset(entry, 0, sizeof *entry);
    }
}

static bool runtime_reserve_entries(
    tp_source_runtime_projection *projection, int required,
    int *capacity) {
    if (required <= *capacity) {
        return true;
    }
    int next = *capacity ? *capacity : 64;
    while (next < required) {
        if (next > INT_MAX / 2) {
            next = required;
            break;
        }
        next *= 2;
    }
    if ((size_t)next > SIZE_MAX / sizeof *projection->entries) {
        return false;
    }
    tp_source_runtime_entry *grown =
        realloc(projection->entries,
                (size_t)next * sizeof *grown);
    if (!grown) {
        return false;
    }
    projection->entries = grown;
    *capacity = next;
    return true;
}

static tp_status runtime_push_entry(
    tp_source_runtime_projection *projection, int *capacity,
    tp_id128 atlas_id, tp_id128 source_id,
    const char *source_key, const char *absolute_path,
    int64_t size, int64_t mtime, runtime_key_set *keys,
    tp_error *err) {
    char normalized_key[TP_SRCKEY_MAX];
    tp_status status = tp_srckey_normalize(
        source_key, normalized_key, sizeof normalized_key, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (runtime_key_set_contains(keys, normalized_key)) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "canonical source-key collision for '%s'",
            normalized_key);
    }
    if (!runtime_reserve_entries(
            projection, projection->entry_count + 1, capacity)) {
        return tp_error_set(
            err, TP_STATUS_OOM,
            "source runtime entry allocation failed");
    }
    char *key = runtime_strdup(normalized_key);
    char *path = runtime_strdup(absolute_path);
    if (!key || !path) {
        free(key);
        free(path);
        return tp_error_set(
            err, TP_STATUS_OOM,
            "source runtime entry string allocation failed");
    }
    projection->entries[projection->entry_count++] =
        (tp_source_runtime_entry){
            .atlas_id = atlas_id,
            .source_id = source_id,
            .source_key = key,
            .absolute_path = path,
            .size = size,
            .mtime = mtime,
        };
    runtime_key_set_insert(keys, key);
    return TP_STATUS_OK;
}

tp_status tp_source_runtime_build(
    const tp_session_snapshot *snapshot,
    tp_source_runtime_projection **out,
    const tp_cancel_token *cancel, tp_error *err) {
    if (!snapshot || !out) {
        return tp_error_set(
            err, TP_STATUS_INVALID_ARGUMENT,
            "source runtime build requires snapshot and output");
    }
    *out = NULL;
    int source_count = 0;
    const int atlas_count =
        tp_session_snapshot_atlas_count(snapshot);
    for (int ai = 0; ai < atlas_count; ++ai) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, ai);
        if (atlas) {
            if (atlas->source_count > INT_MAX - source_count) {
                return tp_error_set(
                    err, TP_STATUS_OUT_OF_BOUNDS,
                    "source runtime source count overflow");
            }
            source_count += atlas->source_count;
        }
    }
    tp_source_runtime_projection *projection =
        calloc(1U, sizeof *projection);
    if (!projection) {
        return tp_error_set(
            err, TP_STATUS_OOM,
            "source runtime projection allocation failed");
    }
    if (source_count > 0) {
        projection->sources = calloc(
            (size_t)source_count, sizeof *projection->sources);
        if (!projection->sources) {
            tp_source_runtime_destroy(projection);
            return tp_error_set(
                err, TP_STATUS_OOM,
                "source runtime source allocation failed");
        }
    }
    int entry_capacity = 0;
    for (int ai = 0; ai < atlas_count; ++ai) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, ai);
        for (int si = 0; atlas && si < atlas->source_count; ++si) {
            if (tp_cancel_requested(cancel)) {
                tp_source_runtime_destroy(projection);
                return tp_error_set(
                    err, TP_STATUS_CANCELLED,
                    "source runtime build cancelled");
            }
            const tp_snapshot_source *source = NULL;
            char absolute[TP_IDENTITY_PATH_MAX] = {0};
            tp_error source_error = {{0}};
            tp_status status =
                tp_session_snapshot_source_resolved_at(
                    snapshot, ai, si, &source, absolute,
                    sizeof absolute, &source_error);
            if (!source) {
                tp_source_runtime_destroy(projection);
                return tp_error_set(
                    err, TP_STATUS_NOT_FOUND,
                    "source runtime membership disappeared");
            }
            tp_source_runtime_source *runtime_source =
                &projection->sources[projection->source_count++];
            runtime_source->atlas_id = atlas->id;
            runtime_source->source_id = source->id;
            runtime_source->folder =
                source->kind == TP_SNAPSHOT_SOURCE_FOLDER;
            runtime_source->status = status;
            runtime_source->first_entry = projection->entry_count;
            runtime_source->absolute_path =
                runtime_strdup(absolute);
            if (!runtime_source->absolute_path) {
                tp_source_runtime_destroy(projection);
                return tp_error_set(
                    err, TP_STATUS_OOM,
                    "source runtime path allocation failed");
            }
            if (status != TP_STATUS_OK) {
                runtime_source_error(
                    runtime_source, status, &source_error,
                    "source path could not be resolved");
                continue;
            }
            tp_scan_kind kind = TP_SCAN_KIND_MISSING;
            status = tp_scan_classify_checked(
                absolute, &kind, &source_error);
            runtime_source->status = status;
            if (status != TP_STATUS_OK) {
                runtime_source_error(
                    runtime_source, status, &source_error,
                    "source path could not be classified");
                continue;
            }
            if (runtime_source->folder) {
                if (kind != TP_SCAN_KIND_DIRECTORY) {
                    runtime_source_error(
                        runtime_source, TP_STATUS_NOT_FOUND, NULL,
                        "folder source path is not a directory");
                    continue;
                }
                tp_scan_result scanned = {0};
                status = tp_scan_dir_cancellable(
                    absolute, &scanned, cancel, &source_error);
                runtime_source->status = status;
                if (status == TP_STATUS_CANCELLED) {
                    tp_scan_free(&scanned);
                    tp_source_runtime_destroy(projection);
                    if (err) {
                        *err = source_error;
                    }
                    return TP_STATUS_CANCELLED;
                }
                if (status == TP_STATUS_OOM) {
                    tp_scan_free(&scanned);
                    tp_source_runtime_destroy(projection);
                    if (err) {
                        *err = source_error;
                    }
                    return TP_STATUS_OOM;
                }
                if (status == TP_STATUS_OK) {
                    runtime_key_set keys = {0};
                    status = runtime_key_set_init(
                        &keys, scanned.count, &source_error);
                    for (int i = 0; i < scanned.count; ++i) {
                        if (status != TP_STATUS_OK) {
                            break;
                        }
                        if (tp_cancel_requested(cancel)) {
                            status = tp_error_set(
                                &source_error,
                                TP_STATUS_CANCELLED,
                                "source runtime build cancelled");
                            break;
                        }
                        status = runtime_push_entry(
                            projection, &entry_capacity,
                            atlas->id, source->id,
                            scanned.entries[i].rel,
                            scanned.entries[i].abs,
                            scanned.entries[i].size,
                            scanned.entries[i].mtime, &keys,
                            &source_error);
                    }
                    free(keys.slots);
                }
                if (status == TP_STATUS_CANCELLED) {
                    tp_scan_free(&scanned);
                    tp_source_runtime_destroy(projection);
                    if (err) {
                        *err = source_error;
                    }
                    return TP_STATUS_CANCELLED;
                }
                if (status == TP_STATUS_OOM) {
                    tp_scan_free(&scanned);
                    tp_source_runtime_destroy(projection);
                    if (err) {
                        *err = source_error;
                    }
                    return TP_STATUS_OOM;
                }
                if (status != TP_STATUS_OK) {
                    runtime_truncate_entries(
                        projection,
                        runtime_source->first_entry);
                    runtime_source_error(
                        runtime_source, status, &source_error,
                        "folder source could not be projected");
                }
                tp_scan_free(&scanned);
            } else {
                if (kind != TP_SCAN_KIND_FILE) {
                    runtime_source_error(
                        runtime_source, TP_STATUS_NOT_FOUND, NULL,
                        "file source path is not a file");
                    continue;
                }
                long long size = 0;
                long long mtime = 0;
                if (!tp_scan_file_stat(absolute, &size, &mtime)) {
                    runtime_source_error(
                        runtime_source,
                        TP_STATUS_PATH_RESOLVE_FAILED, NULL,
                        "file source could not be statted");
                    continue;
                }
                if (tp_cancel_requested(cancel)) {
                    tp_source_runtime_destroy(projection);
                    return tp_error_set(
                        err, TP_STATUS_CANCELLED,
                        "source runtime build cancelled");
                }
                runtime_key_set keys = {0};
                tp_error entry_error = {{0}};
                status = runtime_push_entry(
                    projection, &entry_capacity, atlas->id,
                    source->id, runtime_basename(source->path),
                    absolute, size, mtime, &keys, &entry_error);
                if (status == TP_STATUS_OOM) {
                    tp_source_runtime_destroy(projection);
                    if (err) {
                        *err = entry_error;
                    }
                    return TP_STATUS_OOM;
                }
                if (status != TP_STATUS_OK) {
                    runtime_truncate_entries(
                        projection,
                        runtime_source->first_entry);
                    runtime_source_error(
                        runtime_source, status, &entry_error,
                        "file source key could not be projected");
                }
            }
            runtime_source->entry_count =
                projection->entry_count -
                runtime_source->first_entry;
        }
    }
    if (tp_cancel_requested(cancel)) {
        tp_source_runtime_destroy(projection);
        return tp_error_set(
            err, TP_STATUS_CANCELLED,
            "source runtime build cancelled");
    }
    *out = projection;
    return TP_STATUS_OK;
}

tp_source_runtime_projection *tp_source_runtime_clone(
    const tp_source_runtime_projection *projection) {
    if (!projection) {
        return NULL;
    }
    tp_source_runtime_projection *copy =
        calloc(1U, sizeof *copy);
    if (!copy) {
        return NULL;
    }
    if (projection->source_count > 0) {
        copy->sources = calloc(
            (size_t)projection->source_count,
            sizeof *copy->sources);
    }
    if (projection->entry_count > 0) {
        copy->entries = calloc(
            (size_t)projection->entry_count,
            sizeof *copy->entries);
    }
    if ((projection->source_count > 0 && !copy->sources) ||
        (projection->entry_count > 0 && !copy->entries)) {
        tp_source_runtime_destroy(copy);
        return NULL;
    }
    copy->source_count = projection->source_count;
    copy->entry_count = projection->entry_count;
    for (int i = 0; i < copy->source_count; ++i) {
        copy->sources[i] = projection->sources[i];
        copy->sources[i].absolute_path =
            runtime_strdup(projection->sources[i].absolute_path);
        if (!copy->sources[i].absolute_path) {
            tp_source_runtime_destroy(copy);
            return NULL;
        }
    }
    for (int i = 0; i < copy->entry_count; ++i) {
        copy->entries[i] = projection->entries[i];
        copy->entries[i].source_key =
            runtime_strdup(projection->entries[i].source_key);
        copy->entries[i].absolute_path =
            runtime_strdup(projection->entries[i].absolute_path);
        if (!copy->entries[i].source_key ||
            !copy->entries[i].absolute_path) {
            tp_source_runtime_destroy(copy);
            return NULL;
        }
    }
    return copy;
}

static const tp_source_runtime_entry *runtime_find_entry(
    const tp_source_runtime_projection *projection,
    const tp_source_runtime_entry *needle) {
    for (int i = 0; projection && i < projection->entry_count; ++i) {
        const tp_source_runtime_entry *entry =
            &projection->entries[i];
        if (tp_id128_eq(entry->atlas_id, needle->atlas_id) &&
            tp_id128_eq(entry->source_id, needle->source_id) &&
            strcmp(entry->source_key, needle->source_key) == 0) {
            return entry;
        }
    }
    return NULL;
}

void tp_source_runtime_diff(
    const tp_source_runtime_projection *before,
    const tp_source_runtime_projection *after,
    int *out_added, int *out_removed, int *out_changed,
    int *out_unavailable) {
    int added = 0;
    int removed = 0;
    int changed = 0;
    int unavailable = 0;
    for (int i = 0; after && i < after->entry_count; ++i) {
        const tp_source_runtime_entry *prior =
            runtime_find_entry(before, &after->entries[i]);
        if (!prior) {
            ++added;
        } else if (prior->size != after->entries[i].size ||
                   prior->mtime != after->entries[i].mtime) {
            ++changed;
        }
    }
    for (int i = 0; before && i < before->entry_count; ++i) {
        if (!runtime_find_entry(after, &before->entries[i])) {
            ++removed;
        }
    }
    for (int i = 0; after && i < after->source_count; ++i) {
        if (after->sources[i].status != TP_STATUS_OK) {
            ++unavailable;
        }
    }
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
}

int tp_source_runtime_source_count(
    const tp_source_runtime_projection *projection) {
    return projection ? projection->source_count : 0;
}

const tp_source_runtime_source *tp_source_runtime_source_at(
    const tp_source_runtime_projection *projection, int index) {
    return projection && index >= 0 &&
                   index < projection->source_count
               ? &projection->sources[index]
               : NULL;
}

const tp_source_runtime_source *tp_source_runtime_source_by_id(
    const tp_source_runtime_projection *projection,
    tp_id128 atlas_id, tp_id128 source_id) {
    for (int i = 0; projection && i < projection->source_count; ++i) {
        if (tp_id128_eq(
                projection->sources[i].atlas_id, atlas_id) &&
            tp_id128_eq(
                projection->sources[i].source_id, source_id)) {
            return &projection->sources[i];
        }
    }
    return NULL;
}

int tp_source_runtime_entry_count(
    const tp_source_runtime_projection *projection) {
    return projection ? projection->entry_count : 0;
}

const tp_source_runtime_entry *tp_source_runtime_entry_at(
    const tp_source_runtime_projection *projection, int index) {
    return projection && index >= 0 &&
                   index < projection->entry_count
               ? &projection->entries[index]
               : NULL;
}
