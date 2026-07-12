/* ntpacker -- AI-first CLI frontend over tp_core (plan docs/plans/op-layer-and-cli.md,
 * step B1). Thin client: hand-rolled args, verb dispatch, versioned --json payloads,
 * contract exit codes, structured errors. B1 ships `version` + `help`; the pack/
 * inspect/validate/new verbs land in B2-B4 over this same dispatch. */
#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "log/nt_log.h"

#include "cli_cmds.h"
#include "cli_exit.h"
#include "cli_out.h"
#include "ntpacker_version.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_project.h"

static void indent(cli_sb *sb, int depth) {
    for (int i = 0; i < depth; i++) {
        cli_sb_str(sb, "  ");
    }
}

/* One "caps" object (the exporter's format expressiveness). Fields are the
 * append-only tp_export_caps set; snake_case/lowercase already. */
static void emit_caps(cli_sb *sb, int depth, const tp_export_caps *c) {
    struct {
        const char *key;
        bool val;
    } fields[] = {
        {"rotate90", c->rotate90}, {"flips", c->flips},         {"polygons", c->polygons}, {"pivot", c->pivot},
        {"slice9", c->slice9},     {"multipage", c->multipage}, {"aliases", c->aliases},
    };
    int n = (int)(sizeof fields / sizeof fields[0]);
    cli_sb_str(sb, "{\n");
    for (int i = 0; i < n; i++) {
        indent(sb, depth + 1);
        cli_sb_json_str(sb, fields[i].key);
        cli_sb_str(sb, ": ");
        cli_sb_str(sb, fields[i].val ? "true" : "false");
        cli_sb_str(sb, (i + 1 < n) ? ",\n" : "\n");
    }
    indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* The `version --json` schema manifest (plan "CLI v1 contract"): app version,
 * on-disk project schema, each JSON-emitting verb's payload schema, known export
 * formats + versions, and the live exporter registry with capabilities. Every
 * number/id is sourced from a core constant or the registry -- no hand-copied
 * values, no exporter-id literals (boundary gate R2). */
static void build_manifest(cli_sb *sb) {
    cli_sb_str(sb, "{\n");
    indent(sb, 1);
    cli_sb_json_str(sb, "schema");
    cli_sb_str(sb, ": 1,\n");
    indent(sb, 1);
    cli_sb_json_str(sb, "app_version");
    cli_sb_str(sb, ": ");
    cli_sb_json_str(sb, NTPACKER_VERSION);
    cli_sb_str(sb, ",\n");
    indent(sb, 1);
    cli_sb_json_str(sb, "project_schema");
    cli_sb_str(sb, ": ");
    cli_sb_int(sb, TP_PROJECT_SCHEMA_VERSION);
    cli_sb_str(sb, ",\n");

    /* verbs: each verb that emits a --json payload -> its payload schema version.
     * inspect/validate landed in B2; grows as pack lands. */
    indent(sb, 1);
    cli_sb_json_str(sb, "verbs");
    cli_sb_str(sb, ": {\n");
    indent(sb, 2);
    cli_sb_json_str(sb, "inspect");
    cli_sb_str(sb, ": 1,\n");
    indent(sb, 2);
    cli_sb_json_str(sb, "validate");
    cli_sb_str(sb, ": 1,\n");
    indent(sb, 2);
    cli_sb_json_str(sb, "pack");
    cli_sb_str(sb, ": 1,\n");
    indent(sb, 2);
    cli_sb_json_str(sb, "version");
    cli_sb_str(sb, ": 1\n");
    indent(sb, 1);
    cli_sb_str(sb, "},\n");

    /* formats: export FORMAT -> format-schema version. json-neotolis key comes
     * from the shared exporter-id constant (never a literal); its value is the
     * public json schema constant; defold-tpinfo carries the tpinfo version. */
    indent(sb, 1);
    cli_sb_json_str(sb, "formats");
    cli_sb_str(sb, ": {\n");
    indent(sb, 2);
    cli_sb_json_str(sb, TP_EXPORTER_ID_JSON_NEOTOLIS);
    cli_sb_str(sb, ": ");
    cli_sb_int(sb, TP_JSON_NEOTOLIS_SCHEMA_VERSION);
    cli_sb_str(sb, ",\n");
    indent(sb, 2);
    cli_sb_json_str(sb, "defold-tpinfo");
    cli_sb_str(sb, ": ");
    cli_sb_json_str(sb, TP_DEFOLD_TPINFO_VERSION);
    cli_sb_str(sb, "\n");
    indent(sb, 1);
    cli_sb_str(sb, "},\n");

    /* exporters: the live registry (built-ins + any runtime-registered). */
    indent(sb, 1);
    cli_sb_json_str(sb, "exporters");
    cli_sb_str(sb, ": [\n");
    int count = tp_exporter_count();
    for (int i = 0; i < count; i++) {
        const tp_exporter *e = tp_exporter_at(i);
        indent(sb, 2);
        cli_sb_str(sb, "{\n");
        indent(sb, 3);
        cli_sb_json_str(sb, "id");
        cli_sb_str(sb, ": ");
        cli_sb_json_str(sb, e->id);
        cli_sb_str(sb, ",\n");
        indent(sb, 3);
        cli_sb_json_str(sb, "name");
        cli_sb_str(sb, ": ");
        cli_sb_json_str(sb, e->display_name);
        cli_sb_str(sb, ",\n");
        indent(sb, 3);
        cli_sb_json_str(sb, "ext");
        cli_sb_str(sb, ": ");
        cli_sb_json_str(sb, e->extension);
        cli_sb_str(sb, ",\n");
        indent(sb, 3);
        cli_sb_json_str(sb, "caps");
        cli_sb_str(sb, ": ");
        emit_caps(sb, 3, &e->caps);
        cli_sb_str(sb, "\n");
        indent(sb, 2);
        cli_sb_putc(sb, '}');
        cli_sb_str(sb, (i + 1 < count) ? ",\n" : "\n");
    }
    indent(sb, 1);
    cli_sb_str(sb, "]\n");
    cli_sb_str(sb, "}");
}

static int cmd_version(bool json) {
    if (!json) {
        (void)printf("ntpacker %s\n", NTPACKER_VERSION);
        return CLI_EXIT_OK;
    }
    cli_sb sb = {0};
    build_manifest(&sb);
    if (sb.oom) {
        cli_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building version manifest");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
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
                  "  inspect <project>  Dump project state (--json is the contract; text is a summary)\n"
                  "  validate <project> Report every project problem in one pass\n"
                  "  version            Print the version; --json emits the schema manifest\n"
                  "  help               Show this help\n"
                  "\n"
                  "Planned (not in this build; calling one is a usage error for now):\n"
                  "  new                Create a new project\n"
                  "\n"
                  "pack options:\n"
                  "  --atlas <name>     Only pack this atlas (unknown name -> usage error)\n"
                  "  --target <id>      Only export targets with this exporter id\n"
                  "  --out-dir <dir>    Re-root RELATIVE target out_paths under <dir> (vs the project dir)\n"
                  "  --dry-run          Report what pack WOULD do (pages, would_write, predicted\n"
                  "                     losses) and write NO files\n"
                  "\n"
                  "Global options:\n"
                  "  --json             Machine-readable JSON output (stable per-verb schema)\n"
                  "  --quiet            Suppress progress diagnostics on stderr\n"
                  "  --strict           validate: exit 7 if any error-severity finding\n"
                  "  --help             Show this help\n"
                  "  --version          Print the version\n"
                  "\n"
                  "Exit codes: 0 ok, 1 internal, 2 usage, 3 project, 4 pack, 5 export,\n"
                  "            6 partial, 7 validate(--strict).\n",
                  NTPACKER_VERSION);
}

int main(int argc, char **argv) {
    /* BLOCKER-3: pin dot-decimal float formatting for every payload, before any
     * output. tp_core's %.9g writers and the CLI's cli_sb_num both depend on it. */
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
    bool dry_run = false;           /* pack-only; rejected for other verbs below */
    const char *opt_atlas = NULL;   /* pack-only value flags (rejected elsewhere below) */
    const char *opt_target = NULL;
    const char *opt_out_dir = NULL;
    const char *positionals[8]; /* verb + its operands; plenty for v1 verbs */
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
            dry_run = true; /* pack-only; rejected for other verbs below */
            continue;
        }
        if (strcmp(a, "--atlas") == 0 || strcmp(a, "--target") == 0 || strcmp(a, "--out-dir") == 0) {
            /* value flags: consume the next token (pack-only; rejected elsewhere below). */
            if (i + 1 >= argc) {
                cli_emit_error(json, quiet, "usage", "option '%s' needs a value; try 'ntpacker help'", a);
                return CLI_EXIT_USAGE;
            }
            const char *val = argv[++i];
            if (strcmp(a, "--atlas") == 0) {
                opt_atlas = val;
            } else if (strcmp(a, "--target") == 0) {
                opt_target = val;
            } else {
                opt_out_dir = val;
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
        return cmd_version(json);
    }
    if (want_help) {
        print_usage(stdout);
        return CLI_EXIT_OK;
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
    /* pack-only flags are a usage error on any other verb (mirrors --strict). */
    if (!is_pack && (opt_atlas || opt_target || opt_out_dir || dry_run)) {
        cli_emit_error(json, quiet, "usage", "--atlas/--target/--out-dir/--dry-run are only valid for pack");
        return CLI_EXIT_USAGE;
    }
    if (strcmp(verb, "version") == 0) {
        return cmd_version(json);
    }
    if (strcmp(verb, "help") == 0) {
        print_usage(stdout);
        return CLI_EXIT_OK;
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
        return cmd_pack(positionals[1], opt_atlas, opt_target, opt_out_dir, dry_run, json, quiet);
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
            return cmd_inspect(positionals[1], json, quiet);
        }
        return cmd_validate(positionals[1], json, quiet, strict);
    }
    cli_emit_error(json, quiet, "usage", "unknown command '%s'; try 'ntpacker help'", verb);
    return CLI_EXIT_USAGE;
}
