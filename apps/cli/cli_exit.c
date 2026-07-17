#include "cli_exit.h"

int cli_exit_for_rejected_status(tp_status status) {
    switch (status) {
    case TP_STATUS_OOM:
    case TP_STATUS_RNG_FAILED:
    case TP_STATUS_DUPLICATE_ID:
        return CLI_EXIT_INTERNAL;
    case TP_STATUS_NOT_FOUND:
    case TP_STATUS_OUT_OF_BOUNDS:
        return CLI_EXIT_PROJECT;
    default:
        return CLI_EXIT_USAGE;
    }
}
