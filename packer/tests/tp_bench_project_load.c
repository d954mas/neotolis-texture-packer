/* Project-load admission/resource calibration for tp_bench_foundation. */

#include "tp_bench_project_load.h"
#include "tp_bench_support.h"

#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_project_internal.h"
#include "cJSON.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct project_load_case {
    const char *shape;
    const char *point;
    char *json;
    size_t json_bytes;
    size_t nodes;
    size_t max_entries;
    size_t max_depth;
    size_t expected_atlases;
    size_t expected_sources;
    size_t expected_sprites;
    bool legacy;
} project_load_case;

#define TP_PROJECT_LOAD_ACCOUNTED_BUDGET_BYTES (257U * 1024U * 1024U)

typedef struct json_builder {
    char *bytes;
    size_t len;
    size_t capacity;
} json_builder;

static bool json_builder_reserve(json_builder *b, size_t extra) {
    if (extra > SIZE_MAX - b->len - 1U) {
        return false;
    }
    const size_t needed = b->len + extra + 1U;
    if (needed <= b->capacity) {
        return true;
    }
    size_t capacity = b->capacity ? b->capacity : 1024U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            capacity = needed;
            break;
        }
        capacity *= 2U;
    }
    char *grown = (char *)realloc(b->bytes, capacity);
    if (!grown) {
        return false;
    }
    b->bytes = grown;
    b->capacity = capacity;
    return true;
}

static bool json_builder_append(json_builder *b, const char *format, ...)
    TP_PRINTF_ATTR(2, 3);
static bool json_builder_append(json_builder *b, const char *format, ...) {
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (needed < 0 || !json_builder_reserve(b, (size_t)needed)) {
        va_end(args);
        return false;
    }
    const int written = vsnprintf(b->bytes + b->len,
                                  b->capacity - b->len, format, args);
    va_end(args);
    if (written != needed) {
        return false;
    }
    b->len += (size_t)written;
    return true;
}

static bool json_builder_repeat(json_builder *b, char byte, size_t count) {
    if (!json_builder_reserve(b, count)) {
        return false;
    }
    memset(b->bytes + b->len, byte, count);
    b->len += count;
    b->bytes[b->len] = '\0';
    return true;
}

static bool project_load_case_admission_is_exact(const project_load_case *c) {
    tp_error err = {{0}};
    const tp_project_json_limits exact = {
        c->json_bytes, c->nodes, c->max_entries, c->max_depth,
    };
    if (tp_project__test_json_admit(c->json, c->json_bytes, &exact, &err) !=
        TP_STATUS_OK) {
        (void)fprintf(stderr,
                      "project load fixture admission failed shape=%s point=%s error=%s\n",
                      c->shape, c->point, err.msg);
        return false;
    }
    tp_project_json_limits below = exact;
    below.bytes--;
    if (tp_project__test_json_admit(c->json, c->json_bytes, &below, &err) !=
        TP_STATUS_OUT_OF_BOUNDS) {
        return false;
    }
    below = exact;
    below.nodes--;
    if (tp_project__test_json_admit(c->json, c->json_bytes, &below, &err) !=
        TP_STATUS_OUT_OF_BOUNDS) {
        return false;
    }
    below = exact;
    below.container_entries--;
    if (tp_project__test_json_admit(c->json, c->json_bytes, &below, &err) !=
        TP_STATUS_OUT_OF_BOUNDS) {
        return false;
    }
    below = exact;
    below.depth--;
    return tp_project__test_json_admit(c->json, c->json_bytes, &below, &err) ==
           TP_STATUS_OUT_OF_BOUNDS;
}

static bool project_load_case_mixed(size_t records, const char *point,
                                    project_load_case *out) {
    memset(out, 0, sizeof *out);
    json_builder b = {0};
    bool ok = json_builder_append(
        &b, "{\"version\":3,\"atlases\":[{\"id\":\"atlas_"
            "ffffffffffffffffffffffffffffffff\",\"name\":\"a\","
            "\"sources\":[");
    for (size_t i = 0U; ok && i < records; i++) {
        ok = json_builder_append(
            &b, "%s{\"id\":\"source_%032x\",\"path\":\"path/%zu\"}",
            i == 0U ? "" : ",", (unsigned)(i + 1U), i);
    }
    ok = ok && json_builder_append(&b, "],\"sprites\":[");
    for (size_t i = 0U; ok && i < records; i++) {
        ok = json_builder_append(&b, "%s{\"name\":\"sprite/%zu\"}",
                                 i == 0U ? "" : ",", i);
    }
    ok = ok && json_builder_append(&b, "]}]}");
    if (!ok) {
        free(b.bytes);
        return false;
    }
    *out = (project_load_case){
        "schema_mixed", point, b.bytes, b.len, 14U + 8U * records,
        records, 5U, 1U, records, records, false,
    };
    return project_load_case_admission_is_exact(out);
}

static bool project_load_case_legacy(size_t records, const char *point,
                                     project_load_case *out) {
    memset(out, 0, sizeof *out);
    json_builder b = {0};
    bool ok = json_builder_append(
        &b, "{\"version\":2,\"atlases\":[{\"id\":\"atlas_"
            "ffffffffffffffffffffffffffffffff\",\"name\":\"a\","
            "\"sources\":[");
    for (size_t i = 0U; ok && i < records; i++) {
        ok = json_builder_append(&b, "%s\"path/%zu\"",
                                 i == 0U ? "" : ",", i);
    }
    ok = ok && json_builder_append(&b, "]}]}");
    if (!ok) {
        free(b.bytes);
        return false;
    }
    *out = (project_load_case){
        "legacy_sources", point, b.bytes, b.len, 12U + records,
        records, 4U, 1U, records, 0U, true,
    };
    return project_load_case_admission_is_exact(out);
}

static bool project_load_case_combined(project_load_case *out) {
    memset(out, 0, sizeof *out);
    const size_t arrays = 8U;
    const size_t scalars = (size_t)TP_PROJECT_JSON_MAX_NODES - 9U - arrays;
    json_builder b = {0};
    bool ok = json_builder_append(
        &b, "{\"version\":4,\"atlases\":[],\"ignored\":[");
    size_t emitted = 0U;
    for (size_t array = 0U; ok && array < arrays; array++) {
        ok = json_builder_append(&b, "%s[", array == 0U ? "" : ",");
        const size_t remaining = scalars - emitted;
        const size_t count = remaining < (size_t)TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES
                                 ? remaining
                                 : (size_t)TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES;
        for (size_t i = 0U; ok && i < count; i++) {
            ok = json_builder_append(&b, "%s0", i == 0U ? "" : ",");
        }
        emitted += count;
        ok = ok && json_builder_append(&b, "]");
    }
    ok = ok && emitted == scalars &&
         json_builder_append(&b, "],\"padding\":\"");
    const size_t file_limit = (size_t)TP_IDENTITY_FILE_MAX_BYTES;
    if (!ok || b.len + 2U > file_limit) {
        free(b.bytes);
        return false;
    }
    ok = json_builder_repeat(&b, 'x', file_limit - b.len - 2U) &&
         json_builder_append(&b, "\"}");
    if (!ok || b.len != file_limit) {
        free(b.bytes);
        return false;
    }
    *out = (project_load_case){
        "admission_combined_max", "max", b.bytes, b.len,
        (size_t)TP_PROJECT_JSON_MAX_NODES,
        (size_t)TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES, 3U,
        0U, 0U, 0U, false,
    };
    return project_load_case_admission_is_exact(out);
}

static size_t owned_string_bytes(const char *s) {
    return s ? strlen(s) + 1U : 0U;
}

static size_t project_owned_live_bytes(const tp_project *p) {
    size_t bytes = sizeof *p + owned_string_bytes(p->project_dir) +
                   owned_string_bytes(p->source_base_dir) +
                   (size_t)p->atlas_cap * sizeof *p->atlases;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        bytes += owned_string_bytes(a->name) +
                 (size_t)a->source_cap * sizeof *a->sources +
                 (size_t)a->sprite_cap * sizeof *a->sprites +
                 (size_t)a->animation_cap * sizeof *a->animations +
                 (size_t)a->target_cap * sizeof *a->targets;
        for (int i = 0; i < a->source_count; i++) {
            bytes += owned_string_bytes(a->sources[i].path);
        }
        for (int i = 0; i < a->sprite_count; i++) {
            bytes += owned_string_bytes(a->sprites[i].name) +
                     owned_string_bytes(a->sprites[i].src_key) +
                     owned_string_bytes(a->sprites[i].rename);
        }
        for (int i = 0; i < a->animation_count; i++) {
            const tp_project_anim *anim = &a->animations[i];
            bytes += owned_string_bytes(anim->name) +
                     (size_t)anim->frame_cap * sizeof *anim->frames;
            for (int frame = 0; frame < anim->frame_count; frame++) {
                bytes += owned_string_bytes(anim->frames[frame].name) +
                         owned_string_bytes(anim->frames[frame].src_key);
            }
        }
        for (int i = 0; i < a->target_count; i++) {
            bytes += owned_string_bytes(a->targets[i].exporter_id) +
                     owned_string_bytes(a->targets[i].out_path);
        }
    }
    return bytes;
}

static bool project_load_counts_match(const project_load_case *c,
                                      const tp_project *project) {
    size_t sources = 0U;
    size_t sprites = 0U;
    if (!project || (size_t)project->atlas_count != c->expected_atlases) {
        return false;
    }
    for (int ai = 0; ai < project->atlas_count; ai++) {
        sources += (size_t)project->atlases[ai].source_count;
        sprites += (size_t)project->atlases[ai].sprite_count;
    }
    return sources == c->expected_sources && sprites == c->expected_sprites;
}

static size_t max_size(size_t a, size_t b) { return a > b ? a : b; }

static bool bench_project_load_case(const project_load_case *c, int warmups,
                                    int iterations) {
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    tp_project_load_resources resource_max = {0};
    size_t model_bytes_max = 0U;
    size_t lookup_work_max = 0U;
    size_t id_work_max = 0U;
    size_t legacy_work_max = 0U;
    tp_id128 legacy_first_id = tp_id128_nil();
    for (int sample = 0; sample < warmups + iterations; sample++) {
        tp_project__test_load_resources_reset();
        tp_project__test_load_lookup_work_reset();
        tp_project__test_id_validation_work_reset();
        tp_project__test_legacy_id_work_reset();
        tp_project *project = NULL;
        tp_error err = {{0}};
        const double start = tp_bench_now_ms();
        const tp_status status = tp_project_load_buffer(
            c->json, c->json_bytes, &project, &err);
        const double elapsed = tp_bench_now_ms() - start;
        const tp_project_load_resources resources =
            tp_project__test_load_resources_take();
        const tp_project_load_lookup_work lookup =
            tp_project__test_load_lookup_work_take();
        const size_t id_work = tp_project__test_id_validation_work_take();
        const size_t legacy_work = tp_project__test_legacy_id_work_take();
        bool ok = status == TP_STATUS_OK &&
                  project_load_counts_match(c, project);
        if (ok && c->legacy && c->expected_sources > 0U) {
            const tp_id128 id = project->atlases[0].sources[0].id;
            if (sample == 0) {
                legacy_first_id = id;
            } else {
                ok = tp_id128_eq(id, legacy_first_id);
            }
        }
        const size_t records = c->expected_sources + c->expected_sprites +
                               c->expected_atlases;
        ok = ok && lookup.source_path_comparisons <= c->expected_sources * 64U &&
             lookup.pending_name_comparisons <= c->expected_sprites * 64U &&
             id_work <= records * 64U && legacy_work <= records * 64U;
        const size_t model_bytes = ok ? project_owned_live_bytes(project) : 0U;
        if (resources.source_index_peak_bytes > resource_max.source_index_peak_bytes) {
            resource_max.source_index_peak_bytes = resources.source_index_peak_bytes;
        }
        if (resources.pending_index_peak_bytes > resource_max.pending_index_peak_bytes) {
            resource_max.pending_index_peak_bytes = resources.pending_index_peak_bytes;
        }
        if (resources.id_refs_bytes > resource_max.id_refs_bytes) {
            resource_max.id_refs_bytes = resources.id_refs_bytes;
        }
        if (resources.id_index_bytes > resource_max.id_index_bytes) {
            resource_max.id_index_bytes = resources.id_index_bytes;
        }
        if (resources.legacy_peak_bytes > resource_max.legacy_peak_bytes) {
            resource_max.legacy_peak_bytes = resources.legacy_peak_bytes;
        }
        model_bytes_max = max_size(model_bytes_max, model_bytes);
        lookup_work_max = max_size(
            lookup_work_max, lookup.source_path_comparisons +
                                 lookup.pending_name_comparisons);
        id_work_max = max_size(id_work_max, id_work);
        legacy_work_max = max_size(legacy_work_max, legacy_work);
        tp_project_destroy(project);
        if (!ok) {
            (void)fprintf(stderr,
                          "project load scaling failed shape=%s point=%s status=%s error=%s\n",
                          c->shape, c->point, tp_status_id(status), err.msg);
            if (sample >= warmups) {
                (void)tp_bench_samples_record(&samples, false, elapsed);
            }
            return false;
        }
        if (sample >= warmups &&
            !tp_bench_samples_record(&samples, true, elapsed)) {
            return false;
        }
    }
    if (!tp_bench_samples_valid(&samples)) {
        return false;
    }
    (void)printf("scenario=project_load shape=%s point=%s samples_ms=[",
                 c->shape, c->point);
    for (size_t i = 0U; i < samples.count; i++) {
        (void)printf("%s%.6f", i == 0U ? "" : ",", samples.values[i]);
    }
    (void)printf("]\n");
    const double p50 = tp_bench_samples_percentile(&samples, 50U);
    const double p95 = tp_bench_samples_percentile(&samples, 95U);
    const size_t cjson_requested = c->nodes * sizeof(cJSON) + c->json_bytes;
    const size_t id_transient = resource_max.id_refs_bytes +
                                resource_max.id_index_bytes;
    const size_t transient = max_size(
        max_size(resource_max.source_index_peak_bytes,
                 resource_max.pending_index_peak_bytes),
        max_size(resource_max.legacy_peak_bytes, id_transient));
    const size_t accounted_peak = c->json_bytes + cjson_requested +
                                  model_bytes_max + transient;
    if (accounted_peak > (size_t)TP_PROJECT_LOAD_ACCOUNTED_BUDGET_BYTES) {
        (void)fprintf(stderr,
                      "project load resource budget failed shape=%s point=%s "
                      "accounted=%zu budget=%u\n",
                      c->shape, c->point, accounted_peak,
                      (unsigned)TP_PROJECT_LOAD_ACCOUNTED_BUDGET_BYTES);
        return false;
    }
    (void)printf(
        "scenario=project_load shape=%s point=%s p50_ms=%.6f p95_ms=%.6f "
        "accepted=%zu failed=%zu records=%zu\n",
        c->shape, c->point, p50, p95, samples.count, samples.failed,
        c->expected_sources + c->expected_sprites);
    (void)printf(
        "benchmark_evidence scenario=project_load shape=%s point=%s "
        "raw_input_bytes=%zu admitted_nodes=%zu max_container_entries=%zu "
        "max_depth=%zu cjson_requested_bytes=%zu model_owned_bytes=%zu "
        "source_index_peak_bytes=%zu pending_index_peak_bytes=%zu "
        "legacy_peak_bytes=%zu id_refs_bytes=%zu id_index_bytes=%zu "
        "accounted_peak_bytes=%zu accounted_budget_bytes=%u lookup_work=%zu "
        "id_work=%zu legacy_work=%zu\n",
        c->shape, c->point, c->json_bytes, c->nodes, c->max_entries,
        c->max_depth, cjson_requested, model_bytes_max,
        resource_max.source_index_peak_bytes,
        resource_max.pending_index_peak_bytes,
        resource_max.legacy_peak_bytes, resource_max.id_refs_bytes,
        resource_max.id_index_bytes, accounted_peak,
        (unsigned)TP_PROJECT_LOAD_ACCOUNTED_BUDGET_BYTES, lookup_work_max,
        id_work_max, legacy_work_max);
    return true;
}

int tp_bench_project_load_run(int iterations,
                              char *shipped_huge_json,
                              size_t shipped_huge_json_bytes) {
    const int warmups = 1;
    project_load_case shipped = {
        "shipped_huge", "max", shipped_huge_json, shipped_huge_json_bytes,
        1502805U, 1000U, 6U, 100U, 200U, 100000U, false,
    };
    bool ok = project_load_case_admission_is_exact(&shipped) &&
              bench_project_load_case(&shipped, warmups, iterations);

    static const struct {
        size_t records;
        const char *point;
    } points[] = {
        {1000U, "1k"},
        {10000U, "10k"},
        {(size_t)TP_PROJECT_JSON_MAX_CONTAINER_ENTRIES, "max"},
    };
    for (size_t i = 0U; ok && i < sizeof points / sizeof points[0]; i++) {
        project_load_case c;
        ok = project_load_case_mixed(points[i].records, points[i].point, &c) &&
             bench_project_load_case(&c, warmups, iterations);
        free(c.json);
    }
    for (size_t i = 0U; ok && i < sizeof points / sizeof points[0]; i++) {
        project_load_case c;
        ok = project_load_case_legacy(points[i].records, points[i].point, &c) &&
             bench_project_load_case(&c, warmups, iterations);
        free(c.json);
    }
    if (ok) {
        project_load_case combined;
        ok = project_load_case_combined(&combined) &&
             bench_project_load_case(&combined, warmups, iterations);
        free(combined.json);
    }
    if (!ok) {
        (void)fprintf(stderr, "project load scaling benchmark failed\n");
        return 1;
    }
    (void)printf("tp_bench_foundation project_load_scaling: OK\n");
    return 0;
}
