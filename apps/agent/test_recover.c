/* Test-only bridge to the existing recovery flow. No product recovery CLI. */
#include <stdio.h>
#include "app_paths.h"
#include "tp_core/tp_recovery.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    tp_error err = {0};
    tp_recovery_candidates candidates = {0};
    tp_status status = tp_recovery_scan_root(argv[1], app_recovery_key(), NULL, &candidates, &err);
    if (status == TP_STATUS_OK && candidates.count != 1U) {
        (void)fprintf(stderr, "Expected one recoverable journal; got %zu\n", candidates.count);
        return 1;
    }
    if (status == TP_STATUS_OK) {
        tp_rng rng = tp_rng_os();
        tp_recovery_resolve_result result = {0};
        status = tp_recovery_resolve_journal(argv[1], app_recovery_key(),
            candidates.items[0].journal_path, NULL, TP_RECOVERY_ACTION_SAVE_AS,
            argv[2], &rng, &result, &err);
        if (status == TP_STATUS_OK && !result.project_saved) return 1;
    }
    if (status != TP_STATUS_OK) (void)fprintf(stderr, "%s: %s\n", tp_status_id(status), err.msg);
    return status == TP_STATUS_OK ? 0 : 1;
}
