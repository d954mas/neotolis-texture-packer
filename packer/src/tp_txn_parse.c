/*
 * F2-02 task 3 (decode half): structural decode of the versioned transaction
 * request envelope + the per-op SHAPE collect-all, and the two public entry points
 * (tp_txn_request_decode, tp_model_apply_json). cJSON is a PRIVATE dep confined to
 * this TU + tp_txn_lower.c + tp_txn_json.h.
 *
 * VALIDATION ORDER (C0-02 §5, pinned): (1) structural decode -- envelope faults fail
 * fast and alone (bad JSON, bad/absent schema, bad version, missing/typed field, bad
 * 32-hex id, a number outside the +/-2^53 range-checked converter, an unknown
 * envelope/transaction key); (2) idempotency -- a re-submitted committed id rejects;
 * (3) revision precondition -- a mismatch short-circuits and rejects ALONE (op_index
 * -1) before any per-op work; (4) per-op SHAPE checks (unknown op, unknown field,
 * malformed *_id) collected in STABLE order (op_index asc, then field order). Only
 * when all shape checks pass are ops lowered to typed tp_operation and applied to a
 * clone (where the model-DEPENDENT semantic faults surface first-op-wins -- see
 * tp_txn_apply.c and decision 0011).
 *
 * Unknown-field policy = REJECT at the envelope and transaction levels (structural,
 * here) and at the operation level (shape collect-all, here) -- stricter than the
 * project-file loader's ignore (a dropped mutation field could make a client believe
 * a non-effect happened).
 */

#include "tp_core/tp_transaction.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_core/tp_id.h"
#include "tp_txn_internal.h"
#include "tp_txn_json.h"
#include "tp_txn_parse_priv.h"

static bool is_hex32_lower(const char *s) {
    if (!s) {
        return false;
    }
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return n == 32;
}

/* Map an addressing "*_id" key to the shape-id kind it must carry, or INVALID if
 * the key is not an addressing id. */
static tp_id_kind addr_kind(const char *key) {
    if (strcmp(key, "atlas_id") == 0) return TP_ID_KIND_ATLAS;
    if (strcmp(key, "source_id") == 0) return TP_ID_KIND_SOURCE;
    if (strcmp(key, "anim_id") == 0) return TP_ID_KIND_ANIM;
    if (strcmp(key, "target_id") == 0) return TP_ID_KIND_TARGET;
    return TP_ID_KIND_INVALID;
}

/* Structural decode of the envelope. Fills `req` (schema/id/expected_revision/
 * label/author) and returns the operations array via *out_ops. Fail-fast: on any
 * envelope fault returns non-OK + `err`, and *out_ops is left NULL. */
static tp_status parse_envelope(const cJSON *root, tp_txn_request *req, const cJSON **out_ops, tp_error *err) {
    *out_ops = NULL;

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!schema) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "missing \"schema\"");
    }
    if (!cJSON_IsNumber(schema)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"schema\" must be a number");
    }
    if (schema->valueint != TP_TXN_SCHEMA) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "unknown schema version %d (want %d)", schema->valueint,
                            TP_TXN_SCHEMA);
    }
    req->schema = TP_TXN_SCHEMA;

    for (const cJSON *c = root->child; c; c = c->next) { /* envelope unknown-field REJECT */
        if (c->string && strcmp(c->string, "schema") != 0 && strcmp(c->string, "transaction") != 0) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "unknown envelope key \"%s\"", c->string);
        }
    }

    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(root, "transaction");
    if (!cJSON_IsObject(tx)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "missing \"transaction\" object");
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(tx, "id");
    if (!cJSON_IsString(id)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "missing transaction \"id\"");
    }
    if (!is_hex32_lower(id->valuestring)) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "transaction id must be 32 lowercase hex");
    }
    (void)snprintf(req->id_hex, sizeof req->id_hex, "%s", id->valuestring);

    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(tx, "expected_revision");
    if (!rev) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "missing \"expected_revision\"");
    }
    tp_status rst = j_i64(rev, &req->expected_revision, err);
    if (rst != TP_STATUS_OK) {
        return rst;
    }

    tp_status ls = j_opt_dup(tx, "label", &req->label, err); /* NULL when absent (sparse) */
    if (ls != TP_STATUS_OK) {
        return ls;
    }
    tp_status as = j_opt_dup(tx, "author", &req->author, err);
    if (as != TP_STATUS_OK) {
        return as;
    }

    static const char *const tx_keys[] = {"id", "expected_revision", "label", "author", "operations"};
    for (const cJSON *c = tx->child; c; c = c->next) { /* transaction unknown-field REJECT */
        bool ok = false;
        for (int i = 0; i < 5; i++) {
            if (c->string && strcmp(c->string, tx_keys[i]) == 0) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "unknown transaction key \"%s\"", c->string);
        }
    }

    const cJSON *ops = cJSON_GetObjectItemCaseSensitive(tx, "operations");
    if (!cJSON_IsArray(ops)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "missing \"operations\" array");
    }
    int n = cJSON_GetArraySize(ops);
    for (int i = 0; i < n; i++) { /* each op must be an object with a string "op" */
        const cJSON *oj = cJSON_GetArrayItem(ops, i);
        if (!cJSON_IsObject(oj)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "operation %d is not an object", i);
        }
        const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
        if (!cJSON_IsString(wire)) {
            return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "operation %d missing string \"op\"", i);
        }
    }
    *out_ops = ops;
    return TP_STATUS_OK;
}

/* Per-op SHAPE checks, collected into `out` in (op_index, field) order: an unknown
 * op wire (op_unknown, then skip field checks), an unknown field (unknown_field), and
 * a malformed/wrong-kind addressing id (id_malformed). Returns the number of errors
 * this op contributed. */
static int shape_check_op(const cJSON *oj, int idx, tp_txn_result *out) {
    int before = out->error_count;
    const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
    tp_op_kind kind = tp_op_kind_from_wire(wire->valuestring);
    if (kind == TP_OP_INVALID) {
        char msg[192];
        (void)snprintf(msg, sizeof msg, "unknown operation '%s'", wire->valuestring);
        tp_txn__result_add_error(out, idx, TP_STATUS_UNKNOWN_OP, "op", msg);
        return out->error_count - before; /* cannot check fields of an unknown op */
    }
    for (const cJSON *c = oj->child; c; c = c->next) {
        if (!c->string || strcmp(c->string, "op") == 0) {
            continue;
        }
        if (!tp_op_field_allowed(kind, c->string)) {
            char msg[192];
            (void)snprintf(msg, sizeof msg, "unknown field '%s' for '%s'", c->string, wire->valuestring);
            tp_txn__result_add_error(out, idx, TP_STATUS_INVALID_ARGUMENT, c->string, msg);
            continue;
        }
        tp_id_kind ak = addr_kind(c->string);
        if (ak != TP_ID_KIND_INVALID && cJSON_IsString(c)) {
            tp_id_kind got = TP_ID_KIND_INVALID;
            if (tp_id_parse(c->valuestring, &got, NULL, NULL) != TP_STATUS_OK || got != ak) {
                char msg[192];
                (void)snprintf(msg, sizeof msg, "field '%s' is not a well-formed id", c->string);
                tp_txn__result_add_error(out, idx, TP_STATUS_ID_MALFORMED, c->string, msg);
            }
        }
    }
    return out->error_count - before;
}

/* Lower every op JSON into req->ops (typed). On a lowering fault frees the partial
 * ops and returns the status. */
static tp_status lower_all(const cJSON *ops, tp_txn_request *req, tp_error *err) {
    int n = cJSON_GetArraySize(ops);
    req->ops = (n > 0) ? (tp_operation *)calloc((size_t)n, sizeof(tp_operation)) : NULL;
    if (n > 0 && !req->ops) {
        return tp_error_set(err, TP_STATUS_OOM, "operations alloc");
    }
    req->op_count = 0;
    for (int i = 0; i < n; i++) {
        tp_status st = tp_txn__lower_op(cJSON_GetArrayItem(ops, i), &req->ops[i], err);
        if (st != TP_STATUS_OK) {
            return st; /* req->op_count = the count already fully lowered; free frees them */
        }
        req->op_count = i + 1;
    }
    return TP_STATUS_OK;
}

tp_status tp_txn_request_decode(const char *json, tp_txn_request **out, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!json) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null json");
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "malformed JSON");
    }
    tp_txn_request *req = (tp_txn_request *)calloc(1, sizeof *req);
    if (!req) {
        cJSON_Delete(root);
        return tp_error_set(err, TP_STATUS_OOM, "request alloc");
    }

    const cJSON *ops = NULL;
    tp_status st = parse_envelope(root, req, &ops, err);
    if (st != TP_STATUS_OK) {
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }

    /* Fail-fast shape (single fault) for the standalone decoder: the first op with a
     * shape error stops with that error's status. */
    int n = cJSON_GetArraySize(ops);
    for (int i = 0; i < n; i++) {
        tp_txn_result tmp;
        tp_txn__result_reset(&tmp, req->id_hex);
        if (shape_check_op(cJSON_GetArrayItem(ops, i), i, &tmp) > 0) {
            tp_status code = tmp.errors[0].code;
            (void)tp_error_set(err, code, "%s", tmp.errors[0].message);
            tp_txn_result_free(&tmp);
            tp_txn_request_free(req);
            cJSON_Delete(root);
            return code;
        }
        tp_txn_result_free(&tmp);
    }

    st = lower_all(ops, req, err);
    if (st != TP_STATUS_OK) {
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }
    cJSON_Delete(root);
    if (out) {
        *out = req;
    } else {
        tp_txn_request_free(req);
    }
    return TP_STATUS_OK;
}

tp_status tp_model_apply_json(tp_model *m, const char *json, tp_txn_result *out, tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    tp_txn__result_reset(out, "");

    cJSON *root = json ? cJSON_Parse(json) : NULL;
    if (!root) {
        tp_txn__result_add_error(out, -1, TP_STATUS_INVALID_ARGUMENT, "", "malformed JSON");
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "malformed JSON");
    }
    tp_txn_request *req = (tp_txn_request *)calloc(1, sizeof *req);
    if (!req) {
        cJSON_Delete(root);
        return tp_error_set(err, TP_STATUS_OOM, "request alloc");
    }

    /* 1. Structural decode (fail-fast, op_index -1). */
    const cJSON *ops = NULL;
    tp_status st = parse_envelope(root, req, &ops, err);
    if (st != TP_STATUS_OK) {
        tp_txn__result_reset(out, req->id_hex);
        out->revision = m->revision;
        tp_txn__result_add_error(out, -1, st, "", err ? err->msg : "");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }
    tp_txn__result_reset(out, req->id_hex);

    /* 2. Idempotency (a seen committed id rejects; model unchanged). */
    if (m->idstore && m->idstore->contains && m->idstore->contains(m->idstore->ctx, req->id_hex)) {
        out->revision = m->revision;
        tp_txn__result_add_error(out, -1, TP_STATUS_DUPLICATE_ID, "id", "transaction id already applied");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return tp_error_set(err, TP_STATUS_DUPLICATE_ID, "transaction id already applied");
    }

    /* 3. Revision precondition (short-circuits ALONE, op_index -1). */
    tp_status rv = tp_revision_check(req->expected_revision, m->revision, err);
    if (rv != TP_STATUS_OK) {
        out->revision = m->revision;
        tp_txn__result_add_error(out, -1, rv, "expected_revision", err ? err->msg : "");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return rv;
    }

    /* 4. Per-op SHAPE checks, collected in (op_index, field) order. */
    int n = cJSON_GetArraySize(ops);
    for (int i = 0; i < n; i++) {
        (void)shape_check_op(cJSON_GetArrayItem(ops, i), i, out);
    }
    if (out->error_count > 0) {
        out->committed = false;
        out->revision = m->revision;
        tp_status code = out->errors[0].code;
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return tp_error_set(err, code, "%d shape fault(s) in the batch", out->error_count);
    }

    /* 5. Lower to typed ops. A value-type fault rejects (op_index of the bad op). */
    st = lower_all(ops, req, err);
    if (st != TP_STATUS_OK) {
        out->committed = false;
        out->revision = m->revision;
        tp_txn__result_add_error(out, req->op_count, st, "", err ? err->msg : "");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }
    cJSON_Delete(root);

    /* 6. Atomic commit on the clone (idempotency + revision already passed). */
    st = tp_txn__commit_validated(m, req, out, err);
    tp_txn_request_free(req);
    return st;
}
