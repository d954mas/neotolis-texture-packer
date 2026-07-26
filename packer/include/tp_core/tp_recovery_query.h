#ifndef TP_CORE_TP_RECOVERY_QUERY_H
#define TP_CORE_TP_RECOVERY_QUERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_RECOVERY_MAX_CANDIDATES 16
#define TP_RECOVERY_MAX_SCAN_DIAGNOSTICS 16

typedef struct tp_recovery_candidate {
    char journal_path[TP_IDENTITY_PATH_MAX];
    char original_path[TP_IDENTITY_PATH_MAX];
    char name[256];
    int64_t timestamp;
    tp_journal_recovery_status status;
    bool adoptable;
    tp_id128 file_fingerprint;
    bool has_file_fingerprint;
} tp_recovery_candidate;

typedef struct tp_recovery_scan_diagnostic {
    char journal_path[TP_IDENTITY_PATH_MAX];
    tp_status status;
} tp_recovery_scan_diagnostic;

typedef struct tp_recovery_candidates {
    tp_recovery_candidate items[TP_RECOVERY_MAX_CANDIDATES];
    size_t count;
    tp_recovery_scan_diagnostic diagnostics[TP_RECOVERY_MAX_SCAN_DIAGNOSTICS];
    size_t diagnostic_count;
    bool has_more;
} tp_recovery_candidates;

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_RECOVERY_QUERY_H */
