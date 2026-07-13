#ifndef TP_CORE_SRC_TP_TXN_JSON_H
#define TP_CORE_SRC_TP_TXN_JSON_H

/*
 * F2-02 private cJSON reader helpers shared by the transaction request decoder
 * (tp_txn_parse.c) and the JSON->tp_operation lowering (tp_txn_lower.c). cJSON is a
 * PRIVATE dep confined to these src/ TUs -- it never appears on a public header.
 *
 * EVERY attacker-supplied number routes through the shared range-checked converter
 * j_i64 (integral AND within +/-2^53), so a JSON number is never cast out-of-range
 * to an integer (a UBSan abort in Debug CI): out of range -> a structured
 * TP_STATUS_OUT_OF_RANGE, fractional/inf/NaN -> TP_STATUS_INVALID_ARGUMENT. INT
 * storage is int64_t so 5000000000 round-trips on every OS.
 */

#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

/* Largest double representing consecutive integers exactly (2^53). Beyond it a
 * double->int64 cast is UB and integral round-trips are not width-safe. */
#define TP_TXN_INT_SAFE 9007199254740992.0

/* Range-checked JSON-number -> int64. The range test is false for inf/NaN, so the
 * (int64_t) cast below is always in range: no UB. */
static inline tp_status j_i64(const cJSON *item, int64_t *out, tp_error *err) {
    if (!cJSON_IsNumber(item)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "expected a number");
    }
    double d = item->valuedouble;
    if (!(d >= -TP_TXN_INT_SAFE && d <= TP_TXN_INT_SAFE)) {
        return tp_error_set(err, TP_STATUS_OUT_OF_RANGE, "number out of range (must be within +/-2^53)");
    }
    if (d != (double)(int64_t)d) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "number must be an integer");
    }
    *out = (int64_t)d;
    return TP_STATUS_OK;
}

/* Narrow a range-checked int64 to int. */
static inline tp_status j_i64_to_int(int64_t v, int *out, tp_error *err) {
    if (v < INT_MIN || v > INT_MAX) {
        return tp_error_set(err, TP_STATUS_OUT_OF_RANGE, "number out of int range");
    }
    *out = (int)v;
    return TP_STATUS_OK;
}

/* Optional int64 field. absent -> present=false, OK. present -> range-checked. */
static inline tp_status j_opt_i64(const cJSON *obj, const char *key, int64_t *out, bool *present, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    *present = (it != NULL);
    if (!it) {
        return TP_STATUS_OK;
    }
    return j_i64(it, out, err);
}

/* Optional int field (range-checked int64 then narrowed). */
static inline tp_status j_opt_int(const cJSON *obj, const char *key, int *out, bool *present, tp_error *err) {
    int64_t v = 0;
    tp_status st = j_opt_i64(obj, key, &v, present, err);
    if (st != TP_STATUS_OK || !*present) {
        return st;
    }
    return j_i64_to_int(v, out, err);
}

/* Optional float field. absent -> present=false. present -> must be a number; read
 * as double and narrow to float (a non-finite result is rejected by validate). */
static inline tp_status j_opt_float(const cJSON *obj, const char *key, float *out, bool *present, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    *present = (it != NULL);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsNumber(it)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"%s\" must be a number", key);
    }
    *out = (float)it->valuedouble;
    return TP_STATUS_OK;
}

/* Optional bool field. absent -> present=false. present -> must be a JSON bool. */
static inline tp_status j_opt_bool(const cJSON *obj, const char *key, bool *out, bool *present, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    *present = (it != NULL);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsBool(it)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"%s\" must be a boolean", key);
    }
    *out = cJSON_IsTrue(it) ? true : false;
    return TP_STATUS_OK;
}

/* Optional string field, duplicated. absent -> *out=NULL, OK. present -> must be a
 * string. Caller frees *out. */
static inline tp_status j_opt_dup(const cJSON *obj, const char *key, char **out, tp_error *err) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        *out = NULL;
        return TP_STATUS_OK;
    }
    if (!cJSON_IsString(it)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"%s\" must be a string", key);
    }
    size_t n = strlen(it->valuestring) + 1U;
    char *p = (char *)malloc(n);
    if (!p) {
        return tp_error_set(err, TP_STATUS_OOM, "string dup");
    }
    memcpy(p, it->valuestring, n);
    *out = p;
    return TP_STATUS_OK;
}

/* Optional shape-id field. absent OR malformed -> *out = nil (validate then rejects
 * the nil where a real id is required, and the shape pass reports a malformed id
 * separately). present-but-not-string -> bad-type status. */
static inline tp_status j_opt_shape_id(const cJSON *obj, const char *key, tp_id_kind expected, tp_id128 *out,
                                       tp_error *err) {
    *out = tp_id128_nil();
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!it) {
        return TP_STATUS_OK;
    }
    if (!cJSON_IsString(it)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "\"%s\" must be a string id", key);
    }
    tp_id_kind got = TP_ID_KIND_INVALID;
    tp_id128 id;
    if (tp_id_parse(it->valuestring, &got, &id, NULL) != TP_STATUS_OK || got != expected) {
        return TP_STATUS_OK; /* malformed/wrong-kind -> leave nil; shape/validate reports it */
    }
    *out = id;
    return TP_STATUS_OK;
}

#endif /* TP_CORE_SRC_TP_TXN_JSON_H */
