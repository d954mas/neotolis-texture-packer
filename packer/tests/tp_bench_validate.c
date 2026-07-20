#include <stdio.h>
#include <stdlib.h>

#include "time/nt_time.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_validate.h"
#include "tp_validate_internal.h"

#define VALIDATE_ROWS 1024U
#define VALIDATE_PROBES_PER_ROW_MAX 64U
#define SOURCE_ROWS 256U
#define SOURCE_PROBES_PER_ROW_MAX 64U
#define TARGET_ROWS 4096U
#define TARGET_PROBES_PER_ROW_MAX 32U

static int write_fixture(const char *root, char *project_path, size_t project_path_cap) {
    char sprites_dir[512];
    if ((size_t)snprintf(sprites_dir, sizeof sprites_dir, "%s/sprites", root) >= sizeof sprites_dir ||
        (size_t)snprintf(project_path, project_path_cap, "%s/large.ntpacker_project", root) >= project_path_cap) {
        return 0;
    }
    tp_mkdirs(sprites_dir);
    for (size_t i = 0; i < VALIDATE_ROWS; i++) {
        char path[512];
        if ((size_t)snprintf(path, sizeof path, "%s/sprite_%04zu.png", sprites_dir, i) >= sizeof path) {
            return 0;
        }
        FILE *sprite = fopen(path, "wb");
        if (!sprite || fclose(sprite) != 0) {
            return 0;
        }
    }

    FILE *project = fopen(project_path, "wb");
    if (!project) {
        return 0;
    }
    if (fputs("{\"version\":5,\"atlases\":[{"
              "\"id\":\"atlas_00000000000000000000000000000001\","
              "\"name\":\"large\",\"sources\":[{"
              "\"id\":\"source_00000000000000000000000000000002\","
              "\"path\":\"sprites\"}],\"sprites\":[", project) < 0) {
        (void)fclose(project);
        return 0;
    }
    for (size_t i = 0; i < VALIDATE_ROWS; i++) {
        if (fprintf(project,
                    "%s{\"source\":\"source_00000000000000000000000000000002\","
                    "\"key\":\"sprite_%04zu.png\",\"rename\":\"same\"}",
                    i == 0U ? "" : ",", i) <= 0) {
            (void)fclose(project);
            return 0;
        }
    }
    if (fputs("],\"animations\":[{"
              "\"id\":\"anim_00000000000000000000000000000003\","
              "\"name\":\"run\",\"frames\":[", project) < 0) {
        (void)fclose(project);
        return 0;
    }
    for (size_t i = 0; i < VALIDATE_ROWS; i++) {
        if (fprintf(project,
                    "%s{\"source\":\"source_00000000000000000000000000000002\","
                    "\"key\":\"sprite_%04zu.png\"}",
                    i == 0U ? "" : ",", i) <= 0) {
            (void)fclose(project);
            return 0;
        }
    }
    if (fputs("]}]}]}\n", project) < 0 || fclose(project) != 0) {
        return 0;
    }
    return 1;
}

static int write_source_fixture(const char *root, char *project_path, size_t project_path_cap) {
    if ((size_t)snprintf(project_path, project_path_cap, "%s/source-collisions.ntpacker_project", root) >= project_path_cap) {
        return 0;
    }
    FILE *project = fopen(project_path, "wb");
    if (!project ||
        fputs("{\"version\":5,\"atlases\":[{"
              "\"id\":\"atlas_00000000000000000000000000000011\","
              "\"name\":\"sources\",\"sources\":[", project) < 0) {
        if (project) {
            (void)fclose(project);
        }
        return 0;
    }
    for (size_t i = 0; i < SOURCE_ROWS; i++) {
        char name[] = "missingabcdefgh";
        for (size_t bit = 0; bit < 8U; bit++) {
            if ((i & ((size_t)1U << bit)) != 0U) {
                name[bit] = (char)(name[bit] - ('a' - 'A'));
            }
        }
        if (fprintf(project,
                    "%s{\"id\":\"source_%032zx\",\"path\":\"%s\"}",
                    i == 0U ? "" : ",", i + 0x100U, name) <= 0) {
            (void)fclose(project);
            return 0;
        }
    }
    if (fputs("]}]}\n", project) < 0 || fclose(project) != 0) {
        return 0;
    }
    return 1;
}

static int write_target_fixture(const char *root, char *project_path,
                                size_t project_path_cap) {
    if ((size_t)snprintf(project_path, project_path_cap,
                         "%s/unique-targets.ntpacker_project", root) >=
        project_path_cap) {
        return 0;
    }
    FILE *project = fopen(project_path, "wb");
    if (!project ||
        fputs("{\"version\":5,\"atlases\":[{"
              "\"id\":\"atlas_00000000000000000000000000000021\","
              "\"name\":\"targets\",\"targets\":[",
              project) < 0) {
        if (project) {
            (void)fclose(project);
        }
        return 0;
    }
    for (size_t i = 0U; i < TARGET_ROWS; ++i) {
        if (fprintf(project,
                    "%s{\"id\":\"target_%032zx\","
                    "\"exporter_id\":\"json-neotolis\","
                    "\"out_path\":\"out/%04zu\"}",
                    i == 0U ? "" : ",", i + 0x1000U, i) <= 0) {
            (void)fclose(project);
            return 0;
        }
    }
    return fputs("]}]}\n", project) >= 0 && fclose(project) == 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        (void)fprintf(stderr, "usage: tp_bench_validate <scratch_dir>\n");
        return 2;
    }
    char project_path[512];
    if (!write_fixture(argv[1], project_path, sizeof project_path)) {
        (void)fprintf(stderr, "tp_bench_validate: fixture setup failed\n");
        return 3;
    }

    tp_validation_report report = {0};
    tp_error err = {0};
    tp_validate__test_work_reset();
    const double start = nt_time_now();
    const tp_status status = tp_validate_project_file(project_path, &report, &err);
    const double elapsed_ms = (nt_time_now() - start) * 1000.0;
    const tp_validate_work_stats work = tp_validate__test_work_get();
    if (status != TP_STATUS_OK) {
        (void)fprintf(stderr, "tp_bench_validate: validate failed: %s\n", err.msg);
        return 4;
    }
    if (report.total_finding_count != 1U || report.finding_count != 1U ||
        report.findings[0].severity != TP_VALIDATION_ERROR ||
        work.probes > VALIDATE_ROWS * VALIDATE_PROBES_PER_ROW_MAX) {
        (void)fprintf(stderr,
                      "tp_bench_validate: gate failed rows=%u findings=%zu probes=%zu limit=%u\n",
                      VALIDATE_ROWS, report.total_finding_count, work.probes,
                      VALIDATE_ROWS * VALIDATE_PROBES_PER_ROW_MAX);
        tp_validation_report_free(&report);
        return 5;
    }
    (void)printf("tp_bench_validate rows=%u findings=%zu probes=%zu probe_limit=%u elapsed_ms=%.3f\n",
                 VALIDATE_ROWS, report.total_finding_count, work.probes,
                 VALIDATE_ROWS * VALIDATE_PROBES_PER_ROW_MAX, elapsed_ms);
    tp_validation_report_free(&report);

    if (!write_source_fixture(argv[1], project_path, sizeof project_path)) {
        (void)fprintf(stderr, "tp_bench_validate: source fixture setup failed\n");
        return 6;
    }
    tp_validate__test_work_reset();
    const double source_start = nt_time_now();
    const tp_status source_status = tp_validate_project_file(project_path, &report, &err);
    const double source_elapsed_ms = (nt_time_now() - source_start) * 1000.0;
    const tp_validate_work_stats source_work = tp_validate__test_work_get();
    const size_t expected_findings = SOURCE_ROWS + (SOURCE_ROWS * (SOURCE_ROWS - 1U)) / 2U + 1U;
    if (source_status != TP_STATUS_OK || report.total_finding_count != expected_findings || !report.truncated ||
        source_work.probes > SOURCE_ROWS * SOURCE_PROBES_PER_ROW_MAX) {
        (void)fprintf(stderr,
                      "tp_bench_validate: source gate failed rows=%u status=%d findings=%zu expected=%zu "
                      "probes=%zu limit=%u\n",
                      SOURCE_ROWS, (int)source_status, report.total_finding_count, expected_findings,
                      source_work.probes, SOURCE_ROWS * SOURCE_PROBES_PER_ROW_MAX);
        tp_validation_report_free(&report);
        return 7;
    }
    (void)printf("tp_bench_validate source_rows=%u findings=%zu probes=%zu probe_limit=%u elapsed_ms=%.3f\n",
                 SOURCE_ROWS, report.total_finding_count, source_work.probes,
                 SOURCE_ROWS * SOURCE_PROBES_PER_ROW_MAX, source_elapsed_ms);
    tp_validation_report_free(&report);

    if (!write_target_fixture(argv[1], project_path, sizeof project_path)) {
        (void)fprintf(stderr, "tp_bench_validate: target fixture setup failed\n");
        return 8;
    }
    tp_validate__test_work_reset();
    const double target_start = nt_time_now();
    const tp_status target_status =
        tp_validate_project_file(project_path, &report, &err);
    const double target_elapsed_ms = (nt_time_now() - target_start) * 1000.0;
    const tp_validate_work_stats target_work = tp_validate__test_work_get();
    if (target_status != TP_STATUS_OK || report.total_finding_count != 1U ||
        target_work.probes > TARGET_ROWS * TARGET_PROBES_PER_ROW_MAX) {
        (void)fprintf(stderr,
                      "tp_bench_validate: target gate failed rows=%u status=%d findings=%zu "
                      "probes=%zu limit=%u\n",
                      TARGET_ROWS, (int)target_status, report.total_finding_count,
                      target_work.probes,
                      TARGET_ROWS * TARGET_PROBES_PER_ROW_MAX);
        tp_validation_report_free(&report);
        return 9;
    }
    (void)printf("tp_bench_validate target_rows=%u findings=%zu probes=%zu "
                 "probe_limit=%u elapsed_ms=%.3f\n",
                 TARGET_ROWS, report.total_finding_count, target_work.probes,
                 TARGET_ROWS * TARGET_PROBES_PER_ROW_MAX, target_elapsed_ms);
    tp_validation_report_free(&report);
    return 0;
}
