#ifndef NTPACKER_COMMON_ID_FMT_H
#define NTPACKER_COMMON_ID_FMT_H

/* One shared CLI helper: format a structural shape-ID into `out` (cap must be
 * >= TP_ID_TEXT_CAP), leaving an EMPTY string if formatting fails. This is the
 * single home for the "format-or-empty" snippet the inspect and mutate frontends
 * both emit for --json shape-IDs -- previously triplicated inline. (tp_core keeps
 * its own tp_emit_id in the serializer: a different layer, left alone.) */

#include <stddef.h>

#include "tp_core/tp_id.h"

static inline void ntpacker_fmt_shape_id(char *out, size_t cap, tp_id_kind kind, tp_id128 id) {
    if (tp_id_format(kind, id, out, cap, NULL) != TP_STATUS_OK) {
        out[0] = '\0';
    }
}

#endif /* NTPACKER_COMMON_ID_FMT_H */
