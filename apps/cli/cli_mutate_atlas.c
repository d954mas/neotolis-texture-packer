#include "cli_mutate_internal.h"

#include <stdio.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"

/* ------------------------------------------------------------------ */
/* set (atlas knobs)                                                  */
/* ------------------------------------------------------------------ */

/* Parses one atlas knob key=value INTO an atlas.settings op payload. Returns 0 or
 * CLI_EXIT_USAGE after emitting a structured error. */
static int fill_knob(tp_op_atlas_settings *s, const char *key, const char *val, bool json, bool quiet) {
    const int matched = cli_fill_registry_field(TP_FIELD_FAMILY_ATLAS, s, &s->mask,
                                                key, val, json, quiet);
    if (matched != 0) {
        return matched > 0 ? 0 : CLI_EXIT_USAGE;
    }
    if (strcmp(key, "name") == 0) {
        cli_emit_error(json, quiet, "usage", "%s",
                       "use 'ntpacker atlas rename <project> <old> <new>' to rename an atlas");
        return CLI_EXIT_USAGE;
    }
    char known[256];
    char m[192];
    (void)snprintf(m, sizeof m, "unknown atlas key '%s' (known: %s)", key,
                   cli_field_key_list(TP_FIELD_FAMILY_ATLAS, known, sizeof known));
    cli_emit_error(json, quiet, "usage", "%s", m);
    return CLI_EXIT_USAGE;
}

int do_set(tp_format_catalog *catalog, const char *const *pos, int npos,
           bool dry_run, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "set needs <project> <atlas> <key>=<value>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, catalog, path, atlas, &atlas_dto,
                             dry_run, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_SETTINGS_SET;
    op.atlas_id = atlas_dto->id;
    int applied = 0;
    for (int i = 3; i < npos; i++) {
        char key[64];
        const char *val = split_kv(pos[i], key, sizeof key);
        if (!val) {
            char m[128];
            (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
            return edit_fail_usage(&edit, json, quiet, "usage", m);
        }
        int kr = fill_knob(&op.u.atlas_settings, key, val, json, quiet); /* PARSES into the op */
        if (kr != 0) {
            edit_close(&edit);
            return kr;
        }
        applied++;
    }
    char human[128];
    (void)snprintf(human, sizeof human, "Set %d knob(s) on '%s'", applied, atlas);
    return commit_session_ops(&edit, &op, 1, "set", applied, human, json, quiet);
}

/* ------------------------------------------------------------------ */
/* atlas                                                              */
/* ------------------------------------------------------------------ */

int do_atlas(tp_format_catalog *catalog, const char *const *pos, int npos,
             bool dry_run, bool json, bool quiet) {
    /* atlas <sub> <project> ... (operates at project level, atlases keyed by name) */
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "atlas needs <sub> <project> <name>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    cli_edit edit;
    int rc = edit_open(&edit, catalog, path, dry_run, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }

    if (strcmp(sub, "add") == 0) {
        if (npos != 4) {
            return edit_fail_usage(&edit, json, quiet, "usage", "atlas add needs <name>");
        }
        const char *name = pos[3];
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_CREATE;
        op.u.atlas_create.name = cli_strdup(name);
        if (!op.u.atlas_create.name) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building atlas");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        if (!cli_gen_id(&op.atlas_id)) { /* OS-RNG fault, not OOM (F4) */
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "rng_failed", "could not generate an atlas id");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Added atlas '%s'", name);
        return commit_session_ops(&edit, &op, 1, "atlas", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 4) {
            return edit_fail_usage(&edit, json, quiet, "usage", "atlas remove needs <name>");
        }
        tp_selector_result atlas_result;
        rc = edit_resolve(&edit, tp_id128_nil(), TP_SEL_ATLAS, pos[3],
                          &atlas_result, json, quiet);
        if (rc != CLI_EXIT_OK) {
            edit_close(&edit);
            return rc;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_REMOVE;
        op.atlas_id = atlas_result.id;
        char human[128];
        (void)snprintf(human, sizeof human, "Removed atlas '%s'", pos[3]);
        return commit_session_ops(&edit, &op, 1, "atlas", 1, human, json, quiet);
    }

    if (strcmp(sub, "rename") == 0) {
        if (npos != 5) {
            return edit_fail_usage(&edit, json, quiet, "usage", "atlas rename needs <old> <new>");
        }
        const char *old = pos[3];
        const char *neu = pos[4];
        tp_selector_result atlas_result;
        rc = edit_resolve(&edit, tp_id128_nil(), TP_SEL_ATLAS, old,
                          &atlas_result, json, quiet);
        if (rc != CLI_EXIT_OK) {
            edit_close(&edit);
            return rc;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_RENAME;
        op.atlas_id = atlas_result.id;
        op.u.atlas_rename.name = cli_strdup(neu);
        if (!op.u.atlas_rename.name) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building atlas");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Renamed atlas '%s' -> '%s'", old, neu);
        return commit_session_ops(&edit, &op, 1, "atlas", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown atlas sub-command '%s'", sub);
    return edit_fail_usage(&edit, json, quiet, "usage", m);
}
