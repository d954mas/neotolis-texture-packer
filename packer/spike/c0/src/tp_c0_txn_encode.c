#include "tp_c0/tp_c0_txn.h"

#include <stdlib.h>
#include <string.h>

#include "tp_c0_jw.h"
#include "tp_c0_txn_emit.h"

/* Byte-stable canonical encode of a transaction REQUEST and RESULT (ID-only
 * form). Object keys ascend; each op emits "op" first. A trailing newline
 * matches the project-file writer's convention. */

static void emit_op(tp_c0_jw *w, const tp_c0_op *op, int depth) {
    tp_c0_jw_char(w, '{');
    bool first = true;
    tp_c0_jw_key(w, depth + 1, &first, "op");
    tp_c0_jw_json_string(w, op->wire);
    int order[TP_C0_MAX_FIELDS];
    tp_c0_sort_fields(op->fields, op->field_count, order);
    for (int i = 0; i < op->field_count; i++) {
        tp_c0_emit_field(w, depth + 1, &first, &op->fields[order[i]]);
    }
    tp_c0_jw_char(w, '\n');
    tp_c0_jw_indent(w, depth);
    tp_c0_jw_char(w, '}');
}

char *tp_c0_txn_request_encode(const tp_c0_txn_request *req, tp_c0_detail *detail) {
    if (detail) {
        *detail = TP_C0_OK;
    }
    if (!req) {
        if (detail) {
            *detail = TP_C0_ERR_NULL_ARG;
        }
        return NULL;
    }
    for (int i = 0; i < req->op_count; i++) {
        if (req->ops[i].has_selector) { /* canonical form is ID-only */
            if (detail) {
                *detail = TP_C0_ERR_SELECTOR_UNRESOLVED;
            }
            return NULL;
        }
    }

    tp_c0_jw w = {0};
    tp_c0_jw_char(&w, '{');
    bool first = true;
    tp_c0_jw_key(&w, 1, &first, "schema");
    tp_c0_jw_int(&w, req->schema);

    tp_c0_jw_key(&w, 1, &first, "transaction");
    tp_c0_jw_char(&w, '{');
    bool tf = true;
    if (req->author[0]) {
        tp_c0_jw_key(&w, 2, &tf, "author");
        tp_c0_jw_json_string(&w, req->author);
    }
    tp_c0_jw_key(&w, 2, &tf, "expected_revision");
    tp_c0_jw_int(&w, req->expected_revision);
    tp_c0_jw_key(&w, 2, &tf, "id");
    tp_c0_jw_json_string(&w, req->id_hex);
    if (req->label[0]) {
        tp_c0_jw_key(&w, 2, &tf, "label");
        tp_c0_jw_json_string(&w, req->label);
    }
    tp_c0_jw_key(&w, 2, &tf, "operations");
    if (req->op_count == 0) {
        tp_c0_jw_str(&w, "[]");
    } else {
        tp_c0_jw_char(&w, '[');
        for (int i = 0; i < req->op_count; i++) {
            tp_c0_jw_str(&w, i == 0 ? "\n" : ",\n");
            tp_c0_jw_indent(&w, 3);
            emit_op(&w, &req->ops[i], 3);
        }
        tp_c0_jw_char(&w, '\n');
        tp_c0_jw_indent(&w, 2);
        tp_c0_jw_char(&w, ']');
    }
    tp_c0_jw_char(&w, '\n');
    tp_c0_jw_indent(&w, 1);
    tp_c0_jw_char(&w, '}'); /* close transaction */

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

/* ---- result encode ------------------------------------------------------- */

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
