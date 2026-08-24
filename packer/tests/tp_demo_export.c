/* tp_demo_export -- live-session Export dev driver (NOT a ctest).
 *
 * Opens one saved `.ntpacker_project`, starts the public live-session Export
 * command for every eligible atlas, and drains its one typed terminal receipt
 * through tp_session_update. It deliberately performs no project mutation,
 * source expansion, packing, or export orchestration of its own. This keeps the
 * driver useful as a small smoke tool while making it the live-headless half of
 * the ordinary-CLI parity test.
 *
 * Usage: tp_demo_export <path/to.ntpacker_project> [work_dir]
 *   work_dir (default ".") holds only the live job's private request files;
 *   target outputs land at the saved project's configured out_paths.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "tinycthread.h"

#include "core/nt_assert.h"
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"

static int run_live_export(const char *project_path, const char *work_dir,
                           bool json) {
    tp_rng rng = tp_rng_os();
    tp_session *session = NULL;
    tp_error error = {{0}};
    tp_status status =
        tp_session_open(project_path, &rng, &session, &error);
    if (status != TP_STATUS_OK) {
        (void)fprintf(
            stderr, "cannot open project '%s': %s (%s)\n",
            project_path, tp_status_str(status), error.msg);
        return 1;
    }

    const tp_export_command_request request = {
        .work_dir = work_dir,
        .atlas_id = tp_id128_nil(),
    };
    status = tp_session_export_start(session, &request, &error);
    if (status != TP_STATUS_OK) {
        (void)fprintf(
            stderr, "cannot start live Export: %s (%s)\n",
            tp_status_str(status), error.msg);
        tp_session_destroy(session);
        return 1;
    }

    tp_session_job_result result = {0};
    bool terminal = false;
    for (long spin = 0; spin < 10000000L; ++spin) {
        status = tp_session_update(session, &result, &error);
        if (status != TP_STATUS_OK) {
            (void)fprintf(
                stderr, "live Export update failed: %s (%s)\n",
                tp_status_str(status), error.msg);
            break;
        }
        if (result.kind != TP_SESSION_JOB_NONE) {
            terminal = true;
            break;
        }
        thrd_yield();
    }

    int rc = 1;
    if (!terminal) {
        if (status == TP_STATUS_OK) {
            (void)fprintf(
                stderr, "live Export did not publish a terminal receipt\n");
        }
    } else if (result.kind != TP_SESSION_JOB_EXPORT) {
        (void)fprintf(
            stderr, "live Export published unexpected job kind %d\n",
            (int)result.kind);
    } else {
        const tp_export_command_report *report =
            result.export_result.report;
        NT_ASSERT(report != NULL);
        if (json) {
            (void)printf(
                "{\"schema\":1,\"state\":%d,\"status\":%d,"
                "\"rejection\":%d,\"targets\":%d,\"files\":%d,"
                "\"notices\":%d,\"atlases_ok\":%d,"
                "\"atlases_failed\":%d,\"atlases_skipped\":%d,"
                "\"partial_publication\":%s,"
                "\"publication_uncertain\":%s}\n",
                (int)result.state,
                (int)result.status,
                (int)result.rejection,
                report->targets_ok,
                report->files_written,
                report->notices,
                report->atlases_ok,
                report->atlases_failed,
                report->atlases_skipped,
                report->partial_publication
                    ? "true"
                    : "false",
                report->publication_uncertain
                    ? "true"
                    : "false");
        }
        if (result.state !=
                TP_SESSION_JOB_SUCCEEDED ||
            result.status != TP_STATUS_OK) {
            (void)fprintf(
                stderr,
                "live Export failed: %s (%s)\n",
                tp_status_str(result.status),
                result.error.msg);
        } else if (!json) {
            (void)printf(
                "live Export: %d target(s), %d file(s), %d notice(s), "
                "%d atlas(es) ok, %d failed, %d skipped\n",
                report->targets_ok,
                report->files_written,
                report->notices,
                report->atlases_ok,
                report->atlases_failed,
                report->atlases_skipped);
        }
        if (result.state ==
                TP_SESSION_JOB_SUCCEEDED &&
            result.status == TP_STATUS_OK) {
            rc = 0;
        }
    }

    tp_session_job_result_destroy(&result);
    tp_session_destroy(session);
    return rc;
}

int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    if (argc < 2) {
        (void)fprintf(
            stderr,
            "usage: %s <project.ntpacker_project> [work_dir]\n",
            argv[0]);
        return 2;
    }

    bool json = false;
    const char *work_dir = ".";
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(work_dir, ".") == 0) {
            work_dir = argv[i];
        } else {
            (void)fprintf(
                stderr,
                "usage: %s <project.ntpacker_project> [work_dir] [--json]\n",
                argv[0]);
            return 2;
        }
    }
    tp_mkdirs(work_dir);
    const int rc = run_live_export(
        argv[1], work_dir, json);
    if (rc == 0 && !json) {
        (void)printf("tp_demo_export: OK\n");
    }
    return rc;
}
