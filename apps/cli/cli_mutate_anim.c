#include "cli_cmds.h"
#include "cli_mutate_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "ntpacker_id_fmt.h"
#include "tp_core/tp_srckey.h"

/* ------------------------------------------------------------------ */
/* anim                                                               */
/* ------------------------------------------------------------------ */

/* anim list is a QUERY: {"schema":CLI_INSPECT_SCHEMA,"animations":[...]} -- its animation
 * shape mirrors inspect's, so it shares inspect's query schema (id + name split). */
static int anim_list(const tp_session_snapshot *snapshot,
                     const tp_snapshot_atlas *a, const char *atlas_name,
                     bool json, bool quiet) {
    (void)quiet;
    if (!json) {
        (void)printf("atlas '%s': %d animation(s)\n", atlas_name, a->animation_count);
        for (int i = 0; i < a->animation_count; i++) {
            const tp_snapshot_animation *an =
                tp_session_snapshot_animation_at(snapshot, a->id, i);
            (void)printf("  %s: %d frame(s), fps %.9g, playback %d%s%s\n", an->name, an->frame_count, (double)an->fps,
                         an->playback, an->flip_h ? ", flip_h" : "", an->flip_v ? ", flip_v" : "");
        }
        return CLI_EXIT_OK;
    }
    cli_sb sb = {0};
    bool first = true;
    cli_sb_putc(&sb, '{');
    cli_sb_str(&sb, "\n  \"schema\": ");
    cli_sb_int(&sb, CLI_INSPECT_SCHEMA);
    cli_sb_str(&sb, ",\n  \"animations\": ");
    if (a->animation_count == 0) {
        cli_sb_str(&sb, "[]");
    } else {
        cli_sb_putc(&sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            const tp_snapshot_animation *an =
                tp_session_snapshot_animation_at(snapshot, a->id, i);
            cli_sb_str(&sb, i == 0 ? "\n" : ",\n");
            cli_sb_indent(&sb, 2);
            cli_sb_putc(&sb, '{');
            first = true;
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 3);
            char idtext[TP_ID_TEXT_CAP];
            ntpacker_fmt_shape_id(idtext, sizeof idtext, TP_ID_KIND_ANIM, an->id);
            cli_sb_str(&sb, "\"id\": "); /* structural shape-ID */
            cli_sb_json_str(&sb, idtext);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"name\": "); /* logical/display name (name-keyed) */
            cli_sb_json_str(&sb, an->name);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"fps\": ");
            cli_sb_num(&sb, (double)an->fps);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"playback\": ");
            cli_sb_int(&sb, an->playback);
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"flip_h\": ");
            cli_sb_str(&sb, an->flip_h ? "true" : "false");
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"flip_v\": ");
            cli_sb_str(&sb, an->flip_v ? "true" : "false");
            cli_sb_str(&sb, ",\n");
            cli_sb_indent(&sb, 3);
            cli_sb_str(&sb, "\"frames\": ");
            if (an->frame_count == 0) {
                cli_sb_str(&sb, "[]");
            } else {
                cli_sb_putc(&sb, '[');
                for (int f = 0; f < an->frame_count; f++) {
                    cli_sb_str(&sb, f == 0 ? "\n" : ",\n");
                    cli_sb_indent(&sb, 4);
                    const tp_snapshot_frame *frame =
                        tp_session_snapshot_animation_frame_at(
                            snapshot, a->id, an->id, f);
                    cli_sb_json_str(&sb, frame ? frame->name : "");
                }
                cli_sb_str(&sb, "\n");
                cli_sb_indent(&sb, 3);
                cli_sb_putc(&sb, ']');
            }
            (void)first;
            cli_sb_str(&sb, "\n");
            cli_sb_indent(&sb, 2);
            cli_sb_putc(&sb, '}');
        }
        cli_sb_str(&sb, "\n  ]");
    }
    cli_sb_str(&sb, "\n}");
    if (sb.oom) {
        cli_sb_free(&sb);
        cli_emit_error(true, false, "oom", "out of memory building anim list");
        return CLI_EXIT_INTERNAL;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
    return CLI_EXIT_OK;
}

/* Resolves a frame selector: an all-digit token is an index; anything else is matched
 * against frame names (first match). Returns the index or -1. */
/* Parses `anim set` fields INTO an animation.settings op payload (mask + values). fps
 * PARSES here; its >0-finite RANGE is core's. playback is an enum parse; flips bool.
 * Emits + destroys `p` on a parse error (returns CLI_EXIT_USAGE). */
static int fill_anim_settings(tp_op_anim_settings *s, const char *const *pos, int npos, int first, bool json,
                              bool quiet) {
    for (int i = first; i < npos; i++) {
        char key[32];
        const char *val = split_kv(pos[i], key, sizeof key);
        char m[160];
        if (!val) {
            (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
            cli_emit_error(json, quiet, "usage", "%s", m);
            return CLI_EXIT_USAGE;
        }
        if (strcmp(key, "fps") == 0) {
            float fv = 0.0F;
            if (!to_float(val, &fv)) {
                (void)snprintf(m, sizeof m, "fps = '%s' must be a number", val);
                cli_emit_error(json, quiet, "usage", "%s", m);
                return CLI_EXIT_USAGE;
            }
            s->fps = fv;
            s->mask |= TP_ANF_FPS;
        } else if (strcmp(key, "playback") == 0) {
            int pb = 0;
            if (!parse_playback(val, &pb)) {
                (void)snprintf(m, sizeof m, "playback = '%s' must be 0..6 or a mode name", val);
                cli_emit_error(json, quiet, "usage", "%s", m);
                return CLI_EXIT_USAGE;
            }
            s->playback = pb;
            s->mask |= TP_ANF_PLAYBACK;
        } else if (strcmp(key, "flip_h") == 0) {
            bool bv = false;
            if (!to_bool(val, &bv)) {
                (void)snprintf(m, sizeof m, "flip_h = '%s' must be 0/1/true/false", val);
                cli_emit_error(json, quiet, "usage", "%s", m);
                return CLI_EXIT_USAGE;
            }
            s->flip_h = bv;
            s->mask |= TP_ANF_FLIP_H;
        } else if (strcmp(key, "flip_v") == 0) {
            bool bv = false;
            if (!to_bool(val, &bv)) {
                (void)snprintf(m, sizeof m, "flip_v = '%s' must be 0/1/true/false", val);
                cli_emit_error(json, quiet, "usage", "%s", m);
                return CLI_EXIT_USAGE;
            }
            s->flip_v = bv;
            s->mask |= TP_ANF_FLIP_V;
        } else {
            (void)snprintf(m, sizeof m, "unknown anim key '%s' (known: fps, playback, flip_h, flip_v)", key);
            cli_emit_error(json, quiet, "usage", "%s", m);
            return CLI_EXIT_USAGE;
        }
    }
    return 0;
}

int do_anim(const char *const *pos, int npos, const char *opt_at, bool json, bool quiet) {
    /* anim <sub> <project> <atlas> ... */
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "anim needs <sub> <project> <atlas> ...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    const char *atlas = pos[3];
    if (opt_at && strcmp(sub, "add-frame") != 0) {
        cli_emit_error(json, quiet, "usage", "--at is only valid for 'anim add-frame'");
        return CLI_EXIT_USAGE;
    }
    cli_edit edit;
    const tp_snapshot_atlas *a = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &a, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_id128 aid = a->id;

    if (strcmp(sub, "list") == 0) {
        if (npos != 4) {
            edit_close(&edit);
            cli_emit_error(json, quiet, "usage", "anim list takes no extra arguments");
            return CLI_EXIT_USAGE;
        }
        int r = anim_list(edit.snapshot, a, atlas, json, quiet);
        edit_close(&edit);
        return r; /* read-only: no save */
    }

    if (strcmp(sub, "create") == 0) {
        if (npos < 5) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim create needs <id> [frame-key...]");
        }
        const char *id = pos[4];
        int nframes = npos - 5;
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_CREATE;
        op.atlas_id = aid;
        op.u.anim_create.name = cli_strdup(id);
        op.u.anim_create.fps = TP_PROJECT_ANIM_FPS_DEFAULT;
        op.u.anim_create.playback = TP_PROJECT_ANIM_PLAYBACK_DEFAULT;
        op.u.anim_create.flip_h = false;
        op.u.anim_create.flip_v = false;
        op.u.anim_create.frame_count = nframes;
        if (op.u.anim_create.name == NULL) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building animation");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        if (!cli_gen_id(&op.u.anim_create.anim_id)) { /* OS-RNG fault, not OOM (F4) */
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "rng_failed", "could not generate an animation id");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        bool bad = false;
        if (nframes > 0) {
            op.u.anim_create.frames = (tp_op_sprite_ref *)calloc(
                (size_t)nframes, sizeof *op.u.anim_create.frames);
            if (!op.u.anim_create.frames) {
                bad = true;
                rc = CLI_EXIT_INTERNAL;
            } else {
                for (int i = 0; i < nframes; i++) {
                    tp_selector_result sprite;
                    char key[TP_SRCKEY_MAX];
                    rc = edit_resolve_sprite(&edit, aid, pos[5 + i], &sprite,
                                             &op.u.anim_create.frames[i].source_id,
                                             key, sizeof key, json, quiet);
                    if (rc != CLI_EXIT_OK) {
                        bad = true;
                        break;
                    }
                    op.u.anim_create.frames[i].src_key = cli_strdup(key);
                    if (!op.u.anim_create.frames[i].src_key) {
                        bad = true;
                        rc = CLI_EXIT_INTERNAL;
                        break;
                    }
                }
            }
        }
        if (bad) {
            tp_operation_free(&op);
            if (rc == CLI_EXIT_INTERNAL) {
                cli_emit_error(json, quiet, "oom", "out of memory building animation");
            }
            edit_close(&edit);
            return rc;
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Created animation '%s' with %d frame(s)", id, nframes);
        return commit_session_ops(&edit, &op, 1, "anim", nframes, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 5) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim remove needs <id>");
        }
        tp_selector_result animation;
        rc = edit_resolve(&edit, aid, TP_SEL_ANIM, pos[4], &animation,
                          json, quiet);
        if (rc != CLI_EXIT_OK) {
            edit_close(&edit);
            return rc;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_REMOVE;
        op.atlas_id = aid;
        op.u.anim_ref.anim_id = animation.id;
        char human[128];
        (void)snprintf(human, sizeof human, "Removed animation '%s'", pos[4]);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    /* All the remaining sub-verbs operate on one existing animation. */
    if (npos < 5) {
        return edit_fail_usage(&edit, json, quiet, "usage", "anim needs an <id>");
    }
    const char *id = pos[4];
    tp_selector_result animation;
    rc = edit_resolve(&edit, aid, TP_SEL_ANIM, id, &animation, json, quiet);
    if (rc != CLI_EXIT_OK) {
        edit_close(&edit);
        return rc;
    }
    tp_id128 anim_id = animation.id;

    if (strcmp(sub, "rename") == 0) {
        if (npos != 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim rename needs <id> <new>");
        }
        const char *neu = pos[5];
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_RENAME;
        op.atlas_id = aid;
        op.u.anim_rename.anim_id = anim_id;
        op.u.anim_rename.name = cli_strdup(neu);
        if (!op.u.anim_rename.name) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building animation");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Renamed animation '%s' -> '%s'", id, neu);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "add-frame") == 0) {
        if (npos != 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim add-frame needs <id> <key> [--at N]");
        }
        int index = -1; /* append */
        if (opt_at) {
            long at = 0;
            if (!to_long(opt_at, &at) || at < 0 || at > INT_MAX) {
                return edit_fail_usage(&edit, json, quiet, "usage", "--at must be a non-negative integer");
            }
            index = (int)at; /* apply appends then clamps into place, identical to the inline path */
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_FRAME_ADD;
        op.atlas_id = aid;
        op.u.anim_frame_add.anim_id = anim_id;
        tp_selector_result sprite;
        char source_key[TP_SRCKEY_MAX];
        rc = edit_resolve_sprite(&edit, aid, pos[5], &sprite,
                                 &op.u.anim_frame_add.frame.source_id,
                                 source_key, sizeof source_key, json, quiet);
        if (rc != CLI_EXIT_OK) {
            edit_close(&edit);
            return rc;
        }
        op.u.anim_frame_add.frame.src_key = cli_strdup(source_key);
        op.u.anim_frame_add.index = index;
        if (!op.u.anim_frame_add.frame.src_key) {
            cli_emit_error(json, quiet, "oom", "out of memory building frame");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[160];
        (void)snprintf(human, sizeof human, "Added frame '%s' to animation '%s'", pos[5], id);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove-frame") == 0) {
        if (npos != 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim remove-frame needs <id> <N|key>");
        }
        int fi = -1;
        tp_error frame_error = {0};
        tp_status frame_status = tp_session_snapshot_resolve_frame(
            edit.snapshot, aid, anim_id, pos[5], &fi, &frame_error);
        if (frame_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(frame_status), "%s",
                           frame_error.msg[0] ? frame_error.msg
                                              : tp_status_str(frame_status));
            edit_close(&edit);
            return frame_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                                 : CLI_EXIT_PROJECT;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_FRAME_REMOVE;
        op.atlas_id = aid;
        op.u.anim_frame_rm.anim_id = anim_id;
        op.u.anim_frame_rm.index = fi;
        char human[160];
        (void)snprintf(human, sizeof human, "Removed frame '%s' from animation '%s'", pos[5], id);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "move-frame") == 0) {
        if (npos != 7) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim move-frame needs <id> <from> <to>");
        }
        int from = 0;
        int to = 0;
        if (!to_int(pos[5], &from) || !to_int(pos[6], &to)) {
            return edit_fail_usage(&edit, json, quiet, "usage", "move-frame <from> and <to> must be integers");
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_FRAME_MOVE;
        op.atlas_id = aid;
        op.u.anim_frame_move.anim_id = anim_id;
        op.u.anim_frame_move.from_index = from; /* pre-checked in range above (F3) */
        op.u.anim_frame_move.to_index = to;     /* to is clamped by apply (CLI parity) */
        char human[128];
        (void)snprintf(human, sizeof human, "Moved frame %d -> %d in animation '%s'", from, to, id);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    if (strcmp(sub, "set") == 0) {
        if (npos < 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "anim set needs <id> <key>=<value>...");
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ANIMATION_SETTINGS_SET;
        op.atlas_id = aid;
        op.u.anim_settings.anim_id = anim_id;
        int sr = fill_anim_settings(&op.u.anim_settings, pos, npos, 5, json, quiet);
        if (sr != 0) {
            tp_operation_free(&op);
            edit_close(&edit);
            return sr;
        }
        char human[128];
        (void)snprintf(human, sizeof human, "Updated animation '%s'", id);
        return commit_session_ops(&edit, &op, 1, "anim", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown anim sub-command '%s'", sub);
    return edit_fail_usage(&edit, json, quiet, "usage", m);
}

