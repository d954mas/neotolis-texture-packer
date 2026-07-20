#ifndef TP_VALIDATE_INTERNAL_H
#define TP_VALIDATE_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>

/* Test-only allocation seam for the owned findings vector. -1 disables the
 * seam; N fails the (N+1)th vector allocation once. */
void tp_validate__test_set_alloc_fail(int nth);
void tp_validate__test_fail_sprite_index(bool fail);

/* Deterministic work counter for the large validation gate. A probe is one
 * candidate-key/id inspection; the gate constrains algorithmic growth without
 * turning machine-dependent elapsed time into a flaky contract. */
typedef struct tp_validate_work_stats {
    size_t probes;
} tp_validate_work_stats;

void tp_validate__test_work_reset(void);
tp_validate_work_stats tp_validate__test_work_get(void);

#endif
