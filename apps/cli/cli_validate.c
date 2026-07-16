/* `ntpacker validate` is a thin file-oriented adapter. tp_core owns project
 * loading and every validation rule; this TU owns only rendering and exit-code
 * mapping for the stable CLI contract. */
#include "cli_cmds.h"

#include <stdbool.h>
#include <stdio.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_validate.h"

#define CLI_VALIDATE_SCHEMA 1

static void key(cli_sb *sb, int depth, bool *first, const char *name) {
    cli_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    cli_sb_indent(sb, depth);
    cli_sb_json_str(sb, name);
    cli_sb_str(sb, ": ");
}

static void emit_context(cli_sb *sb, int depth, bool *first, const char *name, const char *value) {
    if (value[0] == '\0') {
        return;
    }
    key(sb, depth, first, name);
    cli_sb_json_str(sb, value);
}

static void build_validate_json(cli_sb *sb, const tp_validation_report *report) {
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, 1, &first, "schema");
    cli_sb_int(sb, CLI_VALIDATE_SCHEMA);

    key(sb, 1, &first, "findings");
    if (report->finding_count == 0U) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (size_t i = 0; i < report->finding_count; i++) {
            const tp_validation_finding *finding = &report->findings[i];
            cli_sb_str(sb, i == 0U ? "\n" : ",\n");
            cli_sb_indent(sb, 2);
            bool finding_first = true;
            cli_sb_putc(sb, '{');
            key(sb, 3, &finding_first, "severity");
            cli_sb_json_str(sb, finding->severity == TP_VALIDATION_ERROR ? "error" : "warning");
            key(sb, 3, &finding_first, "code");
            cli_sb_json_str(sb, finding->code);
            key(sb, 3, &finding_first, "message");
            cli_sb_json_str(sb, finding->message);
            emit_context(sb, 3, &finding_first, "atlas", finding->atlas);
            emit_context(sb, 3, &finding_first, "sprite", finding->sprite);
            emit_context(sb, 3, &finding_first, "anim", finding->anim);
            emit_context(sb, 3, &finding_first, "frame", finding->frame);
            emit_context(sb, 3, &finding_first, "target", finding->target);
            cli_sb_str(sb, "\n");
            cli_sb_indent(sb, 2);
            cli_sb_putc(sb, '}');
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, ']');
    }

    key(sb, 1, &first, "counts");
    {
        bool counts_first = true;
        cli_sb_putc(sb, '{');
        key(sb, 2, &counts_first, "error");
        cli_sb_size(sb, report->error_count);
        key(sb, 2, &counts_first, "warning");
        cli_sb_size(sb, report->warning_count);
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, '}');
    }
    cli_sb_str(sb, "\n}");
}

static void print_validate_human(const tp_validation_report *report, const char *path) {
    (void)printf("%s: %zu error%s, %zu warning%s\n", path, report->error_count,
                 report->error_count == 1U ? "" : "s", report->warning_count,
                 report->warning_count == 1U ? "" : "s");
    for (size_t i = 0; i < report->finding_count; i++) {
        const tp_validation_finding *finding = &report->findings[i];
        (void)printf("  [%s] %s: %s\n", finding->severity == TP_VALIDATION_ERROR ? "error" : "warning",
                     finding->code, finding->message);
    }
}

int cmd_validate(const char *path, bool json, bool quiet, bool strict) {
    tp_session_snapshot *snapshot = NULL;
    int rc = cli_load_snapshot(path, json, quiet, &snapshot);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_validation_report report = {0};
    tp_error err = {0};
    tp_status status = tp_validate_session_snapshot(snapshot, &report, &err);
    tp_session_snapshot_destroy(snapshot);
    if (status != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(status), "%s", err.msg[0] ? err.msg : tp_status_str(status));
        return status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
    }

    if (json) {
        cli_sb sb = {0};
        build_validate_json(&sb, &report);
        if (sb.oom) {
            cli_sb_free(&sb);
            tp_validation_report_free(&report);
            cli_emit_error(true, false, "oom", "out of memory building validate payload");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else {
        print_validate_human(&report, path);
    }

    bool strict_failure = strict && report.error_count > 0U;
    tp_validation_report_free(&report);
    return strict_failure ? CLI_EXIT_VALIDATE : CLI_EXIT_OK;
}
