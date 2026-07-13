/*
 * F2-02 task 3 (encode half): canonical BYTE-STABLE encode of a transaction request
 * and result. Same conventions as the tp_project writer (src/tp_sb.h): 2-space
 * indent, LF, keys ASCENDING with the discriminator ("schema" at the envelope, "op"
 * in an operation) first, a trailing newline; integral 64-bit numbers via PRId64
 * (no decimal point), fractional via "%.9g"; label/author sparse-omitted. Goldens
 * are byte-identical on every OS.
 *
 * Each operation object is emitted by REUSING the F2-01 tp_operation_encode (the one
 * owner of the per-kind canonical op shape) and re-indenting its 2-space-based
 * output into the operations array -- so a batch op is byte-identical to the same op
 * encoded standalone, and no per-kind emit logic is duplicated here.
 */

#include "tp_core/tp_transaction.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_sb.h"

/* Emit `op` as a JSON object at `depth`, reusing tp_operation_encode and shifting
 * its (depth-0-based) indentation by `depth` levels. The caller has already emitted
 * indent(depth) before the opening brace. Returns false on OOM. */
static bool emit_op_embedded(tp_sb *sb, const tp_operation *op, int depth) {
    char *s = tp_operation_encode(op);
    if (!s) {
        sb->oom = true;
        return false;
    }
    size_t n = strlen(s);
    if (n > 0 && s[n - 1] == '\n') { /* drop the standalone trailing newline */
        s[n - 1] = '\0';
    }
    const char *p = s;
    bool first = true;
    for (;;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (!first) { /* every line after the "{" gets a newline + shifted indent */
            tp_sb_char(sb, '\n');
            tp_sb_indent(sb, depth);
        }
        tp_sb_write(sb, p, len);
        first = false;
        if (!nl) {
            break;
        }
        p = nl + 1;
    }
    free(s);
    return !sb->oom;
}

char *tp_txn_request_encode(const tp_txn_request *req) {
    if (!req) {
        return NULL;
    }
    tp_sb sb = {0};
    tp_sb_char(&sb, '{');
    bool first = true;
    tp_obj_key(&sb, 1, &first, "schema");
    tp_sb_i64(&sb, req->schema);

    tp_obj_key(&sb, 1, &first, "transaction");
    tp_sb_char(&sb, '{');
    bool tf = true;
    if (req->author && req->author[0]) {
        tp_obj_key(&sb, 2, &tf, "author");
        tp_sb_json_string(&sb, req->author);
    }
    tp_obj_key(&sb, 2, &tf, "expected_revision");
    tp_sb_i64(&sb, req->expected_revision);
    tp_obj_key(&sb, 2, &tf, "id");
    tp_sb_json_string(&sb, req->id_hex);
    if (req->label && req->label[0]) {
        tp_obj_key(&sb, 2, &tf, "label");
        tp_sb_json_string(&sb, req->label);
    }
    tp_obj_key(&sb, 2, &tf, "operations");
    if (req->op_count == 0) {
        tp_sb_str(&sb, "[]");
    } else {
        tp_sb_char(&sb, '[');
        for (int i = 0; i < req->op_count; i++) {
            tp_sb_str(&sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(&sb, 3);
            emit_op_embedded(&sb, &req->ops[i], 3);
        }
        tp_sb_char(&sb, '\n');
        tp_sb_indent(&sb, 2);
        tp_sb_char(&sb, ']');
    }
    tp_sb_str(&sb, "\n  }\n}\n");
    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

/* ---- result encode ------------------------------------------------------- */

/* One echoed addressing field, sortable by key. */
typedef struct {
    const char *key;
    bool is_id;
    tp_id_kind idk;
    tp_id128 id;
    const char *str;
} echo;

static void emit_result_op(tp_sb *sb, const tp_txn_result_op *ro, int depth) {
    echo e[3];
    int n = 0;
    for (int i = 0; i < ro->addr_count && n < 3; i++) {
        e[n].key = ro->addr[i].key;
        e[n].is_id = ro->addr[i].idk != TP_ID_KIND_INVALID;
        e[n].idk = ro->addr[i].idk;
        e[n].id = ro->addr[i].id;
        e[n].str = ro->addr[i].str;
        n++;
    }
    for (int i = 1; i < n; i++) { /* insertion sort by key (ascending) */
        echo t = e[i];
        int j = i - 1;
        while (j >= 0 && strcmp(e[j].key, t.key) > 0) {
            e[j + 1] = e[j];
            j--;
        }
        e[j + 1] = t;
    }
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "op"); /* discriminator first */
    tp_sb_json_string(sb, ro->wire);
    for (int i = 0; i < n; i++) {
        tp_obj_key(sb, depth + 1, &first, e[i].key);
        if (e[i].is_id) {
            char buf[TP_ID_TEXT_CAP];
            if (tp_id_format(e[i].idk, e[i].id, buf, sizeof buf, NULL) != TP_STATUS_OK) {
                buf[0] = '\0';
            }
            tp_sb_json_string(sb, buf);
        } else {
            tp_sb_json_string(sb, e[i].str ? e[i].str : "");
        }
    }
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

char *tp_txn_result_encode(const tp_txn_result *res) {
    if (!res) {
        return NULL;
    }
    tp_sb sb = {0};
    tp_sb_char(&sb, '{');
    bool first = true;
    tp_obj_key(&sb, 1, &first, "schema");
    tp_sb_i64(&sb, res->schema);
    tp_obj_key(&sb, 1, &first, "result");
    tp_sb_char(&sb, '{');
    bool rf = true;
    if (res->committed) {
        tp_obj_key(&sb, 2, &rf, "operations");
        if (res->op_count == 0) {
            tp_sb_str(&sb, "[]");
        } else {
            tp_sb_char(&sb, '[');
            for (int i = 0; i < res->op_count; i++) {
                tp_sb_str(&sb, i == 0 ? "\n" : ",\n");
                tp_sb_indent(&sb, 3);
                emit_result_op(&sb, &res->ops[i], 3);
            }
            tp_sb_char(&sb, '\n');
            tp_sb_indent(&sb, 2);
            tp_sb_char(&sb, ']');
        }
    } else {
        tp_obj_key(&sb, 2, &rf, "errors");
        if (res->error_count == 0) {
            tp_sb_str(&sb, "[]");
        } else {
            tp_sb_char(&sb, '[');
            for (int i = 0; i < res->error_count; i++) {
                tp_sb_str(&sb, i == 0 ? "\n" : ",\n");
                tp_sb_indent(&sb, 3);
                tp_sb_char(&sb, '{');
                bool ef = true;
                tp_obj_key(&sb, 4, &ef, "code");
                tp_sb_json_string(&sb, tp_status_id(res->errors[i].code));
                if (res->errors[i].field[0]) { /* sparse: omit when "" -- matches F2-01 tp_op_result_encode */
                    tp_obj_key(&sb, 4, &ef, "field");
                    tp_sb_json_string(&sb, res->errors[i].field);
                }
                tp_obj_key(&sb, 4, &ef, "message");
                tp_sb_json_string(&sb, res->errors[i].message);
                tp_obj_key(&sb, 4, &ef, "op_index");
                tp_sb_int(&sb, res->errors[i].op_index);
                tp_sb_char(&sb, '\n');
                tp_sb_indent(&sb, 3);
                tp_sb_char(&sb, '}');
            }
            tp_sb_char(&sb, '\n');
            tp_sb_indent(&sb, 2);
            tp_sb_char(&sb, ']');
        }
    }
    tp_obj_key(&sb, 2, &rf, "revision");
    tp_sb_i64(&sb, res->revision);
    tp_obj_key(&sb, 2, &rf, "status");
    tp_sb_json_string(&sb, res->committed ? "committed" : "rejected");
    tp_obj_key(&sb, 2, &rf, "transaction_id");
    tp_sb_json_string(&sb, res->transaction_id);
    tp_sb_str(&sb, "\n  }\n}\n");
    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
