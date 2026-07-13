#ifndef TP_CORE_SRC_TP_IDSET_INTERNAL_H
#define TP_CORE_SRC_TP_IDSET_INTERNAL_H

/*
 * The one growable 32-hex idempotency id-set, shared by the in-memory idstore
 * (tp_txn_idset.c) and the recovery journal's retained-id index (tp_journal.c) so the
 * set logic lives in ONE place. The journal adds only a reserve/put split on top (it
 * must reserve a slot BEFORE the durable write and fill it alloc-free after), which is
 * why the primitive exposes both the combined add and the split reserve/put_reserved.
 */

#include <stdbool.h>

#include "tp_core/tp_error.h"

#define TP_IDSET_IDLEN 32 /* a transaction id is 32 lowercase-hex chars */

typedef struct tp_idset {
    char (*ids)[TP_IDSET_IDLEN + 1]; /* 32 hex + NUL */
    int count;
    int cap;
} tp_idset;

/* Membership test. NULL-safe. */
bool tp_idset_contains(const tp_idset *s, const char *id_hex);

/* Ensure one free slot exists (grows 16-then-double). OOM -> non-OK, set unchanged. */
tp_status tp_idset_reserve(tp_idset *s);

/* Fill the slot guaranteed by a prior tp_idset_reserve (allocation-free). */
void tp_idset_put_reserved(tp_idset *s, const char *id_hex);

/* contains? no-op : reserve+put. OOM -> non-OK, set unchanged (transactional). */
tp_status tp_idset_add(tp_idset *s, const char *id_hex);

/* Count of retained ids. NULL-safe (0). */
int tp_idset_count(const tp_idset *s);

/* The id at `index` (32-hex NUL-terminated), or NULL if out of range. */
const char *tp_idset_at(const tp_idset *s, int index);

/* Empty the set but keep the capacity (a checkpoint RESETS the retained set). */
void tp_idset_reset(tp_idset *s);

/* Free the backing array (does NOT free `s` itself). NULL-safe. */
void tp_idset_dispose(tp_idset *s);

#endif /* TP_CORE_SRC_TP_IDSET_INTERNAL_H */
