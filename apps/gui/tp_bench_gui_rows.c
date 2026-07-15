/* M0 GUI row-rebuild benchmark. Compiles the production gui_rows.c and gui_scan.c
 * with narrow link stubs for unrelated selection/canvas state. No window, renderer,
 * packer job, or GUI event loop is created. */

#include "gui_pack.h"
#include "gui_rows.h"
#include "gui_scan.h"

#include "tp_bench_support.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"

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
char (*s_multi_sel)[192];
int s_multi_sel_count;
int s_multi_sel_cap;
int s_sel_atlas;
int s_sel_src = -1;
int s_sel_child = -1;
char s_sel_abs[512];
bool s_sel_missing;

static bool s_status_failed;
void set_status_ex(status_sev_t sev, const char *msg) {
    (void)msg;
    if (sev == STATUS_ERROR) {
        s_status_failed = true;
    }
}

const tp_result *gui_pack_result(int atlas_index) {
    (void)atlas_index;
    return NULL;
}

typedef struct row_fixture_spec {
    const char *name;
    int rows;
    int overrides;
} row_fixture_spec;

typedef struct row_fixture {
    row_fixture_spec spec;
    tp_project *project;
    char dir[512];
    size_t scan_bytes;
} row_fixture;

static int process_id(void) {
#ifdef _WIN32
    return _getpid();
#else
    return (int)getpid();
#endif
}

static int remove_empty_dir(const char *path) {
#ifdef _WIN32
    return _rmdir(path);
#else
    return rmdir(path);
#endif
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

static void fixture_cleanup(row_fixture *fixture) {
    if (!fixture) {
        return;
    }
    gui_scan_shutdown();
    tp_project_destroy(fixture->project);
    fixture->project = NULL;
    if (fixture->dir[0] != '\0') {
        (void)remove_empty_dir(fixture->dir);
        fixture->dir[0] = '\0';
    }
}

static bool fixture_prepare(row_fixture *fixture, row_fixture_spec spec, const char *scratch_root) {
    memset(fixture, 0, sizeof *fixture);
    fixture->spec = spec;
    const int children = spec.rows - 1;
    if (children < 1 || spec.overrides < 0 || spec.overrides > children) {
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

    gui_scan_result seeded = {0};
    seeded.entries = (gui_scan_entry *)calloc((size_t)children, sizeof *seeded.entries);
    if (!seeded.entries) {
        fixture_cleanup(fixture);
        return false;
    }
    seeded.count = children;
    fixture->scan_bytes = (size_t)children * sizeof *seeded.entries;
    for (int i = 0; i < children; i++) {
        int rel_n = snprintf(seeded.entries[i].rel, sizeof seeded.entries[i].rel, "sprite_%05d.png", i);
        int abs_n = snprintf(seeded.entries[i].abs, sizeof seeded.entries[i].abs, "%s/sprite_%05d.png",
                             fixture->dir, i);
        if (rel_n < 0 || (size_t)rel_n >= sizeof seeded.entries[i].rel || abs_n < 0 ||
            (size_t)abs_n >= sizeof seeded.entries[i].abs) {
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }

    fixture->project = tp_project_create();
    if (!fixture->project) {
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    tp_project_atlas *atlas = &fixture->project->atlases[0];
    if (tp_project_atlas_add_source_kind(atlas, fixture->dir, TP_SOURCE_KIND_FOLDER) != TP_STATUS_OK) {
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    for (int i = 0; i < spec.overrides; i++) {
        char key[64];
        char rename[64];
        int key_n = snprintf(key, sizeof key, "sprite_%05d", i);
        int rename_n = snprintf(rename, sizeof rename, "renamed_%05d", i);
        tp_project_sprite *sprite = NULL;
        if (key_n < 0 || (size_t)key_n >= sizeof key || rename_n < 0 || (size_t)rename_n >= sizeof rename ||
            tp_project_atlas_add_sprite(atlas, key, &sprite) != TP_STATUS_OK || !sprite ||
            tp_project_atlas_set_sprite_rename(atlas, key, rename) != TP_STATUS_OK) {
            tp_scan_free(&seeded);
            fixture_cleanup(fixture);
            return false;
        }
    }
    if (!gui_scan_bench_seed_owned(fixture->dir, &seeded)) {
        tp_scan_free(&seeded);
        fixture_cleanup(fixture);
        return false;
    }
    return true;
}

static bool run_fixture(row_fixture *fixture, int iterations) {
    tp_project_atlas *atlas = &fixture->project->atlases[0];
    const int warmup_iterations = 2;
    const int override_row = fixture->spec.overrides;
    char expected_override_name[64];
    char expected_override_label[96];
    char expected_last_name[64];
    (void)snprintf(expected_override_name, sizeof expected_override_name, "sprite_%05d", override_row - 1);
    (void)snprintf(expected_override_label, sizeof expected_override_label, "renamed_%05d (sprite_%05d.png)",
                   override_row - 1, override_row - 1);
    (void)snprintf(expected_last_name, sizeof expected_last_name, "sprite_%05d", fixture->spec.rows - 2);
    s_status_failed = false;
    for (int i = 0; i < warmup_iterations; i++) {
        build_rows(fixture->project, atlas);
        if (s_status_failed || s_row_count != fixture->spec.rows || !s_rows[0].is_source ||
            !s_rows[0].is_folder || s_rows[0].child != -1 ||
            strcmp(s_rows[override_row].sprite_name, expected_override_name) != 0 ||
            strcmp(s_rows[override_row].label, expected_override_label) != 0 ||
            strcmp(s_rows[s_row_count - 1].sprite_name, expected_last_name) != 0) {
            (void)fprintf(stderr,
                          "tp_bench_gui_rows: warmup failed for %s (iteration=%d status_failed=%d rows=%d "
                          "expected=%d)\n",
                          fixture->spec.name, i, s_status_failed ? 1 : 0, s_row_count, fixture->spec.rows);
            return false;
        }
    }

    gui_rows_bench_reset_counters();
    gui_scan_bench_reset_counters();
    tp_bench_samples samples;
    tp_bench_samples_init(&samples);

    (void)printf("\n== GUI rows %s ==\n", fixture->spec.name);
    (void)printf("   sources=1 children=%d rows=%d overrides=%d scan_bytes=%zu row_capacity=%d warmup=%d iterations=%d\n",
                 fixture->spec.rows - 1, fixture->spec.rows, fixture->spec.overrides, fixture->scan_bytes,
                 gui_rows_bench_get_counters().row_capacity, warmup_iterations, iterations);
    (void)printf("   samples_ms:");
    for (int i = 0; i < iterations; i++) {
        s_status_failed = false;
        const double start = tp_bench_now_ms();
        build_rows(fixture->project, atlas);
        const double elapsed = tp_bench_now_ms() - start;
        const bool ok = !s_status_failed && s_row_count == fixture->spec.rows && s_rows[0].is_source &&
                        s_rows[0].is_folder && s_rows[0].child == -1 &&
                        strcmp(s_rows[override_row].sprite_name, expected_override_name) == 0 &&
                        strcmp(s_rows[override_row].label, expected_override_label) == 0 &&
                        strcmp(s_rows[s_row_count - 1].sprite_name, expected_last_name) == 0;
        if (!tp_bench_samples_record(&samples, ok, elapsed)) {
            (void)printf(" FAILED\n");
            return false;
        }
        (void)printf(" %s%.6f", i == 0 ? "" : ",", elapsed);
    }
    (void)printf("\n");

    const gui_rows_bench_counters rows = gui_rows_bench_get_counters();
    const gui_scan_bench_counters scan = gui_scan_bench_get_counters();
    if (!tp_bench_samples_valid(&samples) || rows.row_realloc_calls != 0U || scan.directory_walks != 0U ||
        scan.get_calls != (uint64_t)iterations || scan.exists_fs_calls != (uint64_t)iterations ||
        scan.is_dir_fs_calls != (uint64_t)iterations) {
        (void)fprintf(stderr, "tp_bench_gui_rows: counter/postcondition failure for %s\n", fixture->spec.name);
        return false;
    }
    const double p50 = tp_bench_samples_percentile(&samples, 50U);
    const double p95 = tp_bench_samples_percentile(&samples, 95U);
    (void)printf("   p50=%.6f ms p95=%.6f ms accepted=%zu failed=%zu row_reallocs=%llu fs_exists=%llu "
                 "fs_is_dir=%llu directory_walks=%llu\n",
                 p50, p95, samples.count, samples.failed, (unsigned long long)rows.row_realloc_calls,
                 (unsigned long long)scan.exists_fs_calls, (unsigned long long)scan.is_dir_fs_calls,
                 (unsigned long long)scan.directory_walks);
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

    const row_fixture_spec specs[] = {
        {"NORMAL", 256, 64},
        {"LARGE", 4096, 1024},
        {"HUGE", 10000, 9999},
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof specs / sizeof specs[0]; i++) {
        row_fixture fixture;
        if (!fixture_prepare(&fixture, specs[i], scratch_root)) {
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
