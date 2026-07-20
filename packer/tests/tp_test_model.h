#ifndef TP_TEST_MODEL_H
#define TP_TEST_MODEL_H

/* Shared test-fixture helpers for the F2 operation/transaction/journal test
 * suites (test_diff.c, test_transaction.c, test_op_apply.c, test_operation.c,
 * test_journal.c). These were byte-identical copies re-defined per file; this
 * header collapses them to one definition each. Deliberately excluded (kept
 * local, NOT byte-identical): test_diff.c's make_base()/fresh() (a larger
 * 2-atlas + animation fixture) and test_session.c's deterministic_fill() (a
 * different +17U stream) and its helper set. test_journal.c's checkpoint-format
 * serialize() (tp_project_checkpoint_save_buffer) also stays local -- it is not
 * the same contract as tp_test_serialize_project() below. */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_project.h"
#include "tp_project_mutation_internal.h"
#include "tp_project_identity_internal.h"
#include "unity.h"

/* Deterministic distinct non-nil byte fill for tp_rng: byte j of call n is
 * (n + j + 1), counter increments per call. */
static inline int tp_test_det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}

/* A tp_id128 with every byte set to b (readable fixed test ids). */
static inline tp_id128 tp_test_id_of(uint8_t b) {
    tp_id128 x;
    for (int i = 0; i < 16; i++) {
        x.bytes[i] = b;
    }
    return x;
}

/* One default atlas + one folder source + one json-neotolis target, ids
 * promoted to real (addressable) values. */
static inline tp_project *tp_test_base_project(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    uint8_t ctr = 1;
    tp_rng rng = {tp_test_det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_assign_missing_ids(p, &rng, &err));
    return p;
}

/* tp_project_save_buffer (production save format), caller frees. */
static inline char *tp_test_serialize_project(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save_buffer(p, &buf, &len, &err));
    return buf;
}

#endif /* TP_TEST_MODEL_H */
