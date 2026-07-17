#ifndef TP_CORE_SRC_TP_RECOVERY_LIVE_SEAM_H
#define TP_CORE_SRC_TP_RECOVERY_LIVE_SEAM_H

#include "tp_core/tp_recovery.h"
#include "tp_core/tp_transaction.h"

/* Recovery-live lifecycle contract session consumes. Opaque to session: the
 * struct layout stays private to recovery core. */
typedef struct tp_recovery_live tp_recovery_live;

tp_status tp_recovery_live_attach(tp_recovery_live *live, tp_model *model,
                                  const tp_recovery_metadata *metadata,
                                  tp_error *err);
bool tp_recovery_live_healthy(const tp_recovery_live *live);
const char *tp_recovery_live_journal_path(const tp_recovery_live *live);
tp_status tp_recovery_live_finish(tp_recovery_live *live,
                                  bool preserve_journal, tp_error *err);
/* Retire a stale identity slot even after journal health was lost. */
tp_status tp_recovery_live_retire(tp_recovery_live *live, tp_error *err);
void tp_recovery_live_destroy(tp_recovery_live *live);

/* Session-side ACK/compaction failure transition. It preserves the attached
 * journal and makes the live owner reject further durability claims. */
void tp_recovery_live__mark_degraded(tp_recovery_live *live);

/* Session-side save publication: persist the newly authoritative project path
 * and fingerprint while retaining the host-supplied timestamp/name. */
tp_status tp_recovery_live__update_saved_identity(
    tp_recovery_live *live, const char *canonical_path,
    const tp_id128 *file_fingerprint, tp_error *err);

#endif /* TP_CORE_SRC_TP_RECOVERY_LIVE_SEAM_H */
