/* ntpacker mutation verbs: thin file-oriented session adapter.
 *
 * Every mutating verb now BUILDS typed tp_operation(s) and commits them ATOMICALLY
 * through tp_session instead of hand-mutating tp_project fields. The CLI keeps
 * exactly three responsibilities (AGENTS.md tool-parity: mutation/validation/naming
 * rules live BELOW the clients):
 *   1. argument PARSING (kv split and csv/enum parse);
 *   2. building the typed operation(s) for the verb (one verb = one transaction);
 *   3. rendering the result (structured success/error payloads).
 * Selector resolution, uniqueness, vocabulary, range, and reference validation are
 * core-owned. Snapshot DTOs are the only model reads available to this frontend.
 *
 * One-shot lifecycle (spec: ordinary CLI is FILE-oriented, NOT a live session):
 * tp_session_open -> owned snapshot -> apply ONE transaction -> tp_session_save ->
 * destroy. The session owns locking, external-change detection, id promotion, and save.
 *
 * Exit-code contract (agents branch on these):
 *   2 usage   : bad grammar/vocabulary/value BEFORE the model commits -- wrong arg
 *               count, unknown key, malformed key=value, a value core rejects as
 *               out-of-range, an unknown exporter id, a duplicate atlas/anim name.
 *   3 project : load/parse error, `new` on an existing path, or a selector that names
 *               a missing model element (atlas/source/anim/frame/target).
 *   1 internal: OOM / RNG fault from the transaction, promote, or save.
 *   8 file I/O: Save failed before atomic publication (typed phase/path/cause).
 *   0 ok.
 * A committed transaction that rejects maps its tp_status to this split
 * (cli_exit_for_rejected_status); the emitted structured error carries the core
 * status id + field.
 */
#include "cli_cmds.h"
#include "cli_mutate_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_exit.h"
#include "cli_out.h"
#include "ntpacker_id_fmt.h" /* ntpacker_fmt_shape_id (shared with cli_inspect) */
#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"     /* tp_rng_os + id128 generate + shape-ID format */
#include "tp_core/tp_operation.h"
#include "tp_core/tp_scan.h"            /* tp_scan_exists (new-on-existing guard) */
#include "tp_core/tp_session.h"
#include "tp_core/tp_source_plan.h"
#include "tp_core/tp_srckey.h"
#include "tp_core/tp_transaction.h"

const char *const k_atlas_knobs =
    "max_size, padding, margin, extrude, alpha_threshold, max_vertices, shape, "
    "allow_transform, power_of_two, pixels_per_unit";
const char *const k_sprite_fields =
    "origin, slice9, rename, shape, allow_rotate, max_vertices, margin, extrude";

/* ------------------------------------------------------------------ */
/* small parse + path helpers                                         */
/* ------------------------------------------------------------------ */

/* Splits "key=value" at the first '='. Copies the key into kbuf; returns the value
 * pointer (into `tok`) or NULL when there is no '='. */
const char *split_kv(const char *tok, char *kbuf, size_t kcap) {
    const char *eq = strchr(tok, '=');
    if (!eq) {
        return NULL;
    }
    size_t klen = (size_t)(eq - tok);
    if (klen >= kcap) {
        klen = kcap - 1U;
    }
    memcpy(kbuf, tok, klen);
    kbuf[klen] = '\0';
    return eq + 1;
}

bool to_long(const char *s, long *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno == ERANGE || end == s || *end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

/* Parses `s` into an int, rejecting a value that does not FIT int (marshalling into
 * the op's plain-int knob field). The SEMANTIC range (max_size [1..build texture cap],
 * >=0, ...) is core's (tp_operation_validate). A too-big value is a usage error here, never a silent wrap.
 * Uses strtoll so the fit check is meaningful where long == int (Windows LLP64). */
bool to_int(const char *s, int *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    long long v = strtoll(s, &end, 10);
    if (end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) {
        return false;
    }
    *out = (int)v;
    return true;
}

bool to_bool(const char *s, bool *out) {
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0) {
        *out = false;
        return true;
    }
    return false;
}

bool to_float(const char *s, float *out) {
    if (!s || !*s) {
        return false;
    }
    char *end = NULL;
    double v = strtod(s, &end); /* LC_NUMERIC="C" (main) -> '.' decimal */
    if (end == s || *end != '\0') {
        return false;
    }
    *out = (float)v;
    return true;
}

/* Parses exactly `want` comma-separated longs from `s` into out[]. Too few/many -> false. */
bool to_longs_csv(const char *s, long *out, int want) {
    for (int i = 0; i < want; i++) {
        const char *comma = strchr(s, ',');
        char buf[64];
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len >= sizeof buf) {
            return false;
        }
        memcpy(buf, s, len);
        buf[len] = '\0';
        if (!to_long(buf, &out[i])) {
            return false;
        }
        if (i < want - 1) {
            if (!comma) {
                return false; /* not enough components */
            }
            s = comma + 1;
        } else if (comma) {
            return false; /* too many components */
        }
    }
    return true;
}

bool to_floats_csv(const char *s, float *out, int want) {
    for (int i = 0; i < want; i++) {
        const char *comma = strchr(s, ',');
        char buf[64];
        size_t len = comma ? (size_t)(comma - s) : strlen(s);
        if (len >= sizeof buf) {
            return false;
        }
        memcpy(buf, s, len);
        buf[len] = '\0';
        if (!to_float(buf, &out[i])) {
            return false;
        }
        if (i < want - 1) {
            if (!comma) {
                return false;
            }
            s = comma + 1;
        } else if (comma) {
            return false;
        }
    }
    return true;
}

/* Defold-pinned playback ids (gui_canvas.h): accepts 0..6 or the snake_case names.
 * The enum domain ([0..6]) is parsed here (vocabulary); core also range-checks it. */
bool parse_playback(const char *s, int *out) {
    static const char *const names[7] = {"once_forward",  "loop_forward",  "once_backward", "loop_backward",
                                         "once_pingpong", "loop_pingpong", "none"};
    long v = 0;
    if (to_long(s, &v)) {
        if (v < 0 || v > 6) {
            return false;
        }
        *out = (int)v;
        return true;
    }
    for (int i = 0; i < 7; i++) {
        if (strcmp(s, names[i]) == 0) {
            *out = i;
            return true;
        }
    }
    return false;
}

/* Heap dup (the op arms are malloc-owned; tp_operation_free frees them). NULL-safe on
 * a NULL input (returns NULL); the caller distinguishes an OOM from a NULL input. */
char *cli_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *d = (char *)malloc(n);
    if (d) {
        memcpy(d, s, n);
    }
    return d;
}

/* Generates a fresh non-nil structural id via the OS RNG. false on an RNG fault. */
bool cli_gen_id(tp_id128 *out) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    return tp_id128_generate(&rng, out, &err) == TP_STATUS_OK;
}

bool status_is_internal_fault(tp_status status) {
    return cli_exit_for_rejected_status(status) == CLI_EXIT_INTERNAL;
}

static tp_status emit_selector_error(
    bool json, bool quiet, tp_status status, const tp_error *err,
    const tp_selector_candidates *candidates) {
    const char *message = err && err->msg[0] ? err->msg : tp_status_str(status);
    if (!json || status != TP_STATUS_AMBIGUOUS_SELECTOR || !candidates) {
        cli_emit_error(json, quiet, tp_status_id(status), "%s", message);
        return status;
    }
    cli_sb sb = {0};
    cli_sb_str(&sb, "{\"schema\":1,\"error\":{\"id\":");
    cli_sb_json_str(&sb, tp_status_id(status));
    cli_sb_str(&sb, ",\"message\":");
    cli_sb_json_str(&sb, message);
    cli_sb_str(&sb, ",\"candidates\":[");
    for (int i = 0; i < candidates->count; ++i) {
        const tp_selector_candidate *candidate = &candidates->v[i];
        if (i > 0) {
            cli_sb_putc(&sb, ',');
        }
        cli_sb_str(&sb, "{\"id\":");
        cli_sb_json_str(&sb, candidate->idtext);
        cli_sb_str(&sb, ",\"kind\":");
        cli_sb_json_str(&sb, tp_selector_kind_token(candidate->kind));
        cli_sb_str(&sb, ",\"label\":");
        cli_sb_json_str(&sb, candidate->label);
        cli_sb_putc(&sb, '}');
    }
    cli_sb_str(&sb, "]}}");
    if (sb.oom) {
        cli_sb_free(&sb);
        cli_emit_error(true, false, "oom",
                       "out of memory rendering selector candidates");
        return TP_STATUS_OOM;
    }
    cli_out_stdout(&sb);
    cli_sb_free(&sb);
    return status;
}

void edit_close(cli_edit *edit) {
    if (!edit) {
        return;
    }
    tp_session_snapshot_destroy(edit->snapshot);
    tp_session_destroy(edit->session);
    memset(edit, 0, sizeof *edit);
}

int edit_open(cli_edit *edit, const char *path, bool json, bool quiet) {
    memset(edit, 0, sizeof *edit);
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    tp_status status = tp_session_open(path, &rng, &edit->session, &err);
    if (status == TP_STATUS_OK) {
        status = tp_session_snapshot_create(edit->session, &edit->snapshot, &err);
    }
    if (status != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(status), "%s",
                       err.msg[0] ? err.msg : tp_status_str(status));
        edit_close(edit);
        return status_is_internal_fault(status) ? CLI_EXIT_INTERNAL
                                                : CLI_EXIT_PROJECT;
    }
    edit->path = path;
    return CLI_EXIT_OK;
}

int edit_resolve(cli_edit *edit, tp_id128 atlas_scope,
                        tp_selector_kind kind, const char *selector,
                        tp_selector_result *result, bool json, bool quiet) {
    tp_selector_candidates candidates = {0};
    tp_error err = {0};
    const tp_status status = tp_session_snapshot_resolve_selector(
        edit->snapshot, atlas_scope, kind, selector, result, &candidates, &err);
    if (status == TP_STATUS_OK) {
        tp_selector_candidates_free(&candidates);
        return CLI_EXIT_OK;
    }
    const tp_status emitted_status =
        emit_selector_error(json, quiet, status, &err, &candidates);
    tp_selector_candidates_free(&candidates);
    return emitted_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                   : emitted_status == TP_STATUS_INVALID_ARGUMENT
                                         ? CLI_EXIT_USAGE
                                         : CLI_EXIT_PROJECT;
}

int edit_resolve_sprite(cli_edit *edit, tp_id128 atlas_id,
                               const char *selector,
                               tp_selector_result *result,
                               tp_id128 *source_id, char *source_key,
                               size_t source_key_capacity, bool json,
                               bool quiet) {
    tp_selector_candidates candidates = {0};
    tp_error err = {0};
    const tp_status status = tp_session_snapshot_resolve_sprite_selector(
        edit->snapshot, atlas_id, selector, result, source_id, source_key,
        source_key_capacity, &candidates, &err);
    if (status == TP_STATUS_OK) {
        tp_selector_candidates_free(&candidates);
        return CLI_EXIT_OK;
    }
    const tp_status emitted_status =
        emit_selector_error(json, quiet, status, &err, &candidates);
    tp_selector_candidates_free(&candidates);
    return emitted_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                   : emitted_status == TP_STATUS_INVALID_ARGUMENT
                                         ? CLI_EXIT_USAGE
                                         : CLI_EXIT_PROJECT;
}

int edit_open_atlas(cli_edit *edit, const char *path,
                           const char *selector,
                           const tp_snapshot_atlas **atlas, bool json,
                           bool quiet) {
    int rc = edit_open(edit, path, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_selector_result result;
    rc = edit_resolve(edit, tp_id128_nil(), TP_SEL_ATLAS, selector, &result,
                      json, quiet);
    if (rc == CLI_EXIT_OK) {
        *atlas = tp_session_snapshot_atlas_by_id(edit->snapshot, result.id);
        if (*atlas) {
            return CLI_EXIT_OK;
        }
        rc = CLI_EXIT_PROJECT;
    }
    edit_close(edit);
    return rc;
}

/* ------------------------------------------------------------------ */
/* commit / error plumbing (transaction engine)                       */
/* ------------------------------------------------------------------ */

/* Maps a committed-transaction REJECT tp_status to the CLI exit-code contract.
 * OOM/RNG faults are internal (1); a dangling reference / out-of-range slot is a
 * project-state error (3); every other reject (out-of-range value, name collision,
 * bad payload) is a usage error (2) -- the value/vocabulary is fixed BEFORE the model
 * changes, exactly as the pre-cutover inline path classified them. */
/* Emits the structured error for a rejected transaction (id = core status token,
 * message = the offending op's context) and returns the mapped exit code. */
static int emit_reject(const tp_txn_result *res, tp_status st, const tp_error *err, bool json, bool quiet) {
    tp_status code = st;
    const char *msg = (err && err->msg[0]) ? err->msg : tp_status_str(st);
    /* Localization defaults for the no-structured-reject path (res NULL / no errors):
     * "" field + -1 op_index = "envelope level, no field" -- a stable schema for parsers. */
    const char *field = "";
    int op_index = -1;
    if (res && res->error_count > 0) {
        code = res->errors[0].code;
        if (res->errors[0].message[0]) {
            msg = res->errors[0].message;
        }
        field = res->errors[0].field;       /* char[64], always NUL-terminated ("" if none) */
        op_index = res->errors[0].op_index; /* >=0 = op position; -1 = envelope/revision level */
    }
    cli_emit_reject(json, quiet, tp_status_id(code), field, op_index, "%s", msg);
    if (code == TP_STATUS_NOT_FOUND && strcmp(field, "exporter_id") == 0) {
        return CLI_EXIT_USAGE;
    }
    return cli_exit_for_rejected_status(code);
}

/* Frees each operation's malloc-owned arms (NOT the array container). */
void free_ops(tp_operation *ops, int n) {
    for (int i = 0; i < n; i++) {
        tp_operation_free(&ops[i]);
    }
}

static void report_save_notices_human(
    const tp_session_save_result *result, bool quiet) {
    if (!result || quiet) {
        return;
    }
    if (result->file_durability_degraded) {
        (void)fprintf(
            stderr,
            "ntpacker: notice [file_durability_uncertain]: project file was published, but storage durability could not be confirmed\n");
    }
    if (result->recovery_degraded) {
        (void)fprintf(
            stderr,
            "ntpacker: notice [recovery_degraded]: project was saved, but crash recovery is degraded (%s)\n",
            tp_status_id(result->recovery_status));
    }
}

int edit_fail_usage(cli_edit *edit, bool json, bool quiet,
                           const char *id, const char *msg) {
    cli_emit_error(json, quiet, id, "%s", msg);
    edit_close(edit);
    return CLI_EXIT_USAGE;
}

static int emit_save_failure(tp_status status, const tp_error *error,
                             bool json, bool quiet) {
    if (status == TP_STATUS_FILE_IO_FAILED) {
        cli_emit_file_io_error(json, quiet, error);
    } else {
        cli_emit_error(json, quiet, tp_status_id(status), "%s",
                       error && error->msg[0] ? error->msg
                                             : tp_status_str(status));
    }
    return cli_exit_for_save_status(status);
}

int commit_session_ops(cli_edit *edit, tp_operation *ops, int nops,
                              const char *verb, int count,
                              const char *human, bool json, bool quiet) {
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   "00000000000000000000000000000000");
    request.expected_revision = tp_session_snapshot_revision(edit->snapshot);
    request.ops = ops;
    request.op_count = nops;

    tp_txn_result result;
    memset(&result, 0, sizeof result);
    tp_error err = {0};
    tp_status status = tp_session_apply(edit->session, &request, &result, &err);
    int rc = CLI_EXIT_OK;
    if (status != TP_STATUS_OK) {
        rc = emit_reject(&result, status, &err, json, quiet);
    } else {
        tp_session_save_result save_result;
        status = tp_session_save(edit->session, &save_result, &err);
        if (status != TP_STATUS_OK) {
            rc = emit_save_failure(status, &err, json, quiet);
        } else if (json) {
            cli_emit_mutation(verb, count, &save_result);
        } else if (!quiet) {
            report_save_notices_human(&save_result, false);
            (void)printf("%s\n", human);
        }
    }
    free_ops(ops, nops);
    tp_txn_result_free(&result);
    edit_close(edit);
    return rc;
}

/* ------------------------------------------------------------------ */
/* new (session create + Save As)                                     */
/* ------------------------------------------------------------------ */

static int do_new(const char *path, bool json, bool quiet) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    tp_session *session = NULL;
    tp_session_save_result result;
    memset(&result, 0, sizeof result);
    tp_status st = tp_session_create_default_project(&rng, &session, &err);
    if (st == TP_STATUS_OK) {
        st = tp_session_save_new(session, path, &result, &err);
    }
    if (st != TP_STATUS_OK) {
        const int exit_code = emit_save_failure(st, &err, json, quiet);
        tp_session_destroy(session);
        return exit_code;
    }
    char human[CLI_PATH_MAX + 32];
    (void)snprintf(human, sizeof human, "Created project %s", path);
    if (json) {
        cli_emit_mutation("new", 1, &result);
    } else if (!quiet) {
        report_save_notices_human(&result, false);
        (void)printf("%s\n", human);
    }
    tp_session_destroy(session);
    return CLI_EXIT_OK;
}



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

static int do_anim(const char *const *pos, int npos, const char *opt_at, bool json, bool quiet) {
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

/* ------------------------------------------------------------------ */
/* target                                                             */
/* ------------------------------------------------------------------ */

static int do_target(const char *const *pos, int npos, bool json, bool quiet) {
    /* target <sub> <project> <atlas> ... */
    if (npos < 5) {
        cli_emit_error(json, quiet, "usage", "target needs <sub> <project> <atlas> ...; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *sub = pos[1];
    const char *path = pos[2];
    const char *atlas = pos[3];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    tp_id128 aid = atlas_dto->id;

    if (strcmp(sub, "add") == 0) {
        if (npos != 6) {
            return edit_fail_usage(&edit, json, quiet, "usage", "target add needs <exporter-id> <out-path>");
        }
        const char *eid = pos[4];
        const char *out = pos[5];
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_CREATE;
        op.atlas_id = aid;
        op.u.target_create.exporter_id = cli_strdup(eid);
        op.u.target_create.out_path = cli_strdup(out);
        op.u.target_create.enabled = true;
        if (!op.u.target_create.exporter_id || !op.u.target_create.out_path) {
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "oom", "out of memory building target");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        if (!cli_gen_id(&op.u.target_create.target_id)) { /* OS-RNG fault, not OOM (F4) */
            tp_operation_free(&op);
            cli_emit_error(json, quiet, "rng_failed", "could not generate a target id");
            edit_close(&edit);
            return CLI_EXIT_INTERNAL;
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Added target %s -> %s on '%s'", eid, out, atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "remove") == 0) {
        if (npos != 5) {
            return edit_fail_usage(&edit, json, quiet, "usage", "target remove needs <index-or-id>");
        }
        const char *sel = pos[4];
        const tp_snapshot_target *target = NULL;
        tp_error selector_error = {0};
        tp_status selector_status = tp_session_snapshot_resolve_target(
            edit.snapshot, aid, sel, &target, &selector_error);
        if (selector_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(selector_status), "%s",
                           selector_error.msg[0] ? selector_error.msg
                                                 : tp_status_str(selector_status));
            edit_close(&edit);
            return selector_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                                    : CLI_EXIT_PROJECT;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_REMOVE;
        op.atlas_id = aid;
        op.u.target_ref.target_id = target->id;
        char human[160];
        (void)snprintf(human, sizeof human, "Removed target '%s' from '%s'", sel, atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    if (strcmp(sub, "set") == 0) {
        if (npos < 6) {
            return edit_fail_usage(&edit, json, quiet, "usage",
                              "target set needs <index> [exporter=..] [out=..] [enabled=0|1]");
        }
        const tp_snapshot_target *target = NULL;
        tp_error selector_error = {0};
        tp_status selector_status = tp_session_snapshot_resolve_target(
            edit.snapshot, aid, pos[4], &target, &selector_error);
        if (selector_status != TP_STATUS_OK) {
            cli_emit_error(json, quiet, tp_status_id(selector_status), "%s",
                           selector_error.msg[0] ? selector_error.msg
                                                 : tp_status_str(selector_status));
            edit_close(&edit);
            return selector_status == TP_STATUS_OOM ? CLI_EXIT_INTERNAL
                                                    : CLI_EXIT_PROJECT;
        }
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_TARGET_SET;
        op.atlas_id = aid;
        op.u.target_set.target_id = target->id;
        for (int i = 5; i < npos; i++) {
            char key[32];
            const char *val = split_kv(pos[i], key, sizeof key);
            char m[192];
            if (!val) {
                (void)snprintf(m, sizeof m, "expected key=value, got '%s'", pos[i]);
                tp_operation_free(&op);
                return edit_fail_usage(&edit, json, quiet, "usage", m);
            }
            if (strcmp(key, "exporter") == 0) {
                op.u.target_set.mask |= TP_TF_EXPORTER;
                char *replacement = cli_strdup(val);
                if (!replacement) {
                    tp_operation_free(&op);
                    edit_close(&edit);
                    cli_emit_error(json, quiet, "oom", "out of memory building target");
                    return CLI_EXIT_INTERNAL;
                }
                free(op.u.target_set.exporter_id);
                op.u.target_set.exporter_id = replacement;
            } else if (strcmp(key, "out") == 0) {
                op.u.target_set.mask |= TP_TF_OUT_PATH;
                char *replacement = cli_strdup(val);
                if (!replacement) {
                    tp_operation_free(&op);
                    edit_close(&edit);
                    cli_emit_error(json, quiet, "oom", "out of memory building target");
                    return CLI_EXIT_INTERNAL;
                }
                free(op.u.target_set.out_path);
                op.u.target_set.out_path = replacement;
            } else if (strcmp(key, "enabled") == 0) {
                if (!to_bool(val, &op.u.target_set.enabled)) {
                    (void)snprintf(m, sizeof m, "enabled = '%s' must be 0/1/true/false", val);
                    tp_operation_free(&op);
                    return edit_fail_usage(&edit, json, quiet, "usage", m);
                }
                op.u.target_set.mask |= TP_TF_ENABLED;
            } else {
                (void)snprintf(m, sizeof m, "unknown target key '%s' (known: exporter, out, enabled)", key);
                tp_operation_free(&op);
                return edit_fail_usage(&edit, json, quiet, "usage", m);
            }
        }
        char human[192];
        (void)snprintf(human, sizeof human, "Updated target %s on '%s'", pos[4], atlas);
        return commit_session_ops(&edit, &op, 1, "target", 1, human, json, quiet);
    }

    char m[128];
    (void)snprintf(m, sizeof m, "unknown target sub-command '%s'", sub);
    return edit_fail_usage(&edit, json, quiet, "usage", m);
}


/* ------------------------------------------------------------------ */
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

static int dispatch_mutation(int npos, const char *const *positionals,
                             const char *opt_at, bool json, bool quiet) {
    const char *verb = positionals[0];
    if (strcmp(verb, "add") == 0) {
        return do_add(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "remove") == 0) {
        return do_remove_source(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "set") == 0) {
        return do_set(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "sprite") == 0) {
        if (npos < 2) {
            cli_emit_error(json, quiet, "usage",
                           "sprite needs 'set' or 'unset'");
            return CLI_EXIT_USAGE;
        }
        if (strcmp(positionals[1], "set") == 0) {
            return do_sprite_set(positionals, npos, json, quiet);
        }
        if (strcmp(positionals[1], "unset") == 0) {
            return do_sprite_unset(positionals, npos, json, quiet);
        }
        cli_emit_error(json, quiet, "usage",
                       "unknown sprite sub-command '%s' (want set/unset)",
                       positionals[1]);
        return CLI_EXIT_USAGE;
    }
    if (strcmp(verb, "anim") == 0) {
        return do_anim(positionals, npos, opt_at, json, quiet);
    }
    if (strcmp(verb, "target") == 0) {
        return do_target(positionals, npos, json, quiet);
    }
    if (strcmp(verb, "atlas") == 0) {
        return do_atlas(positionals, npos, json, quiet);
    }
    cli_emit_error(json, quiet, "usage",
                   "unknown command '%s'; try 'ntpacker help'", verb);
    return CLI_EXIT_USAGE;
}

int cmd_mutate(int npos, const char *const *positionals, const char *opt_at,
               bool json, bool quiet) {
    const char *verb = positionals[0];
    if (opt_at && strcmp(verb, "anim") != 0) {
        cli_emit_error(json, quiet, "usage",
                       "--at is only valid for 'anim add-frame'");
        return CLI_EXIT_USAGE;
    }
    if (strcmp(verb, "new") == 0) {
        if (npos != 2) {
            cli_emit_error(json, quiet, "usage",
                           "new needs exactly one <path>; try 'ntpacker help'");
            return CLI_EXIT_USAGE;
        }
        return do_new(positionals[1], json, quiet);
    }
    return dispatch_mutation(npos, positionals, opt_at, json, quiet);
}
