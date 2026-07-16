#include "tp_core/tp_client_capability.h"

static const tp_client_capability_availability s_capabilities[3][9] = {
    /* One-shot file CLI: Inspect/Validate/Pack remain synchronous file
     * commands; Export is an external side-effect command. */
    {TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
     TP_CLIENT_CAPABILITY_NOT_APPLICABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_NOT_APPLICABLE,
     TP_CLIENT_CAPABILITY_NOT_APPLICABLE},
    /* Native GUI live host. */
    {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED,
     TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED},
    /* In-process headless live host, before transport work. */
    {TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_AVAILABLE,
     TP_CLIENT_CAPABILITY_AVAILABLE, TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED,
     TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED},
};

tp_status tp_client_capability_query(tp_client_kind client,
                                     tp_client_capability capability,
                                     tp_client_capability_result *out) {
    if (!out || client < TP_CLIENT_FILE_CLI || client > TP_CLIENT_LIVE_HEADLESS ||
        capability < TP_CLIENT_CAPABILITY_TRANSACTION ||
        capability > TP_CLIENT_CAPABILITY_VALIDATE_ASYNC_JOB) {
        return TP_STATUS_INVALID_ARGUMENT;
    }

    out->client = client;
    out->capability = capability;
    out->availability =
        s_capabilities[client - TP_CLIENT_FILE_CLI]
                      [capability - TP_CLIENT_CAPABILITY_TRANSACTION];

    return out->availability == TP_CLIENT_CAPABILITY_AVAILABLE
               ? TP_STATUS_OK
               : TP_STATUS_UNSUPPORTED_CAPABILITY;
}

const char *tp_client_capability_availability_id(
    tp_client_capability_availability availability) {
    switch (availability) {
        case TP_CLIENT_CAPABILITY_AVAILABLE:
            return "available";
        case TP_CLIENT_CAPABILITY_NOT_APPLICABLE:
            return "not_applicable";
        case TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED:
            return "not_implemented";
    }
    return "unknown_availability";
}
