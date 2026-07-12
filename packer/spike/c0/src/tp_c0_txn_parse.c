#include "tp_c0/tp_c0_txn.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_c0_txn_priv.h"

/* Structural decode of the versioned transaction REQUEST envelope into typed
 * structs. Envelope faults are raised here; per-op semantic faults are left to
 * tp_c0_txn_validate so they can be collected in stable order. Shared decode
 * primitives (tpc0_*) live in tp_c0_txn_util.c. */

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
        if (c->string && !tpc0_in_skip(c->string, tx_keys, 5)) {
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
