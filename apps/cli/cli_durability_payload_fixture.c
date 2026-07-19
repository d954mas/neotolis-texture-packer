#include "cli_out.h"

#include <string.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_session.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        return 2;
    }
    tp_session_save_result result = {0};
    result.file_durability_status = TP_STATUS_OK;
    result.recovery_status = TP_STATUS_OK;
    if (strcmp(argv[1], "file") == 0 || strcmp(argv[1], "both") == 0) {
        result.file_durability_degraded = true;
        result.file_durability_status = TP_STATUS_FILE_DURABILITY_UNCERTAIN;
    }
    if (strcmp(argv[1], "recovery") == 0 || strcmp(argv[1], "both") == 0) {
        result.recovery_degraded = true;
        result.recovery_status = TP_STATUS_JOURNAL_FAILED;
    }
    if (!result.file_durability_degraded && !result.recovery_degraded) {
        return 2;
    }
    (void)cli_emit_mutation("set", 1, &result);
    return 0;
}
