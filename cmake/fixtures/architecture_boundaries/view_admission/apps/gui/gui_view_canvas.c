#include "tp_core/tp_session_snapshot_query.h"

int64_t allowed_view_query(const tp_session_snapshot *snapshot) {
    return tp_session_snapshot_revision(snapshot);
}
