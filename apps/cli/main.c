/* ntpacker -- saved-file CLI frontend over tp_core. Thin client: hand-rolled
 * args, verb dispatch, versioned --json payloads, contract exit codes, and
 * structured errors (docs/formats/cli-report.md). */
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "log/nt_log.h"
#include "core/nt_assert.h"

#include "cli_cmds.h"
#include "cli_exit.h"
#include "cli_out.h"
#include "app_format_catalog.h"
#include "ntpacker_version.h"
#if defined(_WIN32)
#include "nt_utf8_argv.h"
#endif
#include "tp_core/tp_build_worker.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"

enum {
    CLI_HELP_SCHEMA = 1,
    CLI_MANIFEST_SCHEMA = 2,
    CLI_MUTATION_APPLY_SCHEMA = 1,
    CLI_MUTATION_DRY_RUN_SCHEMA = 2,
};

static const char *const HELP_COMMANDS[] = {
    "pack", "export", "formats", "inspect", "validate", "new", "add", "remove",
    "set", "sprite", "anim", "target", "atlas", "version", "help",
};

static const char *const HELP_GLOBAL_OPTIONS[] = {
    "--json", "--quiet", "--strict", "--dry-run", "--help", "--version",
};

static void indent(tp_sb *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        tp_sb_str(sb, "  ");
    }
}

/* One "caps" object (the exporter's format expressiveness). Fields are the
 * append-only tp_export_caps set; snake_case/lowercase already. */
static void emit_caps(tp_sb *sb, int depth, const tp_export_caps *c) {
    struct {
        const char *key;
        bool val;
    } fields[] = {
        {"rotate90", tp_export_caps_supports_rotate90(c)},
        {"flips", tp_export_caps_supports_flips(c)},
        {"polygons", c->polygons}, {"pivot", c->pivot},
        {"slice9", c->slice9},     {"multipage", c->multipage},
        {"aliases", c->aliases},   {"animations", c->animations},
    };
    int n = (int)(sizeof fields / sizeof fields[0]);
    tp_sb_str(sb, "{\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "transform_mask");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, c->transform_mask);
    tp_sb_str(sb, ",\n");
    for (int i = 0; i < n; i++) {
        indent(sb, depth + 1);
        tp_sb_json_string(sb, fields[i].key);
        tp_sb_str(sb, ": ");
        tp_sb_str(sb, fields[i].val ? "true" : "false");
        tp_sb_str(sb, (i + 1 < n) ? ",\n" : "\n");
    }
    indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* The `version --json` schema manifest (plan "CLI v1 contract"): app version,
 * on-disk project schema, each JSON-emitting verb's payload schema, known export
 * formats + versions, and the invocation-owned format catalog. Every
 * number/id is sourced from a core constant or the catalog -- no hand-copied
 * values, no exporter-id literals (boundary gate R2). */
static void build_manifest(tp_sb *sb, const tp_format_catalog *catalog) {
    tp_sb_str(sb, "{\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "schema");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_MANIFEST_SCHEMA);
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "app_version");
    tp_sb_str(sb, ": ");
    tp_sb_json_string(sb, NTPACKER_VERSION);
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "project_schema");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, TP_PROJECT_SCHEMA_VERSION);
    tp_sb_str(sb, ",\n");

    /* verbs: each verb that emits a --json payload -> its payload schema version.
     * inspect/validate landed in B2; grows as pack lands. */
    indent(sb, 1);
    tp_sb_json_string(sb, "verbs");
    tp_sb_str(sb, ": {\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "inspect");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_INSPECT_SCHEMA); /* query-payload schema (single source of truth) */
    tp_sb_str(sb, ",\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "validate");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_VALIDATE_SCHEMA);
    tp_sb_str(sb, ",\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "pack");
    tp_sb_str(sb, ": 2,\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "formats");
    tp_sb_str(sb, ": 1,\n");
    /* B4 mutation verbs have distinct apply and dry-run payloads. `anim list` is
     * a query whose payload shares inspect's schema and is advertised separately. */
    static const char *const mut_verbs[] = {"new", "add", "remove", "set", "sprite", "anim", "target", "atlas"};
    for (int i = 0; i < (int)(sizeof mut_verbs / sizeof mut_verbs[0]); i++) {
        indent(sb, 2);
        tp_sb_json_string(sb, mut_verbs[i]);
        tp_sb_str(sb, ": {\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "apply");
        tp_sb_str(sb, ": ");
        tp_sb_int(sb, CLI_MUTATION_APPLY_SCHEMA);
        tp_sb_str(sb, ",\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "dry_run");
        tp_sb_str(sb, ": ");
        tp_sb_int(sb, CLI_MUTATION_DRY_RUN_SCHEMA);
        if (strcmp(mut_verbs[i], "anim") == 0) {
            tp_sb_str(sb, ",\n");
            indent(sb, 3);
            tp_sb_json_string(sb, "list");
            tp_sb_str(sb, ": ");
            tp_sb_int(sb, CLI_INSPECT_SCHEMA);
        }
        tp_sb_str(sb, "\n");
        indent(sb, 2);
        tp_sb_str(sb, "},\n");
    }
    indent(sb, 2);
    tp_sb_json_string(sb, "help");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_HELP_SCHEMA);
    tp_sb_str(sb, ",\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "version");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_MANIFEST_SCHEMA);
    tp_sb_str(sb, "\n");
    indent(sb, 1);
    tp_sb_str(sb, "},\n");

    /* formats: export FORMAT -> format-schema version. json-neotolis key comes
     * from the shared exporter-id constant (never a literal); its value is the
     * public json schema constant; defold-tpinfo carries the tpinfo version. */
    indent(sb, 1);
    tp_sb_json_string(sb, "formats");
    tp_sb_str(sb, ": {\n");
    indent(sb, 2);
    tp_sb_json_string(sb, TP_EXPORTER_ID_JSON_NEOTOLIS);
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, TP_JSON_NEOTOLIS_SCHEMA_VERSION);
    tp_sb_str(sb, ",\n");
    indent(sb, 2);
    tp_sb_json_string(sb, "defold-tpinfo");
    tp_sb_str(sb, ": ");
    tp_sb_json_string(sb, TP_DEFOLD_TPINFO_VERSION);
    tp_sb_str(sb, "\n");
    indent(sb, 1);
    tp_sb_str(sb, "},\n");

    /* Formats: every available row in this invocation's immutable catalog. */
    indent(sb, 1);
    tp_sb_json_string(sb, "exporters");
    tp_sb_str(sb, ": [\n");
    const size_t row_count = tp_format_catalog_row_count(catalog);
    size_t available_count = 0U;
    for (size_t i = 0U; i < row_count; ++i) {
        tp_format_catalog_row row = {0};
        if (tp_format_catalog_row_at(catalog, i, &row) &&
            row.available && row.descriptor) {
            ++available_count;
        }
    }
    size_t emitted = 0U;
    for (size_t i = 0U; i < row_count; ++i) {
        tp_format_catalog_row row = {0};
        if (!tp_format_catalog_row_at(catalog, i, &row) ||
            !row.available || !row.descriptor) {
            continue;
        }
        const tp_format_descriptor *e = row.descriptor;
        indent(sb, 2);
        tp_sb_str(sb, "{\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "id");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, e->id);
        tp_sb_str(sb, ",\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "name");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, e->display_name);
        tp_sb_str(sb, ",\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "ext");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, e->artifacts[0].suffix + 1);
        tp_sb_str(sb, ",\n");
        indent(sb, 3);
        tp_sb_json_string(sb, "caps");
        tp_sb_str(sb, ": ");
        emit_caps(sb, 3, &e->caps);
        tp_sb_str(sb, "\n");
        indent(sb, 2);
        tp_sb_char(sb, '}');
        ++emitted;
        tp_sb_str(sb, emitted < available_count ? ",\n" : "\n");
    }
    indent(sb, 1);
    tp_sb_str(sb, "]\n");
    tp_sb_str(sb, "}");
}

static void print_usage(FILE *out);

static int cmd_version(const tp_format_catalog *catalog, bool json) {
    if (!json) {
        (void)printf("ntpacker %s\n", NTPACKER_VERSION);
        return CLI_EXIT_OK;
    }
    tp_sb sb = {0};
    build_manifest(&sb, catalog);
    if (sb.oom) {
        tp_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building version manifest");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    tp_sb_free(&sb);
    return CLI_EXIT_OK;
}

static const char *catalog_state_id(app_format_catalog_state state) {
    switch (state) {
    case APP_FORMAT_CATALOG_ACTIVE:
        return "active";
    case APP_FORMAT_CATALOG_NATIVE_FALLBACK:
        return "native_fallback";
    default:
        return "closed";
    }
}

static void emit_format_descriptor(tp_sb *sb, int depth,
                                   const tp_format_descriptor *descriptor) {
    static const char *const transform_ids[] = {
        "identity", "flip_h", "flip_v", "rotate_180", "transpose",
        "rotate_90_cw", "rotate_90_ccw", "anti_transpose",
    };
    tp_sb_str(sb, "{\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "api_version");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, (int)descriptor->api_version);
    tp_sb_str(sb, ",\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "id");
    tp_sb_str(sb, ": ");
    tp_sb_json_string(sb, descriptor->id);
    tp_sb_str(sb, ",\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "display_name");
    tp_sb_str(sb, ": ");
    tp_sb_json_string(sb, descriptor->display_name);
    tp_sb_str(sb, ",\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "capabilities");
    tp_sb_str(sb, ": {\n");
    indent(sb, depth + 2);
    tp_sb_json_string(sb, "transforms");
    tp_sb_str(sb, ": [");
    bool first_transform = true;
    for (int value = 0; value < 8; ++value) {
        if ((descriptor->caps.transform_mask & (uint8_t)(1U << value)) == 0U) {
            continue;
        }
        tp_sb_str(sb, first_transform ? "\n" : ",\n");
        indent(sb, depth + 3);
        tp_sb_json_string(sb, transform_ids[value]);
        first_transform = false;
    }
    if (!first_transform) {
        tp_sb_str(sb, "\n");
        indent(sb, depth + 2);
    }
    tp_sb_str(sb, "],\n");
    const struct { const char *key; bool value; } bool_caps[] = {
        {"polygons", descriptor->caps.polygons},
        {"pivot", descriptor->caps.pivot},
        {"slice9", descriptor->caps.slice9},
        {"multipage", descriptor->caps.multipage},
        {"aliases", descriptor->caps.aliases},
        {"animations", descriptor->caps.animations},
    };
    for (size_t i = 0U; i < sizeof bool_caps / sizeof bool_caps[0]; ++i) {
        indent(sb, depth + 2);
        tp_sb_json_string(sb, bool_caps[i].key);
        tp_sb_str(sb, bool_caps[i].value ? ": true" : ": false");
        tp_sb_str(sb, i + 1U < sizeof bool_caps / sizeof bool_caps[0]
                          ? ",\n" : "\n");
    }
    indent(sb, depth + 1);
    tp_sb_char(sb, '}');
    tp_sb_str(sb, ",\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "outputs");
    tp_sb_str(sb, ": [");
    for (int i = 0; i < descriptor->artifact_count; ++i) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        indent(sb, depth + 2);
        tp_sb_str(sb, "{");
        tp_sb_json_string(sb, "id");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, descriptor->artifacts[i].id);
        tp_sb_str(sb, ", ");
        tp_sb_json_string(sb, "suffix");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, descriptor->artifacts[i].suffix);
        tp_sb_char(sb, '}');
    }
    if (descriptor->artifact_count > 0) {
        tp_sb_str(sb, "\n");
        indent(sb, depth + 1);
    }
    tp_sb_str(sb, "],\n");
    indent(sb, depth + 1);
    tp_sb_json_string(sb, "host_facts");
    tp_sb_str(sb, ": [");
    for (int i = 0; i < descriptor->host_fact_count; ++i) {
        const tp_format_host_fact_decl *fact = &descriptor->host_facts[i];
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        indent(sb, depth + 2);
        tp_sb_str(sb, "{");
        tp_sb_json_string(sb, "id");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, fact->id);
        tp_sb_str(sb, ", ");
        tp_sb_json_string(sb, "kind");
        tp_sb_str(sb, ": \"project_resource\", ");
        tp_sb_json_string(sb, "output");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, fact->output_id);
        tp_sb_str(sb, ", ");
        tp_sb_json_string(sb, "root_marker");
        tp_sb_str(sb, ": ");
        tp_sb_json_string(sb, fact->root_marker);
        tp_sb_str(sb, ", ");
        tp_sb_json_string(sb, "missing");
        tp_sb_str(sb, ": \"basename_notice\"}");
    }
    if (descriptor->host_fact_count > 0) {
        tp_sb_str(sb, "\n");
        indent(sb, depth + 1);
    }
    tp_sb_str(sb, "]\n");
    indent(sb, depth);
    tp_sb_char(sb, '}');
}

static int cmd_formats(const app_format_catalog *formats, bool json) {
    const tp_format_catalog *catalog = formats->catalog;
    if (!json) {
        (void)printf("Formats (%s):\n", catalog_state_id(formats->state));
        if (formats->reason_status != TP_STATUS_OK) {
            (void)printf("  fallback: %s: %s\n",
                         tp_status_id(formats->reason_status),
                         formats->reason.msg[0]
                             ? formats->reason.msg
                             : tp_status_str(formats->reason_status));
        }
        const tp_format_diagnostic_report *root_diagnostics =
            tp_format_catalog_root_diagnostics(catalog);
        if (!root_diagnostics) {
            root_diagnostics = formats->failure_diagnostics;
        }
        const tp_format_diagnostic *root_diagnostic =
            tp_format_diagnostic_report_at(root_diagnostics, 0U);
        if (root_diagnostic) {
            (void)printf("  diagnostic: %s: %s\n",
                         tp_format_diagnostic_code_id(root_diagnostic->code),
                         root_diagnostic->message
                             ? root_diagnostic->message
                             : "format catalog error");
        }
        const size_t count = tp_format_catalog_row_count(catalog);
        for (size_t i = 0U; i < count; ++i) {
            tp_format_catalog_row row = {0};
            if (tp_format_catalog_row_at(catalog, i, &row)) {
                (void)printf("  %s  %s  %s\n", row.available ? "ready" : "unavailable",
                             row.implementation == TP_FORMAT_IMPLEMENTATION_LUA
                                 ? "lua"
                                 : "native",
                             row.descriptor ? row.descriptor->id : row.key);
                const tp_format_diagnostic *diagnostic =
                    tp_format_diagnostic_report_at(row.diagnostics, 0U);
                if (diagnostic) {
                    (void)printf("    %s: %s\n",
                                 tp_format_diagnostic_code_id(
                                     diagnostic->code),
                                 diagnostic->message
                                     ? diagnostic->message
                                     : "format package error");
                }
            }
        }
        return CLI_EXIT_OK;
    }
    tp_sb sb = {0};
    tp_sb_str(&sb, "{\n  \"schema\": 1,\n  \"state\": ");
    tp_sb_json_string(&sb, catalog_state_id(formats->state));
    const char *root = tp_format_catalog_root(catalog);
    if (root) {
        tp_sb_str(&sb, ",\n  \"root\": ");
        tp_sb_json_string(&sb, root);
    }
    tp_sb_str(&sb, ",\n  \"root_missing\": ");
    tp_sb_str(&sb, tp_format_catalog_root_missing(catalog) ? "true" : "false");
    tp_sb_str(&sb, ",\n  \"limit_fail_closed\": ");
    tp_sb_str(&sb, tp_format_catalog_limit_fail_closed(catalog) ? "true" : "false");
    if (formats->reason_status != TP_STATUS_OK) {
        tp_sb_str(&sb, ",\n  \"fallback_reason\": {\n    \"id\": ");
        tp_sb_json_string(&sb, tp_status_id(formats->reason_status));
        tp_sb_str(&sb, ",\n    \"message\": ");
        tp_sb_json_string(&sb, formats->reason.msg[0]
                                  ? formats->reason.msg
                                  : tp_status_str(formats->reason_status));
        tp_sb_str(&sb, "\n  }");
    }
    tp_sb_str(&sb, ",\n  \"diagnostics\": ");
    const tp_format_diagnostic_report *root_diagnostics =
        tp_format_catalog_root_diagnostics(catalog);
    cli_out_append_format_diagnostics(
        &sb, 1, root_diagnostics ? root_diagnostics
                                 : formats->failure_diagnostics);
    tp_sb_str(&sb, ",\n  \"diagnostics_truncated\": ");
    tp_sb_str(&sb,
              tp_format_diagnostic_report_truncated(
                  root_diagnostics ? root_diagnostics
                                   : formats->failure_diagnostics)
                  ? "true" : "false");
    tp_sb_str(&sb, ",\n  \"formats\": [");
    const size_t count = tp_format_catalog_row_count(catalog);
    for (size_t i = 0U; i < count; ++i) {
        tp_format_catalog_row row = {0};
        if (!tp_format_catalog_row_at(catalog, i, &row)) {
            continue;
        }
        tp_sb_str(&sb, i == 0U ? "\n" : ",\n");
        indent(&sb, 2);
        tp_sb_str(&sb, "{\n");
        indent(&sb, 3);
        tp_sb_json_string(&sb, "key");
        tp_sb_str(&sb, ": ");
        tp_sb_json_string(&sb, row.key);
        tp_sb_str(&sb, ",\n");
        indent(&sb, 3);
        tp_sb_json_string(&sb, "implementation");
        tp_sb_str(&sb, ": ");
        tp_sb_json_string(&sb, row.implementation == TP_FORMAT_IMPLEMENTATION_LUA
                                   ? "lua"
                                   : "native");
        tp_sb_str(&sb, ",\n");
        indent(&sb, 3);
        tp_sb_json_string(&sb, "available");
        tp_sb_str(&sb, row.available ? ": true" : ": false");
        if (row.package_path) {
            tp_sb_str(&sb, ",\n");
            indent(&sb, 3);
            tp_sb_json_string(&sb, "package_path");
            tp_sb_str(&sb, ": ");
            tp_sb_json_string(&sb, row.package_path);
        }
        if (row.fingerprint) {
            tp_sb_str(&sb, ",\n");
            indent(&sb, 3);
            tp_sb_json_string(&sb, "fingerprint");
            tp_sb_str(&sb, ": ");
            tp_sb_json_string(&sb, row.fingerprint);
        }
        if (row.descriptor) {
            tp_sb_str(&sb, ",\n");
            indent(&sb, 3);
            tp_sb_json_string(&sb, "descriptor");
            tp_sb_str(&sb, ": ");
            emit_format_descriptor(&sb, 3, row.descriptor);
        }
        tp_sb_str(&sb, ",\n");
        indent(&sb, 3);
        tp_sb_json_string(&sb, "diagnostics");
        tp_sb_str(&sb, ": ");
        cli_out_append_format_diagnostics(&sb, 3, row.diagnostics);
        tp_sb_str(&sb, ",\n");
        indent(&sb, 3);
        tp_sb_json_string(&sb, "diagnostics_truncated");
        tp_sb_str(&sb, tp_format_diagnostic_report_truncated(row.diagnostics)
                           ? ": true" : ": false");
        tp_sb_str(&sb, "\n");
        indent(&sb, 2);
        tp_sb_char(&sb, '}');
    }
    if (count > 0U) {
        tp_sb_str(&sb, "\n  ");
    }
    tp_sb_str(&sb, "]\n}");
    if (sb.oom) {
        tp_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building formats report");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    tp_sb_free(&sb);
    return CLI_EXIT_OK;
}

static void emit_string_array(tp_sb *sb, const char *const *items,
                              size_t count) {
    tp_sb_str(sb, "[\n");
    for (size_t i = 0U; i < count; ++i) {
        indent(sb, 2);
        tp_sb_json_string(sb, items[i]);
        tp_sb_str(sb, i + 1U < count ? ",\n" : "\n");
    }
    indent(sb, 1);
    tp_sb_char(sb, ']');
}

static void build_help(tp_sb *sb) {
    /* These ids are the EXIT-CODE vocabulary (cli_exit.h 0..8), NOT the
     * `error.id` vocabulary -- error ids are tp_status ids (docs/formats/
     * cli-report.md). An OOM payload therefore reports id "oom" and exits with
     * the "internal" family (1); it does not get a row here, because there is
     * no distinct OOM exit code to freeze. */
    static const struct {
        const char *id;
        int code;
    } exit_codes[] = {
        {"ok", CLI_EXIT_OK},
        {"internal", CLI_EXIT_INTERNAL},
        {"usage", CLI_EXIT_USAGE},
        {"project", CLI_EXIT_PROJECT},
        {"pack", CLI_EXIT_PACK},
        {"export", CLI_EXIT_EXPORT},
        {"partial", CLI_EXIT_PARTIAL},
        {"validate", CLI_EXIT_VALIDATE},
        {"file_io", CLI_EXIT_FILE_IO},
    };
    tp_sb_str(sb, "{\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "schema");
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, CLI_HELP_SCHEMA);
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "usage");
    tp_sb_str(sb, ": ");
    tp_sb_json_string(sb, "ntpacker <command> [options]");
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "commands");
    tp_sb_str(sb, ": ");
    emit_string_array(sb, HELP_COMMANDS,
                      sizeof HELP_COMMANDS / sizeof HELP_COMMANDS[0]);
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "global_options");
    tp_sb_str(sb, ": ");
    emit_string_array(sb, HELP_GLOBAL_OPTIONS,
                      sizeof HELP_GLOBAL_OPTIONS /
                          sizeof HELP_GLOBAL_OPTIONS[0]);
    tp_sb_str(sb, ",\n");
    indent(sb, 1);
    tp_sb_json_string(sb, "exit_codes");
    tp_sb_str(sb, ": {\n");
    for (size_t i = 0U; i < sizeof exit_codes / sizeof exit_codes[0]; ++i) {
        indent(sb, 2);
        tp_sb_json_string(sb, exit_codes[i].id);
        tp_sb_str(sb, ": ");
        tp_sb_int(sb, exit_codes[i].code);
        tp_sb_str(sb, i + 1U < sizeof exit_codes / sizeof exit_codes[0]
                           ? ",\n"
                           : "\n");
    }
    indent(sb, 1);
    tp_sb_str(sb, "}\n}");
}

static int cmd_help(bool json) {
    if (!json) {
        print_usage(stdout);
        return CLI_EXIT_OK;
    }
    tp_sb sb = {0};
    build_help(&sb);
    if (sb.oom) {
        tp_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building help payload");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    tp_sb_free(&sb);
    return CLI_EXIT_OK;
}

static void print_usage(FILE *out) {
    (void)fprintf(out,
                  "ntpacker %s -- neotolis texture packer (CLI)\n"
                  "\n"
                  "Usage:\n"
                  "  ntpacker <command> [options]\n"
                  "\n"
                  "Commands:\n"
                  "  pack | export <p>  Pack + export every enabled target (== GUI \"Export All\")\n"
                  "  formats            List the active export-format catalog and diagnostics\n"
                  "  inspect <project>  Dump project state (--json is the contract; text is a summary)\n"
                  "  validate <project> Report every project problem in one pass\n"
                  "  new <path>         Create a new project (seeded default target); refuses to overwrite\n"
                  "  version            Print the version; --json emits the schema manifest\n"
                  "  help               Show this help\n"
                  "\n"
                  "Editing (wave 2 -- load -> mutate -> save; --json emits {ok,verb,count}):\n"
                  "  add <p> <atlas> <path>... [--kind file|folder]  Add source(s); kind is required offline\n"
                  "  remove <p> <atlas> <source>          Remove a source\n"
                  "  set <p> <atlas> <key>=<value>...     Set atlas knobs (max_size, padding, ...)\n"
                  "  sprite set <p> <atlas> <key> <field>=<value>...   Per-sprite override (field=inherit clears)\n"
                  "  sprite unset <p> <atlas> <key>       Clear a sprite's whole override\n"
                  "  anim create|remove|rename|list|add-frame|remove-frame|move-frame|set <p> <atlas> ...\n"
                  "  target add|remove|set <p> <atlas> ...            Export targets\n"
                  "  atlas add|remove|rename <p> ...                  Atlases (by name)\n"
                  "\n"
                  "pack options:\n"
                  "  --atlas <name>     Only pack this atlas (unknown name -> usage error)\n"
                  "  --target <id>      Only export targets with this exporter id\n"
                  "  --out-dir <dir>    Re-root RELATIVE target out_paths under <dir> (vs the project dir)\n"
                  "\n"
                  "Pack and mutation preview option:\n"
                  "  --dry-run          Preview pack or mutation commands without writes; not valid for\n"
                  "                     the read-only anim list query\n"
                  "\n"
                  "anim add-frame option:\n"
                  "  --at <N>           Insert the frame at index N (default: append)\n"
                  "\n"
                  "Global options:\n"
                  "  --json             Machine-readable JSON output (stable per-verb schema)\n"
                  "  --quiet            Suppress progress diagnostics on stderr\n"
                  "  --strict           validate: exit 7 if any error-severity finding\n"
                  "  --help             Show this help\n"
                  "  --version          Print the version\n"
                  "\n"
                  "Exit codes: 0 ok, 1 internal, 2 usage, 3 project, 4 pack, 5 export,\n"
                  "            6 partial, 7 validate(--strict), 8 file I/O; 9+ reserved.\n",
                  NTPACKER_VERSION);
}

static int ntpacker_dispatch_utf8(int argc, char **argv,
                                  app_format_catalog *formats) {
    tp_format_catalog *catalog = formats->catalog;
    /* BLOCKER-3: pin dot-decimal float formatting for every payload, before any
     * output. tp_core's %.9g writers and the CLI's tp_sb_num both depend on it. */
    (void)setlocale(LC_NUMERIC, "C");

    bool json = false;
    bool quiet = false;

    /* Pre-scan the global stream flags so error emission honors --json/--quiet
     * regardless of where they sit relative to the offending token. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) {
            json = true;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = true;
        }
    }

    /* Engine logging: the default nt_log writer sends INFO to STDOUT (the builder
     * is chatty), which would corrupt the machine payload. WARN+ERROR already go
     * to stderr, so capping at WARN keeps stdout = payload only; --quiet gags all. */
    nt_log_set_level(quiet ? NT_LOG_LEVEL_NONE : NT_LOG_LEVEL_WARN);

    bool want_help = false;
    bool want_version = false;
    bool strict = false;
    bool dry_run = false;           /* pack/export and mutation preview */
    const char *opt_atlas = NULL;   /* pack-only value flags (rejected elsewhere below) */
    const char *opt_target = NULL;
    const char *opt_out_dir = NULL;
    const char *opt_at = NULL;    /* anim add-frame only (rejected elsewhere below) */
    const char *opt_kind = NULL;  /* add-only offline source classification */
    const char *positionals[128]; /* verb + its operands; large enough for many sources/frames/keys */
    int npos = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--json") == 0 || strcmp(a, "--quiet") == 0) {
            continue; /* consumed in the pre-scan */
        }
        if (strcmp(a, "--strict") == 0) {
            strict = true; /* validate-only; rejected for other verbs below */
            continue;
        }
        if (strcmp(a, "--dry-run") == 0) {
            dry_run = true;
            continue;
        }
        if (strcmp(a, "--atlas") == 0 || strcmp(a, "--target") == 0 || strcmp(a, "--out-dir") == 0 ||
            strcmp(a, "--at") == 0 || strcmp(a, "--kind") == 0) {
            /* value flags: consume the next token (each rejected for the wrong verb below). */
            if (i + 1 >= argc) {
                cli_emit_error(json, quiet, "usage", "option '%s' needs a value; try 'ntpacker help'", a);
                return CLI_EXIT_USAGE;
            }
            const char *val = argv[++i];
            if (strcmp(a, "--atlas") == 0) {
                opt_atlas = val;
            } else if (strcmp(a, "--target") == 0) {
                opt_target = val;
            } else if (strcmp(a, "--out-dir") == 0) {
                opt_out_dir = val;
            } else if (strcmp(a, "--at") == 0) {
                opt_at = val;
            } else {
                opt_kind = val;
            }
            continue;
        }
        if (strcmp(a, "--help") == 0) {
            want_help = true;
            continue;
        }
        if (strcmp(a, "--version") == 0) {
            want_version = true;
            continue;
        }
        if (a[0] == '-') {
            cli_emit_error(json, quiet, "usage", "unknown option '%s'; try 'ntpacker help'", a);
            return CLI_EXIT_USAGE;
        }
        if (npos < (int)(sizeof positionals / sizeof positionals[0])) {
            positionals[npos++] = a;
        } else {
            cli_emit_error(json, quiet, "usage", "too many arguments; try 'ntpacker help'");
            return CLI_EXIT_USAGE;
        }
    }

    /* --version / --help short-circuit any verb (standard CLI behavior). */
    if (want_version) {
        return cmd_version(catalog, json);
    }
    if (want_help) {
        return cmd_help(json);
    }
    if (npos == 0) {
        /* No command is a usage error (stderr, exit 2) -- NOT the help payload;
         * explicit `help`/--help is the exit-0 stdout path. Keeps stdout clean
         * for pipelines and matches the pinned exit-code contract. */
        cli_emit_error(json, quiet, "usage", "no command given; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *verb = positionals[0];
    const bool is_pack = (strcmp(verb, "pack") == 0 || strcmp(verb, "export") == 0);
    const bool is_mutation = strcmp(verb, "new") == 0 || strcmp(verb, "add") == 0 ||
                             strcmp(verb, "remove") == 0 || strcmp(verb, "set") == 0 ||
                             strcmp(verb, "sprite") == 0 || strcmp(verb, "anim") == 0 ||
                             strcmp(verb, "target") == 0 || strcmp(verb, "atlas") == 0;
    if (!is_pack && (opt_atlas || opt_target || opt_out_dir)) {
        cli_emit_error(json, quiet, "usage", "--atlas/--target/--out-dir are only valid for pack");
        return CLI_EXIT_USAGE;
    }
    if (dry_run && !is_pack && !is_mutation) {
        cli_emit_error(json, quiet, "usage", "--dry-run is only valid for pack or mutation commands");
        return CLI_EXIT_USAGE;
    }
    /* --at is anim-add-frame-only on any verb. */
    if (opt_at && strcmp(verb, "anim") != 0) {
        cli_emit_error(json, quiet, "usage", "--at is only valid for 'anim add-frame'");
        return CLI_EXIT_USAGE;
    }
    if (opt_kind && strcmp(verb, "add") != 0) {
        cli_emit_error(json, quiet, "usage", "--kind is only valid for 'add'");
        return CLI_EXIT_USAGE;
    }
    if (strcmp(verb, "version") == 0) {
        return cmd_version(catalog, json);
    }
    if (strcmp(verb, "help") == 0) {
        return cmd_help(json);
    }
    if (strcmp(verb, "formats") == 0) {
        if (npos != 1 || strict || dry_run) {
            cli_emit_error(json, quiet, "usage",
                           "formats accepts only --json/--quiet");
            return CLI_EXIT_USAGE;
        }
        return cmd_formats(formats, json);
    }
    if (is_pack) {
        if (strict) {
            cli_emit_error(json, quiet, "usage", "--strict is only valid for validate");
            return CLI_EXIT_USAGE;
        }
        if (npos != 2) {
            cli_emit_error(json, quiet, "usage", "%s needs exactly one <project> path; try 'ntpacker help'", verb);
            return CLI_EXIT_USAGE;
        }
        return cmd_pack(catalog, positionals[1], opt_atlas, opt_target,
                        opt_out_dir, dry_run, json, quiet);
    }
    if (strcmp(verb, "inspect") == 0 || strcmp(verb, "validate") == 0) {
        if (npos != 2) {
            cli_emit_error(json, quiet, "usage", "%s needs exactly one <project> path; try 'ntpacker help'", verb);
            return CLI_EXIT_USAGE;
        }
        if (strcmp(verb, "inspect") == 0) {
            if (strict) {
                cli_emit_error(json, quiet, "usage", "--strict is only valid for validate");
                return CLI_EXIT_USAGE;
            }
            return cmd_inspect(catalog, positionals[1], json, quiet);
        }
        return cmd_validate(catalog, positionals[1], json, quiet, strict);
    }
    /* Wave-2 mutation verbs (B4). --strict is validate-only; --at is anim-only. Other
     * pack-only flags were already rejected above (the !is_pack guard). */
    if (strcmp(verb, "new") == 0 || strcmp(verb, "add") == 0 || strcmp(verb, "remove") == 0 ||
        strcmp(verb, "set") == 0 || strcmp(verb, "sprite") == 0 || strcmp(verb, "anim") == 0 ||
        strcmp(verb, "target") == 0 || strcmp(verb, "atlas") == 0) {
        if (strict) {
            cli_emit_error(json, quiet, "usage", "--strict is only valid for validate");
            return CLI_EXIT_USAGE;
        }
        return cmd_mutate(catalog, npos, positionals, opt_at, opt_kind,
                          dry_run, json, quiet);
    }
    cli_emit_error(json, quiet, "usage", "unknown command '%s'; try 'ntpacker help'", verb);
    return CLI_EXIT_USAGE;
}

static int ntpacker_main_utf8(int argc, char **argv) {
    app_format_catalog formats = {0};
    tp_error error = {{0}};
    const tp_status format_status =
        app_format_catalog_open_startup(&formats, &error);
    NT_ASSERT(format_status == TP_STATUS_OK);
    NT_ASSERT(formats.catalog != NULL);
    const int result = ntpacker_dispatch_utf8(argc, argv, &formats);
    app_format_catalog_close(&formats);
    return result;
}

#if defined(_WIN32)
static bool narrow_argv_has_flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv) {
    /* Private build-worker re-exec: argv[1] == "__build-worker".
     * FIRST thing, before UTF-16 conversion, engine, or arg parsing -- a pack
     * re-execs this exe as the worker; ASCII arg, so the narrow argv is enough. */
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    nt_utf8_argv utf8 = {0};
    char error[160] = {0};
    if (!nt_utf8_argv_from_command_line(&utf8, error, sizeof error)) {
        const bool json = narrow_argv_has_flag(argc, argv, "--json");
        const bool quiet = narrow_argv_has_flag(argc, argv, "--quiet");
        cli_emit_error(json, quiet, "invalid_utf16", "%s", error);
        return CLI_EXIT_USAGE;
    }
    const int result = ntpacker_main_utf8(utf8.argc, utf8.argv);
    nt_utf8_argv_dispose(&utf8);
    return result;
}
#else
int main(int argc, char **argv) {
    if (tp_build_is_worker_invocation(argc, argv)) {
        return tp_build_worker_main();
    }
    return ntpacker_main_utf8(argc, argv);
}
#endif
