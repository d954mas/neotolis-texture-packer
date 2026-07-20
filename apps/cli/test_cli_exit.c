#include "cli_exit.h"

int main(void) {
    if (CLI_EXIT_OK != 0 || CLI_EXIT_INTERNAL != 1 || CLI_EXIT_USAGE != 2 ||
        CLI_EXIT_PROJECT != 3 || CLI_EXIT_PACK != 4 || CLI_EXIT_EXPORT != 5 ||
        CLI_EXIT_PARTIAL != 6 || CLI_EXIT_VALIDATE != 7 ||
        CLI_EXIT_FILE_IO != 8 ||
        cli_exit_for_rejected_status(TP_STATUS_OOM) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_RNG_FAILED) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_DUPLICATE_ID) != CLI_EXIT_INTERNAL ||
        cli_exit_for_rejected_status(TP_STATUS_NOT_FOUND) != CLI_EXIT_PROJECT ||
        cli_exit_for_rejected_status(TP_STATUS_OUT_OF_BOUNDS) != CLI_EXIT_PROJECT ||
        cli_exit_for_rejected_status(TP_STATUS_INVALID_ARGUMENT) != CLI_EXIT_USAGE ||
        cli_exit_for_rejected_status(TP_STATUS_HASH_COLLISION) != CLI_EXIT_USAGE ||
        cli_exit_for_save_status(TP_STATUS_FILE_IO_FAILED) != CLI_EXIT_FILE_IO ||
        cli_exit_for_save_status(TP_STATUS_OOM) != CLI_EXIT_INTERNAL ||
        cli_exit_for_save_status(TP_STATUS_BAD_PROJECT) != CLI_EXIT_PROJECT) {
        return 1;
    }
    return 0;
}
