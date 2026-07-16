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

/* C-string compatibility entry points must not call unbounded strlen before the
 * wire-size gate. Return MAX+1 as soon as the request is known to be oversized;
 * the length-aware implementation then rejects without invoking cJSON. */
static size_t txn_cstr_len_bounded(const char *json) {
    if (!json) {
        return 0;
    }
    size_t len = 0;
    while (len <= (size_t)TP_TXN_MAX_REQUEST_BYTES && json[len] != '\0') {
        len++;
    }
    return len;
}

static tp_status txn_request_bytes_check(const char *json, size_t json_len, tp_error *err) {
    if (!json) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null json");
    }
    if (json_len > (size_t)TP_TXN_MAX_REQUEST_BYTES) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "transaction request has %zu bytes; maximum is %u",
                            json_len, (unsigned)TP_TXN_MAX_REQUEST_BYTES);
    }
    return TP_STATUS_OK;
}

static bool json_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int json_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct json_precheck_scan {
    const char *json;
    size_t len;
    size_t pos;
    size_t steps;
} json_precheck_scan;

static bool json_precheck_take(json_precheck_scan *scan, char *out) {
    if (scan->pos >= scan->len) {
        return false;
    }
    *out = scan->json[scan->pos++];
    scan->steps++;
    return true;
}

/* Allocation-free JSON string token reader used only by the early resource
 * preflight. The opening quote has already been consumed. It recognizes escaped
 * ASCII keys (including \uXXXX) without duplicating semantic validation. */
static bool json_precheck_string_matches(json_precheck_scan *scan, const char *wanted,
                                         bool *matches) {
    size_t wi = 0U;
    bool same = true;
    char input = '\0';
    while (json_precheck_take(scan, &input)) {
        unsigned value = (unsigned char)input;
        if (value == '"') {
            *matches = same && wanted[wi] == '\0';
            return true;
        }
        if (value == '\\') {
            char esc = '\0';
            if (!json_precheck_take(scan, &esc)) return false;
            switch (esc) {
                case '"': value = '"'; break;
                case '\\': value = '\\'; break;
                case '/': value = '/'; break;
                case 'b': value = '\b'; break;
                case 'f': value = '\f'; break;
                case 'n': value = '\n'; break;
                case 'r': value = '\r'; break;
                case 't': value = '\t'; break;
                case 'u': {
                    unsigned code = 0U;
                    for (int i = 0; i < 4; ++i) {
                        char digit = '\0';
                        if (!json_precheck_take(scan, &digit)) return false;
                        const int nibble = json_hex(digit);
                        if (nibble < 0) return false;
                        code = (code << 4U) | (unsigned)nibble;
                    }
                    value = code;
                    break;
                }
                default: return false;
            }
        }
        if (same) {
            if (value > 0x7fU || wanted[wi] == '\0' || value != (unsigned char)wanted[wi]) {
                same = false;
            } else {
                wi++;
            }
        }
    }
    return false;
}

typedef enum json_precheck_key {
    JSON_PRECHECK_KEY_NONE,
    JSON_PRECHECK_KEY_TRANSACTION,
    JSON_PRECHECK_KEY_OPERATIONS
} json_precheck_key;

/* Single forward lexical pass. It recognizes only root.transaction.operations,
 * counts direct array elements, and leaves all malformed-input/schema semantics to
 * cJSON. Nested fields named "operations" never trigger a suffix rescan. */
static tp_status txn_request_op_count_precheck(const char *json, size_t json_len,
                                               size_t *steps, int *count_out,
                                               tp_error *err) {
    json_precheck_scan scan = {json, json_len, 0U, 0U};
    int depth = 0;
    int root_depth = 0;
    int transaction_depth = 0;
    int operations_depth = 0;
    int operation_count = 0;
    bool transaction_seen = false;
    bool operations_seen = false;
    bool operations_expect_element = false;
    json_precheck_key key_candidate = JSON_PRECHECK_KEY_NONE;
    json_precheck_key expected_value = JSON_PRECHECK_KEY_NONE;
    tp_status status = TP_STATUS_OK;
    char c = '\0';

    while (json_precheck_take(&scan, &c)) {
        if (json_ws(c)) {
            continue;
        }

        if (operations_depth > 0 && depth == operations_depth) {
            if (c == ']') {
                operations_depth = 0;
                operations_expect_element = false;
            } else if (c == ',') {
                operations_expect_element = true;
                key_candidate = JSON_PRECHECK_KEY_NONE;
                continue;
            } else if (operations_expect_element) {
                operations_expect_element = false;
                operation_count++;
                if (operation_count > TP_TXN_MAX_OPS) {
                    status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                          "transaction has more than %d operations",
                                          TP_TXN_MAX_OPS);
                    break;
                }
            }
        }

        if (c == '"') {
            const char *wanted = NULL;
            json_precheck_key kind = JSON_PRECHECK_KEY_NONE;
            if (root_depth > 0 && depth == root_depth && transaction_depth == 0) {
                wanted = "transaction";
                kind = JSON_PRECHECK_KEY_TRANSACTION;
            } else if (transaction_depth > 0 && depth == transaction_depth &&
                       operations_depth == 0) {
                wanted = "operations";
                kind = JSON_PRECHECK_KEY_OPERATIONS;
            }
            bool matches = false;
            if (!json_precheck_string_matches(&scan, wanted ? wanted : "", &matches)) {
                break; /* malformed string remains cJSON's responsibility */
            }
            key_candidate = matches ? kind : JSON_PRECHECK_KEY_NONE;
            continue;
        }

        if (key_candidate != JSON_PRECHECK_KEY_NONE) {
            if (c == ':') {
                if (key_candidate == JSON_PRECHECK_KEY_TRANSACTION) {
                    if (transaction_seen) {
                        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                              "duplicate transaction field");
                        break;
                    }
                    transaction_seen = true;
                } else if (key_candidate == JSON_PRECHECK_KEY_OPERATIONS) {
                    if (operations_seen) {
                        status = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                              "duplicate operations field");
                        break;
                    }
                    operations_seen = true;
                }
                expected_value = key_candidate;
                key_candidate = JSON_PRECHECK_KEY_NONE;
                continue;
            }
            key_candidate = JSON_PRECHECK_KEY_NONE;
        }

        if (expected_value != JSON_PRECHECK_KEY_NONE) {
            const json_precheck_key expected = expected_value;
            expected_value = JSON_PRECHECK_KEY_NONE;
            if (expected == JSON_PRECHECK_KEY_TRANSACTION && c == '{' &&
                depth == root_depth) {
                depth++;
                transaction_depth = depth;
                continue;
            }
            if (expected == JSON_PRECHECK_KEY_OPERATIONS && c == '[' &&
                depth == transaction_depth) {
                depth++;
                operations_depth = depth;
                operation_count = 0;
                operations_expect_element = true;
                continue;
            }
        }

        if (c == '{' || c == '[') {
            depth++;
            if (root_depth == 0 && depth == 1 && c == '{') {
                root_depth = depth;
            }
        } else if (c == '}' || c == ']') {
            if (c == '}' && depth == transaction_depth) {
                transaction_depth = 0;
            }
            if (depth > 0) {
                depth--;
            }
        }
    }
    if (steps) {
        *steps = scan.steps;
    }
    if (count_out && status == TP_STATUS_OK) {
        *count_out = operation_count;
    }
    return status;
}

tp_status tp_txn__test_json_precheck(const char *json, size_t json_len, size_t *steps,
                                     tp_error *err) {
    if (steps) {
        *steps = 0U;
    }
    if (!json) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null json");
    }
    return txn_request_op_count_precheck(json, json_len, steps, NULL, err);
}

tp_status tp_txn__count_operations_json_n(const char *json, size_t json_len,
                                          int *count_out, tp_error *err) {
    if (count_out) {
        *count_out = 0;
    }
    if (!count_out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null operation count out");
    }
    tp_status byte_st = txn_request_bytes_check(json, json_len, err);
    if (byte_st != TP_STATUS_OK) {
        return byte_st;
    }
    return txn_request_op_count_precheck(json, json_len, NULL, count_out, err);
}

static cJSON *txn_parse_exact(const char *json, size_t json_len) {
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, 0);
    if (!root) return NULL;
    size_t consumed = (size_t)(parse_end - json);
    while (consumed < json_len && json_ws(json[consumed])) consumed++;
    if (consumed != json_len) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
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
    /* Route schema through the range-checked integral converter -- a truncating
     * schema->valueint would accept a fractional {"schema":1.9} as 1. */
    int64_t schema_v = 0;
    tp_status sst = j_i64(schema, &schema_v, err); /* non-integral/inf/NaN -> invalid_argument */
    if (sst != TP_STATUS_OK) {
        return sst;
    }
    if (schema_v != TP_TXN_SCHEMA) {
        return tp_error_set(err, TP_STATUS_BAD_VERSION, "unknown schema version %" PRId64 " (want %d)", schema_v,
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
    if (!tp_txn__is_hex32_lower(id->valuestring)) { /* shared with tp_txn__preflight -- cannot drift */
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
    tp_status count_st = tp_txn__check_op_count(n, err);
    if (count_st != TP_STATUS_OK) {
        return count_st; /* before per-op shape work or typed-array materialization */
    }
    int i = 0;
    for (const cJSON *oj = ops->child; oj; oj = oj->next, ++i) {
        /* cJSON arrays are linked lists: direct child/next traversal keeps this
         * accepted max-op path linear. */
        tp_txn__test_count_op_walk(1U);
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

static void shape_record_error(tp_txn_result *out, int reserved_capacity,
                               int *fault_count, bool *store_oom, int idx,
                               tp_status code, const char *field, const char *msg) {
    (*fault_count)++;
    if (!out) {
        return;
    }
    const bool stored = reserved_capacity >= 0
                            ? tp_txn__result_add_error_reserved(
                                  out, reserved_capacity, idx, code, field, msg)
                            : tp_txn__result_add_error(out, idx, code, field, msg);
    if (!stored) {
        *store_oom = true;
    }
}

/* Per-op SHAPE checks, collected into `out` in (op_index, field) order: an unknown
 * op wire (op_unknown, then skip field checks), an unknown field (unknown_field), and
 * a present-but-non-string OR malformed/wrong-kind addressing id (id_malformed).
 * `out` must be non-NULL. Returns true if this op contributed >= 1 shape fault
 * (independent of whether every record could be stored). Sets *store_oom if any fault
 * record could not be stored -- the batch must then reject even though error_count is
 * short, so a shape-faulted batch never falsely commits under allocation pressure. */
static bool shape_check_op(const cJSON *oj, int idx, tp_txn_result *out,
                           int reserved_capacity, int *fault_count, bool *store_oom) {
    const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
    tp_op_kind kind = tp_op_kind_from_wire(wire->valuestring);
    if (kind == TP_OP_INVALID) {
        char msg[192];
        (void)snprintf(msg, sizeof msg, "unknown operation '%s'", wire->valuestring);
        shape_record_error(out, reserved_capacity, fault_count, store_oom, idx,
                           TP_STATUS_UNKNOWN_OP, "op", msg);
        return true; /* cannot check fields of an unknown op */
    }
    bool faulted = false;
    for (const cJSON *c = oj->child; c; c = c->next) {
        if (!c->string || strcmp(c->string, "op") == 0) {
            continue;
        }
        if (!tp_op_field_allowed(kind, c->string)) {
            char msg[192];
            (void)snprintf(msg, sizeof msg, "unknown field '%s' for '%s'", c->string, wire->valuestring);
            shape_record_error(out, reserved_capacity, fault_count, store_oom, idx,
                               TP_STATUS_INVALID_ARGUMENT, c->string, msg);
            faulted = true;
            continue;
        }
        tp_id_kind ak = addr_kind(c->string);
        if (ak == TP_ID_KIND_INVALID) {
            continue;
        }
        if (!cJSON_IsString(c)) { /* a present-but-non-string addressing id is a shape fault */
            char msg[192];
            (void)snprintf(msg, sizeof msg, "field '%s' must be a string id", c->string);
            shape_record_error(out, reserved_capacity, fault_count, store_oom, idx,
                               TP_STATUS_ID_MALFORMED, c->string, msg);
            faulted = true;
            continue;
        }
        tp_id_kind got = TP_ID_KIND_INVALID;
        if (tp_id_parse(c->valuestring, &got, NULL, NULL) != TP_STATUS_OK || got != ak) {
            char msg[192];
            (void)snprintf(msg, sizeof msg, "field '%s' is not a well-formed id", c->string);
            shape_record_error(out, reserved_capacity, fault_count, store_oom, idx,
                               TP_STATUS_ID_MALFORMED, c->string, msg);
            faulted = true;
        }
    }
    return faulted;
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
    int i = 0;
    for (const cJSON *oj = ops->child; oj; oj = oj->next, ++i) {
        tp_txn__test_count_op_walk(1U);
        tp_status st = tp_txn__lower_op(oj, &req->ops[i], err);
        if (st != TP_STATUS_OK) {
            return st; /* req->op_count = the count already fully lowered; free frees them */
        }
        req->op_count = i + 1;
    }
    return TP_STATUS_OK;
}

static tp_status txn_request_decode_n_impl(const char *json, size_t json_len,
                                           int expected_op_count,
                                           bool prechecked,
                                           tp_txn_request **out,
                                           tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (!prechecked) {
        tp_status byte_st = txn_request_bytes_check(json, json_len, err);
        if (byte_st != TP_STATUS_OK) {
            return byte_st;
        }
        tp_status count_st = txn_request_op_count_precheck(
            json, json_len, NULL, NULL, err);
        if (count_st != TP_STATUS_OK) {
            return count_st;
        }
    } else if (!json || expected_op_count < 0 ||
               expected_op_count > TP_TXN_MAX_OPS) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid prechecked transaction span");
    }
    cJSON *root = txn_parse_exact(json, json_len);
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
    if (prechecked && cJSON_GetArraySize(ops) != expected_op_count) {
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "prechecked operation count changed during decode");
    }

    /* Fail-fast shape (single fault) for the standalone decoder: the first op with a
     * shape error stops with that error's status. */
    int i = 0;
    for (const cJSON *oj = ops->child; oj; oj = oj->next, ++i) {
        tp_txn_result tmp;
        tp_txn__result_reset(&tmp, req->id_hex);
        bool store_oom = false;
        int fault_count = 0;
        tp_txn__test_count_op_walk(1U);
        if (shape_check_op(oj, i, &tmp, -1, &fault_count, &store_oom)) {
            tp_status code = (tmp.error_count > 0) ? tmp.errors[0].code : TP_STATUS_OOM;
            (void)tp_error_set(err, code, "%s", tmp.error_count > 0 ? tmp.errors[0].message : "out of memory");
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

tp_status tp_txn_request_decode_n(const char *json, size_t json_len,
                                  tp_txn_request **out, tp_error *err) {
    return txn_request_decode_n_impl(json, json_len, 0, false, out, err);
}

tp_status tp_txn__decode_prechecked_json_n(const char *json, size_t json_len,
                                           int expected_op_count,
                                           tp_txn_request **out,
                                           tp_error *err) {
    return txn_request_decode_n_impl(json, json_len, expected_op_count, true,
                                     out, err);
}

tp_status tp_txn_request_decode(const char *json, tp_txn_request **out, tp_error *err) {
    return tp_txn_request_decode_n(json, txn_cstr_len_bounded(json), out, err);
}

/* The JSON apply flow over a GUARANTEED-non-NULL working result `res` (the wrapper
 * supplies a local when the caller passes out==NULL, so every collect-all path can
 * store into a real result and no path dereferences a NULL out). Does not free
 * `res` (its ops/errors belong to the caller, or to the wrapper's local). */
static tp_status apply_json_into(tp_model *m, const char *json, size_t json_len,
                                 tp_txn_result *res, tp_error *err) {
    tp_txn__result_reset(res, "");

    tp_status byte_st = txn_request_bytes_check(json, json_len, err);
    if (byte_st != TP_STATUS_OK) {
        res->revision = m->revision;
        tp_txn__result_add_error(res, -1, byte_st, "transaction", err ? err->msg : "");
        return byte_st;
    }

    tp_status count_st = txn_request_op_count_precheck(json, json_len, NULL, NULL, err);
    if (count_st != TP_STATUS_OK) {
        res->revision = m->revision;
        tp_txn__result_add_error(res, -1, count_st, "operations", err ? err->msg : "");
        return count_st;
    }

    cJSON *root = txn_parse_exact(json, json_len);
    if (!root) {
        res->revision = m->revision; /* preserve the revision so the client is not told the model reset */
        tp_txn__result_add_error(res, -1, TP_STATUS_INVALID_ARGUMENT, "", "malformed JSON");
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
        tp_txn__result_reset(res, req->id_hex);
        res->revision = m->revision;
        tp_txn__result_add_error(res, -1, st, "", err ? err->msg : "");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }

    /* 2-3. Shared preflight gate: id format + idempotency + revision precondition
     * (op_index -1) -- the SAME helper the typed path runs, so the two cannot drift. */
    tp_status pf = tp_txn__preflight(m, req->id_hex, req->expected_revision, res, err);
    if (pf != TP_STATUS_OK) {
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return pf;
    }

    /* 4. Per-op SHAPE checks, collected in (op_index, field) order. */
    int shape_fault_count = 0;
    bool store_oom = false;
    int i = 0;
    for (const cJSON *oj = ops->child; oj; oj = oj->next, ++i) {
        tp_txn__test_count_op_walk(1U);
        (void)shape_check_op(oj, i, NULL, 0, &shape_fault_count, &store_oom);
    }
    if (shape_fault_count > 0) {
        if (!tp_txn__result_reserve_errors(res, shape_fault_count)) {
            store_oom = true;
        } else {
            int emitted_faults = 0;
            i = 0;
            for (const cJSON *oj = ops->child; oj; oj = oj->next, ++i) {
                tp_txn__test_count_op_walk(1U);
                (void)shape_check_op(oj, i, res, shape_fault_count,
                                     &emitted_faults, &store_oom);
            }
        }
    }
    if (shape_fault_count > 0 || store_oom) {
        res->committed = false;
        res->revision = m->revision;
        tp_txn_request_free(req);
        cJSON_Delete(root);
        if (store_oom) {
            /* A shape fault was detected but its record could not be stored: reject
             * (never commit) even though error_count is short of the real fault count. */
            return tp_error_set(err, TP_STATUS_OOM, "could not record all shape faults (out of memory)");
        }
        tp_status code = (res->error_count > 0) ? res->errors[0].code : TP_STATUS_INVALID_ARGUMENT;
        return tp_error_set(err, code, "%d shape fault(s) in the batch", res->error_count);
    }

    /* 5. Lower to typed ops. A value-type fault rejects (op_index of the bad op). */
    st = lower_all(ops, req, err);
    if (st != TP_STATUS_OK) {
        res->committed = false;
        res->revision = m->revision;
        tp_txn__result_add_error(res, req->op_count, st, "", err ? err->msg : "");
        tp_txn_request_free(req);
        cJSON_Delete(root);
        return st;
    }
    cJSON_Delete(root);

    /* 6. Atomic commit on the clone (idempotency + revision already passed). */
    st = tp_txn__commit_validated(m, req, res, err);
    tp_txn_request_free(req);
    return st;
}

tp_status tp_model_apply_json_n(tp_model *m, const char *json, size_t json_len,
                                tp_txn_result *out, tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    /* The twin tp_model_apply supports out==NULL. The JSON path collects shape faults
     * into a result, so when the caller passes NULL we collect into a local and free
     * it here -- no path ever dereferences a NULL out. */
    tp_txn_result local;
    tp_txn_result *res = out ? out : &local;
    tp_status st = apply_json_into(m, json, json_len, res, err);
    if (!out) {
        tp_txn_result_free(&local);
    }
    return st;
}

tp_status tp_model_apply_json(tp_model *m, const char *json, tp_txn_result *out, tp_error *err) {
    return tp_model_apply_json_n(m, json, txn_cstr_len_bounded(json), out, err);
}
