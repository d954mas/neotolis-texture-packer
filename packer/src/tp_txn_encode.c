/*
 * Canonical BYTE-STABLE encode of a transaction request
 * and result. Same conventions as the tp_project writer (src/tp_sb.h): 2-space
 * indent, LF, keys ASCENDING with the discriminator ("schema" at the envelope, "op"
 * in an operation) first, a trailing newline; integral 64-bit numbers via PRId64
 * (no decimal point), fractional via "%.9g"; label/author sparse-omitted. Goldens
 * are byte-identical on every OS.
 *
 * Each operation object is emitted by REUSING tp_operation_encode (the one
 * owner of the per-kind canonical op shape) and re-indenting its 2-space-based
 * output into the operations array -- so a batch op is byte-identical to the same op
 * encoded standalone, and no per-kind emit logic is duplicated here.
 */

#include "tp_core/tp_transaction.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_encode_internal.h"
#include "tp_op_internal.h"
#include "tp_sb.h"

static _Thread_local size_t s_test_request_encode_calls;
static _Thread_local size_t s_test_last_measure_allocations;

void tp_txn__test_encode_stats_reset(void) {
    s_test_request_encode_calls = 0U;
    s_test_last_measure_allocations = 0U;
}

size_t tp_txn__test_request_encode_calls(void) {
    return s_test_request_encode_calls;
}

size_t tp_txn__test_last_measure_allocations(void) {
    return s_test_last_measure_allocations;
}

/* Emit `op` at its final depth through the operation encoder's one canonical
 * writer. The caller has already emitted indent(depth). */
static bool emit_op_embedded(tp_sb *sb, const tp_operation *op,
                             const tp_project *project, int depth) {
    tp_operation canonical;
    char canonical_path[TP_IDENTITY_PATH_MAX];
    if (project &&
        tp_op__canonical_view(project, op, &canonical, canonical_path,
                              sizeof canonical_path) != TP_STATUS_OK) {
        sb->oom = true;
        return false;
    }
    const tp_operation *emitted = project ? &canonical : op;
    if (tp_operation_emit_canonical(sb, emitted, depth, false)) {
        return true;
    }
    if (!sb->limit_exceeded) {
        sb->oom = true; /* invalid operation or encoder failure -> no partial JSON */
    }
    return false;
}

static bool emit_request(tp_sb *sb, const tp_txn_request *req,
                         const tp_project *project) {
    if (!req || req->op_count < 0 || req->op_count > TP_TXN_MAX_OPS ||
        (req->op_count > 0 && !req->ops)) {
        return false;
    }
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, 1, &first, "schema");
    tp_sb_i64(sb, req->schema);

    tp_obj_key(sb, 1, &first, "transaction");
    tp_sb_char(sb, '{');
    bool tf = true;
    if (req->author && req->author[0]) {
        tp_obj_key(sb, 2, &tf, "author");
        tp_sb_json_string(sb, req->author);
    }
    tp_obj_key(sb, 2, &tf, "expected_revision");
    tp_sb_i64(sb, req->expected_revision);
    tp_obj_key(sb, 2, &tf, "id");
    tp_sb_json_string(sb, req->id_hex);
    if (req->label && req->label[0]) {
        tp_obj_key(sb, 2, &tf, "label");
        tp_sb_json_string(sb, req->label);
    }
    tp_obj_key(sb, 2, &tf, "operations");
    if (req->op_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < req->op_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 3);
            if (!emit_op_embedded(sb, &req->ops[i], project, 3)) break;
        }
        tp_sb_char(sb, '\n');
        tp_sb_indent(sb, 2);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n  }\n}\n");
    return !sb->oom && !sb->limit_exceeded;
}

char *tp_txn_request_encode_bounded(const tp_txn_request *req, size_t max_bytes, bool *too_large) {
    if (too_large) *too_large = false;
    s_test_request_encode_calls++;
    tp_sb sb = {0};
    sb.limit = max_bytes;
    const bool emitted = emit_request(&sb, req, NULL);
    if (too_large) *too_large = sb.limit_exceeded;
    if (!emitted) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

bool tp_txn_request_encoded_size(const tp_txn_request *req, size_t *size_out) {
    if (!size_out) {
        return false;
    }
    *size_out = 0U;
    s_test_last_measure_allocations = 0U;
    tp_sb sb = {
        .count_only = true,
        .allocation_count = &s_test_last_measure_allocations,
    };
    if (!emit_request(&sb, req, NULL)) {
        return false;
    }
    *size_out = sb.len;
    return true;
}

char *tp_txn_request_encode_bounded_for_project(
    const tp_txn_request *req, const tp_project *project, size_t max_bytes,
    bool *too_large) {
    if (too_large) *too_large = false;
    s_test_request_encode_calls++;
    tp_sb sb = {0};
    sb.limit = max_bytes;
    const bool emitted = emit_request(&sb, req, project);
    if (too_large) *too_large = sb.limit_exceeded;
    if (!emitted) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

bool tp_txn_request_encoded_size_for_project(const tp_txn_request *req,
                                             const tp_project *project,
                                             size_t *size_out) {
    if (!size_out) {
        return false;
    }
    *size_out = 0U;
    s_test_last_measure_allocations = 0U;
    tp_sb sb = {
        .count_only = true,
        .allocation_count = &s_test_last_measure_allocations,
    };
    if (!emit_request(&sb, req, project)) {
        return false;
    }
    *size_out = sb.len;
    return true;
}

char *tp_txn_request_encode(const tp_txn_request *req) {
    return tp_txn_request_encode_bounded(req, 0U, NULL);
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
                if (res->errors[i].field[0]) { /* sparse: omit when "" -- matches tp_op_result_encode */
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
    tp_sb_json_string(&sb, res->committed ? "committed"
                                         : (res->no_change ? "no_change"
                                                           : "rejected"));
    tp_obj_key(&sb, 2, &rf, "transaction_id");
    tp_sb_json_string(&sb, res->transaction_id);
    tp_sb_str(&sb, "\n  }\n}\n");
    if (sb.oom) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}
