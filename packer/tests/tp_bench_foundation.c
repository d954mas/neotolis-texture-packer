/* M0 production-path baseline: normal transaction, journal-backed Undo/Redo,
 * recovery, and Save+compaction. This is a release benchmark, not a timing
 * ctest. Setup and cleanup stay outside every timed region. */

#include "tp_bench_support.h"

#include "tp_core/tp_diff.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_transaction.h"
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

static bool fill_atlas(tp_project_atlas *atlas, const fixture_spec *spec) {
    char key[96];
    char value[96];
    for (int i = 0; i < spec->sources_per_atlas; i++) {
        if (snprintf(key, sizeof key, "art/folder_%03d", i) < 0 ||
            tp_project_atlas_add_source(atlas, key) != TP_STATUS_OK) {
            return false;
        }
    }
    for (int i = 0; i < spec->overrides_per_atlas; i++) {
        tp_project_sprite *sprite = NULL;
        if (snprintf(key, sizeof key, "sprites/hero_walk_%05d", i) < 0 ||
            snprintf(value, sizeof value, "player_walk_%05d", i) < 0 ||
            tp_project_atlas_add_sprite(atlas, key, &sprite) != TP_STATUS_OK || !sprite ||
            tp_project_atlas_set_sprite_rename(atlas, key, value) != TP_STATUS_OK) {
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
            if (snprintf(value, sizeof value, "sprites/hero_walk_%05d", frame) < 0 ||
                tp_project_anim_add_frame(animation, value) != TP_STATUS_OK) {
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
        if (!fill_atlas(&project->atlases[index], &spec)) {
            tp_project_destroy(project);
            return false;
        }
    }
    uint64_t counter = 0U;
    tp_rng rng = {deterministic_rng, &counter};
    tp_error err = {{0}};
    if (tp_project_promote_ids(project, &rng, &err) != TP_STATUS_OK) {
        tp_project_destroy(project);
        return false;
    }
    char *serialized = NULL;
    size_t serialized_len = 0U;
    if (tp_project_save_buffer(project, &serialized, &serialized_len, &err) != TP_STATUS_OK) {
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
        const int padding = 2 + (i & 1);
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
    if (!journal || tp_model_attach_journal(model, journal, &err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "history setup failed: attach_journal fixture=%s error=%s\n", f->spec.name,
                      err.msg);
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
        if (!apply_padding(model, atlas_id, (uint64_t)i + 2000U, 2 + (i & 1))) {
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
    for (int i = 0; i < warmups + iterations; i++) {
        char sample_path[768];
        if (!make_path(sample_path, sizeof sample_path, scratch, f, "recovery_sample", i) ||
            !copy_file(baseline, sample_path)) {
            (void)remove(baseline);
            return false;
        }
        tp_journal_io io = tp_journal_io_file(sample_path);
        tp_model *recovered = NULL;
        tp_journal_recovery info;
        tp_error err = {{0}};
        double start = tp_bench_now_ms();
        tp_status status = io.ctx ? tp_model_recover(io, journal_key(), &recovered, &info, &err)
                                  : TP_STATUS_BAD_PROJECT;
        double elapsed = tp_bench_now_ms() - start;
        bool ok = status == TP_STATUS_OK && recovered && info.status == TP_JOURNAL_RECOVERY_OK &&
                  !info.mid_stream_corrupt && info.records_recovered == recovery_ops + 1 &&
                  info.revision == expected_revision && tp_model_revision(recovered) == expected_revision &&
                  info.op_count == (size_t)recovery_ops &&
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
    return true;
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
            return false;
        }
        tp_error err = {{0}};
        double start = tp_bench_now_ms();
        tp_status status = tp_project_save(tp_model_project(model), save_path, &err);
        if (status == TP_STATUS_OK) {
            tp_model_mark_saved(model);
            status = tp_model_compact_journal(model, &err);
        }
        double elapsed = tp_bench_now_ms() - start;
        int64_t saved_bytes = file_size(save_path);
        const int64_t expected_revision = tp_model_revision(model);
        const tp_id128 expected_identity = tp_semantic_identity(tp_model_project(model));
        bool ok = status == TP_STATUS_OK && !tp_model_dirty(model) && saved_bytes > 0 &&
                  tp_model_project(model)->atlases[f->spec.atlases - 1].padding == 9;
        if (ok && (uint64_t)saved_bytes > max_saved_bytes) {
            max_saved_bytes = (uint64_t)saved_bytes;
        }
        tp_model_destroy(model);

        tp_project *loaded = NULL;
        tp_error verify_err = {{0}};
        if (ok) {
            ok = tp_project_load(save_path, &loaded, &verify_err) == TP_STATUS_OK && loaded &&
                 tp_id128_eq(tp_semantic_identity(loaded), expected_identity);
        }
        tp_project_destroy(loaded);

        tp_model *recovered = NULL;
        tp_journal_recovery info;
        memset(&info, 0, sizeof info);
        if (ok) {
            tp_journal_io verify_io = tp_journal_io_file(journal_path);
            ok = verify_io.ctx &&
                 tp_model_recover(verify_io, journal_key(), &recovered, &info, &verify_err) == TP_STATUS_OK &&
                 recovered && info.status == TP_JOURNAL_RECOVERY_OK && !info.mid_stream_corrupt &&
                 info.records_recovered == 1 && info.op_count == 0U && info.revision == expected_revision &&
                 tp_model_revision(recovered) == expected_revision &&
                 tp_id128_eq(tp_semantic_identity(tp_model_project(recovered)), expected_identity);
        }
        tp_journal_recovery_free(&info);
        tp_model_destroy(recovered);
        (void)remove(save_path);
        (void)remove(journal_path);
        if (!ok) {
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
                  bench_history(&f, scratch, warmups, iterations, false) &&
                  bench_history(&f, scratch, warmups, iterations, true) &&
                  bench_recovery(&f, scratch, warmups, iterations, recovery_ops) &&
                  bench_save(&f, scratch, warmups, iterations);
        tp_project_destroy(f.project);
        if (!ok) {
            (void)fprintf(stderr, "benchmark scenario failed: %s\n", specs[i].name);
            return 1;
        }
    }
    (void)printf("\ntp_bench_foundation: OK\n");
    return 0;
}
