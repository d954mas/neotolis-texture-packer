#include "cli_mutate_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_srckey.h"

/* ------------------------------------------------------------------ */
/* sprite set / unset                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    bool set_origin, origin_inherit;
    float ox, oy;
    bool set_slice9, slice9_inherit;
    int s9[4];
    bool set_rename, rename_inherit;
    const char *rename;
    bool set_shape, shape_inherit;
    int shape;
    bool set_rot, rot_inherit;
    int rot;
    bool set_maxv, maxv_inherit;
    int maxv;
    bool set_margin, margin_inherit;
    int margin;
    bool set_extrude, extrude_inherit;
    int extrude;
} sprite_edit;

/* Parses lexical values into the transport-width operation fields. Semantic
 * domains (shape, slice9, max vertices, margins, etc.) are core-owned and are
 * rejected by tp_operation_validate with the same structured result as every
 * other client. */
static int parse_sprite_field(sprite_edit *e, const char *tok, bool json, bool quiet) {
    char key[64];
    const char *val = split_kv(tok, key, sizeof key);
    char m[160];
    if (!val) {
        (void)snprintf(m, sizeof m, "expected field=value, got '%s'", tok);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
    const bool inherit = (strcmp(val, "inherit") == 0);

#define BADF(fmt, ...)                                                                                                  \
    do {                                                                                                               \
        (void)snprintf(m, sizeof m, fmt, __VA_ARGS__);                                                                 \
        cli_emit_error(json, quiet, "usage", "%s", m);                                                                 \
        return CLI_EXIT_USAGE;                                                                                         \
    } while (0)

    if (strcmp(key, "origin") == 0) {
        e->set_origin = true;
        e->origin_inherit = inherit;
        if (!inherit) {
            float xy[2];
            if (!to_floats_csv(val, xy, 2)) {
                BADF("origin = '%s' must be x,y (two numbers) or 'inherit'", val);
            }
            e->ox = xy[0];
            e->oy = xy[1];
        }
    } else if (strcmp(key, "slice9") == 0) {
        e->set_slice9 = true;
        e->slice9_inherit = inherit;
        if (!inherit) {
            long v[4];
            if (!to_longs_csv(val, v, 4)) {
                BADF("slice9 = '%s' must be l,r,t,b (four integers) or 'inherit'", val);
            }
            for (int k = 0; k < 4; k++) {
                if (v[k] < INT_MIN || v[k] > INT_MAX) {
                    BADF("slice9 component %ld does not fit an integer", v[k]);
                }
                e->s9[k] = (int)v[k];
            }
        }
    } else if (strcmp(key, "rename") == 0) {
        e->set_rename = true;
        e->rename_inherit = inherit || val[0] == '\0';
        e->rename = inherit ? NULL : val;
    } else if (strcmp(key, "shape") == 0) {
        e->set_shape = true;
        e->shape_inherit = inherit;
        int v = 0;
        if (!inherit && !to_int(val, &v)) {
            BADF("shape = '%s' must be an integer or 'inherit'", val);
        }
        e->shape = v;
    } else if (strcmp(key, "allow_rotate") == 0) {
        e->set_rot = true;
        e->rot_inherit = inherit;
        int v = 0;
        if (!inherit && !to_int(val, &v)) {
            BADF("allow_rotate = '%s' must be an integer or 'inherit'", val);
        }
        e->rot = v;
    } else if (strcmp(key, "max_vertices") == 0) {
        e->set_maxv = true;
        e->maxv_inherit = inherit;
        int v = 0;
        if (!inherit && !to_int(val, &v)) {
            BADF("max_vertices = '%s' must be an integer or 'inherit'", val);
        }
        e->maxv = v;
    } else if (strcmp(key, "margin") == 0) {
        e->set_margin = true;
        e->margin_inherit = inherit;
        int v = 0;
        if (!inherit && !to_int(val, &v)) {
            BADF("margin = '%s' must be an integer or 'inherit'", val);
        }
        e->margin = v;
    } else if (strcmp(key, "extrude") == 0) {
        e->set_extrude = true;
        e->extrude_inherit = inherit;
        int v = 0;
        if (!inherit && !to_int(val, &v)) {
            BADF("extrude = '%s' must be an integer or 'inherit'", val);
        }
        e->extrude = v;
    } else {
        (void)snprintf(m, sizeof m, "unknown sprite field '%s' (known: %s)", key, k_sprite_fields);
        cli_emit_error(json, quiet, "usage", "%s", m);
        return CLI_EXIT_USAGE;
    }
#undef BADF
    return 0;
}

int do_sprite_set(const char *const *pos, int npos, bool dry_run, bool json, bool quiet) {
    /* sprite set <project> <atlas> <key> <field>=<value>... */
    if (npos < 6) {
        cli_emit_error(json, quiet, "usage",
                       "sprite set needs <project> <atlas> <key> <field>=<value>...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[2];
    const char *atlas = pos[3];
    const char *key = pos[4];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, dry_run, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_id128 aid = atlas_dto->id;
    tp_selector_result sprite_result;
    tp_id128 source_id = tp_id128_nil();
    char source_key[TP_SRCKEY_MAX];
    rc = edit_resolve_sprite(&edit, aid, key, &sprite_result, &source_id,
                             source_key, sizeof source_key, json, quiet);
    if (rc != CLI_EXIT_OK) {
        edit_close(&edit);
        return rc;
    }

    /* Parse ALL fields first (a bad field never leaves a half-applied entry). */
    sprite_edit e;
    memset(&e, 0, sizeof e);
    for (int i = 5; i < npos; i++) {
        int pr = parse_sprite_field(&e, pos[i], json, quiet);
        if (pr != 0) {
            edit_close(&edit);
            return pr;
        }
    }

    /* Build up to two ops. Emit the rename (SPRITE_NAME_SET) BEFORE the override
     * SET/CLEAR, NOT after. When the override clears the last field to INHERIT, the
     * record becomes all-default and apply's post-set prune drops it; a following
     * rename would then re-add it at the END of the array -> reorder -> different saved
     * bytes than the pre-cutover single in-place edit. Doing the rename FIRST leaves the
     * record non-default (its rename is set) when the override is cleared, so the prune
     * keeps it IN PLACE. The frontend selector was resolved above through the runtime
     * sprite index, so every emitted operation carries the canonical persistent
     * (source_id, normalized source-local key) address. */
    tp_operation ops[2];
    int n = 0;
    bool any_override = e.set_origin || e.set_slice9 || e.set_shape || e.set_rot || e.set_maxv || e.set_margin ||
                        e.set_extrude;
    if (e.set_rename) {
        tp_operation *op = &ops[n];
        memset(op, 0, sizeof *op);
        op->kind = TP_OP_SPRITE_NAME_SET;
        op->atlas_id = aid;
        op->u.sprite_name.source_id = source_id;
        op->u.sprite_name.src_key = cli_strdup(source_key);
        op->u.sprite_name.name = e.rename_inherit ? NULL : cli_strdup(e.rename);
        if (!op->u.sprite_name.src_key || (!e.rename_inherit && !op->u.sprite_name.name)) {
            free_ops(ops, n + 1);
            cli_emit_error(json, quiet, "oom", "out of memory building sprite op");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        n++;
    }
    if (any_override) {
        tp_operation *op = &ops[n];
        memset(op, 0, sizeof *op);
        op->kind = TP_OP_SPRITE_OVERRIDE_SET;
        op->atlas_id = aid;
        op->u.sprite_set.source_id = source_id;
        op->u.sprite_set.src_key = cli_strdup(source_key);
        if (!op->u.sprite_set.src_key) {
            free_ops(ops, n + 1); /* also frees the rename op already built at ops[0] */
            cli_emit_error(json, quiet, "oom", "out of memory building sprite op");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        uint32_t mask = 0;
        if (e.set_origin) {
            mask |= TP_SPF_ORIGIN;
            op->u.sprite_set.origin_x = e.origin_inherit ? TP_PROJECT_ORIGIN_DEFAULT : e.ox;
            op->u.sprite_set.origin_y = e.origin_inherit ? TP_PROJECT_ORIGIN_DEFAULT : e.oy;
        }
        if (e.set_slice9) {
            mask |= TP_SPF_SLICE9;
            for (int k = 0; k < 4; k++) {
                op->u.sprite_set.slice9[k] = e.slice9_inherit ? 0 : e.s9[k];
            }
        }
        if (e.set_shape) {
            mask |= TP_SPF_SHAPE;
            op->u.sprite_set.ov_shape = e.shape_inherit ? TP_PROJECT_OV_INHERIT : e.shape;
        }
        if (e.set_rot) {
            mask |= TP_SPF_ALLOW_ROTATE;
            op->u.sprite_set.ov_allow_rotate = e.rot_inherit ? TP_PROJECT_OV_INHERIT : e.rot;
        }
        if (e.set_maxv) {
            mask |= TP_SPF_MAX_VERTICES;
            op->u.sprite_set.ov_max_vertices = e.maxv_inherit ? TP_PROJECT_OV_INHERIT : e.maxv;
        }
        if (e.set_margin) {
            mask |= TP_SPF_MARGIN;
            op->u.sprite_set.ov_margin = e.margin_inherit ? TP_PROJECT_OV_INHERIT : e.margin;
        }
        if (e.set_extrude) {
            mask |= TP_SPF_EXTRUDE;
            op->u.sprite_set.ov_extrude = e.extrude_inherit ? TP_PROJECT_OV_INHERIT : e.extrude;
        }
        op->u.sprite_set.mask = mask;
        n++;
    }

    char human[192];
    (void)snprintf(human, sizeof human, "Set override(s) on sprite '%s' in '%s'", key, atlas);
    return commit_session_ops(&edit, ops, n, "sprite", 1, human, json, quiet);
}

int do_sprite_unset(const char *const *pos, int npos, bool dry_run, bool json, bool quiet) {
    /* sprite unset <project> <atlas> <key> */
    if (npos != 5) {
        cli_emit_error(json, quiet, "usage", "sprite unset needs <project> <atlas> <key>; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[2];
    const char *atlas = pos[3];
    const char *key = pos[4];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, dry_run, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_selector_result sprite_result;
    tp_id128 source_id = tp_id128_nil();
    char source_key[TP_SRCKEY_MAX];
    rc = edit_resolve_sprite(&edit, atlas_dto->id, key, &sprite_result,
                             &source_id, source_key, sizeof source_key, json,
                             quiet);
    if (rc != CLI_EXIT_OK) {
        edit_close(&edit);
        return rc;
    }
    /* Declarative clear: SPRITE_OVERRIDE_CLEAR with the ALL mask drops the whole
     * canonical (source_id, key) record. */
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SPRITE_OVERRIDE_CLEAR;
    op.atlas_id = atlas_dto->id;
    op.u.sprite_clear.source_id = source_id;
    op.u.sprite_clear.src_key = cli_strdup(source_key);
    op.u.sprite_clear.mask = TP_SPF_ALL;
    if (!op.u.sprite_clear.src_key) {
        cli_emit_error(json, quiet, "oom", "out of memory building sprite op");
        edit_close(&edit);
        return CLI_EXIT_INTERNAL;
    }
    char human[160];
    (void)snprintf(human, sizeof human, "Cleared overrides on sprite '%s' in '%s'", key, atlas);
    return commit_session_ops(&edit, &op, 1, "sprite", 1, human, json, quiet);
}
