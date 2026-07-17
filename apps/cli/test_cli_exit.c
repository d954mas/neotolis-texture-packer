#include "cli_exit.h"

int main(void) {
    if (cli_exit_for_rejected_status(TP_STATUS_OOM) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_RNG_FAILED) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_DUPLICATE_ID) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_NOT_FOUND) != CLI_EXIT_PROJECT ||
        cli_exit_for_rejected_status(TP_STATUS_OUT_OF_BOUNDS) != CLI_EXIT_PROJECT ||
        cli_exit_for_rejected_status(TP_STATUS_INVALID_ARGUMENT) != CLI_EXIT_USAGE ||
        cli_exit_for_rejected_status(TP_STATUS_HASH_COLLISION) != CLI_EXIT_USAGE) {
        return 1;
    }
    return 0;
}
