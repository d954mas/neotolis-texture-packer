#ifndef TP_CORE_SRC_TP_SRCKEY_INTERNAL_H
#define TP_CORE_SRC_TP_SRCKEY_INTERNAL_H

#include "tp_core/tp_srckey.h"

/* Core-internal full-size fold used by validation indexes. On success *out is
 * malloc-owned and must be freed by the caller. */
tp_status tp_srckey__casefold_owned(const char *normalized_key, char **out,
                                    tp_error *err);

#endif /* TP_CORE_SRC_TP_SRCKEY_INTERNAL_H */
