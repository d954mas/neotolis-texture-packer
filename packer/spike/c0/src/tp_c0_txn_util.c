#include "tp_c0/tp_c0_txn.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "tp_c0_txn_priv.h"

/* Largest double that represents consecutive integers exactly (2^53). Beyond it a
 * double->int64 cast is UB and integral round-trips are not width-safe. */
#define TPC0_INT_SAFE 9007199254740992.0

/* Shared decode primitives for the request and result parsers (declared in
 * tp_c0_txn_priv.h). cJSON is a PRIVATE dep, kept inside these .c files. */

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

bool tpc0_in_skip(const char *key, const char *const *skip, int skip_n) {
    for (int i = 0; i < skip_n; i++) {
        if (strcmp(key, skip[i]) == 0) {
            return true;
        }
    }
    return false;
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
        /* INT only when integral AND exactly representable (within +/-2^53); the
         * range test excludes inf/NaN, so the cast below is never UB. Otherwise
         * NUM, re-encoded via "%.9g". */
        if (d >= -TPC0_INT_SAFE && d <= TPC0_INT_SAFE && d == floor(d)) {
            out->kind = TP_C0_VAL_INT;
            out->ival = (int64_t)d;
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

/* Decode every child key of `obj` (except `skip`) into a field list. */
tp_c0_detail tpc0_decode_field_list(const void *obj_v, const char *const *skip, int skip_n, tp_c0_field *out,
                                    int *count, int cap, tp_error *err) {
    const cJSON *obj = (const cJSON *)obj_v;
    *count = 0;
    for (const cJSON *c = obj->child; c; c = c->next) {
        if (!c->string || tpc0_in_skip(c->string, skip, skip_n)) {
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

tp_c0_detail tpc0_json_to_i64(const void *item_v, int64_t *out, tp_error *err) {
    const cJSON *item = (const cJSON *)item_v;
    if (!cJSON_IsNumber(item)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "expected an integer number");
    }
    double d = item->valuedouble;
    /* The range test is false for inf/NaN, so floor() and the cast never see them
     * and the (int64_t) cast is always in range -- no UB, no UBSan abort. */
    if (!(d >= -TPC0_INT_SAFE && d <= TPC0_INT_SAFE) || d != floor(d)) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "number out of range (must be an integer within +/-2^53)");
    }
    *out = (int64_t)d;
    return TP_C0_OK;
}

tp_c0_detail tpc0_json_to_int(const void *item_v, int *out, tp_error *err) {
    int64_t v = 0;
    tp_c0_detail d = tpc0_json_to_i64(item_v, &v, err);
    if (d != TP_C0_OK) {
        return d;
    }
    if (v < INT_MIN || v > INT_MAX) {
        return tp_c0_fail(err, TP_C0_ERR_TXN_BAD_TYPE, "number out of int range");
    }
    *out = (int)v;
    return TP_C0_OK;
}
