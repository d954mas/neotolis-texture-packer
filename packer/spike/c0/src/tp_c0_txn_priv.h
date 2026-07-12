#ifndef TP_C0_TXN_PRIV_H
#define TP_C0_TXN_PRIV_H

/*
 * C0-02 internal decode helpers -- NOT installed (src/). Shared by the request
 * parser (tp_c0_txn_parse.c, which defines them) and the result parser
 * (tp_c0_txn_result.c). cJSON is a PRIVATE dep of tp_c0; these signatures take
 * `const void *` for a cJSON node so this header does not leak cJSON onto the
 * include path (each .c casts back after including cJSON.h).
 */

#include <stdbool.h>
#include <stdint.h>

#include "tp_c0/tp_c0_txn.h"

bool tpc0_is_hex32_lower(const char *s);
bool tpc0_in_skip(const char *key, const char *const *skip, int skip_n);
void tpc0_free_val(tp_c0_val *v);
void tpc0_free_fields(tp_c0_field *f, int n);

/* `item`/`obj` are cJSON nodes. Decode one scalar/string-array value, or every
 * child key of an object except `skip` into a field list. */
tp_c0_detail tpc0_decode_val(const void *item, tp_c0_val *out, tp_error *err);
tp_c0_detail tpc0_decode_field_list(const void *obj, const char *const *skip, int skip_n, tp_c0_field *out, int *count,
                                    int cap, tp_error *err);

/* `item` is a cJSON node. Convert attacker JSON to an integer WITHOUT UB: a
 * non-number, a non-integral value, +/-inf, NaN, or a magnitude outside +/-2^53
 * yields txn_bad_type ("number out of range"), never a double->int cast overflow
 * (an UBSan abort in Debug CI). tpc0_json_to_int adds an INT_MIN..INT_MAX range
 * check for fields narrowed into `int`. */
tp_c0_detail tpc0_json_to_i64(const void *item, int64_t *out, tp_error *err);
tp_c0_detail tpc0_json_to_int(const void *item, int *out, tp_error *err);

#endif /* TP_C0_TXN_PRIV_H */
