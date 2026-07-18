#include "cli_mutate_internal.h"

#include <stdio.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"

/* ------------------------------------------------------------------ */
/* set (atlas knobs)                                                  */
/* ------------------------------------------------------------------ */

/* Parses one atlas knob key=value INTO an atlas.settings op payload (mask + value).
 * Returns 0 or CLI_EXIT_USAGE after emitting a structured error. Only PARSES (the
 * string->typed value + the fits-int marshalling); the numeric RANGE is core's now. */
static int fill_knob(tp_op_atlas_settings *s, const char *key, const char *val, bool json, bool quiet) {
    int iv = 0;
    bool bv = false;
    float fv = 0.0F;
    char m[192];

#define BADVAL(fmt, ...)                                                                                                \
    do {                                                                                                               \
        (void)snprintf(m, sizeof m, fmt, __VA_ARGS__);                                                                 \
        cli_emit_error(json, quiet, "usage", "%s", m);                                                                 \
        return CLI_EXIT_USAGE;                                                                                         \
    } while (0)

    if (strcmp(key, "max_size") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("max_size = '%s' must be an integer", val);
        }
        s->max_size = iv;
        s->mask |= TP_AF_MAX_SIZE;
    } else if (strcmp(key, "padding") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("padding = '%s' must be an integer", val);
        }
        s->padding = iv;
        s->mask |= TP_AF_PADDING;
    } else if (strcmp(key, "margin") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("margin = '%s' must be an integer", val);
        }
        s->margin = iv;
        s->mask |= TP_AF_MARGIN;
    } else if (strcmp(key, "extrude") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("extrude = '%s' must be an integer", val);
        }
        s->extrude = iv;
        s->mask |= TP_AF_EXTRUDE;
    } else if (strcmp(key, "alpha_threshold") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("alpha_threshold = '%s' must be an integer", val);
        }
        s->alpha_threshold = iv;
        s->mask |= TP_AF_ALPHA_THRESHOLD;
    } else if (strcmp(key, "max_vertices") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("max_vertices = '%s' must be an integer", val);
        }
        s->max_vertices = iv;
        s->mask |= TP_AF_MAX_VERTICES;
    } else if (strcmp(key, "shape") == 0) {
        if (!to_int(val, &iv)) {
            BADVAL("shape = '%s' must be an integer", val);
        }
        s->shape = iv;
        s->mask |= TP_AF_SHAPE;
    } else if (strcmp(key, "allow_transform") == 0) {
        if (!to_bool(val, &bv)) {
            BADVAL("allow_transform = '%s' must be 0/1/true/false", val);
        }
        s->allow_transform = bv;
        s->mask |= TP_AF_ALLOW_TRANSFORM;
    } else if (strcmp(key, "power_of_two") == 0) {
        if (!to_bool(val, &bv)) {
            BADVAL("power_of_two = '%s' must be 0/1/true/false", val);
        }
        s->power_of_two = bv;
        s->mask |= TP_AF_POWER_OF_TWO;
    } else if (strcmp(key, "pixels_per_unit") == 0) {
        if (!to_float(val, &fv)) {
            BADVAL("pixels_per_unit = '%s' must be a number", val);
        }
        s->pixels_per_unit = fv;
        s->mask |= TP_AF_PIXELS_PER_UNIT;
    } else if (strcmp(key, "name") == 0) {
        BADVAL("%s", "use 'ntpacker atlas rename <project> <old> <new>' to rename an atlas");
    } else {
        (void)snprintf(m, sizeof m, "unknown atlas key '%s' (known: %s)", key, k_atlas_knobs);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
#undef BADVAL
    return 0;
}

int do_set(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "set needs <project> <atlas> <key>=<value>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
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

int do_atlas(const char *const *pos, int npos, bool json, bool quiet) {
    /* atlas <sub> <project> ... (operates at project level, atlases keyed by name) */
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "atlas needs <sub> <project> <name>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    cli_edit edit;
    int rc = edit_open(&edit, path, json, quiet);
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
