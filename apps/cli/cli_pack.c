/* `ntpacker pack <project> [--atlas <name>] [--target <id>] [--out-dir <dir>]
 *                          [--json] [--quiet]`   (alias: `export`).  Plan B3a.
 *
 * Packs + exports every ENABLED target of every atlas through the shared op layer
 * (tp_pack_input_build -> tp_export_run_ex) -- the same entry points the GUI's
 * "Export All" drives, so output is byte-identical. Thin client: no name/desc/
 * exporter logic here (boundary gates R1-R3), just orchestration + reporting.
 *
 * Filters (applied by rewriting the loaded project copy, never the file on disk):
 *   --atlas <name>  only that atlas runs (unknown name -> usage error, exit 2).
 *   --target <id>   only targets with that exporter id run (others disabled).
 *   --out-dir <dir> RELATIVE target out_paths are re-rooted under <dir> (resolved
 *                   against the CWD); absolute out_paths are left untouched.
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

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_arena.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"

#define CLI_PACK_SCHEMA 1

/* ------------------------------------------------------------------ */
/* small platform helpers                                             */
/* ------------------------------------------------------------------ */

static double now_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER f;
    LARGE_INTEGER c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

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

/* Makes `p` absolute against the current working directory (path need not exist). */
static void abspath_cwd(const char *p, char *out, size_t cap) {
    if (path_is_abs(p)) {
        (void)snprintf(out, cap, "%s", p);
        return;
    }
    char cwd[512];
#ifdef _WIN32
    if (!_getcwd(cwd, (int)sizeof cwd)) {
        (void)snprintf(out, cap, "%s", p);
        return;
    }
#else
    if (!getcwd(cwd, sizeof cwd)) {
        (void)snprintf(out, cap, "%s", p);
        return;
    }
#endif
    (void)snprintf(out, cap, "%s/%s", cwd, p);
}

/* A transient work dir for the session .ntpack files (system temp; gitignored). */
static void cli_work_dir(char *out, size_t cap) {
#ifdef _WIN32
    char tmp[512];
    DWORD n = GetTempPathA((DWORD)sizeof tmp, tmp); /* ends with a backslash */
    if (n == 0 || n >= sizeof tmp) {
        (void)snprintf(out, cap, ".");
    } else {
        (void)snprintf(out, cap, "%sntpacker_work", tmp);
    }
#else
    const char *t = getenv("TMPDIR");
    if (!t || !t[0]) {
        t = "/tmp";
    }
    (void)snprintf(out, cap, "%s/ntpacker_work", t);
#endif
    tp_mkdirs(out);
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

static void key(cli_sb *sb, int depth, bool *first, const char *k) {
    cli_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    cli_sb_indent(sb, depth);
    cli_sb_json_str(sb, k);
    cli_sb_str(sb, ": ");
}

static void emit_pages(cli_sb *sb, int depth, const tp_export_report_run *run) {
    if (!run || run->page_count == 0) {
        cli_sb_str(sb, "[]");
        return;
    }
    cli_sb_putc(sb, '[');
    for (int i = 0; i < run->page_count; i++) {
        const tp_export_report_page *pg = &run->pages[i];
        cli_sb_str(sb, i == 0 ? "\n" : ",\n");
        cli_sb_indent(sb, depth + 1);
        bool pf = true;
        cli_sb_putc(sb, '{');
        key(sb, depth + 2, &pf, "index");
        cli_sb_int(sb, pg->index);
        key(sb, depth + 2, &pf, "w");
        cli_sb_int(sb, pg->w);
        key(sb, depth + 2, &pf, "h");
        cli_sb_int(sb, pg->h);
        key(sb, depth + 2, &pf, "occupancy_pct");
        cli_sb_num(sb, pg->occupancy_pct);
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, '}');
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, ']');
}

static void emit_notice(cli_sb *sb, int depth, const tp_export_notice *nt) {
    bool nf = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &nf, "field");
    cli_sb_json_str(sb, notice_field_name(nt->field_id));
    key(sb, depth + 1, &nf, "reason");
    cli_sb_json_str(sb, notice_reason_name(nt->reason_id));
    if (nt->sprite) {
        key(sb, depth + 1, &nf, "sprite");
        cli_sb_json_str(sb, nt->sprite);
    }
    if (nt->target) {
        key(sb, depth + 1, &nf, "target");
        cli_sb_json_str(sb, nt->target);
    }
    key(sb, depth + 1, &nf, "message");
    cli_sb_json_str(sb, nt->msg);
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void emit_str_array(cli_sb *sb, int depth, const char *const *items, int count) {
    if (count <= 0 || !items) {
        cli_sb_str(sb, "[]");
        return;
    }
    cli_sb_putc(sb, '[');
    for (int i = 0; i < count; i++) {
        cli_sb_str(sb, i == 0 ? "\n" : ",\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_json_str(sb, items[i]);
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, ']');
}

static void emit_target(cli_sb *sb, int depth, const tp_export_report_target *rt, const tp_export_notices *notices) {
    bool tf = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &tf, "exporter_id");
    cli_sb_json_str(sb, rt->exporter_id ? rt->exporter_id : "");
    key(sb, depth + 1, &tf, "out_path");
    cli_sb_json_str(sb, rt->out_path ? rt->out_path : "");
    key(sb, depth + 1, &tf, "pack_run");
    cli_sb_int(sb, rt->pack_run);
    key(sb, depth + 1, &tf, "status");
    cli_sb_json_str(sb, rt->ok ? "ok" : "failed");
    if (!rt->ok && rt->error) {
        key(sb, depth + 1, &tf, "error");
        cli_sb_json_str(sb, rt->error);
    }
    key(sb, depth + 1, &tf, "written_files");
    emit_str_array(sb, depth + 1, rt->written_files, rt->written_file_count);

    key(sb, depth + 1, &tf, "notices");
    int nb = rt->notice_begin;
    int ne = rt->notice_end;
    if (!notices || ne <= nb) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = nb; i < ne && i < notices->count; i++) {
            cli_sb_str(sb, i == nb ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_notice(sb, depth + 2, &notices->items[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* Emits one atlas object. `report` may be NULL (a skipped atlas); `note` (nullable)
 * records why it was skipped. `pages` uses the PRIMARY pack run (runs[0]); a target
 * on a different run is flagged by its own `pack_run` index. */
static void emit_atlas(cli_sb *sb, int depth, const char *name, int sprite_count, int missing_sources,
                       const tp_export_report *report, const tp_export_notices *notices, const char *note) {
    bool af = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &af, "name");
    cli_sb_json_str(sb, name);
    key(sb, depth + 1, &af, "sprite_count");
    cli_sb_int(sb, sprite_count);
    key(sb, depth + 1, &af, "missing_sources");
    cli_sb_int(sb, missing_sources);
    if (note) {
        key(sb, depth + 1, &af, "note");
        cli_sb_json_str(sb, note);
    }
    key(sb, depth + 1, &af, "pack_runs");
    cli_sb_int(sb, report ? report->run_count : 0);

    key(sb, depth + 1, &af, "pages");
    const tp_export_report_run *primary = (report && report->run_count > 0) ? &report->runs[0] : NULL;
    emit_pages(sb, depth + 1, primary);

    key(sb, depth + 1, &af, "targets");
    if (!report || report->target_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < report->target_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_target(sb, depth + 2, &report->targets[i], notices);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* ------------------------------------------------------------------ */
/* pack                                                               */
/* ------------------------------------------------------------------ */

/* Rewrites the loaded project's targets for the --target / --out-dir filters. */
static void apply_filters(tp_project *p, const char *opt_target, const char *opt_out_dir, const char *out_dir_abs) {
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        for (int t = 0; t < a->target_count; t++) {
            tp_project_target *tg = &a->targets[t];
            if (!tg->exporter_id || !tg->out_path) {
                continue;
            }
            bool en = tg->enabled;
            if (opt_target && strcmp(tg->exporter_id, opt_target) != 0) {
                en = false;
            }
            const char *out = tg->out_path;
            char newout[700];
            if (opt_out_dir && !path_is_abs(out)) {
                (void)snprintf(newout, sizeof newout, "%s/%s", out_dir_abs, out);
                out = newout;
            }
            /* set_target dups both strings BEFORE freeing the old ones, so aliasing
             * tg->exporter_id / tg->out_path is safe. */
            (void)tp_project_atlas_set_target(a, t, tg->exporter_id, out, en);
        }
    }
}

/* Emits the stderr progress + notice lines for one completed atlas (human aid;
 * suppressed by --quiet). JSON payload stays on stdout, untouched. */
static void report_progress(const char *name, const tp_export_report *report, const tp_export_notices *notices,
                            bool json) {
    if (!report) {
        return;
    }
    for (int i = 0; i < report->target_count; i++) {
        const tp_export_report_target *rt = &report->targets[i];
        if (rt->ok) {
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

/* Human summary lines for one atlas (stdout). */
static void print_atlas_human(const char *name, int sprite_count, int missing_sources,
                              const tp_export_report *report, const char *note) {
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
        if (rt->ok) {
            (void)printf("  %-16s -> %s  ok (%d file%s)\n", rt->exporter_id ? rt->exporter_id : "?",
                         rt->out_path ? rt->out_path : "", rt->written_file_count,
                         rt->written_file_count == 1 ? "" : "s");
        } else {
            (void)printf("  %-16s -> FAILED: %s\n", rt->exporter_id ? rt->exporter_id : "?",
                         rt->error ? rt->error : "export failed");
        }
    }
}

int cmd_pack(const char *project_path, const char *opt_atlas, const char *opt_target, const char *opt_out_dir,
             bool json, bool quiet) {
    tp_project *p = NULL;
    int rc = cli_load_project(project_path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    /* --atlas: an unknown name is a usage error listing the known names. */
    if (opt_atlas) {
        bool found = false;
        for (int i = 0; i < p->atlas_count; i++) {
            if (p->atlases[i].name && strcmp(p->atlases[i].name, opt_atlas) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            char known[512];
            size_t used = 0;
            known[0] = '\0';
            for (int i = 0; i < p->atlas_count; i++) {
                int n = snprintf(known + used, sizeof known - used, "%s%s", (i == 0) ? "" : ", ",
                                 p->atlases[i].name ? p->atlases[i].name : "");
                if (n < 0 || (size_t)n >= sizeof known - used) {
                    break;
                }
                used += (size_t)n;
            }
            cli_emit_error(json, quiet, "usage", "unknown atlas '%s' (known: %s)", opt_atlas, known);
            tp_project_destroy(p);
            return CLI_EXIT_USAGE;
        }
    }

    char out_dir_abs[600] = {0};
    if (opt_out_dir) {
        abspath_cwd(opt_out_dir, out_dir_abs, sizeof out_dir_abs);
    }
    apply_filters(p, opt_target, opt_out_dir, out_dir_abs);

    char work_dir[512];
    cli_work_dir(work_dir, sizeof work_dir);

    int total_targets_ok = 0;
    int total_targets_failed = 0;
    int total_files = 0;
    bool had_pack_fail = false;
    bool had_export_fail = false;
    const double t0 = now_ms();

    cli_sb sb = {0};
    if (json) {
        bool rf = true;
        cli_sb_putc(&sb, '{');
        key(&sb, 1, &rf, "schema");
        cli_sb_int(&sb, CLI_PACK_SCHEMA);
        key(&sb, 1, &rf, "atlases");
        cli_sb_putc(&sb, '[');
    }
    bool any_atlas_emitted = false;

    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (opt_atlas && (!a->name || strcmp(a->name, opt_atlas) != 0)) {
            continue;
        }

        tp_pack_input input;
        tp_error e = {0};
        tp_status bst = tp_pack_input_build(p, ai, &input, &e);
        int sprite_count = (bst == TP_STATUS_OK) ? input.count : 0;
        int missing = (bst == TP_STATUS_OK) ? input.missing_sources : 0;
        int enabled = 0;
        for (int t = 0; t < a->target_count; t++) {
            if (a->targets[t].enabled) {
                enabled++;
            }
        }

        tp_arena *arena = NULL;
        tp_export_notices notices;
        tp_export_notices_init(&notices);
        tp_export_report report;
        memset(&report, 0, sizeof report);
        const char *note = NULL;
        bool ran = false;

        if (bst != TP_STATUS_OK) {
            note = "could not assemble sprites (input build failed)";
            had_pack_fail = true;
        } else if (sprite_count == 0) {
            note = "no usable images (skipped)";
        } else if (enabled == 0) {
            note = "no enabled targets (skipped)";
        } else {
            /* Create each enabled target's output parent dir (tp_export_run needs it). */
            for (int t = 0; t < a->target_count; t++) {
                if (!a->targets[t].enabled) {
                    continue;
                }
                char out_abs[700];
                if (tp_project_resolve_path(p, a->targets[t].out_path, out_abs, sizeof out_abs) == TP_STATUS_OK) {
                    tp_mkdirs_parent(out_abs);
                }
            }
            arena = tp_arena_create(0);
            if (!arena) {
                note = "out of memory";
                had_pack_fail = true;
            } else {
                tp_error re = {0};
                (void)tp_export_run_ex(p, ai, input.descs, input.count, work_dir, arena, &notices, NULL, &report, &re);
                ran = true;
                if (report.pack_failed) {
                    had_pack_fail = true;
                }
                for (int i = 0; i < report.target_count; i++) {
                    if (report.target_count > 0 && report.targets[i].ok) {
                        total_targets_ok++;
                        total_files += report.targets[i].written_file_count;
                    } else {
                        total_targets_failed++;
                        had_export_fail = true;
                    }
                }
            }
        }

        /* Emit (JSON payload accumulates; human prints now). */
        if (json) {
            cli_sb_str(&sb, any_atlas_emitted ? ",\n" : "\n");
            cli_sb_indent(&sb, 2);
            emit_atlas(&sb, 2, a->name ? a->name : "", sprite_count, missing, ran ? &report : NULL, ran ? &notices : NULL,
                       note);
        } else {
            print_atlas_human(a->name ? a->name : "", sprite_count, missing, ran ? &report : NULL, note);
        }
        any_atlas_emitted = true;

        if (!quiet) {
            if (note && !ran) {
                (void)fprintf(stderr, "ntpacker: %s: %s\n", a->name ? a->name : "?", note);
            }
            report_progress(a->name ? a->name : "?", ran ? &report : NULL, ran ? &notices : NULL, json);
        }

        tp_export_notices_free(&notices);
        if (arena) {
            tp_arena_destroy(arena);
        }
        tp_pack_input_free(&input);
    }

    const double elapsed = now_ms() - t0;

    /* Exit-code aggregation (plan): 0 clean; 6 partial (some ok + some failed);
     * 4 total pack failure; 5 total export failure. */
    int exit_code;
    if (!had_pack_fail && !had_export_fail) {
        exit_code = CLI_EXIT_OK;
    } else if (total_targets_ok > 0) {
        exit_code = CLI_EXIT_PARTIAL;
    } else if (had_pack_fail) {
        exit_code = CLI_EXIT_PACK;
    } else {
        exit_code = CLI_EXIT_EXPORT;
    }

    if (json) {
        if (any_atlas_emitted) {
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 1);
        }
        cli_sb_putc(&sb, ']');
        bool tf = false; /* root object already has schema+atlases -> comma needed */
        key(&sb, 1, &tf, "totals");
        {
            bool cf = true;
            cli_sb_putc(&sb, '{');
            key(&sb, 2, &cf, "targets_ok");
            cli_sb_int(&sb, total_targets_ok);
            key(&sb, 2, &cf, "targets_failed");
            cli_sb_int(&sb, total_targets_failed);
            key(&sb, 2, &cf, "files_written");
            cli_sb_int(&sb, total_files);
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 1);
            cli_sb_putc(&sb, '}');
        }
        key(&sb, 1, &tf, "timings_ms");
        {
            bool mf = true;
            cli_sb_putc(&sb, '{');
            key(&sb, 2, &mf, "total");
            cli_sb_num(&sb, elapsed);
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 1);
            cli_sb_putc(&sb, '}');
        }
        cli_sb_str(&sb, "\n}");
        if (sb.oom) {
            cli_sb_free(&sb);
            tp_project_destroy(p);
            cli_emit_error(true, false, "oom", "out of memory building pack report");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else {
        if (exit_code == CLI_EXIT_OK) {
            (void)printf("OK (%d target%s, %d file%s)\n", total_targets_ok, total_targets_ok == 1 ? "" : "s",
                         total_files, total_files == 1 ? "" : "s");
        } else {
            (void)printf("FAILED (%d ok, %d failed)\n", total_targets_ok, total_targets_failed);
        }
    }

    tp_project_destroy(p);
    return exit_code;
}
