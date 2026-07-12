/* `ntpacker inspect <project> [--json]` (plan B2). Dumps project state: settings,
 * sources (with resolved abs path + kind), sprites RESOLVED via a tp_pack_input_build
 * DRY pass (no packing -- descs only, disk-touching for folder sources is intended),
 * animations, and targets. The --json payload (CLI_INSPECT_SCHEMA) is the contract; the
 * human summary is explicitly cosmetic (tested only as non-empty, exit 0). */
#include "cli_cmds.h" /* CLI_INSPECT_SCHEMA */

#include <stdio.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h"

/* Emits `,\n` (or `\n` for the first entry) + indent + "key": -- the same
 * first-tracking pattern tp_project.c's writer uses, so nesting stays balanced. */
static void key(cli_sb *sb, int depth, bool *first, const char *k) {
    cli_sb_str(sb, *first ? "\n" : ",\n");
    *first = false;
    cli_sb_indent(sb, depth);
    cli_sb_json_str(sb, k);
    cli_sb_str(sb, ": ");
}

static void emit_num_array2(cli_sb *sb, double a, double b) {
    cli_sb_putc(sb, '[');
    cli_sb_num(sb, a);
    cli_sb_str(sb, ", ");
    cli_sb_num(sb, b);
    cli_sb_putc(sb, ']');
}

static void emit_settings(cli_sb *sb, int depth, const tp_project_atlas *a) {
    bool first = true;
    cli_sb_putc(sb, '{');
    /* All knobs AS STORED (not the export-path clamp) -- the raw project state. */
    key(sb, depth + 1, &first, "max_size");
    cli_sb_int(sb, a->max_size);
    key(sb, depth + 1, &first, "padding");
    cli_sb_int(sb, a->padding);
    key(sb, depth + 1, &first, "margin");
    cli_sb_int(sb, a->margin);
    key(sb, depth + 1, &first, "extrude");
    cli_sb_int(sb, a->extrude);
    key(sb, depth + 1, &first, "alpha_threshold");
    cli_sb_int(sb, a->alpha_threshold);
    key(sb, depth + 1, &first, "max_vertices");
    cli_sb_int(sb, a->max_vertices);
    key(sb, depth + 1, &first, "shape");
    cli_sb_int(sb, a->shape);
    key(sb, depth + 1, &first, "allow_transform");
    cli_sb_str(sb, a->allow_transform ? "true" : "false");
    key(sb, depth + 1, &first, "power_of_two");
    cli_sb_str(sb, a->power_of_two ? "true" : "false");
    key(sb, depth + 1, &first, "pixels_per_unit");
    cli_sb_num(sb, (double)a->pixels_per_unit);
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* Resolves a source to its abs path + kind (file/dir/missing) for the dump. */
static void emit_source(cli_sb *sb, int depth, const tp_project *p, const char *src) {
    char abs[512];
    const char *kind;
    if (tp_project_resolve_path(p, src, abs, sizeof abs) != TP_STATUS_OK) {
        abs[0] = '\0';
        kind = "missing";
    } else if (!tp_scan_exists(abs)) {
        kind = "missing";
    } else if (tp_scan_is_dir(abs)) {
        kind = "dir";
    } else {
        kind = "file";
    }
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &first, "path");
    cli_sb_json_str(sb, src);
    key(sb, depth + 1, &first, "abs");
    cli_sb_json_str(sb, abs);
    key(sb, depth + 1, &first, "kind");
    cli_sb_json_str(sb, kind);
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* One resolved sprite: raw name (ext kept), export key, abs decode path, and any
 * per-sprite overrides SET on the project sprite (read from the project model, not
 * the encoded desc -- keeps the frontend clear of the pack-desc encoding, R3 gate). */
static void emit_sprite(cli_sb *sb, int depth, tp_project_atlas *a, const char *raw, const char *abs) {
    char keybuf[256];
    tp_sprite_export_key(raw, keybuf, sizeof keybuf);
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &first, "name");
    cli_sb_json_str(sb, raw);
    key(sb, depth + 1, &first, "key");
    cli_sb_json_str(sb, keybuf);
    key(sb, depth + 1, &first, "abs");
    cli_sb_json_str(sb, abs ? abs : "");
    const tp_project_sprite *ov = tp_project_atlas_find_sprite(a, keybuf);
    if (ov) {
        if (ov->rename) {
            key(sb, depth + 1, &first, "rename");
            cli_sb_json_str(sb, ov->rename);
        }
        if (ov->origin_x != TP_PROJECT_ORIGIN_DEFAULT || ov->origin_y != TP_PROJECT_ORIGIN_DEFAULT) {
            key(sb, depth + 1, &first, "origin");
            emit_num_array2(sb, (double)ov->origin_x, (double)ov->origin_y);
        }
        if (ov->slice9_lrtb[0] || ov->slice9_lrtb[1] || ov->slice9_lrtb[2] || ov->slice9_lrtb[3]) {
            key(sb, depth + 1, &first, "slice9");
            cli_sb_putc(sb, '[');
            for (int k = 0; k < 4; k++) {
                if (k) {
                    cli_sb_str(sb, ", ");
                }
                cli_sb_int(sb, (long)ov->slice9_lrtb[k]);
            }
            cli_sb_putc(sb, ']');
        }
        /* Overrides carry the INHERIT sentinel when unset (never emitted). shape/
         * margin/etc. are the stored atlas-shape-semantics values. */
        if (ov->ov_shape != TP_PROJECT_OV_INHERIT) {
            key(sb, depth + 1, &first, "shape");
            cli_sb_int(sb, (long)ov->ov_shape);
        }
        if (ov->ov_allow_rotate != TP_PROJECT_OV_INHERIT) {
            key(sb, depth + 1, &first, "allow_rotate");
            cli_sb_int(sb, (long)ov->ov_allow_rotate);
        }
        if (ov->ov_max_vertices != TP_PROJECT_OV_INHERIT) {
            key(sb, depth + 1, &first, "max_vertices");
            cli_sb_int(sb, (long)ov->ov_max_vertices);
        }
        if (ov->ov_margin != TP_PROJECT_OV_INHERIT) {
            key(sb, depth + 1, &first, "margin");
            cli_sb_int(sb, (long)ov->ov_margin);
        }
        if (ov->ov_extrude != TP_PROJECT_OV_INHERIT) {
            key(sb, depth + 1, &first, "extrude");
            cli_sb_int(sb, (long)ov->ov_extrude);
        }
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* Format a structural shape-ID into `out` (>= TP_ID_TEXT_CAP); empty on failure. */
static void fmt_id(char *out, size_t cap, tp_id_kind kind, tp_id128 id) {
    if (tp_id_format(kind, id, out, cap, NULL) != TP_STATUS_OK) {
        out[0] = '\0';
    }
}

static void emit_anim(cli_sb *sb, int depth, const tp_project_anim *an) {
    bool first = true;
    cli_sb_putc(sb, '{');
    char idtext[TP_ID_TEXT_CAP];
    fmt_id(idtext, sizeof idtext, TP_ID_KIND_ANIM, an->id);
    key(sb, depth + 1, &first, "id"); /* structural shape-ID */
    cli_sb_json_str(sb, idtext);
    key(sb, depth + 1, &first, "name"); /* logical/display name (name-keyed) */
    cli_sb_json_str(sb, an->name);
    key(sb, depth + 1, &first, "fps");
    cli_sb_num(sb, (double)an->fps);
    key(sb, depth + 1, &first, "playback");
    cli_sb_int(sb, an->playback);
    key(sb, depth + 1, &first, "flip_h");
    cli_sb_str(sb, an->flip_h ? "true" : "false");
    key(sb, depth + 1, &first, "flip_v");
    cli_sb_str(sb, an->flip_v ? "true" : "false");
    key(sb, depth + 1, &first, "frames");
    if (an->frame_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < an->frame_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            cli_sb_json_str(sb, an->frames[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void emit_target(cli_sb *sb, int depth, const tp_project_target *t) {
    bool first = true;
    cli_sb_putc(sb, '{');
    char idtext[TP_ID_TEXT_CAP];
    fmt_id(idtext, sizeof idtext, TP_ID_KIND_TARGET, t->id);
    key(sb, depth + 1, &first, "id"); /* structural shape-ID */
    cli_sb_json_str(sb, idtext);
    key(sb, depth + 1, &first, "exporter_id");
    cli_sb_json_str(sb, t->exporter_id);
    key(sb, depth + 1, &first, "out_path");
    cli_sb_json_str(sb, t->out_path);
    key(sb, depth + 1, &first, "enabled");
    cli_sb_str(sb, t->enabled ? "true" : "false");
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void emit_atlas(cli_sb *sb, int depth, tp_project *p, int ai) {
    tp_project_atlas *a = &p->atlases[ai];
    bool first = true;
    cli_sb_putc(sb, '{');

    char idtext[TP_ID_TEXT_CAP];
    fmt_id(idtext, sizeof idtext, TP_ID_KIND_ATLAS, a->id);
    key(sb, depth + 1, &first, "id"); /* structural shape-ID */
    cli_sb_json_str(sb, idtext);

    key(sb, depth + 1, &first, "name");
    cli_sb_json_str(sb, a->name);

    key(sb, depth + 1, &first, "settings");
    emit_settings(sb, depth + 1, a);

    key(sb, depth + 1, &first, "sources");
    if (a->source_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < a->source_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_source(sb, depth + 2, p, a->sources[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    /* Sprites: DRY pass through the shared bridge (same descs a pack would use). */
    key(sb, depth + 1, &first, "sprites");
    tp_pack_input input;
    tp_error err = {0};
    tp_status bst = tp_pack_input_build(p, ai, &input, &err);
    if (bst != TP_STATUS_OK || input.count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < input.count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_sprite(sb, depth + 2, a, input.descs[i].name, input.descs[i].path);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }
    tp_pack_input_free(&input);

    key(sb, depth + 1, &first, "animations");
    if (a->animation_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_anim(sb, depth + 2, &a->animations[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    key(sb, depth + 1, &first, "targets");
    if (a->target_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < a->target_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_target(sb, depth + 2, &a->targets[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void build_inspect(cli_sb *sb, tp_project *p, const char *path) {
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, 1, &first, "schema");
    cli_sb_int(sb, CLI_INSPECT_SCHEMA);

    key(sb, 1, &first, "project");
    {
        bool pf = true;
        cli_sb_putc(sb, '{');
        key(sb, 2, &pf, "path");
        cli_sb_json_str(sb, path);
        key(sb, 2, &pf, "schema_version");
        cli_sb_int(sb, p->schema_version);
        key(sb, 2, &pf, "project_dir");
        cli_sb_json_str(sb, p->project_dir ? p->project_dir : "");
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, '}');
    }

    key(sb, 1, &first, "atlases");
    if (p->atlas_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < p->atlas_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, 2);
            emit_atlas(sb, 2, p, i);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, ']');
    }
    cli_sb_str(sb, "\n}");
}

/* Cosmetic human summary (NOT a contract): one line per atlas. */
static void print_inspect_human(tp_project *p, const char *path) {
    (void)printf("project: %s (schema %d)\n", path, p->schema_version);
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        tp_pack_input input;
        tp_error err = {0};
        int sprites = 0;
        int missing = 0;
        if (tp_pack_input_build(p, ai, &input, &err) == TP_STATUS_OK) {
            sprites = input.count;
            missing = input.missing_sources;
        }
        tp_pack_input_free(&input);
        (void)printf("atlas '%s': %d sprites, %d source%s (%d missing), %d target%s, %d animation%s\n", a->name,
                     sprites, a->source_count, a->source_count == 1 ? "" : "s", missing, a->target_count,
                     a->target_count == 1 ? "" : "s", a->animation_count, a->animation_count == 1 ? "" : "s");
    }
}

int cmd_inspect(const char *path, bool json, bool quiet) {
    tp_project *p = NULL;
    int rc = cli_load_project(path, json, quiet, &p);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    if (json) {
        cli_sb sb = {0};
        build_inspect(&sb, p, path);
        if (sb.oom) {
            cli_sb_free(&sb);
            tp_project_destroy(p);
            cli_emit_error(true, false, "oom", "out of memory building inspect payload");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else {
        print_inspect_human(p, path);
    }
    tp_project_destroy(p);
    return CLI_EXIT_OK;
}
