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
/* dispatch                                                           */
/* ------------------------------------------------------------------ */

static int dispatch_mutation(int npos, const char *const *positionals,
                             const char *opt_at, const char *opt_kind,
                             bool json, bool quiet) {
    const char *verb = positionals[0];
    if (strcmp(verb, "add") == 0) {
        return do_add(positionals, npos, opt_kind, json, quiet);
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
               const char *opt_kind, bool json, bool quiet) {
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
    return dispatch_mutation(npos, positionals, opt_at, opt_kind, json, quiet);
}
