/* M0 production-path baseline: normal transaction, journal-backed Undo/Redo,
 * recovery, and Save+compaction. This is a release benchmark, not a timing
 * ctest. Setup and cleanup stay outside every timed region. */

#include "tp_bench_support.h"
#include "tp_bench_project_load.h"

#include "tp_core/tp_diff.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_project_identity_internal.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_sprite_index.h"
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h"
#include "tp_model_seam.h"
#include "tp_op_internal.h"
#include "tp_project_internal.h"
#include "tp_project_mutation_internal.h"
#include "tp_session_internal.h"
#include "tp_txn_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

typedef struct fixture_spec {
    const char *name;
    int atlases;
    int sources_per_atlas;
    int overrides_per_atlas;
    int animations_per_atlas;
    int frames_per_animation;
} fixture_spec;

typedef struct fixture {
    fixture_spec spec;
    tp_project *project;
    size_t serialized_bytes;
} fixture;

static int process_id(void) {
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

static int deterministic_rng(void *ctx, uint8_t *out, size_t len) {
    uint64_t *counter = (uint64_t *)ctx;
    const uint64_t value = ++(*counter);
    for (size_t i = 0; i < len; i++) {
        const unsigned shift = (unsigned)((i % sizeof value) * 8U);
        out[i] = (uint8_t)(value >> shift) ^ (uint8_t)(0xA5U + (uint8_t)(i * 17U));
    }
    return (int)len;
}

static bool parse_positive(const char *text, int max_value, int *out) {
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 1L || value > max_value) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool fill_atlas(tp_project *project, tp_project_atlas *atlas,
                       const fixture_spec *spec, tp_rng *rng,
                       tp_error *err) {
    char key[96];
    char value[96];
    for (int i = 0; i < spec->sources_per_atlas; i++) {
        if (snprintf(key, sizeof key, "art/folder_%03d", i) < 0 ||
            tp_project_atlas_add_source(atlas, key) != TP_STATUS_OK) {
            return false;
        }
    }
    if ((spec->overrides_per_atlas > 0 || spec->animations_per_atlas > 0) &&
        spec->sources_per_atlas == 0) {
        return false;
    }
    if (tp_project_assign_missing_ids(project, rng, err) != TP_STATUS_OK) {
        return false;
    }
    for (int i = 0; i < spec->overrides_per_atlas; i++) {
        tp_project_sprite *sprite = NULL;
        const tp_id128 source_id =
            atlas->sources[i % spec->sources_per_atlas].id;
        if (snprintf(key, sizeof key, "sprites/hero_walk_%05d.png", i) < 0 ||
            snprintf(value, sizeof value, "player_walk_%05d", i) < 0 ||
            tp_project_atlas_add_sprite_by_source_key(atlas, source_id, key,
                                                      &sprite) != TP_STATUS_OK ||
            !sprite ||
            tp_project_atlas_set_sprite_rename_by_source_key(
                atlas, source_id, key, value) != TP_STATUS_OK) {
            return false;
        }
        sprite->origin_x = 0.25F;
        sprite->slice9_lrtb[0] = 4U;
    }
    for (int i = 0; i < spec->animations_per_atlas; i++) {
        tp_project_anim *animation = NULL;
        if (snprintf(key, sizeof key, "anim_%03d", i) < 0 ||
            tp_project_atlas_add_animation(atlas, key, &animation) != TP_STATUS_OK || !animation) {
            return false;
        }
        animation->fps = 24.0F;
        for (int frame = 0; frame < spec->frames_per_animation; frame++) {
            const tp_id128 source_id =
                atlas->sources[frame % spec->sources_per_atlas].id;
            if (snprintf(value, sizeof value,
                         "sprites/hero_walk_%05d.png", frame) < 0 ||
                tp_project_anim_add_frame(animation, source_id, value) !=
                    TP_STATUS_OK) {
                return false;
            }
        }
    }
    return tp_project_atlas_add_target(atlas, "json-neotolis", "out/atlas", NULL) == TP_STATUS_OK;
}

static bool fixture_prepare(fixture *out, fixture_spec spec) {
    memset(out, 0, sizeof *out);
    out->spec = spec;
    tp_project *project = tp_project_create();
    if (!project) {
        return false;
    }
    uint64_t counter = 0U;
    tp_rng rng = {deterministic_rng, &counter};
    tp_error err = {{0}};
    for (int i = 0; i < spec.atlases; i++) {
        int index = 0;
        if (i > 0) {
            char name[64];
            if (snprintf(name, sizeof name, "atlas_%03d", i) < 0 ||
                tp_project_add_atlas(project, name, &index) != TP_STATUS_OK) {
                tp_project_destroy(project);
                return false;
            }
        }
        if (!fill_atlas(project, &project->atlases[index], &spec, &rng,
                        &err)) {
            tp_project_destroy(project);
            return false;
        }
    }
    if (tp_project_assign_missing_ids(project, &rng, &err) != TP_STATUS_OK) {
        tp_project_destroy(project);
        return false;
    }
    char *serialized = NULL;
    size_t serialized_len = 0U;
    if (tp_project_save_buffer(project, &serialized, &serialized_len, &err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "fixture serialization failed: %s error=%s\n",
                      spec.name, err.msg);
        tp_project_destroy(project);
        return false;
    }
    tp_project *roundtrip = NULL;
    if (tp_project_load_buffer(serialized, serialized_len, &roundtrip, &err) != TP_STATUS_OK || !roundtrip ||
        roundtrip->atlas_count != spec.atlases ||
        !tp_id128_eq(tp_semantic_identity(project), tp_semantic_identity(roundtrip))) {
        (void)fprintf(stderr, "fixture round-trip failed: %s error=%s\n", spec.name, err.msg);
        tp_project_destroy(roundtrip);
        free(serialized);
        tp_project_destroy(project);
        return false;
    }
    tp_project_destroy(roundtrip);
    free(serialized);
    out->project = project;
    out->serialized_bytes = serialized_len;
    return true;
}

static void fixture_free(fixture *f) {
    if (!f) {
        return;
    }
    tp_project_destroy(f->project);
    memset(f, 0, sizeof *f);
}

static void print_fixture(const fixture *f, int warmups, int iterations, int recovery_ops) {
    const fixture_spec *s = &f->spec;
    (void)printf("\nfixture=%s atlases=%d sources=%d overrides=%d animations=%d frames=%d "
                 "serialized_bytes=%zu warmups=%d iterations=%d recovery_ops=%d\n",
                 s->name, s->atlases, s->atlases * s->sources_per_atlas,
                 s->atlases * s->overrides_per_atlas, s->atlases * s->animations_per_atlas,
                 s->atlases * s->animations_per_atlas * s->frames_per_animation,
                 f->serialized_bytes, warmups, iterations, recovery_ops);
}

static void request_padding(tp_txn_request *request, tp_operation *operation,
                            const tp_model *model, tp_id128 atlas_id, uint64_t id, int padding) {
    memset(operation, 0, sizeof *operation);
    operation->kind = TP_OP_ATLAS_SETTINGS_SET;
    operation->atlas_id = atlas_id;
    operation->u.atlas_settings.mask = TP_AF_PADDING;
    operation->u.atlas_settings.padding = padding;
    memset(request, 0, sizeof *request);
    request->schema = TP_TXN_SCHEMA;
    (void)snprintf(request->id_hex, sizeof request->id_hex, "%032" PRIx64, id);
    request->expected_revision = tp_model_revision(model);
    request->label = (char *)"M0 benchmark edit";
    request->ops = operation;
    request->op_count = 1;
}

static bool padding_result_valid(tp_model *model, tp_id128 atlas_id, int padding, int64_t before,
                                 tp_status status, const tp_txn_result *result, const tp_error *err) {
    const tp_project *project = tp_model_project(model);
    const int atlas_index = project ? tp_project_find_atlas_by_id(project, atlas_id) : -1;
    bool ok = status == TP_STATUS_OK && result->committed && result->revision == before + 1 &&
              tp_model_revision(model) == before + 1 && atlas_index >= 0 &&
              project->atlases[atlas_index].padding == padding;
    if (!ok) {
        (void)fprintf(stderr,
                      "apply_padding failed: status=%s error=%s before=%" PRId64 " result=%" PRId64
                      " model=%" PRId64 " committed=%d\n",
                      tp_status_id(status), err->msg, before, result->revision, tp_model_revision(model),
                      result->committed ? 1 : 0);
    }
    return ok;
}

static bool apply_padding(tp_model *model, tp_id128 atlas_id, uint64_t id, int padding) {
    tp_operation operation;
    tp_txn_request request;
    request_padding(&request, &operation, model, atlas_id, id, padding);
    const int64_t before = tp_model_revision(model);
    tp_txn_result result = {0};
    tp_error err = {{0}};
    tp_status status = tp_model_apply(model, &request, &result, &err);
    const bool ok = padding_result_valid(model, atlas_id, padding, before, status, &result, &err);
    tp_txn_result_free(&result);
    return ok;
}

static void report_samples(const char *scenario, const fixture *f, tp_bench_samples *samples,
                           uint64_t count_a, const char *count_a_name) {
    (void)printf("scenario=%s fixture=%s samples_ms=[", scenario, f->spec.name);
    for (size_t i = 0; i < samples->count; i++) {
        (void)printf("%s%.6f", i == 0U ? "" : ",", samples->values[i]);
    }
    (void)printf("]\n");
    double p50 = tp_bench_samples_percentile(samples, 50U);
    double p95 = tp_bench_samples_percentile(samples, 95U);
    (void)printf("scenario=%s fixture=%s p50_ms=%.6f p95_ms=%.6f accepted=%zu failed=%zu %s=%" PRIu64 "\n",
                 scenario, f->spec.name, p50, p95, samples->count, samples->failed,
                 count_a_name, count_a);
}

static int run_project_load_scaling(int iterations) {
    const fixture_spec huge_spec = {"HUGE", 100, 2, 1000, 0, 0};
    fixture huge;
    if (!fixture_prepare(&huge, huge_spec)) {
        return 1;
    }
    char *huge_json = NULL;
    size_t huge_json_bytes = 0U;
    tp_error err = {{0}};
    if (tp_project_save_buffer(huge.project, &huge_json, &huge_json_bytes,
                               &err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "shipped HUGE serialization failed: %s\n",
                      err.msg);
        fixture_free(&huge);
        return 1;
    }
    fixture_free(&huge);
    const int result = tp_bench_project_load_run(
        iterations, huge_json, huge_json_bytes);
    free(huge_json);
    return result;
}

typedef struct recovery_read_probe {
    const uint8_t *source;
    size_t source_len;
    uint8_t *returned_buffer;
    size_t read_calls;
} recovery_read_probe;

static int64_t recovery_probe_write(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    (void)data;
    (void)len;
    return -1;
}

static int64_t recovery_probe_length(void *ctx) {
    const recovery_read_probe *probe = (const recovery_read_probe *)ctx;
    return probe->source_len <= (size_t)INT64_MAX ? (int64_t)probe->source_len : -1;
}

static int recovery_probe_truncate(void *ctx, size_t len) {
    (void)ctx;
    (void)len;
    return -1;
}

static int recovery_probe_read_all(void *ctx, size_t max_len, uint8_t **out,
                                   size_t *out_len) {
    recovery_read_probe *probe = (recovery_read_probe *)ctx;
    *out = NULL;
    *out_len = 0U;
    probe->read_calls++;
    if (probe->source_len > max_len) {
        return -1;
    }
    if (probe->source_len == 0U) {
        return 0;
    }
    uint8_t *buffer = (uint8_t *)malloc(probe->source_len);
    if (!buffer) {
        return -1;
    }
    memcpy(buffer, probe->source, probe->source_len);
    probe->returned_buffer = buffer;
    *out = buffer;
    *out_len = probe->source_len;
    return 0;
}

static void recovery_probe_destroy(void *ctx) {
    /* The benchmark retains the probe until after tp_model_recover returns so a
     * hard-fault sample can inspect the read contract without a dangling ctx. */
    (void)ctx;
}

static tp_journal_io recovery_probe_io(const uint8_t *source, size_t source_len,
                                       recovery_read_probe **out_probe) {
    tp_journal_io io;
    memset(&io, 0, sizeof io);
    recovery_read_probe *probe = (recovery_read_probe *)calloc(1, sizeof *probe);
    if (!probe) {
        *out_probe = NULL;
        return io;
    }
    probe->source = source;
    probe->source_len = source_len;
    io.ctx = probe;
    io.write = recovery_probe_write;
    io.length = recovery_probe_length;
    io.truncate = recovery_probe_truncate;
    io.read_all = recovery_probe_read_all;
    io.destroy = recovery_probe_destroy;
    *out_probe = probe;
    return io;
}

static bool bench_normal_transaction(const fixture *f, int warmups, int iterations) {
    tp_project *clone = tp_project_clone(f->project);
    tp_model *model = clone ? tp_model_wrap(clone) : NULL;
    if (!model) {
        tp_project_destroy(clone);
        return false;
    }
    tp_id128 atlas_id = tp_model_project(model)->atlases[f->spec.atlases - 1].id;
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    uint64_t max_clone_allocations = 0U;
    for (int i = 0; i < warmups + iterations; i++) {
        /* A timing sample must exercise a committed mutation: fixtures may start
         * at any valid padding, and no-change requests intentionally do not commit. */
        const int current_padding =
            tp_model_project(model)->atlases[f->spec.atlases - 1].padding;
        const int padding = current_padding == 3 ? 4 : 3;
        tp_operation operation;
        tp_txn_request request;
        request_padding(&request, &operation, model, atlas_id, (uint64_t)i + 1U, padding);
        const int64_t before = tp_model_revision(model);
        tp_txn_result result = {0};
        tp_error err = {{0}};
        tp_project__test_set_clone_alloc_fail(-1);
        double start = tp_bench_now_ms();
        tp_status status = tp_model_apply(model, &request, &result, &err);
        double elapsed = tp_bench_now_ms() - start;
        bool ok = padding_result_valid(model, atlas_id, padding, before, status, &result, &err);
        uint64_t allocations = (uint64_t)tp_project__test_clone_alloc_count();
        tp_txn_result_free(&result);
        if (allocations > max_clone_allocations) {
            max_clone_allocations = allocations;
        }
        if (!ok) {
            if (i >= warmups) {
                (void)tp_bench_samples_record(&samples, false, elapsed);
            }
            tp_model_destroy(model);
            return false;
        }
        if (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed)) {
            tp_model_destroy(model);
            return false;
        }
    }
    tp_model_destroy(model);
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    report_samples("normal_transaction", f, &samples, max_clone_allocations, "clone_allocations_max");
    return true;
}

/* Snapshot timing is accepted only after the complete immutable DTO graph has
 * been checked outside the timed region. Atlas count alone would let a broken
 * shallow snapshot look artificially fast. */
static bool nullable_string_equal(const char *a, const char *b) {
    return (!a && !b) || (a && b && strcmp(a, b) == 0);
}

static bool snapshot_shape_valid(const tp_session_snapshot *snapshot,
                                 const fixture *f) {
    if (!snapshot || tp_session_snapshot_atlas_count(snapshot) != f->spec.atlases ||
        !tp_id128_eq(tp_session_snapshot_semantic_identity(snapshot),
                     tp_semantic_identity(f->project))) {
        return false;
    }
    for (int ai = 0; ai < f->spec.atlases; ++ai) {
        const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, ai);
        const tp_project_atlas *model_atlas = &f->project->atlases[ai];
        if (!atlas || atlas->source_count != f->spec.sources_per_atlas ||
            atlas->sprite_count != f->spec.overrides_per_atlas ||
            atlas->animation_count != f->spec.animations_per_atlas ||
            atlas->target_count != 1 ||
            !tp_id128_eq(atlas->id, model_atlas->id) ||
            !nullable_string_equal(atlas->name, model_atlas->name) ||
            atlas->max_size != model_atlas->max_size ||
            atlas->padding != model_atlas->padding ||
            atlas->margin != model_atlas->margin ||
            atlas->extrude != model_atlas->extrude ||
            atlas->alpha_threshold != model_atlas->alpha_threshold ||
            atlas->max_vertices != model_atlas->max_vertices ||
            atlas->shape != model_atlas->shape ||
            atlas->allow_transform != model_atlas->allow_transform ||
            atlas->power_of_two != model_atlas->power_of_two ||
            atlas->pixels_per_unit != model_atlas->pixels_per_unit) {
            return false;
        }
        for (int si = 0; si < atlas->source_count; ++si) {
            const tp_snapshot_source *dto =
                tp_session_snapshot_source_at(snapshot, atlas->id, si);
            const tp_project_source *model = &model_atlas->sources[si];
            if (!dto || !tp_id128_eq(dto->id, model->id) ||
                dto->kind != (tp_snapshot_source_kind)model->kind ||
                !nullable_string_equal(dto->path, model->path)) {
                return false;
            }
        }
        for (int si = 0; si < atlas->sprite_count; ++si) {
            const tp_snapshot_sprite *dto =
                tp_session_snapshot_sprite_at_index(snapshot, ai, si);
            const tp_project_sprite *model = &model_atlas->sprites[si];
            const tp_id128 expected_id =
                !tp_id128_is_nil(model->source_ref) && model->src_key
                    ? tp_sprite_id(model->source_ref, model->src_key)
                    : tp_id128_nil();
            if (!dto || !tp_id128_eq(dto->id, expected_id) ||
                !tp_id128_eq(dto->source_id, model->source_ref) ||
                !nullable_string_equal(dto->source_key, model->src_key) ||
                !nullable_string_equal(dto->name, model->name) ||
                dto->origin_x != model->origin_x ||
                dto->origin_y != model->origin_y ||
                memcmp(dto->slice9_lrtb, model->slice9_lrtb,
                       sizeof dto->slice9_lrtb) != 0 ||
                !nullable_string_equal(dto->rename, model->rename) ||
                dto->override_shape != model->ov_shape ||
                dto->override_allow_rotate != model->ov_allow_rotate ||
                dto->override_max_vertices != model->ov_max_vertices ||
                dto->override_margin != model->ov_margin ||
                dto->override_extrude != model->ov_extrude) {
                return false;
            }
        }
        for (int ni = 0; ni < atlas->animation_count; ++ni) {
            const tp_snapshot_animation *animation =
                tp_session_snapshot_animation_at(snapshot, atlas->id, ni);
            const tp_project_anim *model_animation =
                &model_atlas->animations[ni];
            if (!animation || animation->frame_count != f->spec.frames_per_animation ||
                !tp_id128_eq(animation->id, model_animation->id) ||
                !nullable_string_equal(animation->name, model_animation->name) ||
                animation->fps != model_animation->fps ||
                animation->playback != model_animation->playback ||
                animation->flip_h != model_animation->flip_h ||
                animation->flip_v != model_animation->flip_v) {
                return false;
            }
            for (int fi = 0; fi < animation->frame_count; ++fi) {
                const tp_snapshot_frame *dto =
                    tp_session_snapshot_animation_frame_at(
                        snapshot, atlas->id, animation->id, fi);
                const tp_project_frame *model = &model_animation->frames[fi];
                const tp_id128 expected_id =
                    !tp_id128_is_nil(model->source_ref) && model->src_key
                        ? tp_sprite_id(model->source_ref, model->src_key)
                        : tp_id128_nil();
                if (!dto || !tp_id128_eq(dto->sprite_id, expected_id) ||
                    !tp_id128_eq(dto->source_id, model->source_ref) ||
                    !nullable_string_equal(dto->source_key, model->src_key) ||
                    !nullable_string_equal(dto->name, model->name)) {
                    return false;
                }
            }
        }
        for (int ti = 0; ti < atlas->target_count; ++ti) {
            const tp_snapshot_target *dto =
                tp_session_snapshot_target_at(snapshot, atlas->id, ti);
            const tp_project_target *model = &model_atlas->targets[ti];
            if (!dto || !tp_id128_eq(dto->id, model->id) ||
                !nullable_string_equal(dto->exporter_id, model->exporter_id) ||
                !nullable_string_equal(dto->out_path, model->out_path) ||
                dto->enabled != model->enabled) {
                return false;
            }
        }
    }
    return true;
}

static bool bench_session_snapshot(const fixture *f, int warmups, int iterations) {
    tp_project *project = tp_project_clone(f->project);
    uint64_t rng_counter = 1000000U;
    tp_rng rng = {deterministic_rng, &rng_counter};
    tp_session *session = NULL;
    tp_error err = {{0}};
    if (!project || tp_session_adopt_owned(project, &rng, &session, &err) != TP_STATUS_OK) {
        return false;
    }
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    uint64_t max_clone_allocations = 0U;
    size_t max_clone_bytes = 0U;
    size_t max_snapshot_allocations = 0U;
    size_t max_snapshot_dto_bytes = 0U;
    size_t max_snapshot_total_bytes = 0U;
    for (int i = 0; i < warmups + iterations; ++i) {
        tp_project__test_set_clone_alloc_fail(-1);
        tp_session__test_reset_snapshot_allocations();
        tp_session_snapshot *snapshot = NULL;
        double start = tp_bench_now_ms();
        tp_status status = tp_session_snapshot_create(session, &snapshot, &err);
        double elapsed = tp_bench_now_ms() - start;
        const uint64_t clone_allocations = (uint64_t)tp_project__test_clone_alloc_count();
        const size_t clone_bytes = tp_project__test_clone_allocation_bytes();
        const size_t snapshot_allocations = tp_session__test_snapshot_allocation_count();
        const size_t snapshot_bytes = tp_session__test_snapshot_allocation_bytes();
        const bool ok = status == TP_STATUS_OK && snapshot_shape_valid(snapshot, f);
        tp_session_snapshot_destroy(snapshot);
        if (clone_allocations > max_clone_allocations) {
            max_clone_allocations = clone_allocations;
        }
        if (clone_bytes > max_clone_bytes) {
            max_clone_bytes = clone_bytes;
        }
        if (snapshot_allocations > max_snapshot_allocations) {
            max_snapshot_allocations = snapshot_allocations;
        }
        if (snapshot_bytes > max_snapshot_dto_bytes) {
            max_snapshot_dto_bytes = snapshot_bytes;
        }
        if (clone_bytes <= SIZE_MAX - snapshot_bytes &&
            clone_bytes + snapshot_bytes > max_snapshot_total_bytes) {
            max_snapshot_total_bytes = clone_bytes + snapshot_bytes;
        }
        if (!ok || (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed))) {
            tp_session_destroy(session);
            return false;
        }
    }
    tp_session_destroy(session);
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    report_samples("session_snapshot", f, &samples, max_clone_allocations,
                   "project_clone_allocations_max");
    (void)printf("scenario=session_snapshot fixture=%s snapshot_dto_allocations_max=%zu "
                 "snapshot_dto_live_bytes_max=%zu project_clone_live_bytes_max=%zu "
                 "snapshot_total_live_bytes_max=%zu\n",
                 f->spec.name, max_snapshot_allocations, max_snapshot_dto_bytes,
                 max_clone_bytes, max_snapshot_total_bytes);
    return true;
}

typedef struct history_sample {
    tp_model *model;
    tp_journal_io io;
    char path[768];
} history_sample;

static tp_id128 journal_key(void) {
    tp_id128 key = {{0}};
    key.bytes[15] = 1U;
    return key;
}

static bool history_sample_prepare(const fixture *f, const char *scratch, bool prepare_redo,
                                   uint64_t id, int ordinal, history_sample *out) {
    memset(out, 0, sizeof *out);
    const int path_n = snprintf(out->path, sizeof out->path, "%s/%s_history_%s_%d_%d.tmp", scratch,
                                f->spec.name, prepare_redo ? "redo" : "undo", process_id(), ordinal);
    if (path_n < 0 || (size_t)path_n >= sizeof out->path) {
        return false;
    }
    (void)remove(out->path);
    tp_project *clone = tp_project_clone(f->project);
    tp_model *model = clone ? tp_model_wrap(clone) : NULL;
    if (!model) {
        tp_project_destroy(clone);
        return false;
    }
    if (tp_model_enable_history(model) != TP_STATUS_OK) {
        (void)fprintf(stderr, "history setup failed: enable_history fixture=%s\n", f->spec.name);
        tp_model_destroy(model);
        (void)remove(out->path);
        return false;
    }
    tp_journal_io io = tp_journal_io_file(out->path);
    tp_journal *journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    tp_error err = {{0}};
    const tp_status attach_status = journal
                                        ? tp_model_attach_journal(model, journal, &err)
                                        : TP_STATUS_OOM;
    if (attach_status != TP_STATUS_OK) {
        (void)fprintf(stderr,
                      "history setup failed: attach_journal fixture=%s status=%s error=%s\n",
                      f->spec.name, tp_status_id(attach_status), err.msg);
        if (journal) {
            tp_journal_destroy(journal);
        }
        tp_model_destroy(model);
        (void)remove(out->path);
        return false;
    }
    tp_id128 atlas_id = tp_model_project(model)->atlases[f->spec.atlases - 1].id;
    if (!apply_padding(model, atlas_id, id, 7) || tp_model_undo_depth(model) != 1) {
        (void)fprintf(stderr, "history setup failed: initial apply fixture=%s undo_depth=%d\n", f->spec.name,
                      tp_model_undo_depth(model));
        tp_model_destroy(model);
        (void)remove(out->path);
        return false;
    }
    if (prepare_redo && (tp_model_undo(model, &err) != TP_STATUS_OK || tp_model_redo_depth(model) != 1)) {
        (void)fprintf(stderr, "history setup failed: prepare redo fixture=%s error=%s redo_depth=%d\n",
                      f->spec.name, err.msg, tp_model_redo_depth(model));
        tp_model_destroy(model);
        (void)remove(out->path);
        return false;
    }
    out->model = model;
    out->io = io;
    return true;
}

static bool bench_history(const fixture *f, const char *scratch, int warmups, int iterations, bool redo) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    uint64_t max_append_bytes = 0U;
    for (int i = 0; i < warmups + iterations; i++) {
        history_sample sample;
        if (!history_sample_prepare(f, scratch, redo, (uint64_t)i + 1000U, i, &sample)) {
            (void)fprintf(stderr, "history sample prepare failed: scenario=%s fixture=%s iteration=%d\n",
                          redo ? "journal_redo" : "journal_undo", f->spec.name, i);
            return false;
        }
        int64_t before_len = sample.io.length(sample.io.ctx);
        int64_t before_revision = tp_model_revision(sample.model);
        tp_error err = {{0}};
        double start = tp_bench_now_ms();
        tp_status status = redo ? tp_model_redo(sample.model, &err) : tp_model_undo(sample.model, &err);
        double elapsed = tp_bench_now_ms() - start;
        int64_t after_len = sample.io.length(sample.io.ctx);
        int64_t after_revision = tp_model_revision(sample.model);
        const int expected_padding = redo ? 7 : f->project->atlases[f->spec.atlases - 1].padding;
        bool ok = before_len >= 0 && after_len >= 0 && status == TP_STATUS_OK &&
                  after_revision == before_revision + 1 &&
                  (redo ? tp_model_undo_depth(sample.model) == 1 : tp_model_redo_depth(sample.model) == 1) &&
                  tp_model_project(sample.model)->atlases[f->spec.atlases - 1].padding == expected_padding &&
                  after_len > before_len;
        uint64_t append_bytes = ok ? (uint64_t)(after_len - before_len) : 0U;
        if (append_bytes > max_append_bytes) {
            max_append_bytes = append_bytes;
        }
        tp_model_destroy(sample.model);
        (void)remove(sample.path);
        if (!ok) {
            (void)fprintf(stderr,
                          "history sample failed: scenario=%s fixture=%s iteration=%d status=%s error=%s "
                          "before_len=%" PRId64 " after_len=%" PRId64 " before_revision=%" PRId64
                          " after_revision=%" PRId64 "\n",
                          redo ? "journal_redo" : "journal_undo", f->spec.name, i, tp_status_id(status),
                          err.msg, before_len, after_len, before_revision, after_revision);
            if (i >= warmups) {
                (void)tp_bench_samples_record(&samples, false, elapsed);
            }
            return false;
        }
        if (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed)) {
            return false;
        }
    }
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    report_samples(redo ? "journal_redo" : "journal_undo", f, &samples,
                   max_append_bytes, "append_bytes_max");
    return true;
}

static bool make_path(char *out, size_t cap, const char *root, const fixture *f,
                      const char *scenario, int ordinal) {
    int n = snprintf(out, cap, "%s/%s_%s_%d_%d.tmp", root, f->spec.name, scenario, process_id(), ordinal);
    return n >= 0 && (size_t)n < cap;
}

static bool copy_file(const char *source, const char *destination) {
    FILE *in = fopen(source, "rb");
    FILE *out = in ? fopen(destination, "wb") : NULL;
    if (!in || !out) {
        if (in) {
            (void)fclose(in);
        }
        if (out) {
            (void)fclose(out);
        }
        return false;
    }
    bool ok = true;
    uint8_t buffer[65536];
    while (!feof(in)) {
        size_t read = fread(buffer, 1U, sizeof buffer, in);
        if (read > 0U && fwrite(buffer, 1U, read, out) != read) {
            ok = false;
            break;
        }
        if (ferror(in)) {
            ok = false;
            break;
        }
    }
    const int flush_result = fflush(out);
    const int close_result = fclose(out);
    if (flush_result != 0 || close_result != 0) {
        ok = false;
    }
    if (fclose(in) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)remove(destination);
    }
    return ok;
}

static int64_t file_size(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file || fseek(file, 0L, SEEK_END) != 0) {
        if (file) {
            (void)fclose(file);
        }
        return -1;
    }
    long value = ftell(file);
    (void)fclose(file);
    return value < 0L ? -1 : (int64_t)value;
}

static bool recovery_baseline_create(const fixture *f, const char *path, int recovery_ops,
                                     tp_id128 *out_identity, int64_t *out_revision) {
    (void)remove(path);
    tp_project *clone = tp_project_clone(f->project);
    tp_model *model = clone ? tp_model_wrap(clone) : NULL;
    if (!model) {
        tp_project_destroy(clone);
        return false;
    }
    tp_journal_io io = tp_journal_io_file(path);
    tp_journal *journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    tp_error err = {{0}};
    if (!journal || tp_model_attach_journal(model, journal, &err) != TP_STATUS_OK) {
        if (journal) {
            tp_journal_destroy(journal);
        }
        tp_model_destroy(model);
        (void)remove(path);
        return false;
    }
    tp_id128 atlas_id = tp_model_project(model)->atlases[f->spec.atlases - 1].id;
    for (int i = 0; i < recovery_ops; i++) {
        /* Preserve one durable record per requested recovery operation under the
         * no-change contract instead of assuming the fixture's initial padding. */
        const int current_padding =
            tp_model_project(model)->atlases[f->spec.atlases - 1].padding;
        const int padding = current_padding == 3 ? 4 : 3;
        if (!apply_padding(model, atlas_id, (uint64_t)i + 2000U, padding)) {
            tp_model_destroy(model);
            (void)remove(path);
            return false;
        }
    }
    *out_identity = tp_semantic_identity(tp_model_project(model));
    *out_revision = tp_model_revision(model);
    tp_model_destroy(model);
    if (file_size(path) <= 0) {
        (void)remove(path);
        return false;
    }
    return true;
}

static bool bench_recovery(const fixture *f, const char *scratch, int warmups,
                           int iterations, int recovery_ops) {
    char baseline[768];
    tp_id128 expected_identity = tp_id128_nil();
    int64_t expected_revision = 0;
    if (!make_path(baseline, sizeof baseline, scratch, f, "recovery_base", 0) ||
        !recovery_baseline_create(f, baseline, recovery_ops, &expected_identity, &expected_revision)) {
        return false;
    }
    int64_t baseline_bytes = file_size(baseline);
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    tp_journal_recovery_copy_stats copies_max = {0};
    for (int i = 0; i < warmups + iterations; i++) {
        char sample_path[768];
        if (!make_path(sample_path, sizeof sample_path, scratch, f, "recovery_sample", i) ||
            !copy_file(baseline, sample_path)) {
            (void)remove(baseline);
            return false;
        }
        tp_journal_io io = tp_journal_io_file(sample_path);
        tp_model *recovered = NULL;
        tp_journal_recovery info = {0};
        tp_error err = {{0}};
        double start = tp_bench_now_ms();
        tp_status status = io.ctx ? tp_model_recover(io, journal_key(), &recovered, &info, &err)
                                  : TP_STATUS_BAD_PROJECT;
        double elapsed = tp_bench_now_ms() - start;
        tp_journal_recovery_copy_stats copies = {0};
        tp_journal__test_recovery_copy_stats(&info, &copies);
        if (copies.raw_storage_copies > copies_max.raw_storage_copies) {
            copies_max.raw_storage_copies = copies.raw_storage_copies;
        }
        if (copies.raw_storage_bytes > copies_max.raw_storage_bytes) {
            copies_max.raw_storage_bytes = copies.raw_storage_bytes;
        }
        if (copies.checkpoint_payload_copies > copies_max.checkpoint_payload_copies) {
            copies_max.checkpoint_payload_copies = copies.checkpoint_payload_copies;
        }
        if (copies.checkpoint_payload_bytes > copies_max.checkpoint_payload_bytes) {
            copies_max.checkpoint_payload_bytes = copies.checkpoint_payload_bytes;
        }
        if (copies.operation_payload_copies > copies_max.operation_payload_copies) {
            copies_max.operation_payload_copies = copies.operation_payload_copies;
        }
        if (copies.operation_payload_bytes > copies_max.operation_payload_bytes) {
            copies_max.operation_payload_bytes = copies.operation_payload_bytes;
        }
        bool ok = status == TP_STATUS_OK && recovered && info.status == TP_JOURNAL_RECOVERY_OK &&
                  !info.mid_stream_corrupt && info.records_recovered == recovery_ops + 1 &&
                  info.revision == expected_revision && tp_model_revision(recovered) == expected_revision &&
                  info.op_count == (size_t)recovery_ops &&
                  copies.raw_storage_copies == 1U &&
                  copies.raw_storage_bytes == info.bytes_total &&
                  copies.checkpoint_payload_copies == 0U &&
                  copies.checkpoint_payload_bytes == 0U &&
                  copies.operation_payload_copies == 0U &&
                  copies.operation_payload_bytes == 0U &&
                  tp_journal__test_recovery_ops_borrow_raw(&info) &&
                  tp_id128_eq(tp_semantic_identity(tp_model_project(recovered)), expected_identity);
        if (!io.ctx) {
            memset(&info, 0, sizeof info);
        }
        tp_journal_recovery_free(&info);
        tp_model_destroy(recovered);
        (void)remove(sample_path);
        if (!ok) {
            if (i >= warmups) {
                (void)tp_bench_samples_record(&samples, false, elapsed);
            }
            (void)remove(baseline);
            return false;
        }
        if (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed)) {
            (void)remove(baseline);
            return false;
        }
    }
    (void)remove(baseline);
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    report_samples("recovery", f, &samples, (uint64_t)baseline_bytes, "journal_bytes");
    (void)fprintf(stdout,
                  "benchmark_evidence scenario=recovery fixture=%s raw_storage_copies_max=%zu "
                  "raw_storage_bytes_max=%zu checkpoint_payload_copies_max=%zu "
                  "checkpoint_payload_bytes_max=%zu op_payload_copies_max=%zu "
                  "op_payload_bytes_max=%zu\n",
                  f->spec.name, copies_max.raw_storage_copies,
                  copies_max.raw_storage_bytes, copies_max.checkpoint_payload_copies,
                  copies_max.checkpoint_payload_bytes, copies_max.operation_payload_copies,
                  copies_max.operation_payload_bytes);
    return true;
}

typedef struct recovery_scaling_baseline {
    uint8_t *bytes;
    size_t byte_count;
    size_t checkpoint_bytes;
    size_t payload_bytes;
    int record_count;
    int operations_per_payload;
    size_t total_frame_count;
    tp_id128 expected_identity;
} recovery_scaling_baseline;

static void recovery_scaling_baseline_free(recovery_scaling_baseline *baseline) {
    free(baseline->bytes);
    memset(baseline, 0, sizeof *baseline);
}

/* `requested_operations == 0` fills exactly to the writer's recoverable 64 MiB
 * boundary and reports the resulting maximum count for this canonical payload. */
static bool recovery_scaling_baseline_create(const fixture *f, int requested_operations,
                                             recovery_scaling_baseline *out) {
    memset(out, 0, sizeof *out);
    char *snapshot = NULL;
    size_t snapshot_len = 0U;
    tp_error err = {{0}};
    if (tp_project_save_buffer(f->project, &snapshot, &snapshot_len, &err) != TP_STATUS_OK) {
        return false;
    }

    tp_project *expected_project = tp_project_clone(f->project);
    tp_model *expected_model = expected_project ? tp_model_wrap(expected_project) : NULL;
    if (!expected_model) {
        tp_project_destroy(expected_project);
        free(snapshot);
        return false;
    }
    const tp_id128 atlas_id = tp_model_project(expected_model)->atlases[f->spec.atlases - 1].id;
    tp_operation operation;
    tp_txn_request request;
    request_padding(&request, &operation, expected_model, atlas_id, 0x5ca1eU, 7);
    char *payload = tp_txn_request_encode(&request);
    if (!payload || !apply_padding(expected_model, atlas_id, 0x5ca1eU, 7)) {
        free(payload);
        tp_model_destroy(expected_model);
        free(snapshot);
        return false;
    }

    tp_journal_io io = tp_journal_io_memory();
    tp_journal *journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    if (!journal || tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot,
                                               snapshot_len, 0, &err) != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        free(payload);
        tp_model_destroy(expected_model);
        free(snapshot);
        return false;
    }
    free(snapshot);

    const size_t payload_len = strlen(payload);
    int count = 0;
    for (;;) {
        if (requested_operations > 0 && count >= requested_operations) {
            break;
        }
        char id_hex[33];
        (void)snprintf(id_hex, sizeof id_hex, "%032" PRIx64, (uint64_t)count + 1U);
        memset(&err, 0, sizeof err);
        tp_status status = tp_journal_append_txn_counted(
            journal, id_hex, (int64_t)count + 1, (const uint8_t *)payload,
            payload_len, 1U, &err);
        if (status != TP_STATUS_OK) {
            if (requested_operations == 0 && status == TP_STATUS_JOURNAL_FAILED &&
                strstr(err.msg, "file-size limit") != NULL && count > 0) {
                break;
            }
            tp_journal_destroy(journal);
            free(payload);
            tp_model_destroy(expected_model);
            return false;
        }
        count++;
    }
    free(payload);

    if (io.read_all(io.ctx, SIZE_MAX, &out->bytes, &out->byte_count) != 0 || !out->bytes) {
        tp_journal_destroy(journal);
        tp_model_destroy(expected_model);
        recovery_scaling_baseline_free(out);
        return false;
    }
    out->checkpoint_bytes = snapshot_len;
    out->payload_bytes = payload_len;
    out->record_count = count;
    out->operations_per_payload = 1;
    out->total_frame_count = (size_t)count + 1U;
    out->expected_identity = tp_semantic_identity(tp_model_project(expected_model));
    tp_journal_destroy(journal);
    tp_model_destroy(expected_model);
    return out->byte_count <= (size_t)TP_JOURNAL_MAX_FILE_BYTES;
}

/* Accepted records at both transaction admission maxima: exactly the maximum
 * canonical request bytes and exactly TP_TXN_MAX_OPS operations. `fill_file` repeats
 * that padded payload to the writer's 64 MiB boundary. The single-record shape is a
 * component boundary; the full-file shape is deliberately kept separate from the
 * operation-density maximum below. */
static bool recovery_max_payload_baseline_create(const fixture *f, bool fill_file,
                                                 recovery_scaling_baseline *out) {
    memset(out, 0, sizeof *out);
    bool ok = false;
    char *snapshot = NULL;
    size_t snapshot_len = 0U;
    tp_project *expected_project = NULL;
    tp_model *expected_model = NULL;
    tp_operation *operations = NULL;
    char *label = NULL;
    char *payload = NULL;
    tp_journal *journal = NULL;
    tp_error err = {{0}};
    const char *stage = "save_checkpoint";
    size_t empty_label_bytes = 0U;
    size_t label_field_overhead = 0U;
    size_t encoded_payload_bytes = 0U;

    if (tp_project_save_buffer(f->project, &snapshot, &snapshot_len, &err) != TP_STATUS_OK) {
        goto cleanup;
    }
    stage = "clone_model";
    expected_project = tp_project_clone(f->project);
    expected_model = expected_project ? tp_model_wrap(expected_project) : NULL;
    if (!expected_model) {
        tp_project_destroy(expected_project);
        expected_project = NULL;
        goto cleanup;
    }
    expected_project = NULL; /* owned by expected_model */
    stage = "allocate_operations";
    const tp_id128 atlas_id = tp_model_project(expected_model)->atlases[f->spec.atlases - 1].id;
    operations = (tp_operation *)calloc((size_t)TP_TXN_MAX_OPS, sizeof *operations);
    if (!operations) {
        goto cleanup;
    }
    tp_operation operation;
    tp_txn_request request;
    request_padding(&request, &operation, expected_model, atlas_id, UINT64_C(0xffff), 7);
    for (int i = 0; i < TP_TXN_MAX_OPS; ++i) {
        operations[i] = operation;
    }
    request.ops = operations;
    request.op_count = TP_TXN_MAX_OPS;
    request.label = (char *)"";
    stage = "encode_empty_label";
    char *empty_label_payload = tp_txn_request_encode(&request);
    if (!empty_label_payload) {
        goto cleanup;
    }
    empty_label_bytes = strlen(empty_label_payload);
    free(empty_label_payload);
    request.label = (char *)"x";
    char *one_byte_label_payload = tp_txn_request_encode(&request);
    if (!one_byte_label_payload) {
        goto cleanup;
    }
    const size_t one_byte_label_bytes = strlen(one_byte_label_payload);
    free(one_byte_label_payload);
    if (one_byte_label_bytes <= empty_label_bytes + 1U) {
        goto cleanup;
    }
    label_field_overhead = one_byte_label_bytes - empty_label_bytes - 1U;
    if (empty_label_bytes + label_field_overhead > (size_t)TP_TXN_MAX_REQUEST_BYTES) {
        goto cleanup;
    }
    const size_t label_bytes = (size_t)TP_TXN_MAX_REQUEST_BYTES - empty_label_bytes -
                               label_field_overhead;
    label = (char *)malloc(label_bytes + 1U);
    if (!label) {
        goto cleanup;
    }
    memset(label, 'x', label_bytes);
    label[label_bytes] = '\0';
    request.label = label;
    stage = "encode_exact_payload";
    payload = tp_txn_request_encode(&request);
    encoded_payload_bytes = payload ? strlen(payload) : 0U;
    if (!payload || encoded_payload_bytes != (size_t)TP_TXN_MAX_REQUEST_BYTES) {
        goto cleanup;
    }
    tp_txn_request *decoded = NULL;
    stage = "decode_exact_payload";
    if (tp_txn_request_decode_n(payload, (size_t)TP_TXN_MAX_REQUEST_BYTES,
                                &decoded, &err) != TP_STATUS_OK || !decoded ||
        decoded->op_count != TP_TXN_MAX_OPS) {
        tp_txn_request_free(decoded);
        goto cleanup;
    }
    tp_txn_request_free(decoded);
    stage = "apply_expected_model";
    if (!apply_padding(expected_model, atlas_id, UINT64_C(0x10000), 7)) {
        goto cleanup;
    }

    tp_journal_io io = tp_journal_io_memory();
    stage = "write_journal";
    journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    if (!journal ||
        tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot, snapshot_len,
                                   0, &err) != TP_STATUS_OK) {
        goto cleanup;
    }
    int record_count = 0;
    for (;;) {
        char record_id[33];
        (void)snprintf(record_id, sizeof record_id, "%032" PRIx64,
                       (uint64_t)record_count + UINT64_C(0x100000));
        memset(&err, 0, sizeof err);
        const tp_status append_status = tp_journal_append_txn_counted(
            journal, record_id, (int64_t)record_count + 1, (const uint8_t *)payload,
            (size_t)TP_TXN_MAX_REQUEST_BYTES, (size_t)TP_TXN_MAX_OPS, &err);
        if (append_status != TP_STATUS_OK) {
            if (fill_file && record_count > 0 && append_status == TP_STATUS_JOURNAL_FAILED &&
                strstr(err.msg, "file-size limit") != NULL) {
                break;
            }
            goto cleanup;
        }
        record_count++;
        if (!fill_file) {
            break;
        }
    }
    if (io.read_all(io.ctx, SIZE_MAX, &out->bytes, &out->byte_count) != 0 || !out->bytes) {
        goto cleanup;
    }
    out->checkpoint_bytes = snapshot_len;
    out->payload_bytes = (size_t)TP_TXN_MAX_REQUEST_BYTES;
    out->record_count = record_count;
    out->operations_per_payload = TP_TXN_MAX_OPS;
    out->total_frame_count = (size_t)record_count + 1U;
    out->expected_identity = tp_semantic_identity(tp_model_project(expected_model));
    ok = out->byte_count <= (size_t)TP_JOURNAL_MAX_FILE_BYTES;

cleanup:
    tp_journal_destroy(journal);
    free(payload);
    free(label);
    free(operations);
    tp_model_destroy(expected_model);
    tp_project_destroy(expected_project);
    free(snapshot);
    if (!ok) {
        (void)fprintf(stderr,
                      "max payload baseline failed stage=%s error=%s empty_payload_bytes=%zu "
                      "label_field_overhead=%zu encoded_payload_bytes=%zu journal_bytes=%zu\n",
                      stage, err.msg, empty_label_bytes, label_field_overhead,
                      encoded_payload_bytes, out->byte_count);
        recovery_scaling_baseline_free(out);
    }
    return ok;
}

/* True maximum-operation-density recovery shape: the shortest catalog operation
 * that can be replayed repeatedly without growing the model is atlas.rename. Alternate
 * two one-byte names so all TP_TXN_MAX_OPS applications are real, append without
 * label padding to the aggregate replay-operation boundary, and retain the exact
 * operation count for the timed recovery assertion. The replay-reference bound
 * counts transaction records, not operations inside a transaction; both dimensions
 * are checked explicitly. */
static bool recovery_max_op_density_baseline_create(const fixture *f,
                                                    recovery_scaling_baseline *out) {
    memset(out, 0, sizeof *out);
    bool ok = false;
    char *snapshot = NULL;
    size_t snapshot_len = 0U;
    tp_project *expected_project = NULL;
    tp_operation *operations = NULL;
    char *payload = NULL;
    tp_journal *journal = NULL;
    tp_error err = {{0}};
    const char *stage = "save_checkpoint";

    if (tp_project_save_buffer(f->project, &snapshot, &snapshot_len, &err) != TP_STATUS_OK) {
        goto cleanup;
    }
    stage = "clone_project";
    expected_project = tp_project_clone(f->project);
    if (!expected_project) {
        goto cleanup;
    }

    stage = "allocate_operations";
    operations = (tp_operation *)calloc((size_t)TP_TXN_MAX_OPS, sizeof *operations);
    if (!operations) {
        goto cleanup;
    }
    const tp_id128 atlas_id =
        expected_project->atlases[f->spec.atlases - 1].id;
    for (int i = 0; i < TP_TXN_MAX_OPS; ++i) {
        operations[i].kind = TP_OP_ATLAS_RENAME;
        operations[i].atlas_id = atlas_id;
        operations[i].u.atlas_rename.name = (char *)((i & 1) == 0 ? "a" : "b");
    }
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%032" PRIx64,
                   UINT64_C(0xd31517));
    request.expected_revision = 0;
    request.ops = operations;
    request.op_count = TP_TXN_MAX_OPS;

    stage = "encode_dense_payload";
    payload = tp_txn_request_encode(&request);
    const size_t payload_bytes = payload ? strlen(payload) : 0U;
    if (!payload || payload_bytes == 0U ||
        payload_bytes >= (size_t)TP_TXN_MAX_REQUEST_BYTES) {
        goto cleanup;
    }
    tp_txn_request *decoded = NULL;
    stage = "decode_dense_payload";
    if (tp_txn_request_decode_n(payload, payload_bytes, &decoded, &err) != TP_STATUS_OK ||
        !decoded || decoded->op_count != TP_TXN_MAX_OPS) {
        tp_txn_request_free(decoded);
        goto cleanup;
    }
    tp_txn_request_free(decoded);

    stage = "apply_expected_project";
    for (int i = 0; i < TP_TXN_MAX_OPS; ++i) {
        tp_op_reject reject;
        memset(&reject, 0, sizeof reject);
        if (tp_operation_apply(expected_project, &operations[i], &reject) !=
            TP_STATUS_OK) {
            goto cleanup;
        }
    }

    tp_journal_io io = tp_journal_io_memory();
    stage = "write_journal";
    journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    if (!journal ||
        tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot, snapshot_len,
                                   0, &err) != TP_STATUS_OK) {
        goto cleanup;
    }
    int record_count = 0;
    for (;;) {
        char record_id[33];
        (void)snprintf(record_id, sizeof record_id, "%032" PRIx64,
                       (uint64_t)record_count + UINT64_C(0x200000));
        memset(&err, 0, sizeof err);
        const tp_status append_status = tp_journal_append_txn_counted(
            journal, record_id, (int64_t)record_count + 1,
            (const uint8_t *)payload, payload_bytes, (size_t)TP_TXN_MAX_OPS, &err);
        if (append_status != TP_STATUS_OK) {
            if (record_count > 0 && append_status == TP_STATUS_OUT_OF_BOUNDS &&
                strstr(err.msg, "replay operation limit") != NULL) {
                break;
            }
            goto cleanup;
        }
        record_count++;
    }
    const size_t aggregate_operations =
        (size_t)record_count * (size_t)TP_TXN_MAX_OPS;
    if (record_count <= 0 ||
        (size_t)record_count > (size_t)TP_JOURNAL_MAX_REPLAY_RECORDS ||
        aggregate_operations != (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS ||
        io.read_all(io.ctx, SIZE_MAX, &out->bytes, &out->byte_count) != 0 || !out->bytes) {
        goto cleanup;
    }
    out->checkpoint_bytes = snapshot_len;
    out->payload_bytes = payload_bytes;
    out->record_count = record_count;
    out->operations_per_payload = TP_TXN_MAX_OPS;
    out->total_frame_count = (size_t)record_count + 1U;
    out->expected_identity = tp_semantic_identity(expected_project);
    ok = out->byte_count <= (size_t)TP_JOURNAL_MAX_FILE_BYTES;

cleanup:
    tp_journal_destroy(journal);
    free(payload);
    free(operations);
    tp_project_destroy(expected_project);
    free(snapshot);
    if (!ok) {
        (void)fprintf(stderr,
                      "max operation-density baseline failed stage=%s error=%s "
                      "journal_bytes=%zu\n",
                      stage, err.msg, out->byte_count);
        recovery_scaling_baseline_free(out);
    }
    return ok;
}

/* Exact total-frame budget: one real checkpoint followed by the smallest valid
 * metadata frame until TP_JOURNAL_MAX_RECORDS is reached. Constructing the
 * repeated byte-identical META frames directly keeps benchmark setup out of the
 * timed region and avoids half a million writer sync calls. Recovery still runs
 * through the production frame walker and must classify every frame. */
static bool recovery_max_total_frames_baseline_create(
    const fixture *f, recovery_scaling_baseline *out) {
    memset(out, 0, sizeof *out);
    char *snapshot = NULL;
    size_t snapshot_len = 0U;
    tp_error err = {{0}};
    if (tp_project_save_buffer(f->project, &snapshot, &snapshot_len, &err) !=
        TP_STATUS_OK) {
        return false;
    }

    tp_journal_io io = tp_journal_io_memory();
    tp_journal *journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    if (!journal ||
        tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot,
                                   snapshot_len, 0, &err) != TP_STATUS_OK) {
        tp_journal_destroy(journal);
        free(snapshot);
        return false;
    }
    uint8_t *checkpoint = NULL;
    size_t checkpoint_len = 0U;
    if (io.read_all(io.ctx, SIZE_MAX, &checkpoint, &checkpoint_len) != 0 || !checkpoint) {
        tp_journal_destroy(journal);
        free(snapshot);
        return false;
    }
    tp_journal_destroy(journal);

    enum { META_PAYLOAD_LEN = TP_JRN_META_FIXED + TP_JRN_LEN_FIELD };
    const size_t frame_len = (size_t)TP_JRN_SYNC_FIELD +
                             (size_t)TP_JRN_LEN_FIELD + META_PAYLOAD_LEN +
                             (size_t)TP_JRN_CRC_FIELD;
    uint8_t frame[TP_JRN_SYNC_FIELD + TP_JRN_LEN_FIELD + META_PAYLOAD_LEN +
                  TP_JRN_CRC_FIELD];
    memset(frame, 0, sizeof frame);
    tp_jrn_put_u32(frame, (uint32_t)TP_JRN_SYNC_WORD);
    tp_jrn_put_u32(frame + TP_JRN_SYNC_FIELD, META_PAYLOAD_LEN);
    uint8_t *meta = frame + TP_JRN_SYNC_FIELD + TP_JRN_LEN_FIELD;
    meta[0] = (uint8_t)TP_JRN_REC_META;
    tp_jrn_put_i64(meta + 1, 1);
    tp_jrn_put_u32(meta + 9, 0U);
    tp_jrn_put_u32(meta + TP_JRN_META_FIXED, 0U);
    const size_t crc_span = (size_t)TP_JRN_SYNC_FIELD +
                            (size_t)TP_JRN_LEN_FIELD + META_PAYLOAD_LEN;
    tp_jrn_put_u32(frame + crc_span, tp_jrn_crc32(0, frame, crc_span));

    const size_t metadata_frames = (size_t)TP_JOURNAL_MAX_RECORDS - 1U;
    if (metadata_frames > (SIZE_MAX - checkpoint_len) / frame_len) {
        free(checkpoint);
        free(snapshot);
        return false;
    }
    const size_t total_len = checkpoint_len + metadata_frames * frame_len;
    if (total_len > (size_t)TP_JOURNAL_MAX_FILE_BYTES) {
        free(checkpoint);
        free(snapshot);
        return false;
    }
    out->bytes = (uint8_t *)malloc(total_len);
    if (!out->bytes) {
        free(checkpoint);
        free(snapshot);
        return false;
    }
    memcpy(out->bytes, checkpoint, checkpoint_len);
    for (size_t i = 0U; i < metadata_frames; ++i) {
        memcpy(out->bytes + checkpoint_len + i * frame_len, frame, frame_len);
    }
    free(checkpoint);
    out->byte_count = total_len;
    out->checkpoint_bytes = snapshot_len;
    out->payload_bytes = META_PAYLOAD_LEN;
    out->record_count = 0;
    out->operations_per_payload = 0;
    out->total_frame_count = (size_t)TP_JOURNAL_MAX_RECORDS;
    out->expected_identity = tp_semantic_identity(f->project);
    free(snapshot);
    return true;
}

static const char *recovery_target_scope(const char *point) {
    if (strcmp(point, "max_records") == 0) {
        return "max_total_frame_budget";
    }
    if (strcmp(point, "max_file_txn_density") == 0) {
        return "max_file_record_density";
    }
    if (strcmp(point, "max_replay_ops_density") == 0) {
        return "max_replay_operation_budget";
    }
    return "none";
}

static void report_recovery_scaling(const fixture *f, const char *point,
                                    const recovery_scaling_baseline *baseline,
                                    int warmups, int iterations, tp_bench_samples *samples,
                                    const tp_journal_recovery_copy_stats *copies_max,
                                    bool applied_operations_counted,
                                    size_t applied_operations_observed) {
    const char *target_scope = recovery_target_scope(point);
    const bool has_target = strcmp(target_scope, "none") != 0;
    (void)printf("scenario=recovery_scaling point=%s records=%d total_frames=%zu samples_ms=[",
                 point, baseline->record_count, baseline->total_frame_count);
    for (size_t i = 0U; i < samples->count; ++i) {
        (void)printf("%s%.6f", i == 0U ? "" : ",", samples->values[i]);
    }
    (void)printf("]\n");
    const double p50_ms = tp_bench_samples_percentile(samples, 50U);
    const double p95_ms = tp_bench_samples_percentile(samples, 95U);
    const char *target_result =
        !has_target ? "not_applicable" : (p95_ms < 1000.0 ? "pass" : "miss");
    (void)printf("scenario=recovery_scaling point=%s records=%d total_frames=%zu operations_per_payload=%d "
                 "payload_bytes=%zu checkpoint_bytes=%zu model_atlases=%d model_sources=%d "
                 "model_overrides=%d model_animations=%d model_frames=%d warmups=%d iterations=%d "
                 "p50_ms=%.6f p95_ms=%.6f accepted=%zu failed=%zu journal_bytes=%zu "
                 "tp_status=ok recovery_status=ok raw_storage_copies_max=%zu "
                 "raw_storage_bytes_max=%zu checkpoint_payload_copies_max=%zu "
                 "checkpoint_payload_bytes_max=%zu op_payload_copies_max=%zu "
                 "op_payload_bytes_max=%zu applied_ops_expected=%zu applied_ops_counted=%d "
                 "applied_ops_observed=%zu "
                 "target_p95_ms=%s target_scope=%s target_result=%s "
                 "target_enforcement=advisory\n",
                 point, baseline->record_count, baseline->total_frame_count,
                 baseline->operations_per_payload,
                 baseline->payload_bytes, baseline->checkpoint_bytes, f->spec.atlases,
                 f->spec.atlases * f->spec.sources_per_atlas,
                 f->spec.atlases * f->spec.overrides_per_atlas,
                 f->spec.atlases * f->spec.animations_per_atlas,
                 f->spec.atlases * f->spec.animations_per_atlas * f->spec.frames_per_animation,
                 warmups, iterations, p50_ms, p95_ms, samples->count, samples->failed,
                 baseline->byte_count, copies_max->raw_storage_copies,
                 copies_max->raw_storage_bytes, copies_max->checkpoint_payload_copies,
                 copies_max->checkpoint_payload_bytes, copies_max->operation_payload_copies,
                 copies_max->operation_payload_bytes,
                 (size_t)baseline->record_count * (size_t)baseline->operations_per_payload,
                 applied_operations_counted ? 1 : 0,
                 applied_operations_observed,
                 has_target ? "1000" : "none", target_scope, target_result);
}

static bool bench_recovery_scaling_point(const fixture *f, const char *point,
                                         int requested_operations, int max_payload_mode,
                                         int warmups, int iterations) {
    recovery_scaling_baseline baseline;
    const bool baseline_ok = max_payload_mode == 3
                                 ? recovery_max_op_density_baseline_create(f, &baseline)
                                 : max_payload_mode == 4
                                       ? recovery_max_total_frames_baseline_create(f, &baseline)
                                 : max_payload_mode != 0
                                       ? recovery_max_payload_baseline_create(
                                             f, max_payload_mode == 2, &baseline)
                                       : recovery_scaling_baseline_create(
                                             f, requested_operations, &baseline);
    if (!baseline_ok) {
        return false;
    }
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    tp_journal_recovery_copy_stats copies_max = {0};
    size_t applied_operations_observed = 0U;
    const size_t expected_applied_operations =
        (size_t)baseline.record_count * (size_t)baseline.operations_per_payload;
    const bool count_applied_operations = true;
    for (int i = 0; i < warmups + iterations; ++i) {
        recovery_read_probe *probe = NULL;
        tp_journal_io io = recovery_probe_io(baseline.bytes, baseline.byte_count, &probe);
        if (!io.ctx || !probe) {
            recovery_scaling_baseline_free(&baseline);
            return false;
        }
        tp_model *recovered = NULL;
        tp_journal_recovery info;
        memset(&info, 0, sizeof info);
        tp_error err = {{0}};
        if (count_applied_operations) {
            tp_op__test_apply_count_reset();
        }
        const double start = tp_bench_now_ms();
        const tp_status status = tp_model_recover(io, journal_key(), &recovered, &info, &err);
        const double elapsed = tp_bench_now_ms() - start;
        const size_t applied_operations = count_applied_operations
                                              ? tp_op__test_apply_count_take()
                                              : 0U;
        const size_t read_calls = probe->read_calls;
        const bool raw_owner_adopted = info._raw_record_buffer == probe->returned_buffer;
        tp_journal_recovery_copy_stats copies = {0};
        tp_journal__test_recovery_copy_stats(&info, &copies);
        if (copies.raw_storage_copies > copies_max.raw_storage_copies) {
            copies_max.raw_storage_copies = copies.raw_storage_copies;
        }
        if (copies.raw_storage_bytes > copies_max.raw_storage_bytes) {
            copies_max.raw_storage_bytes = copies.raw_storage_bytes;
        }
        if (copies.checkpoint_payload_copies > copies_max.checkpoint_payload_copies) {
            copies_max.checkpoint_payload_copies = copies.checkpoint_payload_copies;
        }
        if (copies.checkpoint_payload_bytes > copies_max.checkpoint_payload_bytes) {
            copies_max.checkpoint_payload_bytes = copies.checkpoint_payload_bytes;
        }
        if (copies.operation_payload_copies > copies_max.operation_payload_copies) {
            copies_max.operation_payload_copies = copies.operation_payload_copies;
        }
        if (copies.operation_payload_bytes > copies_max.operation_payload_bytes) {
            copies_max.operation_payload_bytes = copies.operation_payload_bytes;
        }
        if (applied_operations > applied_operations_observed) {
            applied_operations_observed = applied_operations;
        }
        const bool ok = status == TP_STATUS_OK && recovered &&
                        info.status == TP_JOURNAL_RECOVERY_OK && !info.mid_stream_corrupt &&
                        info.records_recovered == baseline.record_count + 1 &&
                        info.op_count == (size_t)baseline.record_count &&
                        info.revision == (int64_t)baseline.record_count &&
                        tp_model_revision(recovered) == (int64_t)baseline.record_count &&
                        read_calls == 1U && raw_owner_adopted &&
                        copies.raw_storage_copies == 1U &&
                        copies.raw_storage_bytes == baseline.byte_count &&
                        copies.checkpoint_payload_copies == 0U &&
                        copies.checkpoint_payload_bytes == 0U &&
                        copies.operation_payload_copies == 0U &&
                        copies.operation_payload_bytes == 0U &&
                        (!count_applied_operations ||
                         applied_operations == expected_applied_operations) &&
                        tp_journal__test_recovery_ops_borrow_raw(&info) &&
                        tp_id128_eq(tp_semantic_identity(tp_model_project(recovered)),
                                    baseline.expected_identity);
        if (!ok) {
            (void)fprintf(stderr,
                          "recovery scaling validation failed point=%s status=%s error=%s "
                          "recovery_status=%d records=%d/%d op_records=%zu revision=%" PRId64
                          "/%d read_calls=%zu raw_owner_adopted=%d borrowed=%d "
                          "applied_ops=%zu/%zu\n",
                          point, tp_status_id(status), err.msg, (int)info.status,
                          info.records_recovered, baseline.record_count + 1, info.op_count,
                          info.revision, baseline.record_count, read_calls,
                          raw_owner_adopted ? 1 : 0,
                          tp_journal__test_recovery_ops_borrow_raw(&info) ? 1 : 0,
                          applied_operations, expected_applied_operations);
        }
        tp_journal_recovery_free(&info);
        tp_model_destroy(recovered);
        free(probe);
        if (!ok || (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed))) {
            recovery_scaling_baseline_free(&baseline);
            return false;
        }
    }
    const bool valid = tp_bench_samples_valid(&samples) &&
                       copies_max.raw_storage_copies <= 1U &&
                       copies_max.raw_storage_bytes <= baseline.byte_count &&
                       copies_max.checkpoint_payload_copies == 0U &&
                       copies_max.checkpoint_payload_bytes == 0U &&
                       copies_max.operation_payload_copies == 0U &&
                       copies_max.operation_payload_bytes == 0U;
    if (valid) {
        report_recovery_scaling(f, point, &baseline, warmups, iterations, &samples,
                                &copies_max, count_applied_operations,
                                applied_operations_observed);
    }
    recovery_scaling_baseline_free(&baseline);
    return valid;
}

static int run_recovery_scaling(int iterations) {
    const fixture_spec spec = {"RECOVERY", 3, 4, 20, 3, 8};
    fixture f;
    if (!fixture_prepare(&f, spec)) {
        (void)fprintf(stderr, "recovery scaling fixture setup failed\n");
        return 1;
    }
    const int warmups = 1;
    const bool ok = bench_recovery_scaling_point(&f, "1k", 1000, 0, warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "10k", 10000, 0, warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "max_file_txn_density", 0, 0,
                                                 warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "max_records", 0, 4,
                                                 warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "max_payload_max_ops", 1, 1,
                                                 warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "max_file_max_payload_max_ops", 0, 2,
                                                 warmups, iterations) &&
                    bench_recovery_scaling_point(&f, "max_replay_ops_density", 0, 3,
                                                 warmups, iterations);
    fixture_free(&f);
    if (!ok) {
        (void)fprintf(stderr, "recovery scaling benchmark failed\n");
        return 1;
    }
    (void)printf("tp_bench_foundation recovery_scaling: OK\n");
    return 0;
}

static bool save_sample_prepare(const fixture *f, const char *journal_path,
                                uint64_t id, tp_model **out) {
    *out = NULL;
    (void)remove(journal_path);
    tp_project *clone = tp_project_clone(f->project);
    tp_model *model = clone ? tp_model_wrap(clone) : NULL;
    if (!model) {
        tp_project_destroy(clone);
        return false;
    }
    tp_journal_io io = tp_journal_io_file(journal_path);
    tp_journal *journal = io.ctx ? tp_journal_create(io, journal_key()) : NULL;
    tp_error err = {{0}};
    if (!journal || tp_model_attach_journal(model, journal, &err) != TP_STATUS_OK) {
        if (journal) {
            tp_journal_destroy(journal);
        }
        tp_model_destroy(model);
        (void)remove(journal_path);
        return false;
    }
    tp_id128 atlas_id = tp_model_project(model)->atlases[f->spec.atlases - 1].id;
    if (!apply_padding(model, atlas_id, id, 9) || !tp_model_dirty(model)) {
        tp_model_destroy(model);
        (void)remove(journal_path);
        return false;
    }
    *out = model;
    return true;
}

static bool checkpoint_roundtrip_identity(const tp_project *project,
                                          tp_id128 *out, tp_error *err) {
    char *checkpoint = NULL;
    size_t checkpoint_len = 0U;
    tp_project *roundtrip = NULL;
    const tp_status save_status = tp_project_checkpoint_save_buffer(
        project, &checkpoint, &checkpoint_len, err);
    const tp_status load_status =
        save_status == TP_STATUS_OK
            ? tp_project_load_buffer(checkpoint, checkpoint_len, &roundtrip, err)
            : save_status;
    free(checkpoint);
    if (load_status != TP_STATUS_OK || !roundtrip) {
        tp_project_destroy(roundtrip);
        return false;
    }
    *out = tp_semantic_identity(roundtrip);
    tp_project_destroy(roundtrip);
    return true;
}

static tp_status save_model_candidate(tp_model *model, const char *path,
                                      tp_error *err) {
    tp_project *candidate = tp_project_clone(tp_model_project(model));
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "benchmark save candidate clone failed");
    }
    const tp_status status = tp_project_save_candidate_with_fingerprint(
        candidate, path, NULL, false, NULL, err);
    if (status != TP_STATUS_OK) {
        tp_project_destroy(candidate);
        return status;
    }
    tp_model__adopt_project(model, candidate);
    return TP_STATUS_OK;
}

static bool bench_save(const fixture *f, const char *scratch, int warmups, int iterations) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    uint64_t max_saved_bytes = 0U;
    for (int i = 0; i < warmups + iterations; i++) {
        char save_path[768];
        char journal_path[768];
        if (!make_path(save_path, sizeof save_path, scratch, f, "save_project", i) ||
            !make_path(journal_path, sizeof journal_path, scratch, f, "save_journal", i)) {
            return false;
        }
        (void)remove(save_path);
        tp_model *model = NULL;
        if (!save_sample_prepare(f, journal_path, (uint64_t)i + 5000U, &model)) {
            (void)fprintf(stderr,
                          "benchmark_evidence scenario=save_compact fixture=%s "
                          "phase=prepare sample=%d failed\n",
                          f->spec.name, i);
            return false;
        }
        tp_error err = {{0}};
        double start = tp_bench_now_ms();
        tp_status status = save_model_candidate(model, save_path, &err);
        if (status == TP_STATUS_OK) {
            tp_model_mark_saved(model);
            status = tp_model_compact_journal(model, &err);
        }
        double elapsed = tp_bench_now_ms() - start;
        int64_t saved_bytes = file_size(save_path);
        const int64_t expected_revision = tp_model_revision(model);
        const tp_id128 expected_identity = tp_semantic_identity(tp_model_project(model));
        tp_id128 expected_recovery_identity = tp_id128_nil();
        const bool recovery_identity_ready =
            status == TP_STATUS_OK &&
            checkpoint_roundtrip_identity(tp_model_project(model),
                                          &expected_recovery_identity, &err);
        if (status == TP_STATUS_OK && !recovery_identity_ready) {
            (void)fprintf(stderr,
                          "benchmark_evidence scenario=save_compact fixture=%s "
                          "phase=checkpoint_oracle sample=%d error=%s\n",
                          f->spec.name, i, err.msg);
        }
        const bool dirty_after_save = tp_model_dirty(model);
        bool ok = status == TP_STATUS_OK && recovery_identity_ready &&
                  !dirty_after_save && saved_bytes > 0 &&
                  tp_model_project(model)->atlases[f->spec.atlases - 1].padding == 9;
        if (ok && (uint64_t)saved_bytes > max_saved_bytes) {
            max_saved_bytes = (uint64_t)saved_bytes;
        }
        tp_model_destroy(model);

        tp_project *loaded = NULL;
        tp_error verify_err = {{0}};
        if (ok) {
            const tp_status load_status =
                tp_project_load(save_path, &loaded, &verify_err);
            ok = load_status == TP_STATUS_OK && loaded &&
                 tp_id128_eq(tp_semantic_identity(loaded), expected_identity);
            if (!ok) {
                (void)fprintf(stderr,
                              "benchmark_evidence scenario=save_compact fixture=%s "
                              "phase=reload sample=%d status=%s error=%s\n",
                              f->spec.name, i, tp_status_str(load_status),
                              verify_err.msg);
            }
        }
        tp_project_destroy(loaded);

        tp_model *recovered = NULL;
        tp_journal_recovery info;
        memset(&info, 0, sizeof info);
        if (ok) {
            tp_journal_io verify_io = tp_journal_io_file(journal_path);
            const tp_status recover_status =
                verify_io.ctx
                    ? tp_model_recover(verify_io, journal_key(), &recovered,
                                       &info, &verify_err)
                    : TP_STATUS_BAD_PROJECT;
            ok = verify_io.ctx && recover_status == TP_STATUS_OK &&
                 recovered && info.status == TP_JOURNAL_RECOVERY_OK && !info.mid_stream_corrupt &&
                 info.records_recovered == 1 && info.op_count == 0U && info.revision == expected_revision &&
                 tp_model_revision(recovered) == expected_revision &&
                 tp_id128_eq(tp_semantic_identity(tp_model_project(recovered)),
                             expected_recovery_identity);
            if (!ok) {
                const int64_t recovered_revision =
                    recovered ? tp_model_revision(recovered) : -1;
                const bool identity_equal =
                    recovered && tp_id128_eq(
                                     tp_semantic_identity(
                                         tp_model_project(recovered)),
                                     expected_recovery_identity);
                (void)fprintf(stderr,
                              "benchmark_evidence scenario=save_compact fixture=%s "
                              "phase=recover sample=%d status=%s recovery_status=%d "
                              "records=%d ops=%zu revision=%" PRId64
                              " expected_revision=%" PRId64
                              " recovered_revision=%" PRId64
                              " mid_stream=%d identity_equal=%d error=%s\n",
                              f->spec.name, i, tp_status_str(recover_status),
                              (int)info.status, info.records_recovered,
                              info.op_count, info.revision, expected_revision,
                              recovered_revision,
                              info.mid_stream_corrupt ? 1 : 0,
                              identity_equal ? 1 : 0, verify_err.msg);
            }
        }
        tp_journal_recovery_free(&info);
        tp_model_destroy(recovered);
        (void)remove(save_path);
        (void)remove(journal_path);
        if (!ok) {
            if (status != TP_STATUS_OK || dirty_after_save || saved_bytes <= 0) {
                (void)fprintf(stderr,
                              "benchmark_evidence scenario=save_compact fixture=%s "
                              "phase=save sample=%d status=%s dirty=%d bytes=%" PRId64
                              " error=%s\n",
                              f->spec.name, i, tp_status_str(status),
                              dirty_after_save ? 1 : 0, saved_bytes,
                              err.msg);
            }
            if (i >= warmups) {
                (void)tp_bench_samples_record(&samples, false, elapsed);
            }
            return false;
        }
        if (i >= warmups && !tp_bench_samples_record(&samples, true, elapsed)) {
            return false;
        }
    }
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    report_samples("save_compact", f, &samples, max_saved_bytes, "saved_bytes_max");
    return true;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--project-load-scaling") == 0) {
        int scaling_iterations = 3;
        if (argc > 3 ||
            (argc == 3 && !parse_positive(argv[2], (int)TP_BENCH_MAX_SAMPLES,
                                          &scaling_iterations))) {
            (void)fprintf(
                stderr,
                "usage: tp_bench_foundation --project-load-scaling [iterations 1..%u]\n",
                (unsigned)TP_BENCH_MAX_SAMPLES);
            return 2;
        }
        (void)printf("tp_bench_foundation clock=monotonic source=nt_time_now "
                     "mode=project_load_scaling thresholds=accounted-resource-hard-timing-advisory\n");
        return run_project_load_scaling(scaling_iterations);
    }
    if (argc > 1 && strcmp(argv[1], "--recovery-scaling") == 0) {
        int scaling_iterations = 3;
        if (argc > 3 ||
            (argc == 3 && !parse_positive(argv[2], (int)TP_BENCH_MAX_SAMPLES,
                                          &scaling_iterations))) {
            (void)fprintf(stderr,
                          "usage: tp_bench_foundation --recovery-scaling [iterations 1..%u]\n",
                          (unsigned)TP_BENCH_MAX_SAMPLES);
            return 2;
        }
        (void)printf("tp_bench_foundation clock=monotonic source=nt_time_now "
                     "mode=recovery_scaling correctness=count/copy-hard "
                     "timing=advisory\n");
        return run_recovery_scaling(scaling_iterations);
    }
    const char *scratch = argc > 1 ? argv[1] : "tp_bench_foundation_tmp";
    int iterations = 20;
    int recovery_ops = 100;
    if (argc > 4 || (argc > 2 && !parse_positive(argv[2], (int)TP_BENCH_MAX_SAMPLES, &iterations)) ||
        (argc > 3 && !parse_positive(argv[3], 10000, &recovery_ops))) {
        (void)fprintf(stderr, "usage: tp_bench_foundation [scratch_dir] [iterations 1..%u] [recovery_ops 1..10000]\n",
                      (unsigned)TP_BENCH_MAX_SAMPLES);
        return 2;
    }
    tp_mkdirs(scratch);
    const int warmups = 2;
    const fixture_spec specs[] = {
        {"NORMAL", 3, 4, 20, 3, 8},
        {"LARGE", 20, 4, 200, 5, 16},
        {"HUGE", 100, 2, 1000, 0, 0},
    };
    (void)printf("tp_bench_foundation clock=monotonic source=nt_time_now thresholds=advisory\n");
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        fixture f;
        if (!fixture_prepare(&f, specs[i])) {
            (void)fprintf(stderr, "fixture setup failed: %s\n", specs[i].name);
            return 1;
        }
        print_fixture(&f, warmups, iterations, recovery_ops);
        bool ok = bench_normal_transaction(&f, warmups, iterations) &&
                  bench_session_snapshot(&f, warmups, iterations) &&
                  bench_history(&f, scratch, warmups, iterations, false) &&
                  bench_history(&f, scratch, warmups, iterations, true) &&
                  bench_recovery(&f, scratch, warmups, iterations, recovery_ops) &&
                  bench_save(&f, scratch, warmups, iterations);
        fixture_free(&f);
        if (!ok) {
            (void)fprintf(stderr, "benchmark scenario failed: %s\n", specs[i].name);
            return 1;
        }
    }
    (void)printf("\ntp_bench_foundation: OK\n");
    return 0;
}
