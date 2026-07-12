#include "tp_c0/tp_c0_txn.h"

#include <stdlib.h>

#include "tp_c0_jw.h"
#include "tp_c0_txn_emit.h"

/* Byte-stable canonical encode of a transaction REQUEST (ID-only form). Object
 * keys ascend; each op emits "op" first. A trailing newline matches the
 * project-file writer's convention. */

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
