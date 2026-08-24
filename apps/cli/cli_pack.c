/* `ntpacker pack <project> [--atlas <name>] [--target <id>] [--out-dir <dir>]
 *                          [--json] [--quiet]`   (alias: `export`).  Plan B3a.
 *
 * Packs + exports every ENABLED target of every atlas through the synchronous
 * snapshot facade over the same outer worker/controller/report assembler used
 * by the GUI's live Export. Thin client: no worker-frame, binding, Lua, name,
 * descriptor, or exporter policy here (boundary gates R1-R3), just arguments
 * and typed-report presentation.
 *
 * Filters (captured in the immutable worker request, never written to the project):
 *   --atlas <name>  only that atlas runs (unknown name -> usage error, exit 2).
 *   --target <id>   only targets with that exporter id run (others disabled).
 *   --out-dir <dir> RELATIVE target out_paths are re-rooted under <dir> (resolved
 *                   against the CWD); absolute out_paths are left untouched.
 *   --dry-run       Pack + predict, write NO files (no mkdirs either). Each target
 *                   reports would_write + predicted-loss notices; report.dry_run=true.
 *
 * Exit codes (cli_exit.h): 0 all ok; 3 project load; 4 pack failure (nothing
 * produced); 5 export/writer failure (nothing produced); 6 partial (some targets
 * ok, some failed). An atlas with no usable images or no enabled targets is a
 * warning + a report note, never a failure (an agent should not hard-fail a
 * preview-only atlas). Report schema + --out-dir semantics: docs/formats/cli-report.md.
 */
#include "cli_cmds.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"
#ifdef _WIN32
#include "nt_utf8_argv.h"
#else
#include <unistd.h>
#endif

#include "app_scratch.h"
#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_job.h"
#include "tp_core/tp_session.h"

#define CLI_PACK_SCHEMA 2

static bool path_is_abs(const char *p) {
    if (!p || !p[0]) {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
#ifdef _WIN32
    if (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':') {
        return true;
    }
#endif
    return false;
}

/* Makes `p` absolute against the current working directory (path need not
 * exist). Windows retrieves the directory through the UTF-16 OS boundary. */
static tp_status abspath_cwd(const char *p, char *out, size_t cap,
                             tp_error *err) {
    if (!p || !out || cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid output-directory path");
    }
    if (path_is_abs(p)) {
        const int copied = snprintf(out, cap, "%s", p);
        return copied >= 0 && (size_t)copied < cap
                   ? TP_STATUS_OK
                   : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                  "output-directory path is too long");
    }
    char cwd[TP_IDENTITY_PATH_MAX];
#ifdef _WIN32
    char platform_error[160] = {0};
    if (!nt_win_current_directory_utf8(cwd, sizeof cwd, platform_error,
                                       sizeof platform_error)) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED, "%s",
                            platform_error);
    }
#else
    if (!getcwd(cwd, sizeof cwd)) {
        return tp_error_set(err, TP_STATUS_PATH_RESOLVE_FAILED,
                            "could not read the current directory");
    }
#endif
    const int joined = snprintf(out, cap, "%s/%s", cwd, p);
    return joined >= 0 && (size_t)joined < cap
               ? TP_STATUS_OK
               : tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                              "absolute output-directory path is too long");
}

/* One `ntpacker pack` process serves exactly one request, so its request id is
 * fixed; the pid in the directory name is what separates concurrent runs. */
#define CLI_PACK_REQUEST_ID UINT64_C(1)

/* A PRIVATE work dir for this run's session .ntpack files, under the shared app
 * scratch root (apps/common/app_scratch.h). Not the system temp dir: Linux /tmp
 * is usually a RAM-backed tmpfs, so a large artifact would be charged to RAM,
 * and Windows disk cleaners may remove files mid-run. Not a shared directory
 * either: two concurrent runs used to write the same `<atlas>.ntpack`, and
 * tp_pack does not delete the artifact it produced, so the shared directory
 * also accumulated one file per atlas forever. The run releases this directory
 * on every exit path through app_scratch_request_dir_release(). */
static tp_status cli_work_dir(char *out, size_t cap, tp_error *err) {
    if (!out || cap == 0U) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid pack work-directory output");
    }
    char root[TP_IDENTITY_PATH_MAX];
    const tp_status status = app_scratch_root(root, sizeof root, err);
    if (status != TP_STATUS_OK) {
        out[0] = '\0';
        return status;
    }
    return app_scratch_request_dir(root, CLI_PACK_REQUEST_ID, out, cap, err);
}

/* Stable string names for the structured notice enums (JSON is machine-readable:
 * an agent matches on these tokens, not on prose). */
static const char *notice_field_name(int field_id) {
    switch (field_id) {
        case TP_NOTICE_FIELD_TRANSFORM: return "transform";
        case TP_NOTICE_FIELD_POLYGON: return "polygon";
        case TP_NOTICE_FIELD_SLICE9: return "slice9";
        case TP_NOTICE_FIELD_PIVOT: return "pivot";
        case TP_NOTICE_FIELD_ALIAS: return "alias";
        case TP_NOTICE_FIELD_MULTIPAGE: return "multipage";
        case TP_NOTICE_FIELD_ANIMATION: return "animation";
        default: return "none";
    }
}

static const char *notice_reason_name(int reason_id) {
    switch (reason_id) {
        case TP_NOTICE_REASON_CAPS_UNSUPPORTED: return "caps_unsupported";
        default: return "none";
    }
}

/* ------------------------------------------------------------------ */
/* JSON emission                                                      */
/* ------------------------------------------------------------------ */

static void emit_pages(tp_sb *sb, int depth, const tp_export_report_run *run) {
    if (!run || run->page_count == 0) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < run->page_count; i++) {
        const tp_export_report_page *pg = &run->pages[i];
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 1);
        bool pf = true;
        tp_sb_char(sb, '{');
        tp_obj_key(sb, depth + 2, &pf, "index");
        tp_sb_int(sb, pg->index);
        tp_obj_key(sb, depth + 2, &pf, "w");
        tp_sb_int(sb, pg->w);
        tp_obj_key(sb, depth + 2, &pf, "h");
        tp_sb_int(sb, pg->h);
        tp_obj_key(sb, depth + 2, &pf, "occupancy_pct");
        tp_sb_num(sb, pg->occupancy_pct);
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, '}');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, ']');
}

static void emit_notice(tp_sb *sb, int depth, const tp_export_notice *nt) {
    bool nf = true;
    tp_sb_char(sb, '{');
    tp_obj_key(sb, depth + 1, &nf, "field");
    tp_sb_json_string(sb, notice_field_name(nt->field_id));
    tp_obj_key(sb, depth + 1, &nf, "reason");
    tp_sb_json_string(sb, notice_reason_name(nt->reason_id));
    if (nt->sprite) {
        tp_obj_key(sb, depth + 1, &nf, "sprite");
        tp_sb_json_string(sb, nt->sprite);
    }
    if (nt->target) {
        tp_obj_key(sb, depth + 1, &nf, "target");
        tp_sb_json_string(sb, nt->target);
    }
    tp_obj_key(sb, depth + 1, &nf, "message");
    tp_sb_json_string(sb, nt->msg);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void emit_str_array(tp_sb *sb, int depth, const char *const *items, int count) {
    if (count <= 0 || !items) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < count; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_json_string(sb, items[i]);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, ']');
}

static void emit_target(tp_sb *sb, int depth, const tp_export_report_target *rt, const tp_export_notices *notices,
                        bool dry_run) {
    bool tf = true;
    tp_sb_char(sb, '{');
    tp_obj_key(sb, depth + 1, &tf, "exporter_id");
    tp_sb_json_string(sb, rt->exporter_id ? rt->exporter_id : "");
    tp_obj_key(sb, depth + 1, &tf, "out_path");
    tp_sb_json_string(sb, rt->out_path ? rt->out_path : "");
    tp_obj_key(sb, depth + 1, &tf, "pack_run");
    tp_sb_int(sb, rt->pack_run);
    tp_obj_key(sb, depth + 1, &tf, "status");
    tp_sb_json_string(sb, rt->ok ? "ok" : "failed");
    tp_obj_key(sb, depth + 1, &tf, "publication_uncertain");
    tp_sb_str(sb, rt->publication_uncertain ? "true" : "false");
    tp_obj_key(sb, depth + 1, &tf, "format_diagnostics");
    cli_out_append_format_diagnostics(sb, depth + 1,
                                      rt->format_diagnostics);
    tp_obj_key(sb, depth + 1, &tf, "format_diagnostics_truncated");
    tp_sb_str(sb,
              tp_format_diagnostic_report_truncated(rt->format_diagnostics)
                  ? "true"
                  : "false");
    if (!rt->ok) {
        tp_obj_key(sb, depth + 1, &tf, "error");
        tp_sb_json_string(sb, rt->error ? rt->error
                                      : "export target failed (error detail unavailable)");
    }
    /* written_files is always present (empty on a dry run); would_write is added
     * only on a dry run -- the paths that WOULD be produced (docs/formats/cli-report.md). */
    tp_obj_key(sb, depth + 1, &tf, "written_files");
    emit_str_array(sb, depth + 1, rt->written_files, rt->written_file_count);
    if (dry_run) {
        tp_obj_key(sb, depth + 1, &tf, "would_write");
        emit_str_array(sb, depth + 1, rt->would_write, rt->would_write_count);
    }

    tp_obj_key(sb, depth + 1, &tf, "notices");
    int nb = rt->notice_begin;
    int ne = rt->notice_end;
    if (!notices || ne <= nb) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = nb; i < ne && i < notices->count; i++) {
            tp_sb_str(sb, i == nb ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            emit_notice(sb, depth + 2, &notices->items[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* Emits one atlas object. `report` may be NULL (a skipped atlas); `note` (nullable)
 * records why it was skipped. `pages` uses the PRIMARY pack run (runs[0]); a target
 * on a different run is flagged by its own `pack_run` index. */
static void emit_atlas(tp_sb *sb, int depth, const char *name, int sprite_count, int missing_sources,
                       const tp_export_report *report, const tp_export_notices *notices,
                       const char *skip_notice_id, const char *note,
                       tp_status error_status, const tp_error *error,
                       bool dry_run) {
    bool af = true;
    tp_sb_char(sb, '{');
    tp_obj_key(sb, depth + 1, &af, "name");
    tp_sb_json_string(sb, name);
    tp_obj_key(sb, depth + 1, &af, "sprite_count");
    tp_sb_int(sb, sprite_count);
    tp_obj_key(sb, depth + 1, &af, "missing_sources");
    tp_sb_int(sb, missing_sources);
    if (note) {
        tp_obj_key(sb, depth + 1, &af, "note");
        tp_sb_json_string(sb, note);
    }
    if (skip_notice_id) {
        tp_obj_key(sb, depth + 1, &af, "notices");
        tp_sb_str(sb, "[\n");
        tp_sb_indent(sb, depth + 2);
        bool nf = true;
        tp_sb_char(sb, '{');
        tp_obj_key(sb, depth + 3, &nf, "id");
        tp_sb_json_string(sb, skip_notice_id);
        tp_obj_key(sb, depth + 3, &nf, "atlas");
        tp_sb_json_string(sb, name);
        tp_obj_key(sb, depth + 3, &nf, "message");
        tp_sb_json_string(sb, note ? note : "");
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 2);
        tp_sb_str(sb, "}\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (error_status != TP_STATUS_OK) {
        tp_obj_key(sb, depth + 1, &af, "error");
        bool ef = true;
        tp_sb_char(sb, '{');
        tp_obj_key(sb, depth + 2, &ef, "id");
        tp_sb_json_string(sb, tp_status_id(error_status));
        tp_obj_key(sb, depth + 2, &ef, "atlas");
        tp_sb_json_string(sb, name);
        tp_obj_key(sb, depth + 2, &ef, "message");
        tp_sb_json_string(
            sb, error && error->msg[0] ? error->msg
                                      : tp_status_str(error_status));
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, '}');
    }
    tp_obj_key(sb, depth + 1, &af, "pack_runs");
    tp_sb_int(sb, report ? report->run_count : 0);

    tp_obj_key(sb, depth + 1, &af, "pages");
    const tp_export_report_run *primary = (report && report->run_count > 0) ? &report->runs[0] : NULL;
    emit_pages(sb, depth + 1, primary);

    tp_obj_key(sb, depth + 1, &af, "targets");
    if (!report || report->target_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < report->target_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            emit_target(sb, depth + 2, &report->targets[i], notices, dry_run);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }

    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* ------------------------------------------------------------------ */
/* pack                                                               */
/* ------------------------------------------------------------------ */

/* Emits the stderr progress + notice lines for one completed atlas (human aid;
 * suppressed by --quiet). JSON payload stays on stdout, untouched. */
static void report_progress(const char *name, const tp_export_report *report, const tp_export_notices *notices,
                            bool json) {
    if (!report) {
        return;
    }
    const bool dry = report->dry_run;
    for (int i = 0; i < report->target_count; i++) {
        const tp_export_report_target *rt = &report->targets[i];
        if (rt->ok && dry) {
            (void)fprintf(stderr, "ntpacker: %s / %s: would write %d file%s (dry-run)\n", name,
                          rt->exporter_id ? rt->exporter_id : "?", rt->would_write_count,
                          rt->would_write_count == 1 ? "" : "s");
        } else if (rt->ok) {
            (void)fprintf(stderr, "ntpacker: %s / %s: ok (%d file%s)\n", name, rt->exporter_id ? rt->exporter_id : "?",
                          rt->written_file_count, rt->written_file_count == 1 ? "" : "s");
        } else {
            (void)fprintf(stderr, "ntpacker: %s / %s: FAILED: %s\n", name, rt->exporter_id ? rt->exporter_id : "?",
                          rt->error ? rt->error : "export failed");
        }
    }
    /* In human mode the full notice list also goes to stderr (the A0 fix: notices
     * finally reach users). In --json they live in the payload, so skip here. */
    if (!json && notices) {
        for (int i = 0; i < notices->count; i++) {
            (void)fprintf(stderr, "ntpacker: notice: %s\n", notices->items[i].msg);
        }
    }
}

/* Human summary lines for one atlas (stdout). On a dry run each ok target reports
 * the count it WOULD write instead of a written count. */
static void print_atlas_human(const char *name, int sprite_count, int missing_sources,
                              const tp_export_report *report, const char *note, bool dry_run) {
    if (note) {
        (void)printf("atlas '%s': %s\n", name, note);
        return;
    }
    int pages = (report && report->run_count > 0) ? report->runs[0].page_count : 0;
    (void)printf("atlas '%s': %d sprite%s, %d page%s%s\n", name, sprite_count, sprite_count == 1 ? "" : "s", pages,
                 pages == 1 ? "" : "s", missing_sources > 0 ? " (missing sources skipped)" : "");
    if (!report) {
        return;
    }
    for (int i = 0; i < report->target_count; i++) {
        const tp_export_report_target *rt = &report->targets[i];
        if (rt->ok && dry_run) {
            (void)printf("  %-16s -> %s  would write %d file%s\n", rt->exporter_id ? rt->exporter_id : "?",
                         rt->out_path ? rt->out_path : "", rt->would_write_count,
                         rt->would_write_count == 1 ? "" : "s");
        } else if (rt->ok) {
            (void)printf("  %-16s -> %s  ok (%d file%s)\n", rt->exporter_id ? rt->exporter_id : "?",
                         rt->out_path ? rt->out_path : "", rt->written_file_count,
                         rt->written_file_count == 1 ? "" : "s");
        } else {
            (void)printf("  %-16s -> FAILED: %s\n", rt->exporter_id ? rt->exporter_id : "?",
                         rt->error ? rt->error : "export failed");
        }
    }
}

/* The synchronous saved-file client uses the same session-owned request
 * builder, process controller, and owned report adoption path as GUI jobs. */
static int run_export_worker(
    const tp_session_snapshot *snapshot,
    const char *atlas_name, const char *target_filter,
    const char *out_dir, const char *work_dir, bool dry_run, bool json,
    bool quiet) {
    tp_id128 atlas_id = tp_id128_nil();
    if (atlas_name) {
        const int count = tp_session_snapshot_atlas_count(snapshot);
        for (int i = 0; i < count; ++i) {
            const tp_snapshot_atlas *atlas =
                tp_session_snapshot_atlas_at(snapshot, i);
            if (atlas && strcmp(atlas->name, atlas_name) == 0) {
                atlas_id = atlas->id;
                break;
            }
        }
    }
    tp_error error = {0};
    const tp_export_command_request request = {
        .work_dir = work_dir,
        .session_instance_generation = 1U,
        .request_id = CLI_PACK_REQUEST_ID,
        .atlas_id = atlas_id,
        .target_exporter_id = target_filter,
        .out_dir = out_dir,
        .dry_run = dry_run,
    };
    tp_session_job_result result = {0};
    tp_status status = tp_export_command_run_snapshot(
        snapshot, &request, &result, &error);
    const tp_export_command_report *report = result.export_result.report;
    NT_ASSERT(status != TP_STATUS_OK || report != NULL);
    if (!report) {
        const tp_status failure = status != TP_STATUS_OK ? status : result.status;
        const char *message = error.msg[0] ? error.msg
                              : result.error.msg[0] ? result.error.msg
                                                    : tp_status_str(failure);
        cli_emit_error(json, quiet, tp_status_id(failure), "%s", message);
    }
    if (json && report) {
        tp_sb sb = {0};
        bool root_first = true;
        tp_sb_char(&sb, '{');
        tp_obj_key(&sb, 1, &root_first, "schema");
        tp_sb_int(&sb, CLI_PACK_SCHEMA);
        tp_obj_key(&sb, 1, &root_first, "dry_run");
        tp_sb_str(&sb, report->dry_run ? "true" : "false");
        tp_obj_key(&sb, 1, &root_first, "atlases");
        tp_sb_char(&sb, '[');
        for (int i = 0; i < report->atlas_count; ++i) {
            const tp_export_command_atlas_report *atlas = &report->atlases[i];
            tp_sb_str(&sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(&sb, 2);
            emit_atlas(&sb, 2, atlas->name ? atlas->name : "",
                       atlas->sprite_count, atlas->missing_sources,
                       atlas->report_present ? &atlas->report : NULL,
                       atlas->report_present ? &atlas->notices : NULL,
                       atlas->skip_notice_id, atlas->note, atlas->status,
                       &atlas->error, report->dry_run);
        }
        if (report->atlas_count > 0) {
            tp_sb_str(&sb, "\n");
            tp_sb_indent(&sb, 1);
        }
        tp_sb_char(&sb, ']');
        tp_obj_key(&sb, 1, &root_first, "totals");
        bool totals_first = true;
        tp_sb_char(&sb, '{');
        tp_obj_key(&sb, 2, &totals_first, "targets_ok");
        tp_sb_int(&sb, report->targets_ok);
        tp_obj_key(&sb, 2, &totals_first, "targets_failed");
        tp_sb_int(&sb, report->targets_failed);
        tp_obj_key(&sb, 2, &totals_first, "files_written");
        tp_sb_int(&sb, report->files_written);
        tp_sb_str(&sb, "\n");
        tp_sb_indent(&sb, 1);
        tp_sb_char(&sb, '}');
        tp_obj_key(&sb, 1, &root_first, "timings_ms");
        bool timing_first = true;
        tp_sb_char(&sb, '{');
        tp_obj_key(&sb, 2, &timing_first, "total");
        tp_sb_num(&sb, result.elapsed_ms);
        tp_sb_str(&sb, "\n");
        tp_sb_indent(&sb, 1);
        tp_sb_char(&sb, '}');
        tp_sb_str(&sb, "\n}");
        if (sb.oom) {
            tp_sb_free(&sb);
            cli_emit_error(true, false, "oom",
                           "out of memory building pack payload");
            tp_session_job_result_destroy(&result);
            return CLI_EXIT_INTERNAL;
        } else {
            cli_out_stdout(&sb);
            tp_sb_free(&sb);
        }
    } else if (!json && report) {
        for (int i = 0; i < report->atlas_count; ++i) {
            const tp_export_command_atlas_report *atlas = &report->atlases[i];
            print_atlas_human(
                atlas->name ? atlas->name : "", atlas->sprite_count,
                atlas->missing_sources,
                atlas->report_present ? &atlas->report : NULL, atlas->note,
                report->dry_run);
            if (!quiet) {
                report_progress(atlas->name ? atlas->name : "?",
                                atlas->report_present ? &atlas->report : NULL,
                                atlas->report_present ? &atlas->notices : NULL,
                                false);
            }
        }
        if (!report->had_pack_failure && !report->had_export_failure &&
            report->dry_run) {
            (void)printf("OK dry-run (%d target%s, no files written)\n",
                         report->targets_ok,
                         report->targets_ok == 1 ? "" : "s");
        } else if (!report->had_pack_failure && !report->had_export_failure) {
            (void)printf("OK (%d target%s, %d file%s)\n", report->targets_ok,
                         report->targets_ok == 1 ? "" : "s",
                         report->files_written,
                         report->files_written == 1 ? "" : "s");
        } else {
            (void)printf("FAILED (%d ok, %d failed)\n", report->targets_ok,
                         report->targets_failed);
        }
    }
    const tp_status terminal_status = status == TP_STATUS_OK
                                          ? result.status
                                          : status;
    const tp_session_job_state terminal_state =
        status == TP_STATUS_OK ? result.state : TP_SESSION_JOB_FAILED;
    const int exit_code = cli_exit_for_export_result(
        terminal_state, terminal_status, report);
    tp_session_job_result_destroy(&result);
    return exit_code;
}

int cmd_pack(tp_format_catalog *catalog, const char *project_path,
             const char *opt_atlas, const char *opt_target, const char *opt_out_dir,
             bool dry_run, bool json, bool quiet) {
    tp_session_snapshot *snapshot = NULL;
    int rc = cli_load_snapshot(catalog, project_path, json, quiet, &snapshot);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* --atlas: an unknown name is a usage error listing the known names. */
    if (opt_atlas) {
        bool found = false;
        const int atlas_count = tp_session_snapshot_atlas_count(snapshot);
        for (int i = 0; i < atlas_count; i++) {
            const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
            if (atlas->name && strcmp(atlas->name, opt_atlas) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            char known[512];
            size_t used = 0;
            known[0] = '\0';
            for (int i = 0; i < atlas_count; i++) {
                const tp_snapshot_atlas *atlas = tp_session_snapshot_atlas_at(snapshot, i);
                int n = snprintf(known + used, sizeof known - used, "%s%s", (i == 0) ? "" : ", ",
                                 atlas->name ? atlas->name : "");
                if (n < 0 || (size_t)n >= sizeof known - used) {
                    break;
                }
                used += (size_t)n;
            }
            cli_emit_error(json, quiet, "usage", "unknown atlas '%s' (known: %s)", opt_atlas, known);
            tp_session_snapshot_destroy(snapshot);
            return CLI_EXIT_USAGE;
        }
    }

    char out_dir_abs[TP_IDENTITY_PATH_MAX] = {0};
    if (opt_out_dir) {
        tp_error path_error = {0};
        const tp_status path_status = abspath_cwd(
            opt_out_dir, out_dir_abs, sizeof out_dir_abs, &path_error);
        if (path_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(path_status), "%s",
                           path_error.msg[0] ? path_error.msg
                                             : tp_status_str(path_status));
            tp_session_snapshot_destroy(snapshot);
            return path_status == TP_STATUS_OUT_OF_BOUNDS ||
                           path_status == TP_STATUS_INVALID_ARGUMENT
                       ? CLI_EXIT_USAGE
                       : CLI_EXIT_INTERNAL;
        }
    }
    char work_dir[TP_IDENTITY_PATH_MAX];
    tp_error work_error = {0};
    const tp_status work_status =
        cli_work_dir(work_dir, sizeof work_dir, &work_error);
    if (work_status != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(work_status), "%s",
                       work_error.msg[0] ? work_error.msg
                                         : tp_status_str(work_status));
        tp_session_snapshot_destroy(snapshot);
        return CLI_EXIT_PACK;
    }

    const int worker_exit = run_export_worker(
        snapshot, opt_atlas, opt_target,
        opt_out_dir ? out_dir_abs : NULL, work_dir, dry_run, json, quiet);
    tp_session_snapshot_destroy(snapshot);
    app_scratch_request_dir_release(work_dir);
    return worker_exit;
}
