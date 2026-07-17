/* `ntpacker inspect <project> [--json]` (plan B2). Dumps project state: settings,
 * sources (with resolved abs path + kind), sprites RESOLVED via a tp_pack_input_build
 * DRY pass (no packing -- descs only, disk-touching for folder sources is intended),
 * animations, and targets. The --json payload (CLI_INSPECT_SCHEMA) is the contract; the
 * human summary is explicitly cosmetic (tested only as non-empty, exit 0). */
#include "cli_cmds.h" /* CLI_INSPECT_SCHEMA */

#include <stdio.h>
#include <string.h>
#ifdef NTPACKER_CLI_INSPECT_FAULT_SEAM
#include <stdlib.h>
#endif

#include "cli_exit.h"
#include "cli_out.h"
#include "ntpacker_id_fmt.h" /* ntpacker_fmt_shape_id (shared with cli_mutate) */
#include "tp_core/tp_error.h"
#include "tp_core/tp_names.h"
#include "tp_core/tp_scan.h"
#include "tp_core/tp_session.h"
#include "tp_core/tp_sprite_index.h" /* resolved sprite index: sprite_id + owning source */

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

static void emit_settings(cli_sb *sb, int depth, const tp_snapshot_atlas *a) {
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

/* Dumps a tagged source (schema v3): its structural id, project-relative path,
 * resolved abs path, and the RUNTIME kind (file/dir/missing -- what a scan sees on
 * disk right now, which may differ from the stored classification for a missing
 * source). `stored_kind` reports the persisted folder/file classification. */
static void emit_source(cli_sb *sb, int depth, const tp_session_snapshot *snapshot,
                        int atlas_index, int source_index) {
    char abs[TP_IDENTITY_PATH_MAX];
    const tp_snapshot_source *s = NULL;
    const char *kind;
    if (tp_session_snapshot_source_resolved_at(snapshot, atlas_index, source_index,
                                               &s, abs, sizeof abs, NULL) != TP_STATUS_OK || !s) {
        abs[0] = '\0';
        kind = "missing";
    } else if (!tp_scan_exists(abs)) {
        kind = "missing";
    } else if (tp_scan_is_dir(abs)) {
        kind = "dir";
    } else {
        kind = "file";
    }
    char idtext[TP_ID_TEXT_CAP];
    ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_SOURCE, s->id);
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent) */
    cli_sb_json_str(sb, idtext);
    key(sb, depth + 1, &first, "path");
    cli_sb_json_str(sb, s->path);
    key(sb, depth + 1, &first, "abs");
    cli_sb_json_str(sb, abs);
    key(sb, depth + 1, &first, "stored_kind"); /* persisted folder/file classification */
    cli_sb_json_str(sb, s->kind == TP_SNAPSHOT_SOURCE_FILE ? "file" : "folder");
    key(sb, depth + 1, &first, "kind"); /* runtime disk state: file/dir/missing */
    cli_sb_json_str(sb, kind);
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

/* One resolved sprite: its derived identity (sprite_id, owning source) + raw name
 * (ext kept), export key, abs decode path, and any per-sprite overrides SET on the
 * project sprite (read from the project model, not the encoded desc -- keeps the
 * frontend clear of the pack-desc encoding, R3 gate). */
static void emit_sprite(cli_sb *sb, int depth, const tp_session_snapshot *snapshot,
                        tp_id128 atlas_id, const tp_sprite_ref *r) {
    const char *keybuf = r->export_key;
    char spid[TP_ID_TEXT_CAP];
    char srcid[TP_ID_TEXT_CAP];
    tp_sprite_id_format(r->sprite_id, spid, sizeof spid);
    ntpacker_fmt_shape_id(srcid, sizeof srcid, TP_ID_KIND_SOURCE, r->source_id);
    bool first = true;
    cli_sb_putc(sb, '{');
    key(sb, depth + 1, &first, "abs");
    cli_sb_json_str(sb, r->abs_path ? r->abs_path : "");
    key(sb, depth + 1, &first, "key");
    cli_sb_json_str(sb, keybuf);
    key(sb, depth + 1, &first, "name");
    cli_sb_json_str(sb, r->raw_name);
    key(sb, depth + 1, &first, "source"); /* owning source structural shape-ID */
    cli_sb_json_str(sb, srcid);
    key(sb, depth + 1, &first, "sprite_id"); /* derived deterministic id (source + key) */
    cli_sb_json_str(sb, spid);
    const tp_snapshot_sprite *ov = tp_session_snapshot_sprite_by_key(
        snapshot, atlas_id, r->source_id, r->source_key);
    if (ov) {
        if (ov->rename) {
            key(sb, depth + 1, &first, "rename");
            cli_sb_json_str(sb, ov->rename);
        }
        if (ov->origin_x != 0.5f || ov->origin_y != 0.5f) {
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
        if (ov->override_shape != -1) {
            key(sb, depth + 1, &first, "shape");
            cli_sb_int(sb, (long)ov->override_shape);
        }
        if (ov->override_allow_rotate != -1) {
            key(sb, depth + 1, &first, "allow_rotate");
            cli_sb_int(sb, (long)ov->override_allow_rotate);
        }
        if (ov->override_max_vertices != -1) {
            key(sb, depth + 1, &first, "max_vertices");
            cli_sb_int(sb, (long)ov->override_max_vertices);
        }
        if (ov->override_margin != -1) {
            key(sb, depth + 1, &first, "margin");
            cli_sb_int(sb, (long)ov->override_margin);
        }
        if (ov->override_extrude != -1) {
            key(sb, depth + 1, &first, "extrude");
            cli_sb_int(sb, (long)ov->override_extrude);
        }
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void emit_anim(cli_sb *sb, int depth, const tp_session_snapshot *snapshot,
                      tp_id128 atlas_id, const tp_snapshot_animation *an) {
    bool first = true;
    cli_sb_putc(sb, '{');
    char idtext[TP_ID_TEXT_CAP];
    ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_ANIM, an->id);
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
            const tp_snapshot_frame *frame = tp_session_snapshot_animation_frame_at(
                snapshot, atlas_id, an->id, i);
            cli_sb_json_str(sb, frame ? frame->name : "");
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }
    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
}

static void emit_target(cli_sb *sb, int depth, const tp_snapshot_target *t) {
    bool first = true;
    cli_sb_putc(sb, '{');
    char idtext[TP_ID_TEXT_CAP];
    ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_TARGET, t->id);
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

static tp_status inspect_build_sprite_index(
    const tp_session_snapshot *snapshot, int atlas_index,
    tp_sprite_index *out, tp_error *err) {
#ifdef NTPACKER_CLI_INSPECT_FAULT_SEAM
    if (getenv("NTPACKER_TEST_INSPECT_INDEX_FAIL")) {
        memset(out, 0, sizeof *out);
        return tp_error_set(err, TP_STATUS_OOM,
                            "injected inspect sprite-index failure");
    }
#endif
    return tp_sprite_index_build_snapshot(snapshot, atlas_index, out, err);
}

static tp_status emit_atlas(cli_sb *sb, int depth,
                            const tp_session_snapshot *snapshot, int ai,
                            tp_error *err) {
    const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snapshot, ai);
    bool first = true;
    cli_sb_putc(sb, '{');

    char idtext[TP_ID_TEXT_CAP];
    ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_ATLAS, a->id);
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
            emit_source(sb, depth + 2, snapshot, ai, i);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    /* Sprites: resolved index (same iteration a pack uses) carries each sprite's
     * derived identity (sprite_id + owning source) alongside its keys/paths. */
    key(sb, depth + 1, &first, "sprites");
    tp_sprite_index idx;
    tp_status bst = inspect_build_sprite_index(snapshot, ai, &idx, err);
    if (bst != TP_STATUS_OK) {
        tp_sprite_index_free(&idx);
        return bst;
    }
    if (idx.count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < idx.count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_sprite(sb, depth + 2, snapshot, a->id, &idx.refs[i]);
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }
    tp_sprite_index_free(&idx);

    key(sb, depth + 1, &first, "animations");
    if (a->animation_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, depth + 2);
            emit_anim(sb, depth + 2, snapshot, a->id,
                      tp_session_snapshot_animation_at(snapshot, a->id, i));
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
            emit_target(sb, depth + 2,
                        tp_session_snapshot_target_at(snapshot, a->id, i));
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, depth + 1);
        cli_sb_putc(sb, ']');
    }

    cli_sb_str(sb, "\n");
    cli_sb_indent(sb, depth);
    cli_sb_putc(sb, '}');
    return TP_STATUS_OK;
}

static tp_status build_inspect(cli_sb *sb, const tp_session_snapshot *snapshot,
                               const char *path, tp_error *err) {
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
        cli_sb_int(sb, tp_session_snapshot_project_schema_version(snapshot));
        key(sb, 2, &pf, "project_dir");
        cli_sb_json_str(sb, tp_session_snapshot_project_dir(snapshot));
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, '}');
    }

    key(sb, 1, &first, "atlases");
    const int atlas_count = tp_session_snapshot_atlas_count(snapshot);
    if (atlas_count == 0) {
        cli_sb_str(sb, "[]");
    } else {
        cli_sb_putc(sb, '[');
        for (int i = 0; i < atlas_count; i++) {
            cli_sb_str(sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(sb, 2);
            tp_status st = emit_atlas(sb, 2, snapshot, i, err);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
        cli_sb_str(sb, "\n");
        cli_sb_indent(sb, 1);
        cli_sb_putc(sb, ']');
    }
    cli_sb_str(sb, "\n}");
    return TP_STATUS_OK;
}

/* Cosmetic human summary (NOT a contract): one line per atlas. */
static tp_status print_inspect_human(const tp_session_snapshot *snapshot,
                                     const char *path, tp_error *err) {
    (void)printf("project: %s (schema %d)\n", path,
                 tp_session_snapshot_project_schema_version(snapshot));
    const int atlas_count = tp_session_snapshot_atlas_count(snapshot);
    for (int ai = 0; ai < atlas_count; ai++) {
        const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snapshot, ai);
        tp_sprite_index index = {0};
        int sprites = 0;
        int missing = 0;
        tp_status st = inspect_build_sprite_index(snapshot, ai, &index, err);
        if (st != TP_STATUS_OK) {
            tp_sprite_index_free(&index);
            return st;
        }
        sprites = index.count;
        for (int si = 0; si < a->source_count; ++si) {
            const tp_snapshot_source *source = NULL;
            char resolved[TP_IDENTITY_PATH_MAX];
            if (tp_session_snapshot_source_resolved_at(snapshot, ai, si, &source,
                                                       resolved, sizeof resolved,
                                                       NULL) != TP_STATUS_OK ||
                !tp_scan_exists(resolved)) {
                missing++;
            }
        }
        tp_sprite_index_free(&index);
        (void)printf("atlas '%s': %d sprites, %d source%s (%d missing), %d target%s, %d animation%s\n", a->name,
                     sprites, a->source_count, a->source_count == 1 ? "" : "s", missing, a->target_count,
                     a->target_count == 1 ? "" : "s", a->animation_count, a->animation_count == 1 ? "" : "s");
    }
    return TP_STATUS_OK;
}

int cmd_inspect(const char *path, bool json, bool quiet) {
    tp_session_snapshot *snapshot = NULL;
    int rc = cli_load_snapshot(path, json, quiet, &snapshot);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_error inspect_error = {0};
    tp_status inspect_status = TP_STATUS_OK;
    if (json) {
        cli_sb sb = {0};
        inspect_status = build_inspect(&sb, snapshot, path, &inspect_error);
        if (inspect_status != TP_STATUS_OK) {
            cli_sb_free(&sb);
            tp_session_snapshot_destroy(snapshot);
            cli_emit_error(true, quiet, tp_status_id(inspect_status), "%s",
                           inspect_error.msg[0] ? inspect_error.msg : tp_status_str(inspect_status));
            return inspect_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
        }
        if (sb.oom) {
            cli_sb_free(&sb);
            tp_session_snapshot_destroy(snapshot);
            cli_emit_error(true, false, "oom", "out of memory building inspect payload");
            return CLI_EXIT_INTERNAL;
        }
        cli_out_stdout(&sb);
        cli_sb_free(&sb);
    } else {
        inspect_status = print_inspect_human(snapshot, path, &inspect_error);
        if (inspect_status != TP_STATUS_OK) {
            tp_session_snapshot_destroy(snapshot);
            cli_emit_error(false, quiet, tp_status_id(inspect_status), "%s",
                           inspect_error.msg[0] ? inspect_error.msg : tp_status_str(inspect_status));
            return inspect_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL : CLI_EXIT_PROJECT;
        }
    }
    tp_session_snapshot_destroy(snapshot);
    return CLI_EXIT_OK;
}
