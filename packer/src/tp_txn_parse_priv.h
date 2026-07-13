#ifndef TP_CORE_SRC_TP_TXN_PARSE_PRIV_H
#define TP_CORE_SRC_TP_TXN_PARSE_PRIV_H

/* F2-02 private interface between the request decoder (tp_txn_parse.c) and the
 * JSON->tp_operation lowering (tp_txn_lower.c). Carries cJSON types, so it is a
 * src/-private header included ONLY by those two TUs (never by tp_txn_internal.h,
 * which tp_txn_apply.c includes -- cJSON stays out of the apply core). */

#include "cJSON.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_transaction.h"

/* Lower a shape-valid op JSON object into a typed tp_operation. PRE: `oj` passed
 * the shape pass (wire known, fields allowed). Fills *out (owns its strings; free
 * with tp_operation_free). A wrong-value-type / out-of-range fault returns the
 * structured status + `err`. */
tp_status tp_txn__lower_op(const cJSON *oj, tp_operation *out, tp_error *err);

#endif /* TP_CORE_SRC_TP_TXN_PARSE_PRIV_H */
