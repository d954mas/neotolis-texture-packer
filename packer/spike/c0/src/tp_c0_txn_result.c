#include "tp_c0/tp_c0_txn.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_c0_jw.h"
#include "tp_c0_txn_emit.h"
#include "tp_c0_txn_priv.h"

/* Decode + byte-stable encode of the versioned transaction RESULT JSON: a
 * committed record (per-op before/after diffs) or a rejected record (stable
 * ordered structured errors). */

/* ---- decode -------------------------------------------------------------- */

static tp_c0_op_class class_from_str(const char *s, bool *ok) {
    *ok = true;
    if (strcmp(s, "create") == 0) {
        return TP_C0_OP_CLASS_CREATE;
    }
    if (strcmp(s, "remove") == 0) {
        return TP_C0_OP_CLASS_REMOVE;
    }
    if (strcmp(s, "move") == 0) {
        return TP_C0_OP_CLASS_MOVE;
    }
    if (strcmp(s, "set") == 0) {
        return TP_C0_OP_CLASS_SET;
    }
    *ok = false;
    return TP_C0_OP_CLASS_SET;
}

static tp_c0_detail code_from_str(const char *s, tp_c0_detail *out) {
    for (int d = TP_C0_OK; d <= TP_C0_ERR_INVALID_REVISION; d++) {
        if (strcmp(tp_c0_detail_id((tp_c0_detail)d), s) == 0) {
            *out = (tp_c0_detail)d;
            return TP_C0_OK;
        }
    }
    return TP_C0_ERR_TXN_BAD_TYPE;
}

static tp_c0_detail decode_diff(const cJSON *dj, tp_c0_diff *diff, tp_error *err) {
    memset(diff, 0, sizeof *diff);
    const cJSON *cls = cJSON_GetObjectItemCaseSensitive(dj, "class");
    if (!cJSON_IsString(cls)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "diff missing \"class\"");
    }
    bool ok = false;
    diff->cls = class_from_str(cls->valuestring, &ok);
    if (!ok) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "unknown diff class '%s'", cls->valuestring);
    }
    const cJSON *before = cJSON_GetObjectItemCaseSensitive(dj, "before");
    if (cJSON_IsObject(before)) {
        tp_c0_detail d = tpc0_decode_field_list(before, NULL, 0, diff->before, &diff->before_count, TP_C0_MAX_FIELDS, err);
        if (d != TP_C0_OK) {
            return d;
        }
        diff->has_before = true;
    }
    const cJSON *after = cJSON_GetObjectItemCaseSensitive(dj, "after");
    if (cJSON_IsObject(after)) {
        tp_c0_detail d = tpc0_decode_field_list(after, NULL, 0, diff->after, &diff->after_count, TP_C0_MAX_FIELDS, err);
        if (d != TP_C0_OK) {
            tpc0_free_fields(diff->before, diff->before_count);
            return d;
        }
        diff->has_after = true;
    }
    const cJSON *bi = cJSON_GetObjectItemCaseSensitive(dj, "before_index");
    const cJSON *ai = cJSON_GetObjectItemCaseSensitive(dj, "after_index");
    if (cJSON_IsNumber(bi) && cJSON_IsNumber(ai)) {
        diff->before_index = (int)bi->valuedouble;
        diff->after_index = (int)ai->valuedouble;
        diff->has_indices = true;
    }
    const cJSON *pos = cJSON_GetObjectItemCaseSensitive(dj, "position");
    if (cJSON_IsNumber(pos)) {
        diff->position = (int)pos->valuedouble;
        diff->has_position = true;
    }
    return TP_C0_OK;
}

static tp_c0_detail fail_res(tp_c0_txn_result *res, cJSON *root, tp_c0_detail *detail, tp_c0_detail d) {
    if (detail) {
        *detail = d;
    }
    tp_c0_txn_result_free(res);
    cJSON_Delete(root);
    return d;
}

tp_c0_txn_result *tp_c0_txn_result_decode(const char *json, tp_c0_detail *detail, tp_error *err) {
    if (detail) {
        *detail = TP_C0_OK;
    }
    if (!json) {
        if (detail) {
            *detail = TP_C0_ERR_NULL_ARG;
        }
        (void)tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "null json");
        return NULL;
    }
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        if (detail) {
            *detail = TP_C0_ERR_BAD_JSON;
        }
        (void)tp_c0_fail(err, TP_C0_ERR_BAD_JSON, "malformed JSON");
        return NULL;
    }
    tp_c0_txn_result *res = (tp_c0_txn_result *)calloc(1, sizeof *res);
    if (!res) {
        if (detail) {
            *detail = TP_C0_ERR_OOM;
        }
        (void)tp_c0_fail(err, TP_C0_ERR_OOM, "result alloc");
        cJSON_Delete(root);
        return NULL;
    }

    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    if (!cJSON_IsNumber(schema)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing numeric \"schema\"");
        return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
    }
    if (schema->valueint != TP_C0_TXN_SCHEMA) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_VERSION, "unknown schema version %d", schema->valueint);
        return (fail_res(res, root, detail, TP_C0_ERR_TXN_BAD_VERSION), NULL);
    }
    res->schema = TP_C0_TXN_SCHEMA;

    const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsObject(r)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "missing \"result\" object");
        return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
    }
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
    const cJSON *rev = cJSON_GetObjectItemCaseSensitive(r, "revision");
    const cJSON *tid = cJSON_GetObjectItemCaseSensitive(r, "transaction_id");
    if (!cJSON_IsString(status) || !cJSON_IsNumber(rev) || !cJSON_IsString(tid)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "result needs status/revision/transaction_id");
        return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
    }
    if (!tpc0_is_hex32_lower(tid->valuestring)) {
        (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_ID, "transaction_id must be 32 lowercase hex");
        return (fail_res(res, root, detail, TP_C0_ERR_TXN_BAD_ID), NULL);
    }
    (void)snprintf(res->txn_id_hex, sizeof res->txn_id_hex, "%s", tid->valuestring);
    res->revision = (long)rev->valuedouble;
    res->committed = strcmp(status->valuestring, "committed") == 0;

    if (res->committed) {
        const cJSON *ops = cJSON_GetObjectItemCaseSensitive(r, "operations");
        if (!cJSON_IsArray(ops)) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "committed result needs \"operations\"");
            return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
        }
        int n = cJSON_GetArraySize(ops);
        if (n > TP_C0_MAX_OPS) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "too many operations");
            return (fail_res(res, root, detail, TP_C0_ERR_TXN_BAD_TYPE), NULL);
        }
        static const char *const skip[] = {"op", "diff"};
        for (int i = 0; i < n; i++) {
            const cJSON *oj = cJSON_GetArrayItem(ops, i);
            const cJSON *wire = cJSON_GetObjectItemCaseSensitive(oj, "op");
            const cJSON *dj = cJSON_GetObjectItemCaseSensitive(oj, "diff");
            if (!cJSON_IsObject(oj) || !cJSON_IsString(wire) || !cJSON_IsObject(dj)) {
                (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "result op %d needs op/diff", i);
                return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
            }
            tp_c0_result_op *ro = &res->ops[res->op_count];
            (void)snprintf(ro->wire, sizeof ro->wire, "%s", wire->valuestring);
            tp_c0_detail fd = tpc0_decode_field_list(oj, skip, 2, ro->addr, &ro->addr_count, 6, err);
            if (fd != TP_C0_OK) {
                return (fail_res(res, root, detail, fd), NULL);
            }
            tp_c0_detail dd = decode_diff(dj, &ro->diff, err);
            if (dd != TP_C0_OK) {
                tpc0_free_fields(ro->addr, ro->addr_count);
                return (fail_res(res, root, detail, dd), NULL);
            }
            res->op_count++;
        }
    } else {
        const cJSON *errs = cJSON_GetObjectItemCaseSensitive(r, "errors");
        if (!cJSON_IsArray(errs)) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "rejected result needs \"errors\"");
            return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
        }
        int n = cJSON_GetArraySize(errs);
        if (n > TP_C0_MAX_ERRORS) {
            (void)tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "too many errors");
            return (fail_res(res, root, detail, TP_C0_ERR_TXN_BAD_TYPE), NULL);
        }
        for (int i = 0; i < n; i++) {
            const cJSON *ej = cJSON_GetArrayItem(errs, i);
            const cJSON *code = cJSON_GetObjectItemCaseSensitive(ej, "code");
            const cJSON *msg = cJSON_GetObjectItemCaseSensitive(ej, "message");
            const cJSON *oi = cJSON_GetObjectItemCaseSensitive(ej, "op_index");
            if (!cJSON_IsString(code) || !cJSON_IsNumber(oi)) {
                (void)tp_c0_fail(err, TP_C0_ERR_TXN_MISSING_FIELD, "error %d needs code/op_index", i);
                return (fail_res(res, root, detail, TP_C0_ERR_TXN_MISSING_FIELD), NULL);
            }
            tp_c0_txn_error *e = &res->errors[res->error_count];
            tp_c0_detail cd = code_from_str(code->valuestring, &e->code);
            if (cd != TP_C0_OK) {
                (void)tp_c0_fail(err, cd, "unknown error code '%s'", code->valuestring);
                return (fail_res(res, root, detail, cd), NULL);
            }
            e->op_index = (int)oi->valuedouble;
            if (cJSON_IsString(msg)) {
                (void)snprintf(e->message, sizeof e->message, "%s", msg->valuestring);
            }
            res->error_count++;
        }
    }

    cJSON_Delete(root);
    return res;
}

void tp_c0_txn_result_free(tp_c0_txn_result *res) {
    if (!res) {
        return;
    }
    for (int i = 0; i < res->op_count; i++) {
        tp_c0_result_op *ro = &res->ops[i];
        tpc0_free_fields(ro->addr, ro->addr_count);
        tpc0_free_fields(ro->diff.before, ro->diff.before_count);
        tpc0_free_fields(ro->diff.after, ro->diff.after_count);
    }
    free(res);
}

/* ---- encode -------------------------------------------------------------- */

static void emit_diff(tp_c0_jw *w, const tp_c0_diff *d, int depth) {
    tp_c0_jw_char(w, '{');
    bool first = true;
    if (d->has_after) {
        tp_c0_jw_key(w, depth + 1, &first, "after");
        tp_c0_emit_field_object(w, d->after, d->after_count, depth + 1);
    }
    if (d->has_indices) {
        tp_c0_jw_key(w, depth + 1, &first, "after_index");
        tp_c0_jw_int(w, d->after_index);
    }
    if (d->has_before) {
        tp_c0_jw_key(w, depth + 1, &first, "before");
        tp_c0_emit_field_object(w, d->before, d->before_count, depth + 1);
    }
    if (d->has_indices) {
        tp_c0_jw_key(w, depth + 1, &first, "before_index");
        tp_c0_jw_int(w, d->before_index);
    }
    tp_c0_jw_key(w, depth + 1, &first, "class");
    tp_c0_jw_json_string(w, tp_c0_op_class_name(d->cls));
    if (d->has_position) {
        tp_c0_jw_key(w, depth + 1, &first, "position");
        tp_c0_jw_int(w, d->position);
    }
    tp_c0_jw_char(w, '\n');
    tp_c0_jw_indent(w, depth);
    tp_c0_jw_char(w, '}');
}

/* Emit a committed result op: "op" first, then addr fields + "diff" merged in
 * ascending key order. */
static void emit_result_op(tp_c0_jw *w, const tp_c0_result_op *ro, int depth) {
    tp_c0_jw_char(w, '{');
    bool first = true;
    tp_c0_jw_key(w, depth + 1, &first, "op");
    tp_c0_jw_json_string(w, ro->wire);

    const char *keys[8];
    int is_diff[8];
    int idx[8];
    int m = 0;
    for (int i = 0; i < ro->addr_count && m < 8; i++) {
        keys[m] = ro->addr[i].key;
        is_diff[m] = 0;
        idx[m] = i;
        m++;
    }
    if (m < 8) {
        keys[m] = "diff";
        is_diff[m] = 1;
        idx[m] = -1;
        m++;
    }
    for (int i = 1; i < m; i++) { /* insertion sort by key */
        const char *kv = keys[i];
        int dv = is_diff[i], iv = idx[i], j = i - 1;
        while (j >= 0 && strcmp(keys[j], kv) > 0) {
            keys[j + 1] = keys[j];
            is_diff[j + 1] = is_diff[j];
            idx[j + 1] = idx[j];
            j--;
        }
        keys[j + 1] = kv;
        is_diff[j + 1] = dv;
        idx[j + 1] = iv;
    }
    for (int i = 0; i < m; i++) {
        if (is_diff[i]) {
            tp_c0_jw_key(w, depth + 1, &first, "diff");
            emit_diff(w, &ro->diff, depth + 1);
        } else {
            tp_c0_emit_field(w, depth + 1, &first, &ro->addr[idx[i]]);
        }
    }
    tp_c0_jw_char(w, '\n');
    tp_c0_jw_indent(w, depth);
    tp_c0_jw_char(w, '}');
}

char *tp_c0_txn_result_encode(const tp_c0_txn_result *res, tp_c0_detail *detail) {
    if (detail) {
        *detail = TP_C0_OK;
    }
    if (!res) {
        if (detail) {
            *detail = TP_C0_ERR_NULL_ARG;
        }
        return NULL;
    }
    tp_c0_jw w = {0};
    tp_c0_jw_char(&w, '{');
    bool first = true;
    tp_c0_jw_key(&w, 1, &first, "schema");
    tp_c0_jw_int(&w, res->schema);
    tp_c0_jw_key(&w, 1, &first, "result");
    tp_c0_jw_char(&w, '{');
    bool rf = true;
    if (res->committed) {
        tp_c0_jw_key(&w, 2, &rf, "operations");
        if (res->op_count == 0) {
            tp_c0_jw_str(&w, "[]");
        } else {
            tp_c0_jw_char(&w, '[');
            for (int i = 0; i < res->op_count; i++) {
                tp_c0_jw_str(&w, i == 0 ? "\n" : ",\n");
                tp_c0_jw_indent(&w, 3);
                emit_result_op(&w, &res->ops[i], 3);
            }
            tp_c0_jw_char(&w, '\n');
            tp_c0_jw_indent(&w, 2);
            tp_c0_jw_char(&w, ']');
        }
    } else {
        tp_c0_jw_key(&w, 2, &rf, "errors");
        if (res->error_count == 0) {
            tp_c0_jw_str(&w, "[]");
        } else {
            tp_c0_jw_char(&w, '[');
            for (int i = 0; i < res->error_count; i++) {
                tp_c0_jw_str(&w, i == 0 ? "\n" : ",\n");
                tp_c0_jw_indent(&w, 3);
                tp_c0_jw_char(&w, '{');
                bool ef = true;
                tp_c0_jw_key(&w, 4, &ef, "code");
                tp_c0_jw_json_string(&w, tp_c0_detail_id(res->errors[i].code));
                tp_c0_jw_key(&w, 4, &ef, "message");
                tp_c0_jw_json_string(&w, res->errors[i].message);
                tp_c0_jw_key(&w, 4, &ef, "op_index");
                tp_c0_jw_int(&w, res->errors[i].op_index);
                tp_c0_jw_char(&w, '\n');
                tp_c0_jw_indent(&w, 3);
                tp_c0_jw_char(&w, '}');
            }
            tp_c0_jw_char(&w, '\n');
            tp_c0_jw_indent(&w, 2);
            tp_c0_jw_char(&w, ']');
        }
    }
    tp_c0_jw_key(&w, 2, &rf, "revision");
    tp_c0_jw_int(&w, res->revision);
    tp_c0_jw_key(&w, 2, &rf, "status");
    tp_c0_jw_json_string(&w, res->committed ? "committed" : "rejected");
    tp_c0_jw_key(&w, 2, &rf, "transaction_id");
    tp_c0_jw_json_string(&w, res->txn_id_hex);
    tp_c0_jw_char(&w, '\n');
    tp_c0_jw_indent(&w, 1);
    tp_c0_jw_char(&w, '}'); /* close result */
    tp_c0_jw_char(&w, '\n');
    tp_c0_jw_char(&w, '}'); /* close root */
    tp_c0_jw_char(&w, '\n');

    if (w.oom) {
        free(w.buf);
        if (detail) {
            *detail = TP_C0_ERR_OOM;
        }
        return NULL;
    }
    return w.buf;
}
