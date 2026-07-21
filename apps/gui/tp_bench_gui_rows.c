/* GUI row benchmark over the exact production build_rows(void) path and a real
 * immutable tp_session snapshot. No window, renderer, pack job, or event loop. */

#include "gui_pack.h"
#include "gui_rows.h"
#include "gui_scan.h"

#include "tp_bench_support.h"
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

/* Exact narrow stubs for the non-build_rows references in gui_rows.c. */
gui_selected_sprite *s_multi_sel;
int s_multi_sel_count;
int s_multi_sel_cap;
int s_sel_atlas;
int s_sel_src = -1;
int s_sel_child = -1;
char s_sel_abs[TP_IDENTITY_PATH_MAX];
bool s_sel_missing;
bool s_reselect_pending;
tp_id128 s_reselect_source_id;
char s_reselect_key[TP_SRCKEY_MAX];
int s_focus_view = -1;     /* build_view() re-pins keyboard focus; the bench never drives a view */
int s_sel_anchor_row = -1; /* build_view() re-pins the shift-anchor; unused in the row benchmark */
static const tp_session_snapshot *s_fixture_snapshot;
static uint64_t s_fixture_snapshot_lifetime;

const tp_session_snapshot *gui_project_snapshot(void) {
    return s_fixture_snapshot;
}

uint64_t gui_project_snapshot_lifetime_generation(void) {
    return s_fixture_snapshot_lifetime;
}

static bool s_status_failed;
void set_status_ex(status_sev_t sev, const char *msg) {
    (void)msg;
    if (sev == STATUS_ERROR) {
        s_status_failed = true;
    }
}

void set_statusf_ex(status_sev_t sev, const char *fmt, ...) {
    set_status_ex(sev, fmt);
}

const tp_result *gui_pack_result(int atlas_index) {
    (void)atlas_index;
    return NULL;
}

bool gui_pack_sprite_matches_ref(int atlas_index, int sprite_index,
                                 tp_id128 source_id,
                                 const char *source_key) {
    (void)atlas_index;
    (void)sprite_index;
    (void)source_id;
    (void)source_key;
    return false; /* the row benchmark deliberately has no pack-result slot */
}

typedef struct row_fixture_spec {
    const char *name;
    int atlases;
    int sources;
    int children;
    int overrides;
    bool long_keys;
} row_fixture_spec;

typedef struct row_fixture {
    row_fixture_spec spec;
    tp_session *session;
    tp_session_snapshot *snapshot;
    char dir[512];
    char project_path[512];
    size_t scan_bytes;
} row_fixture;

static int deterministic_fill(void *ctx, uint8_t *out, size_t len) {
    uint64_t *counter = (uint64_t *)ctx;
    const uint64_t value = (*counter)++;
    for (size_t i = 0U; i < len; ++i) {
        const unsigned shift = (unsigned)(i % sizeof value) * 8U;
        out[i] = (uint8_t)(value >> shift) ^ (uint8_t)(0xA5U + i);
    }
    return (int)len;
}

static int process_id(void) {
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

static char *bench_strdup(const char *text) {
    const size_t length = strlen(text) + 1U;
    char *copy = (char *)malloc(length);
    if (copy) {
        memcpy(copy, text, length);
    }
    return copy;
}

static int remove_empty_dir(const char *path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
}

static bool absolute_existing_dir(const char *input, char *out, size_t capacity) {
#ifdef _WIN32
    if (!_fullpath(out, input, capacity)) {
        return false;
    }
#else
    char *resolved = realpath(input, NULL);
    if (!resolved) {
        return false;
    }
    const size_t length = strlen(resolved);
    if (length >= capacity) {
        free(resolved);
        return false;
    }
    memcpy(out, resolved, length + 1U);
    free(resolved);
#endif
    normalize_slashes(out);
    return true;
}

static bool parse_iterations(const char *text, int *out) {
    if (!text || !out || text[0] == '\0') {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 1L || value > (long)TP_BENCH_MAX_SAMPLES) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool make_long_source_key(int index, char *out, size_t capacity) {
    const size_t prefix = 700U;
    if (!out || capacity <= prefix + 7U) {
        return false;
    }
    memset(out, 'a', prefix);
    const int written = snprintf(out + prefix, capacity - prefix,
                                 "_%d.png", index);
    return written > 0 && (size_t)written < capacity - prefix;
}

static void fixture_cleanup(row_fixture *fixture) {
    if (!fixture) {
        return;
    }
    if (s_fixture_snapshot == fixture->snapshot) {
        s_fixture_snapshot = NULL;
    }
    gui_scan_shutdown();
    tp_session_snapshot_destroy(fixture->snapshot);
    tp_session_destroy(fixture->session);
    fixture->snapshot = NULL;
    fixture->session = NULL;
    if (fixture->project_path[0] != '\0') {
        (void)remove(fixture->project_path);
        fixture->project_path[0] = '\0';
    }
    if (fixture->dir[0] != '\0') {
        (void)remove_empty_dir(fixture->dir);
        fixture->dir[0] = '\0';
    }
}

static bool fixture_prepare(row_fixture *fixture, row_fixture_spec spec, const char *scratch_root) {
    memset(fixture, 0, sizeof *fixture);
    fixture->spec = spec;
    if (spec.atlases < 1 || spec.sources < 1 || spec.children < 0 ||
        (spec.children > 0 && spec.sources != 1) || spec.overrides < 0 ||
        spec.overrides > spec.children) {
        return false;
    }
    int path_n = snprintf(fixture->dir, sizeof fixture->dir, "%s/%s_%d", scratch_root, spec.name, process_id());
    if (path_n < 0 || (size_t)path_n >= sizeof fixture->dir) {
        return false;
    }
    normalize_slashes(fixture->dir);
    tp_mkdirs(fixture->dir);
    if (!tp_scan_exists(fixture->dir) || !tp_scan_is_dir(fixture->dir)) {
        return false;
    }
    const int project_n = snprintf(fixture->project_path,
                                   sizeof fixture->project_path,
                                   "%s/fixture.ntpacker_project", fixture->dir);
    if (project_n < 0 || (size_t)project_n >= sizeof fixture->project_path) {
        fixture_cleanup(fixture);
        return false;
    }

    gui_scan_result seeded = {0};
    if (spec.children > 0) {
        seeded.entries = (gui_scan_entry *)calloc(
            (size_t)spec.children, sizeof *seeded.entries);
        if (!seeded.entries) {
            fixture_cleanup(fixture);
            return false;
        }
        seeded.count = spec.children;
        fixture->scan_bytes =
            (size_t)spec.children * sizeof *seeded.entries;
        for (int i = 0; i < spec.children; ++i) {
            char rel[TP_SRCKEY_MAX];
            char abs[TP_IDENTITY_PATH_MAX];
            const int rel_n = spec.long_keys
                                  ? (make_long_source_key(
                                         i, rel, sizeof rel)
                                         ? (int)strlen(rel)
                                         : -1)
                                  : snprintf(rel, sizeof rel,
                                             "sprite_%05d.png", i);
            const int abs_n = snprintf(abs, sizeof abs,
                                       "%s/sprite_%05d.png", fixture->dir, i);
            if (rel_n < 0 ||
                (size_t)rel_n >= sizeof rel || abs_n < 0 ||
                (size_t)abs_n >= sizeof abs) {
                tp_scan_free(&seeded);
                fixture_cleanup(fixture);
                return false;
            }
            seeded.entries[i].rel = bench_strdup(rel);
            seeded.entries[i].abs = bench_strdup(abs);
            if (!seeded.entries[i].rel || !seeded.entries[i].abs) {
                tp_scan_free(&seeded);
                fixture_cleanup(fixture);
                return false;
            }
            fixture->scan_bytes += strlen(rel) + 1U + strlen(abs) + 1U;
        }
    }

    tp_project *project = tp_project_create();
    if (!project) {
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    for (int i = 1; i < spec.atlases; ++i) {
        char atlas_name[64];
        const int atlas_name_n = snprintf(atlas_name, sizeof atlas_name,
                                          "atlas_%05d", i);
        if (atlas_name_n < 0 || (size_t)atlas_name_n >= sizeof atlas_name ||
            tp_project_add_atlas(project, atlas_name, NULL) != TP_STATUS_OK) {
            tp_project_destroy(project);
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }
    tp_project_atlas *atlas = &project->atlases[spec.atlases - 1];
    for (int i = 0; i < spec.sources; ++i) {
        char source_path[512];
        const tp_source_kind kind = spec.children > 0
                                        ? TP_SOURCE_KIND_FOLDER
                                        : TP_SOURCE_KIND_FILE;
        const int source_n = spec.children > 0
                                 ? snprintf(source_path, sizeof source_path,
                                            "%s", fixture->dir)
                                 : snprintf(source_path, sizeof source_path,
                                            "%s/missing_%05d.png", fixture->dir,
                                            i);
        if (source_n < 0 || (size_t)source_n >= sizeof source_path ||
            tp_project_atlas_add_source_kind(atlas, source_path, kind) !=
                TP_STATUS_OK) {
            tp_project_destroy(project);
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }
    tp_error err = {{0}};
    uint64_t counter = 1U;
    tp_rng rng = {deterministic_fill, &counter};
    if (tp_project_assign_missing_ids(project, &rng, &err) != TP_STATUS_OK) {
        tp_project_destroy(project);
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    for (int i = 0; i < spec.overrides; ++i) {
        char key[64];
        char rename[64];
        int key_n = snprintf(key, sizeof key, "sprite_%05d.png", i);
        int rename_n = snprintf(rename, sizeof rename, "renamed_%05d", i);
        tp_project_sprite *sprite = NULL;
        if (key_n < 0 || (size_t)key_n >= sizeof key || rename_n < 0 || (size_t)rename_n >= sizeof rename ||
            tp_project_atlas_add_sprite_by_source_key(
                atlas, atlas->sources[0].id, key, &sprite) != TP_STATUS_OK ||
            !sprite ||
            tp_project_atlas_set_sprite_rename_by_source_key(
                atlas, atlas->sources[0].id, key, rename) != TP_STATUS_OK) {
            tp_project_destroy(project);
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }
    if (tp_project_save(project, fixture->project_path, &err) != TP_STATUS_OK) {
        (void)fprintf(stderr, "tp_bench_gui_rows: project save failed: %s\n",
                      err.msg);
        tp_project_destroy(project);
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    tp_project_destroy(project);
    if (tp_session_open(fixture->project_path, &rng, &fixture->session,
                        &err) != TP_STATUS_OK ||
        tp_session_snapshot_create(fixture->session, &fixture->snapshot,
                                   &err) != TP_STATUS_OK) {
        (void)fprintf(stderr,
                      "tp_bench_gui_rows: session snapshot setup failed: %s\n",
                      err.msg);
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    s_fixture_snapshot = fixture->snapshot;
    s_sel_atlas = spec.atlases - 1;
    if (spec.children > 0) {
        const tp_snapshot_source *source = NULL;
        char resolved_dir[TP_IDENTITY_PATH_MAX];
        if (tp_session_snapshot_source_resolved_at(
                fixture->snapshot, s_sel_atlas, 0, &source, resolved_dir,
                sizeof resolved_dir, &err) != TP_STATUS_OK ||
            !source || !gui_scan_bench_seed_owned(resolved_dir, &seeded)) {
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }
    return true;
}

static int fixture_row_count(const row_fixture *fixture) {
    return fixture->spec.sources + fixture->spec.children;
}

static bool fixture_rows_match(const row_fixture *fixture) {
    const int expected_rows = fixture_row_count(fixture);
    if (s_status_failed || s_row_count != expected_rows ||
        !s_rows[0].is_source || s_rows[0].child != -1) {
        return false;
    }
    if (fixture->spec.children == 0) {
        return s_rows[0].missing && s_rows[expected_rows - 1].missing &&
               s_rows[expected_rows - 1].src == fixture->spec.sources - 1;
    }
    if (fixture->spec.long_keys) {
        char key0[TP_SRCKEY_MAX];
        char key1[TP_SRCKEY_MAX];
        if (!make_long_source_key(0, key0, sizeof key0) ||
            !make_long_source_key(1, key1, sizeof key1)) {
            return false;
        }
        return strcmp(s_rows[1].source_key, key0) == 0 &&
               strcmp(s_rows[2].source_key, key1) == 0 &&
               strcmp(s_rows[1].source_key, s_rows[2].source_key) != 0 &&
               strlen(s_rows[1].source_key) > 511U &&
               strlen(s_rows[1].sprite_name) > 511U &&
               strcmp(s_rows[1].sprite_name, s_rows[2].sprite_name) != 0;
    }
    char expected_last[64];
    (void)snprintf(expected_last, sizeof expected_last, "sprite_%05d",
                   fixture->spec.children - 1);
    if (!s_rows[0].is_folder ||
        strcmp(s_rows[expected_rows - 1].sprite_name, expected_last) != 0) {
        return false;
    }
    if (fixture->spec.overrides > 0) {
        const int child = fixture->spec.overrides - 1;
        char expected_label[96];
        (void)snprintf(expected_label, sizeof expected_label,
                       "renamed_%05d (sprite_%05d.png)", child, child);
        if (strcmp(s_rows[child + 1].label, expected_label) != 0) {
            return false;
        }
    }
    return true;
}

static bool fixture_publish_source_generation(row_fixture *fixture) {
    tp_error err = {{0}};
    if (tp_session_invalidate_sources(fixture->session, &err) != TP_STATUS_OK) {
        return false;
    }
    tp_session_snapshot *next = NULL;
    if (tp_session_snapshot_create(fixture->session, &next, &err) !=
        TP_STATUS_OK) {
        return false;
    }
    tp_session_snapshot_destroy(fixture->snapshot);
    fixture->snapshot = next;
    s_fixture_snapshot = next;
    s_fixture_snapshot_lifetime++;
    return true;
}

static bool fixture_replace_snapshot_same_generation(row_fixture *fixture) {
    tp_error err = {{0}};
    tp_session_snapshot *next = NULL;
    if (tp_session_snapshot_create(fixture->session, &next, &err) !=
        TP_STATUS_OK) {
        return false;
    }
    tp_session_snapshot_destroy(fixture->snapshot);
    fixture->snapshot = next;
    s_fixture_snapshot = next;
    s_fixture_snapshot_lifetime++;
    return true;
}

static bool run_fixture(row_fixture *fixture, int iterations) {
    const int warmup_iterations = 2;
    s_status_failed = false;
    for (int i = 0; i < warmup_iterations; ++i) {
        build_rows();
        if (!fixture_rows_match(fixture)) {
            (void)fprintf(stderr,
                          "tp_bench_gui_rows: warmup failed for %s "
                          "(iteration=%d status_failed=%d rows=%d expected=%d)\n",
                          fixture->spec.name, i, s_status_failed ? 1 : 0,
                          s_row_count, fixture_row_count(fixture));
            return false;
        }
    }
    if (fixture->spec.children > 0) {
        s_sel_src = 0;
        s_sel_child = fixture->spec.overrides > 0
                          ? fixture->spec.overrides - 1
                          : fixture->spec.children - 1;
        if (!gui_rows_selected_leaf()) {
            return false;
        }
        const tp_snapshot_sprite *selected_override =
            gui_rows_selected_override();
        if ((fixture->spec.overrides > 0) != (selected_override != NULL)) {
            return false;
        }
    }

    /* Save and identity-only paths may replace the immutable snapshot without
     * changing model/source generations. Prove borrowed override DTOs still
     * force exactly one row-cache rebuild on that lifetime transition. */
    if (!fixture_replace_snapshot_same_generation(fixture)) {
        return false;
    }
    gui_rows_bench_reset_counters();
    /* Accessors must detect a snapshot replacement that happens after the
     * frame's build_rows() call and rebuild before touching borrowed slots. */
    if (fixture->spec.children > 0) {
        if (!gui_rows_selected_leaf()) {
            return false;
        }
    } else {
        build_rows();
    }
    if (gui_rows_bench_get_counters().rebuilds != 1U ||
        !fixture_rows_match(fixture)) {
        (void)fprintf(stderr,
                      "tp_bench_gui_rows: snapshot lifetime rebuild failed for %s\n",
                      fixture->spec.name);
        return false;
    }
    if (fixture->spec.children > 0) {
        const tp_snapshot_sprite *selected_override =
            gui_rows_selected_override();
        if ((fixture->spec.overrides > 0) != (selected_override != NULL)) {
            return false;
        }
    }

    gui_rows_bench_reset_counters();
    gui_scan_bench_reset_counters();
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);
    (void)printf("\n== GUI rows %s ==\n", fixture->spec.name);
        (void)printf("   atlases=%d sources=%d children=%d rows=%d overrides=%d "
                 "scan_bytes=%zu row_capacity=%d warmup=%d iterations=%d\n",
                 fixture->spec.atlases, fixture->spec.sources, fixture->spec.children,
                 fixture_row_count(fixture), fixture->spec.overrides,
                 fixture->scan_bytes,
                 gui_rows_bench_get_counters().row_capacity,
                 warmup_iterations, iterations);
    (void)printf("   samples_ms:");
    for (int i = 0; i < iterations; ++i) {
        s_status_failed = false;
        const double start = tp_bench_now_ms();
        build_rows();
        if (fixture->spec.children > 0) {
            if (!gui_rows_selected_leaf()) {
                return false;
            }
            (void)gui_rows_selected_override();
        }
        const double elapsed = tp_bench_now_ms() - start;
        if (!tp_bench_samples_record(&samples, fixture_rows_match(fixture),
                                     elapsed)) {
            (void)printf(" FAILED\n");
            return false;
        }
        (void)printf(" %s%.6f", i == 0 ? "" : ",", elapsed);
    }
    (void)printf("\n");

    const gui_rows_bench_counters rows = gui_rows_bench_get_counters();
    const gui_scan_bench_counters scan = gui_scan_bench_get_counters();
    if (!tp_bench_samples_valid(&samples) ||
        rows.cache_key_checks != (uint64_t)iterations || rows.rebuilds != 0U ||
        rows.row_realloc_calls != 0U ||
        rows.override_index_realloc_calls != 0U ||
        rows.override_slot_clears != 0U ||
        rows.source_iterations != 0U || rows.path_resolve_calls != 0U ||
        rows.child_iterations != 0U || scan.directory_walks != 0U ||
        rows.selected_row_iterations != 0U ||
        (fixture->spec.children > 0 && rows.override_lookup_calls != 0U) ||
        scan.get_calls != 0U || scan.exists_fs_calls != 0U ||
        scan.is_dir_fs_calls != 0U) {
        (void)fprintf(stderr,
                      "tp_bench_gui_rows: unchanged-frame counter failure for %s\n",
                      fixture->spec.name);
        return false;
    }
    const double p50 = tp_bench_samples_percentile(&samples, 50U);
    const double p95 = tp_bench_samples_percentile(&samples, 95U);
    (void)printf("   steady_p50=%.6f ms steady_p95=%.6f ms accepted=%zu "
                 "failed=%zu key_checks=%llu row_heap_allocs=%llu fs_calls=0\n",
                 p50, p95, samples.count, samples.failed,
                 (unsigned long long)rows.cache_key_checks,
                 (unsigned long long)(rows.row_realloc_calls +
                                      rows.override_index_realloc_calls));

    /* Invalidation and snapshot replacement stay outside each timed rebuild. */
    tp_bench_samples rebuild_samples;
    tp_bench_samples_init(&rebuild_samples);
    gui_rows_bench_counters rebuild_rows = {0};
    gui_scan_bench_counters rebuild_scan = {0};
    const uint64_t linear_units = (uint64_t)fixture->spec.sources +
                                  (uint64_t)fixture->spec.children +
                                  (uint64_t)fixture->spec.overrides;
    const uint64_t expected_lookups = (uint64_t)fixture->spec.children +
                                      (fixture->spec.children > 0 ? 1U : 0U);
    const uint64_t expected_gets = fixture->spec.children > 0 ? 1U : 0U;
    const uint64_t expected_is_dir = fixture->spec.children > 0 ? 1U : 0U;
    (void)printf("   rebuild_samples_ms:");
    for (int i = 0; i < warmup_iterations + iterations; ++i) {
        if (!fixture_publish_source_generation(fixture)) {
            (void)fprintf(stderr,
                          "tp_bench_gui_rows: source generation failed for %s\n",
                          fixture->spec.name);
            return false;
        }
        gui_rows_bench_reset_counters();
        gui_scan_bench_reset_counters();
        s_status_failed = false;
        const double rebuild_start = tp_bench_now_ms();
        build_rows();
        if (fixture->spec.children > 0) {
            if (!gui_rows_selected_leaf()) {
                return false;
            }
            (void)gui_rows_selected_override();
        }
        const double rebuild_ms = tp_bench_now_ms() - rebuild_start;
        rebuild_rows = gui_rows_bench_get_counters();
        rebuild_scan = gui_scan_bench_get_counters();
        const bool rebuild_valid =
            fixture_rows_match(fixture) && rebuild_rows.cache_key_checks == 1U &&
            rebuild_rows.rebuilds == 1U && rebuild_rows.row_realloc_calls == 0U &&
            rebuild_rows.override_index_realloc_calls == 0U &&
            rebuild_rows.override_slot_clears == 0U &&
            rebuild_rows.source_iterations == (uint64_t)fixture->spec.sources &&
            rebuild_rows.path_resolve_calls == (uint64_t)fixture->spec.sources &&
            rebuild_rows.child_iterations == (uint64_t)fixture->spec.children &&
            rebuild_rows.override_inserts == (uint64_t)fixture->spec.overrides &&
            rebuild_rows.override_lookup_calls == expected_lookups &&
            rebuild_rows.override_probes <= linear_units * 8U &&
            rebuild_scan.get_calls == expected_gets &&
            rebuild_scan.exists_fs_calls == (uint64_t)fixture->spec.sources &&
            rebuild_scan.is_dir_fs_calls == expected_is_dir &&
            rebuild_scan.directory_walks == 0U;
        if (!rebuild_valid ||
            (i >= warmup_iterations &&
             !tp_bench_samples_record(&rebuild_samples, true, rebuild_ms))) {
            (void)printf(" FAILED\n");
            (void)fprintf(stderr,
                          "tp_bench_gui_rows: rebuild counter failure for %s\n",
                          fixture->spec.name);
            return false;
        }
        if (i >= warmup_iterations) {
            (void)printf(" %s%.6f", i == warmup_iterations ? "" : ",",
                         rebuild_ms);
        }
    }
    (void)printf("\n");
    (void)printf("   rebuild_p50=%.6f ms rebuild_p95=%.6f ms accepted=%zu "
                 "failed=%zu sources=%llu children=%llu "
                 "override_inserts=%llu override_lookups=%llu "
                 "override_probes=%llu linear_units=%llu row_heap_allocs=0\n",
                 tp_bench_samples_percentile(&rebuild_samples, 50U),
                 tp_bench_samples_percentile(&rebuild_samples, 95U),
                 rebuild_samples.count, rebuild_samples.failed,
                 (unsigned long long)rebuild_rows.source_iterations,
                 (unsigned long long)rebuild_rows.child_iterations,
                 (unsigned long long)rebuild_rows.override_inserts,
                 (unsigned long long)rebuild_rows.override_lookup_calls,
                 (unsigned long long)rebuild_rows.override_probes,
                 (unsigned long long)linear_units);
    return true;
}

int main(int argc, char **argv) {
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    (void)setvbuf(stderr, NULL, _IONBF, 0);
    const char *scratch_root = argc > 1 ? argv[1] : "tp_bench_gui_rows_tmp";
    int iterations = 20;
    if (argc > 3 || (argc > 2 && !parse_iterations(argv[2], &iterations))) {
        (void)fprintf(stderr, "usage: tp_bench_gui_rows [scratch_dir] [iterations 1..%u]\n",
                      (unsigned)TP_BENCH_MAX_SAMPLES);
        return 2;
    }
    tp_mkdirs(scratch_root);
    if (!tp_scan_exists(scratch_root) || !tp_scan_is_dir(scratch_root)) {
        (void)fprintf(stderr, "tp_bench_gui_rows: scratch directory unavailable: %s\n", scratch_root);
        return 1;
    }
    char scratch_absolute[TP_IDENTITY_PATH_MAX];
    if (!absolute_existing_dir(scratch_root, scratch_absolute,
                               sizeof scratch_absolute)) {
        (void)fprintf(stderr,
                      "tp_bench_gui_rows: scratch identity failed: %s\n",
                      scratch_root);
        return 1;
    }

    const row_fixture_spec specs[] = {
        {"LONG_KEYS", 1, 1, 2, 0, true},
        {"NORMAL_CHILDREN", 1, 1, 255, 64, false},
        {"MANY_SOURCES", 1, 4096, 0, 0, false},
        {"MANY_CHILDREN", 1, 1, 9999, 4096, false},
        {"MANY_ATLASES", 2048, 1, 255, 64, false},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        row_fixture fixture;
        if (!fixture_prepare(&fixture, specs[i], scratch_absolute)) {
            (void)fprintf(stderr, "tp_bench_gui_rows: fixture setup failed for %s\n", specs[i].name);
            ok = false;
            break;
        }
        if (!run_fixture(&fixture, iterations)) {
            ok = false;
        }
        fixture_cleanup(&fixture);
        if (!ok) {
            break;
        }
    }
    gui_rows_bench_shutdown();
    if (!ok) {
        return 1;
    }
    (void)printf("\ntp_bench_gui_rows: OK\n");
    return 0;
}
