#ifndef TP_C0_TXN_EMIT_H
#define TP_C0_TXN_EMIT_H

/*
 * C0-02 internal byte-stable field emitters -- NOT installed (src/). Shared by
 * the request and result encoders. Object keys are emitted in ascending ASCII
 * order (the repo determinism convention, tp_project.c); an op/result object
 * emits its discriminator ("op"/"schema") first, then this sorts the rest.
 */

#include <string.h>

#include "tp_c0/tp_c0_txn.h"
#include "tp_c0_jw.h"

/* Ascending-key insertion sort of field indices (count <= TP_C0_MAX_FIELDS). */
static inline void tp_c0_sort_fields(const tp_c0_field *fields, int count, int *order) {
    for (int i = 0; i < count; i++) {
        order[i] = i;
    }
    for (int i = 1; i < count; i++) {
        int v = order[i];
        int j = i - 1;
        while (j >= 0 && strcmp(fields[order[j]].key, fields[v].key) > 0) {
            order[j + 1] = order[j];
            j--;
        }
        order[j + 1] = v;
    }
}

static inline void tp_c0_emit_value(tp_c0_jw *w, const tp_c0_val *v, int depth) {
    switch (v->kind) {
        case TP_C0_VAL_INT: tp_c0_jw_int(w, v->ival); break;
        case TP_C0_VAL_NUM: tp_c0_jw_num(w, v->nval); break;
        case TP_C0_VAL_BOOL: tp_c0_jw_bool(w, v->bval); break;
        case TP_C0_VAL_STR: tp_c0_jw_json_string(w, v->sval); break;
        case TP_C0_VAL_STR_ARRAY:
            if (v->item_count == 0) {
                tp_c0_jw_str(w, "[]");
            } else {
                tp_c0_jw_char(w, '[');
                for (int i = 0; i < v->item_count; i++) {
                    tp_c0_jw_str(w, i == 0 ? "\n" : ",\n");
                    tp_c0_jw_indent(w, depth + 1);
                    tp_c0_jw_json_string(w, v->items[i]);
                }
                tp_c0_jw_char(w, '\n');
                tp_c0_jw_indent(w, depth);
                tp_c0_jw_char(w, ']');
            }
            break;
    }
}

static inline void tp_c0_emit_field(tp_c0_jw *w, int depth, bool *first, const tp_c0_field *f) {
    tp_c0_jw_key(w, depth, first, f->key);
    tp_c0_emit_value(w, &f->val, depth);
}

/* "{ <sorted fields> }" (or "{}") at object-body indent `depth`. */
static inline void tp_c0_emit_field_object(tp_c0_jw *w, const tp_c0_field *fields, int count, int depth) {
    if (count == 0) {
        tp_c0_jw_str(w, "{}");
        return;
    }
    tp_c0_jw_char(w, '{');
    int order[TP_C0_MAX_FIELDS];
    tp_c0_sort_fields(fields, count, order);
    bool first = true;
    for (int i = 0; i < count; i++) {
        tp_c0_emit_field(w, depth + 1, &first, &fields[order[i]]);
    }
    tp_c0_jw_char(w, '\n');
    tp_c0_jw_indent(w, depth);
    tp_c0_jw_char(w, '}');
}

#endif /* TP_C0_TXN_EMIT_H */
