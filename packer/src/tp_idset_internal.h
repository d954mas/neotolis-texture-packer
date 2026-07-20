#ifndef TP_CORE_SRC_TP_IDSET_INTERNAL_H
#define TP_CORE_SRC_TP_IDSET_INTERNAL_H

/* Bounded applied-transaction retention shared by the model and journal. Keys
 * are stored once in binary form. A separate open-address index owns lookup;
 * `order` is the deterministic oldest-to-newest FIFO eviction order. */

#include <stdbool.h>
#include <stdint.h>

#include "tp_core/tp_transaction.h"

#define TP_IDSET_IDLEN 32
#define TP_IDSET_TABLE_CAP (TP_TXN_RETAINED_ID_CAP * 2)

typedef struct tp_idset {
    tp_id128 *order; /* fixed TP_TXN_RETAINED_ID_CAP ring */
    uint16_t *slots; /* fixed open-address table; ring index + 1, 0 = empty */
    int count;
    int head;
} tp_idset;

/* Allocate the fixed-capacity backing arrays once. Idempotent. */
tp_status tp_idset_reserve(tp_idset *s);

/* Lowercase 32-hex facade over binary open-address lookup. */
bool tp_idset_valid_hex(const char *id_hex);
bool tp_idset_contains(const tp_idset *s, const char *id_hex);

/* Insert into a slot guaranteed by reserve. Duplicate = no-op. When full,
 * evicts exactly the oldest key. No allocation and no failure. */
void tp_idset_put_reserved(tp_idset *s, const char *id_hex);

/* Validate + reserve + insert. Storage remains unchanged on validation/OOM. */
tp_status tp_idset_add(tp_idset *s, const char *id_hex);

int tp_idset_count(const tp_idset *s);

/* Chronological lookup: index 0 is the oldest retained key. */
bool tp_idset_at(const tp_idset *s, int index, tp_id128 *out);
bool tp_idset_format_at(const tp_idset *s, int index, char out[TP_IDSET_IDLEN + 1]);

void tp_idset_reset(tp_idset *s);
void tp_idset_dispose(tp_idset *s);

/* Test-only, thread-local collision/probe instrumentation. bucket < 0 restores
 * production hashing. reset starts counting slot inspections; take stops counting. */
void tp_idset__test_force_bucket(int bucket);
void tp_idset__test_probe_reset(void);
size_t tp_idset__test_probe_take(void);

#endif /* TP_CORE_SRC_TP_IDSET_INTERNAL_H */
