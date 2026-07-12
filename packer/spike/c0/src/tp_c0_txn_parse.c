#include "tp_c0/tp_c0_txn.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_c0_txn_priv.h"

/* Decode of the versioned transaction request/result JSON into typed structs.
 * Structural/envelope faults are raised here; per-op semantic faults are left to
 * tp_c0_txn_validate so they can be collected in stable order. The tpc0_* helpers
 * are shared with the result parser via tp_c0_txn_priv.h. */

/* ---- small helpers ------------------------------------------------------- */

static char *dup_str(const char *s) {
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

bool tpc0_is_hex32_lower(const char *s) {
    if (!s) {
        return false;
    }
    int n = 0;
    for (; s[n]; n++) {
        char c = s[n];
        bool lc_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lc_hex) {
            return false;
        }
    }
    return n == 32;
}

void tpc0_free_val(tp_c0_val *v) {
    if (v->kind == TP_C0_VAL_STR_ARRAY && v->items) {
        for (int i = 0; i < v->item_count; i++) {
            free(v->items[i]);
        }
        free(v->items);
        v->items = NULL;
        v->item_count = 0;
    }
}

void tpc0_free_fields(tp_c0_field *f, int n) {
    for (int i = 0; i < n; i++) {
        tpc0_free_val(&f[i].val);
    }
}

/* Decode one scalar/string-array value. Objects/null are rejected (canonical op
 * fields are scalars or a string list; nested diff objects are decoded apart). */
tp_c0_detail tpc0_decode_val(const void *item_v, tp_c0_val *out, tp_error *err) {
    const cJSON *item = (const cJSON *)item_v;
    memset(out, 0, sizeof *out);
    if (cJSON_IsBool(item)) {
        out->kind = TP_C0_VAL_BOOL;
        out->bval = cJSON_IsTrue(item) ? true : false;
        return TP_C0_OK;
    }
    if (cJSON_IsNumber(item)) {
        double d = item->valuedouble;
        if (d == (double)(long)d) {
            out->kind = TP_C0_VAL_INT;
            out->ival = (long)d;
        } else {
            out->kind = TP_C0_VAL_NUM;
            out->nval = d;
        }
        return TP_C0_OK;
    }
    if (cJSON_IsString(item)) {
        if (strlen(item->valuestring) >= TP_C0_STR_CAP) {
            return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "string value exceeds %d bytes", TP_C0_STR_CAP - 1);
        }
        out->kind = TP_C0_VAL_STR;
        (void)snprintf(out->sval, sizeof out->sval, "%s", item->valuestring);
        return TP_C0_OK;
    }
    if (cJSON_IsArray(item)) {
        int n = cJSON_GetArraySize(item);
        out->kind = TP_C0_VAL_STR_ARRAY;
        out->item_count = 0;
        out->items = (n > 0) ? (char **)calloc((size_t)n, sizeof(char *)) : NULL;
        if (n > 0 && !out->items) {
            return tp_c0_fail(err, TP_C0_ERR_OOM, "array alloc");
        }
        for (int i = 0; i < n; i++) {
            const cJSON *el = cJSON_GetArrayItem(item, i);
            if (!cJSON_IsString(el)) {
                tpc0_free_val(out);
                return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "array element %d is not a string", i);
            }
            out->items[i] = dup_str(el->valuestring);
            if (!out->items[i]) {
                tpc0_free_val(out);
                return tp_c0_fail(err, TP_C0_ERR_OOM, "array element dup");
            }
            out->item_count++;
        }
        return TP_C0_OK;
    }
    return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "unsupported value type");
}

static bool in_skip(const char *key, const char *const *skip, int skip_n) {
    for (int i = 0; i < skip_n; i++) {
        if (strcmp(key, skip[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* Decode every child key of `obj` (except `skip`) into a field list. */
tp_c0_detail tpc0_decode_field_list(const void *obj_v, const char *const *skip, int skip_n, tp_c0_field *out,
                                    int *count, int cap, tp_error *err) {
    const cJSON *obj = (const cJSON *)obj_v;
    *count = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (!c->string || in_skip(c->string, skip, skip_n)) {
            continue;
        }
        if (*count >= cap) {
            tpc0_free_fields(out, *count);
            return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "too many fields (>%d)", cap);
        }
        if (strlen(c->string) >= sizeof out[0].key) {
            tpc0_free_fields(out, *count);
            return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "field key too long");
        }
        tp_c0_field *f = &out[*count];
        (void)snprintf(f->key, sizeof f->key, "%s", c->string);
        tp_c0_detail d = tpc0_decode_val(c, &f->val, err);
        if (d != TP_C0_OK) {
            tpc0_free_fields(out, *count);
            return d;
        }
        (*count)++;
    }
    return TP_C0_OK;
}

/* ---- request decode ------------------------------------------------------ */

/* schema: absent -> missing_field; not-number -> bad_type; != 1 -> bad_version. */
static tp_c0_detail check_schema(const cJSON *root, tp_error *err) {
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!schema) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing \"schema\"");
    }
    if (!cJSON_IsNumber(schema)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "\"schema\" must be a number");
    }
    if (schema->valueint != TP_C0_TXN_SCHEMA) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_VERSION, "unknown schema version %d (want %d)", schema->valueint,
                          TP_C0_TXN_SCHEMA);
    }
    return TP_C0_OK;
}

static tp_c0_detail fail_req(tp_c0_txn_request *req, cJSON *root, tp_c0_detail *detail, tp_c0_detail d) {
    if (detail) {
        *detail = d;
    }
    tp_c0_txn_request_free(req);
    cJSON_Delete(root);
    return d;
}

tp_c0_txn_request *tp_c0_txn_request_decode(const char *json, tp_c0_detail *detail, tp_error *err) {
    if (detail) {
        *detail = TP_C0_OK;
    }
    if (!json) {
        (void)tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null json");
        if (detail) {
            *detail = TP_C0_ERR_NULL_ARG;
        }
        return NULL;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        (void)tp_c0_fail(err, TP_C0_ERR_BAD_JSON, "malformed JSON");
        if (detail) {
            *detail = TP_C0_ERR_BAD_JSON;
        }
        return NULL;
    }
    tp_c0_txn_request *req = (tp_c0_txn_request *)calloc(1, sizeof *req);
    if (!req) {
        (void)tp_c0_fail(err, TP_C0_ERR_OOM, "request alloc");
        if (detail) {
            *detail = TP_C0_ERR_OOM;
        }
        cJSON_Delete(root);
        return NULL;
    }

    tp_c0_detail d = check_schema(root, err);
    if (d != TP_C0_OK) {
        (void)fail_req(req, root, detail, d);
        return NULL;
    }
    req->schema = TP_C0_TXN_SCHEMA;

    /* Envelope unknown-field policy: REJECT (contract §2). */
    for (const cJSON *c = root->child; c; c = c->next) {
        if (c->string && strcmp(c->string, "schema") != 0 && strcmp(c->string, "transaction") != 0) {
            (void)tp_c0_fail(err, TP_C0_ERR_UNKNOWN_FIELD, "unknown envelope key \"%s\"", c->string);
            (void)fail_req(req, root, detail, TP_C0_ERR_UNKNOWN_FIELD);
            return NULL;
        }
    }

    const cJSON *tx = cJSON_GetObjectItemCaseSensitive(root, "transaction");
    if (!cJSON_IsObject(tx)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing \"transaction\" object");
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_MISSING_FIELD);
        return NULL;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(tx, "id");
    if (!cJSON_IsString(id)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing transaction \"id\"");
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_MISSING_FIELD);
        return NULL;
    }
    if (!tpc0_is_hex32_lower(id->valuestring)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_ID, "transaction id must be 32 lowercase hex");
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_BAD_ID);
        return NULL;
    }
    (void)snprintf(req->id_hex, sizeof req->id_hex, "%s", id->valuestring);

    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(tx, "expected_revision");
    if (!cJSON_IsNumber(rev)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing numeric \"expected_revision\"");
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_MISSING_FIELD);
        return NULL;
    }
    req->expected_revision = (long)rev->valuedouble;

    const cJSON *label = cJSON_GetObjectItemCaseSensitive(tx, "label");
    if (label && cJSON_IsString(label)) {
        (void)snprintf(req->label, sizeof req->label, "%s", label->valuestring);
    }
    const cJSON *author = cJSON_GetObjectItemCaseSensitive(tx, "author");
    if (author && cJSON_IsString(author)) {
        (void)snprintf(req->author, sizeof req->author, "%s", author->valuestring);
    }

    /* Transaction-level unknown-field policy: REJECT. */
    static const char *const tx_keys[] = {"id", "expected_revision", "label", "author", "operations"};
    for (const cJSON *c = tx->child; c; c = c->next) {
        if (c->string && !in_skip(c->string, tx_keys, 5)) {
            (void)tp_c0_fail(err, TP_C0_ERR_UNKNOWN_FIELD, "unknown transaction key \"%s\"", c->string);
            (void)fail_req(req, root, detail, TP_C0_ERR_UNKNOWN_FIELD);
            return NULL;
        }
    }

    const cJSON *ops = cJSON_GetObjectItemCaseSensitive(tx, "operations");
    if (!cJSON_IsArray(ops)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing \"operations\" array");
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_MISSING_FIELD);
        return NULL;
    }
    int nops = cJSON_GetArraySize(ops);
    if (nops > TP_C0_MAX_OPS) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "too many operations (>%d)", TP_C0_MAX_OPS);
        (void)fail_req(req, root, detail, TP_C0_ERR_TXN_BAD_TYPE);
        return NULL;
    }
    static const char *const op_skip[] = {"op", "selector"};
    for (int i = 0; i < nops; i++) {
        const cJSON *oj = cJSON_GetArrayItem(ops, i);
        if (!cJSON_IsObject(oj)) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "operation %d is not an object", i);
            (void)fail_req(req, root, detail, TP_C0_ERR_TXN_BAD_TYPE);
            return NULL;
        }
        tp_c0_op *op = &req->ops[req->op_count];
        const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
        if (!cJSON_IsString(wire)) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "operation %d missing string \"op\"", i);
            (void)fail_req(req, root, detail, TP_C0_ERR_TXN_MISSING_FIELD);
            return NULL;
        }
        (void)snprintf(op->wire, sizeof op->wire, "%s", wire->valuestring);
        op->kind = tp_c0_op_kind_from_wire(op->wire); /* INVALID -> validate flags op_unknown */
        op->has_selector = cJSON_GetObjectItemCaseSensitive(oj, "selector") != NULL;
        tp_c0_detail fd = tpc0_decode_field_list(oj, op_skip, 2, op->fields, &op->field_count, TP_C0_MAX_FIELDS, err);
        if (fd != TP_C0_OK) {
            (void)fail_req(req, root, detail, fd);
            return NULL;
        }
        req->op_count++;
    }

    cJSON_Delete(root);
    return req;
}

void tp_c0_txn_request_free(tp_c0_txn_request *req) {
    if (!req) {
        return;
    }
    for (int i = 0; i < req->op_count; i++) {
        tpc0_free_fields(req->ops[i].fields, req->ops[i].field_count);
    }
    free(req);
}

/* ---- revision precondition + idempotency set ----------------------------- */

tp_c0_detail tp_c0_revision_check(long expected_revision, long current_revision, tp_error *err) {
    if (expected_revision == current_revision) {
        return TP_C0_OK;
    }
    if (expected_revision < current_revision) {
        return tp_c0_fail(err, TP_C0_ERR_REVISION_CONFLICT, "expected_revision %ld < current %ld", expected_revision,
                          current_revision);
    }
    return tp_c0_fail(err, TP_C0_ERR_INVALID_REVISION, "expected_revision %ld > current %ld", expected_revision,
                      current_revision);
}

tp_c0_detail tp_c0_txn_idset_add(tp_c0_txn_idset *set, const char *id_hex, tp_error *err) {
    if (!set || !id_hex) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null idset/id");
    }
    if (!tpc0_is_hex32_lower(id_hex)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_ID, "transaction id must be 32 lowercase hex");
    }
    for (int i = 0; i < set->count; i++) {
        if (strcmp(set->ids[i], id_hex) == 0) {
            return tp_c0_fail(err, TP_C0_ERR_TXN_DUPLICATE_ID, "transaction id already applied");
        }
    }
    if (set->count >= TP_C0_IDSET_CAP) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "idempotency set full");
    }
    (void)snprintf(set->ids[set->count], sizeof set->ids[0], "%s", id_hex);
    set->count++;
    return TP_C0_OK;
}
