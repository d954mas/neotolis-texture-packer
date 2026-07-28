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

static void key(tp_sb *sb, int depth, bool *first, const char *name) {
    tp_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    tp_sb_indent(sb, depth);
    tp_sb_json_string(sb, name);
    tp_sb_str(sb, ": ");
}

static void emit_context(tp_sb *sb, int depth, bool *first, const char *name, const char *value) {
    if (value[0] == '\0') {
        return;
    }
    key(sb, depth, first, name);
    tp_sb_json_string(sb, value);
}

static bool emit_id_context(tp_sb *sb, int depth, bool *first,
                            const char *name, tp_id_kind kind, tp_id128 id) {
    if (tp_id128_is_nil(id)) {
        return true;
    }
    char text[TP_ID_TEXT_CAP];
    if (tp_id_format(kind, id, text, sizeof text, NULL) != TP_STATUS_OK) {
        return false;
    }
    key(sb, depth, first, name);
    tp_sb_json_string(sb, text);
    return true;
}

static bool build_validate_json(tp_sb *sb,
                                const tp_validation_report *report) {
    bool first = true;
    bool ids_ok = true;
    tp_sb_char(sb, '{');
    key(sb, 1, &first, "schema");
    tp_sb_int(sb, CLI_VALIDATE_SCHEMA);

    key(sb, 1, &first, "findings");
    if (report->finding_count == 0U) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (size_t i = 0; i < report->finding_count; i++) {
            const tp_validation_finding *finding = &report->findings[i];
            tp_sb_str(sb, i == 0U ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            bool finding_first = true;
            tp_sb_char(sb, '{');
            key(sb, 3, &finding_first, "severity");
            tp_sb_json_string(sb, finding->severity == TP_VALIDATION_ERROR ? "error" : "warning");
            key(sb, 3, &finding_first, "code");
            tp_sb_json_string(sb, finding->code);
            key(sb, 3, &finding_first, "message");
            tp_sb_json_string(sb, finding->message);
            emit_context(sb, 3, &finding_first, "atlas", finding->atlas);
            if (!emit_id_context(sb, 3, &finding_first, "atlas_id",
                                 TP_ID_KIND_ATLAS, finding->atlas_id)) {
                ids_ok = false;
            }
            emit_context(sb, 3, &finding_first, "source", finding->source);
            if (!emit_id_context(sb, 3, &finding_first, "source_id",
                                 TP_ID_KIND_SOURCE, finding->source_id)) {
                ids_ok = false;
            }
            emit_context(sb, 3, &finding_first, "sprite", finding->sprite);
            emit_context(sb, 3, &finding_first, "anim", finding->anim);
            if (!emit_id_context(sb, 3, &finding_first, "animation_id",
                                 TP_ID_KIND_ANIM,
                                 finding->animation_id)) {
                ids_ok = false;
            }
            emit_context(sb, 3, &finding_first, "frame", finding->frame);
            emit_context(sb, 3, &finding_first, "target", finding->target);
            if (!emit_id_context(sb, 3, &finding_first, "target_id",
                                 TP_ID_KIND_TARGET, finding->target_id)) {
                ids_ok = false;
            }
            tp_sb_str(sb, "\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '}');
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }

    key(sb, 1, &first, "counts");
    {
        bool counts_first = true;
        tp_sb_char(sb, '{');
        key(sb, 2, &counts_first, "error");
        tp_sb_size(sb, report->error_count);
        key(sb, 2, &counts_first, "warning");
        tp_sb_size(sb, report->warning_count);
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, '}');
    }
    tp_sb_str(sb, "\n}");
    return ids_ok;
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
        tp_sb sb = {0};
        const bool ids_ok = build_validate_json(&sb, &report);
        if (!ids_ok) {
            tp_sb_free(&sb);
            tp_validation_report_free(&report);
            cli_emit_error(true, false,
                           tp_status_id(TP_STATUS_ID_MALFORMED),
                           "validation report contained an unformattable structural id");
            return CLI_EXIT_INTERNAL;
        }
        if (sb.oom) {
            tp_sb_free(&sb);
            tp_validation_report_free(&report);
            cli_emit_error(true, false, "oom", "out of memory building validate payload");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        tp_sb_free(&sb);
    } else {
        print_validate_human(&report, path);
    }

    bool strict_failure = strict && report.error_count > 0U;
    tp_validation_report_free(&report);
    return strict_failure ? CLI_EXIT_VALIDATE : CLI_EXIT_OK;
}
