#include "tp_core/tp_source_plan.h"

#include <stdlib.h>
#include <string.h>

#include "tp_source_plan_internal.h"
#include "tp_strutil.h"

typedef struct source_identity {
    char *absolute;
    char *canonical;
    const tp_snapshot_source *source;
} source_identity;

struct tp_source_plan_storage {
    source_identity *accepted;
};

static char *source_strdup(const char *text) {
    const size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static void source_identity_drop(source_identity *identity) {
    if (!identity) {
        return;
    }
    free(identity->absolute);
    free(identity->canonical);
    memset(identity, 0, sizeof *identity);
}

tp_status tp_source_path_identity_from_input(const char *input,
                                             tp_source_path_identity *out,
                                             tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source identity output is required");
    }
    memset(out, 0, sizeof *out);
    if (!input || input[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source path is empty");
    }
    tp_status status = tp_project_path_slash_normalize(
        input, out->canonical, sizeof out->canonical);
    if (status != TP_STATUS_OK) {
        return tp_error_set(err, status,
                            "source path exceeds the supported limit");
    }
    status = tp_identity_path_absolute_lexical(
        out->canonical, out->absolute, sizeof out->absolute, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    out->has_canonical =
        tp_identity_path_canonical(out->absolute, out->canonical,
                                   sizeof out->canonical, NULL) == TP_STATUS_OK;
    return TP_STATUS_OK;
}

tp_status tp_source_path_identity_from_stored(const tp_project *project,
                                              const char *path,
                                              bool resolve_canonical,
                                              tp_source_path_identity *out,
                                              tp_error *err) {
    if (!project || !path || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "stored source identity arguments are invalid");
    }
    memset(out, 0, sizeof *out);
    tp_status status = tp_project_path_slash_normalize(
        path, out->canonical, sizeof out->canonical);
    if (status != TP_STATUS_OK) {
        return tp_error_set(err, status,
                            "source path exceeds the supported limit");
    }
    status = tp_project_resolve_source_path(
        project, out->canonical, out->absolute, sizeof out->absolute);
    if (status != TP_STATUS_OK) {
        status = tp_identity_path_absolute_lexical(
            out->canonical, out->absolute, sizeof out->absolute, err);
    }
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = tp_identity_path_absolute_lexical(
        out->absolute, out->canonical, sizeof out->canonical, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    memcpy(out->absolute, out->canonical, strlen(out->canonical) + 1U);
    out->has_canonical =
        resolve_canonical &&
        tp_identity_path_canonical(out->absolute, out->canonical,
                                   sizeof out->canonical, NULL) == TP_STATUS_OK;
    return TP_STATUS_OK;
}

bool tp_source_path_identity_equal_text(const char *left_absolute,
                                        const char *left_canonical,
                                        const char *right_absolute,
                                        const char *right_canonical) {
    if (tp_identity_path_equal(left_absolute, right_absolute)) {
        return true;
    }
    return left_canonical && right_canonical &&
           tp_identity_path_equal(left_canonical, right_canonical);
}

static tp_status source_identity_from_input(const char *input,
                                            source_identity *out,
                                            tp_error *err) {
    memset(out, 0, sizeof *out);
    tp_source_path_identity identity;
    tp_status status = tp_source_path_identity_from_input(input, &identity,
                                                          err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    out->absolute = source_strdup(identity.absolute);
    if (!out->absolute) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "source identity allocation failed");
    }
    if (identity.has_canonical) {
        out->canonical = source_strdup(identity.canonical);
        if (!out->canonical) {
            source_identity_drop(out);
            return tp_error_set(err, TP_STATUS_OOM,
                                "source identity allocation failed");
        }
    }
    return TP_STATUS_OK;
}

static tp_status source_identity_from_snapshot(
    const tp_session_snapshot *snapshot, int atlas_index, int source_index,
    source_identity *out, tp_error *err) {
    memset(out, 0, sizeof *out);
    const tp_snapshot_source *source = NULL;
    char resolved[TP_IDENTITY_PATH_MAX];
    tp_status status = tp_session_snapshot_source_resolved_at(
        snapshot, atlas_index, source_index, &source, resolved,
        sizeof resolved, err);
    if (status != TP_STATUS_OK) {
        /* An unsaved in-memory project has no project_dir yet. Its relative
         * source spelling still has a well-defined request-edge identity: the
         * same process CWD used for a relative incoming path. */
        if (!source || !source->path) {
            return status;
        }
        status = source_identity_from_input(source->path, out, err);
        if (status == TP_STATUS_OK) {
            out->source = source;
            if (err) {
                memset(err, 0, sizeof *err);
            }
        }
        return status;
    }
    status = source_identity_from_input(resolved, out, err);
    if (status == TP_STATUS_OK) {
        out->source = source;
    }
    return status;
}

static bool source_identity_equal(const source_identity *left,
                                  const source_identity *right) {
    return tp_source_path_identity_equal_text(
        left->absolute, left->canonical, right->absolute, right->canonical);
}

static int snapshot_atlas_index(const tp_session_snapshot *snapshot,
                                tp_id128 atlas_id) {
    const int count = tp_session_snapshot_atlas_count(snapshot);
    for (int i = 0; i < count; ++i) {
        const tp_snapshot_atlas *atlas =
            tp_session_snapshot_atlas_at(snapshot, i);
        if (atlas && tp_id128_eq(atlas->id, atlas_id)) {
            return i;
        }
    }
    return -1;
}

static void source_identities_free(source_identity *identities, int count) {
    if (!identities) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        source_identity_drop(&identities[i]);
    }
    free(identities);
}

static tp_status snapshot_source_identities(
    const tp_session_snapshot *snapshot, int atlas_index,
    source_identity **out_identities, int *out_count, tp_error *err) {
    *out_identities = NULL;
    *out_count = 0;
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, atlas_index);
    if (!atlas || atlas->source_count <= 0) {
        return TP_STATUS_OK;
    }
    source_identity *identities = (source_identity *)calloc(
        (size_t)atlas->source_count, sizeof *identities);
    if (!identities) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "source identity allocation failed");
    }
    for (int i = 0; i < atlas->source_count; ++i) {
        tp_status status = source_identity_from_snapshot(
            snapshot, atlas_index, i, &identities[i], err);
        if (status != TP_STATUS_OK) {
            source_identities_free(identities, atlas->source_count);
            return status;
        }
    }
    *out_identities = identities;
    *out_count = atlas->source_count;
    return TP_STATUS_OK;
}

tp_status tp_source_batch_plan_create(
    const tp_session_snapshot *snapshot, tp_id128 atlas_id,
    const char *const *inputs, int input_count, tp_source_batch_plan *out,
    tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!snapshot || tp_id128_is_nil(atlas_id) || !out || input_count < 0 ||
        (input_count > 0 && !inputs)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source batch plan arguments are invalid");
    }
    const int atlas_index = snapshot_atlas_index(snapshot, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "source batch atlas was not found");
    }
    if (input_count == 0) {
        return TP_STATUS_OK;
    }

    source_identity *existing = NULL;
    int existing_count = 0;
    tp_status status = snapshot_source_identities(
        snapshot, atlas_index, &existing, &existing_count, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    tp_source_batch_item *items = (tp_source_batch_item *)calloc(
        (size_t)input_count, sizeof *items);
    tp_source_plan_storage *storage =
        (tp_source_plan_storage *)calloc(1U, sizeof *storage);
    source_identity *accepted = (source_identity *)calloc(
        (size_t)input_count, sizeof *accepted);
    if (!items || !storage || !accepted) {
        free(items);
        free(storage);
        free(accepted);
        source_identities_free(existing, existing_count);
        return tp_error_set(err, TP_STATUS_OOM,
                            "source batch plan allocation failed");
    }
    storage->accepted = accepted;

    int accepted_count = 0;
    int duplicate_count = 0;
    for (int i = 0; i < input_count; ++i) {
        source_identity candidate;
        status = source_identity_from_input(inputs[i], &candidate, err);
        if (status != TP_STATUS_OK) {
            source_identities_free(existing, existing_count);
            for (int j = 0; j < accepted_count; ++j) {
                source_identity_drop(&accepted[j]);
            }
            free(accepted);
            free(storage);
            free(items);
            return status;
        }
        bool duplicate = false;
        for (int j = 0; j < existing_count && !duplicate; ++j) {
            duplicate = source_identity_equal(&candidate, &existing[j]);
        }
        for (int j = 0; j < accepted_count && !duplicate; ++j) {
            duplicate = source_identity_equal(&candidate, &accepted[j]);
        }
        if (duplicate) {
            duplicate_count++;
            source_identity_drop(&candidate);
            continue;
        }
        accepted[accepted_count] = candidate;
        items[accepted_count].input_index = i;
        items[accepted_count].path = candidate.absolute;
        accepted_count++;
    }
    source_identities_free(existing, existing_count);
    out->items = items;
    out->count = accepted_count;
    out->duplicate_count = duplicate_count;
    out->storage = storage;
    return TP_STATUS_OK;
}

void tp_source_batch_plan_free(tp_source_batch_plan *plan) {
    if (!plan) {
        return;
    }
    tp_source_plan_storage *storage = plan->storage;
    if (storage) {
        for (int i = 0; i < plan->count; ++i) {
            source_identity_drop(&storage->accepted[i]);
        }
        free(storage->accepted);
        free(storage);
    }
    free(plan->items);
    memset(plan, 0, sizeof *plan);
}

tp_status tp_source_snapshot_find(const tp_session_snapshot *snapshot,
                                  tp_id128 atlas_id, const char *input,
                                  const tp_snapshot_source **out,
                                  tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!snapshot || tp_id128_is_nil(atlas_id) || !input || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "source lookup arguments are invalid");
    }
    const int atlas_index = snapshot_atlas_index(snapshot, atlas_id);
    if (atlas_index < 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND,
                            "source lookup atlas was not found");
    }
    source_identity candidate;
    tp_status status = source_identity_from_input(input, &candidate, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    const tp_snapshot_atlas *atlas =
        tp_session_snapshot_atlas_at(snapshot, atlas_index);
    for (int i = 0; i < atlas->source_count; ++i) {
        source_identity existing;
        status = source_identity_from_snapshot(snapshot, atlas_index, i,
                                               &existing, err);
        if (status != TP_STATUS_OK) {
            source_identity_drop(&candidate);
            return status;
        }
        const bool equal = source_identity_equal(&candidate, &existing);
        if (equal) {
            *out = existing.source;
        }
        source_identity_drop(&existing);
        if (equal) {
            source_identity_drop(&candidate);
            return TP_STATUS_OK;
        }
    }
    source_identity_drop(&candidate);
    return tp_error_set(err, TP_STATUS_NOT_FOUND,
                        "atlas has no source matching '%s'", input);
}
