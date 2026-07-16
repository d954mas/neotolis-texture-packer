#include "tp_core/tp_client_capability.h"

tp_status tp_client_capability_query(tp_client_kind client,
                                     tp_client_capability capability,
                                     tp_client_capability_result *out) {
    if (!out || client < TP_CLIENT_FILE_CLI || client > TP_CLIENT_LIVE_HEADLESS ||
        capability < TP_CLIENT_CAPABILITY_TRANSACTION ||
        capability > TP_CLIENT_CAPABILITY_LIVE_JOBS) {
        return TP_STATUS_INVALID_ARGUMENT;
    }

    out->client = client;
    out->capability = capability;
    out->availability = TP_CLIENT_CAPABILITY_NOT_IMPLEMENTED;

    switch (client) {
        case TP_CLIENT_FILE_CLI:
            /* Ordinary CLI uses typed operations internally, but its public
             * shape is one-shot file commands: it exposes no live revision /
             * transaction-id admission contract. Persistence is the only
             * capability in this matrix that applies to that client shape. */
            out->availability = capability == TP_CLIENT_CAPABILITY_PERSISTENCE
                                    ? TP_CLIENT_CAPABILITY_AVAILABLE
                                    : TP_CLIENT_CAPABILITY_NOT_APPLICABLE;
            break;
        case TP_CLIENT_GUI:
            out->availability = TP_CLIENT_CAPABILITY_AVAILABLE;
            break;
        case TP_CLIENT_LIVE_HEADLESS:
            out->availability = TP_CLIENT_CAPABILITY_AVAILABLE;
            break;
    }

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
