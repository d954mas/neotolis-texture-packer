#ifndef TP_CORE_TP_CLIENT_CAPABILITY_H
#define TP_CORE_TP_CLIENT_CAPABILITY_H

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Product-client shapes, not transports. FILE_CLI is deliberately one-shot;
 * LIVE_HEADLESS is the in-process session surface a later MCP/Dev API transport
 * may adapt without changing session semantics. */
typedef enum tp_client_kind {
    TP_CLIENT_FILE_CLI = 1,
    TP_CLIENT_GUI = 2,
    TP_CLIENT_LIVE_HEADLESS = 3
} tp_client_kind;

typedef enum tp_client_capability {
    TP_CLIENT_CAPABILITY_TRANSACTION = 1,
    TP_CLIENT_CAPABILITY_PERSISTENCE = 2,
    TP_CLIENT_CAPABILITY_EVENTS = 3,
    TP_CLIENT_CAPABILITY_HISTORY = 4,
    TP_CLIENT_CAPABILITY_RECOVERY = 5,
    TP_CLIENT_CAPABILITY_LIVE_JOBS = 6
} tp_client_capability;

typedef enum tp_client_capability_availability {
    TP_CLIENT_CAPABILITY_AVAILABLE = 1,
    TP_CLIENT_CAPABILITY_NOT_APPLICABLE = 2,
    TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED = 3
} tp_client_capability_availability;

typedef struct tp_client_capability_result {
    tp_client_kind client;
    tp_client_capability capability;
    tp_client_capability_availability availability;
} tp_client_capability_result;

/* Returns OK only when available. A valid but unsupported client/capability pair
 * returns TP_STATUS_UNSUPPORTED_CAPABILITY and still fills the typed result so a
 * machine client can distinguish not-applicable from not-yet-implemented. */
tp_status tp_client_capability_query(tp_client_kind client,
                                     tp_client_capability capability,
                                     tp_client_capability_result *out);

/* Stable machine token for the typed availability field. */
const char *tp_client_capability_availability_id(
    tp_client_capability_availability availability);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_CLIENT_CAPABILITY_H */
