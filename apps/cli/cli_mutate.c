/* ntpacker mutation verbs (M5: thin CLI session cutover).
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
 * Exit-code split (agents branch on these; UNCHANGED from B4):
 *   2 usage   : bad grammar/vocabulary/value BEFORE the model commits -- wrong arg
 *               count, unknown key, malformed key=value, a value core rejects as
 *               out-of-range, an unknown exporter id, a duplicate atlas/anim name.
 *   3 project : load/parse error, `new` on an existing path, or a selector that names
 *               a missing model element (atlas/source/anim/frame/target).
 *   1 internal: OOM / RNG fault from the transaction, promote, or save.
 *   0 ok.
 * A committed transaction that rejects maps its tp_status to this split
 * (map_reject_exit); the emitted structured error carries the core status id + field.
 */
#include "cli_cmds.h"

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

#define CLI_PATH_MAX TP_IDENTITY_PATH_MAX

static const char *const k_atlas_knobs =
    "max_size, padding, margin, extrude, alpha_threshold, max_vertices, shape, "
    "allow_transform, power_of_two, pixels_per_unit";
static const char *const k_sprite_fields =
    "origin, slice9, rename, shape, allow_rotate, max_vertices, margin, extrude";

/* ------------------------------------------------------------------ */
/* small parse + path helpers                                         */
/* ------------------------------------------------------------------ */

/* Splits "key=value" at the first '='. Copies the key into kbuf; returns the value
 * pointer (into `tok`) or NULL when there is no '='. */
static const char *split_kv(const char *tok, char *kbuf, size_t kcap) {
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

static bool to_long(const char *s, long *out) {
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
static bool to_int(const char *s, int *out) {
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

static bool to_bool(const char *s, bool *out) {
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

static bool to_float(const char *s, float *out) {
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
static bool to_longs_csv(const char *s, long *out, int want) {
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

static bool to_floats_csv(const char *s, float *out, int want) {
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
static bool parse_playback(const char *s, int *out) {
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
static char *cli_strdup(const char *s) {
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
static bool cli_gen_id(tp_id128 *out) {
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    return tp_id128_generate(&rng, out, &err) == TP_STATUS_OK;
}

typedef struct cli_edit {
    tp_session *session;
    tp_session_snapshot *snapshot;
    const char *path;
} cli_edit;

static bool status_is_internal_fault(tp_status status) {
    return status == TP_STATUS_OOM || status == TP_STATUS_RNG_FAILED ||
           status == TP_STATUS_ID_COLLISION_EXHAUSTED;
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

static void edit_close(cli_edit *edit) {
    if (!edit) {
        return;
    }
    tp_session_snapshot_destroy(edit->snapshot);
    tp_session_destroy(edit->session);
    memset(edit, 0, sizeof *edit);
}

static int edit_open(cli_edit *edit, const char *path, bool json, bool quiet) {
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

static int edit_resolve(cli_edit *edit, tp_id128 atlas_scope,
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

static int edit_resolve_sprite(cli_edit *edit, tp_id128 atlas_id,
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

static int edit_open_atlas(cli_edit *edit, const char *path,
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
static int map_reject_exit(tp_status code) {
    switch (code) {
        case TP_STATUS_OOM:
        case TP_STATUS_RNG_FAILED:
        case TP_STATUS_ID_COLLISION_EXHAUSTED: return CLI_EXIT_INTERNAL;
        case TP_STATUS_NOT_FOUND:
        case TP_STATUS_OUT_OF_BOUNDS: return CLI_EXIT_PROJECT;
        default: return CLI_EXIT_USAGE;
    }
}

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
    return map_reject_exit(code);
}

/* Frees each operation's malloc-owned arms (NOT the array container). */
static void free_ops(tp_operation *ops, int n) {
    for (int i = 0; i < n; i++) {
        tp_operation_free(&ops[i]);
    }
}

static int edit_fail_usage(cli_edit *edit, bool json, bool quiet,
                           const char *id, const char *msg) {
    cli_emit_error(json, quiet, id, "%s", msg);
    edit_close(edit);
    return CLI_EXIT_USAGE;
}

static int commit_session_ops(cli_edit *edit, tp_operation *ops, int nops,
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
            cli_emit_error(json, quiet, tp_status_id(status), "%s",
                           err.msg[0] ? err.msg : tp_status_str(status));
            rc = status_is_internal_fault(status) ? CLI_EXIT_INTERNAL
                                                   : CLI_EXIT_PROJECT;
        } else if (json) {
            cli_emit_mutation(verb, count);
        } else if (!quiet) {
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
    if (tp_scan_exists(path)) {
        cli_emit_error(json, quiet, "file_exists", "refusing to overwrite existing path '%s'", path);
        return CLI_EXIT_PROJECT;
    }
    tp_rng rng = tp_rng_os();
    tp_error err = {0};
    tp_session *session = NULL;
    tp_status st = tp_session_create_default_project(&rng, &session, &err);
    if (st == TP_STATUS_OK) {
        tp_session_save_result result;
        st = tp_session_save_as(session, path, &result, &err);
    }
    if (st != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(st), "%s",
                       err.msg[0] ? err.msg : tp_status_str(st));
        tp_session_destroy(session);
        return st == TP_STATUS_OOM || st == TP_STATUS_RNG_FAILED ||
                       st == TP_STATUS_ID_COLLISION_EXHAUSTED
                   ? CLI_EXIT_INTERNAL
                   : CLI_EXIT_PROJECT;
    }
    char human[CLI_PATH_MAX + 32];
    (void)snprintf(human, sizeof human, "Created project %s", path);
    if (json) {
        cli_emit_mutation("new", 1);
    } else if (!quiet) {
        (void)printf("%s\n", human);
    }
    tp_session_destroy(session);
    return CLI_EXIT_OK;
}

/* ------------------------------------------------------------------ */
/* add / remove source                                                */
/* ------------------------------------------------------------------ */

static int do_add(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos < 4) {
        cli_emit_error(json, quiet, "usage", "add needs <project> <atlas> <path>... ; try 'ntpacker help'");
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
    tp_id128 aid = atlas_dto->id;

    int maxn = npos - 3;
    tp_source_batch_plan plan = {0};
    tp_error plan_error = {0};
    tp_status plan_status = tp_source_batch_plan_create(
        edit.snapshot, aid, &pos[3], maxn, &plan, &plan_error);
    if (plan_status != TP_STATUS_OK) {
        cli_emit_error(json, quiet, tp_status_id(plan_status), "%s",
                       plan_error.msg[0] ? plan_error.msg
                                         : tp_status_str(plan_status));
        edit_close(&edit);
        return status_is_internal_fault(plan_status) ? CLI_EXIT_INTERNAL
                                                     : CLI_EXIT_USAGE;
    }
    tp_operation *ops = plan.count > 0
                            ? (tp_operation *)calloc((size_t)plan.count,
                                                     sizeof *ops)
                            : NULL;
    if (plan.count > 0 && !ops) {
        free(ops);
        tp_source_batch_plan_free(&plan);
        cli_emit_error(json, quiet, "oom", "out of memory building sources");
        edit_close(&edit);
        return CLI_EXIT_INTERNAL;
    }

    int added = 0;
    const int dup = plan.duplicate_count;
    bool oom = false;
    bool rngfail = false;
    for (int i = 0; i < plan.count; i++) {
        tp_operation *op = &ops[added];
        memset(op, 0, sizeof *op);
        op->kind = TP_OP_SOURCE_ADD;
        op->atlas_id = aid;
        op->u.source_add.kind = TP_SOURCE_KIND_FOLDER; /* kind-agnostic default (matches add_source) */
        op->u.source_add.key = cli_strdup(plan.items[i].path);
        if (!op->u.source_add.key) {
            oom = true;
            break;
        }
        if (!cli_gen_id(&op->u.source_add.source_id)) { /* OS-RNG fault, not OOM (F4) */
            rngfail = true;
            break;
        }
        added++;
    }
    tp_source_batch_plan_free(&plan);

    if (oom || rngfail) {
        free_ops(ops, added + 1); /* +1: free the partially-built op the loop broke on */
        free(ops);
        if (rngfail) {
            cli_emit_error(json, quiet, "rng_failed", "could not generate a source id");
        } else {
            cli_emit_error(json, quiet, "oom", "out of memory building sources");
        }
        edit_close(&edit);
        return CLI_EXIT_INTERNAL;
    }

    char human[128];
    (void)snprintf(human, sizeof human, "Added %d source(s)%s to '%s'", added,
                   dup ? " (some already present)" : "", atlas);
    rc = commit_session_ops(&edit, ops, added, "add", added, human, json, quiet);
    free(ops);
    return rc;
}

static int do_remove_source(const char *const *pos, int npos, bool json, bool quiet) {
    if (npos != 4) {
        cli_emit_error(json, quiet, "usage", "remove needs <project> <atlas> <source>; try 'ntpacker help'");
        return CLI_EXIT_USAGE;
    }
    const char *path = pos[1];
    const char *atlas = pos[2];
    const char *src = pos[3];
    cli_edit edit;
    const tp_snapshot_atlas *atlas_dto = NULL;
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
    if (rc != CLI_EXIT_OK) {
        return rc;
    }
    const tp_snapshot_source *source = NULL;
    if (tp_source_snapshot_find(edit.snapshot, atlas_dto->id, src, &source,
                                NULL) !=
        TP_STATUS_OK) {
        cli_emit_error(json, quiet, "source_not_found", "atlas '%s' has no source matching '%s'", atlas, src);
        edit_close(&edit);
        return CLI_EXIT_PROJECT;
    }
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_SOURCE_REMOVE;
    op.atlas_id = atlas_dto->id;
    op.u.source_ref.source_id = source->id;
    char human[128];
    (void)snprintf(human, sizeof human, "Removed source '%s' from '%s'", src, atlas);
    return commit_session_ops(&edit, &op, 1, "remove", 1, human, json, quiet);
}

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

static int do_set(const char *const *pos, int npos, bool json, bool quiet) {
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

static int do_sprite_set(const char *const *pos, int npos, bool json, bool quiet) {
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
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
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

static int do_sprite_unset(const char *const *pos, int npos, bool json, bool quiet) {
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
    int rc = edit_open_atlas(&edit, path, atlas, &atlas_dto, json, quiet);
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
/* atlas                                                              */
/* ------------------------------------------------------------------ */

static int do_atlas(const char *const *pos, int npos, bool json, bool quiet) {
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
