/* F2-04 recovery journal fault suite (master spec §7.1-7.2, §22.3). Proves the
 * completion contract: any ACKNOWLEDGED (durably appended) transaction is recoverable
 * and never duplicated after restart; any UNACKNOWLEDGED (torn/failed) transaction is
 * invisible; and the reader is UB-clean on arbitrary/corrupt/short/torn bytes.
 *
 *   - byte-identity: attaching a journal does NOT change project serialization (sidecar);
 *   - append fail after model apply -> exact rollback, no acknowledgement, live model
 *     byte-unchanged, same txn id retryable, then recoverable exactly once;
 *   - append OOM (retained-id reserve) -> nothing durable, id retryable;
 *   - checkpoint + journal replay -> committed state + retained tx-id set fully restored;
 *   - duplicate retry after restart -> acknowledged id de-duplicated (idempotency §7.2);
 *   - short write at EVERY byte boundary -> recover the complete-record prefix, ignore the
 *     partial tail, never UB / never crash;
 *   - torn tail (payload truncated) -> invisible, no dup on retry;
 *   - checksum mismatch (flipped byte) -> corruption boundary, safe fallback to prior;
 *   - stale journal for a moved project -> detected via the key, not misapplied;
 *   - arbitrary/garbage bytes + an absurd length prefix -> structured status, no huge alloc;
 *   - side-effect coordinator prepare/publish/abort fire at the right commit points;
 *   - a real on-disk file journal round-trips (honest durability).
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_diff.h"   /* R4: tp_model_enable_history / tp_model_undo / tp_model_redo */
#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h"    /* M3 bounded-history test seam + byte accounting */
#include "tp_idset_internal.h"   /* M3 collision/probe bound for retained ids */
#include "tp_journal_internal.h" /* format constants + memory-io fault seams */
#include "tp_model_seam.h"
#include "tp_op_internal.h"      /* exact recovery replay operation count */
#include "tp_project_internal.h" /* checkpoint materialization counters */
#include "tp_project_mutation_internal.h"
#include "tp_txn_internal.h"     /* clone fault seam + replay count preflight */
#include "tp_test_model.h"
#include "unity.h"

static const char *g_dir = NULL; /* scratch dir for the on-disk file journal test */

void setUp(void) {
    tp_journal__test_set_record_limit(0U);
    tp_journal__test_set_file_limit(0U);
    tp_project__test_serialization_stats_reset();
}
void tearDown(void) {
    tp_journal__test_set_record_limit(0U);
    tp_journal__test_set_file_limit(0U);
    tp_project__test_serialization_stats_reset();
    tp_history__test_set_limits(0, 0U, 0U);
    tp_idset__test_force_bucket(-1);
    (void)tp_idset__test_probe_take();
    (void)tp_op__test_apply_count_take();
}

static size_t checkpoint_store_bytes(size_t store_base, size_t snapshot_bytes,
                                     int retained_ids) {
    return store_base + (size_t)TP_JRN_SYNC_FIELD +
           (size_t)TP_JRN_LEN_FIELD + (size_t)TP_JRN_CKPT_FIXED +
           (size_t)retained_ids * (size_t)TP_JRN_IDLEN + snapshot_bytes +
           (size_t)TP_JRN_CRC_FIELD;
}

/* ---- fixtures -------------------------------------------------------------
 * det_fill / base_project / key_of: shared byte-identical fixtures, see
 * tp_test_model.h (tp_test_det_fill / tp_test_base_project / tp_test_id_of;
 * key_of here was a same-body twin of id_of elsewhere). serialize() below
 * stays LOCAL: it uses tp_project_checkpoint_save_buffer (checkpoint format),
 * not the tp_test_serialize_project() save_buffer contract. */

static char *serialize(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_checkpoint_save_buffer(p, &buf, &len, &err));
    return buf;
}

static void retention_id(unsigned value, char out[33]) {
    (void)snprintf(out, 33U, "%032x", value);
}

static uint8_t *snapshot_io(tp_journal_io io, size_t *len);
static tp_journal_io io_from_bytes(const uint8_t *bytes, size_t len);
static size_t record_offset(const uint8_t *buf, size_t len, int idx);

static bool span_contains(const char *span, size_t span_len, const char *text) {
    const size_t text_len = strlen(text);
    if (!span || text_len > span_len) {
        return false;
    }
    for (size_t i = 0U; i <= span_len - text_len; ++i) {
        if (memcmp(span + i, text, text_len) == 0) {
            return true;
        }
    }
    return false;
}

void test_idset_backward_shift_eviction_has_linear_probe_bound(void) {
    tp_idset ids = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_idset_reserve(&ids));
    tp_idset__test_force_bucket(TP_IDSET_TABLE_CAP - 17); /* cluster wraps index 0 */
    char id[TP_IDSET_IDLEN + 1];
    for (unsigned i = 0U; i < (unsigned)TP_TXN_RETAINED_ID_CAP; ++i) {
        retention_id(i + 1U, id);
        tp_idset_put_reserved(&ids, id);
    }
    TEST_ASSERT_EQUAL_INT(TP_TXN_RETAINED_ID_CAP, tp_idset_count(&ids));

    char newest[TP_IDSET_IDLEN + 1];
    retention_id((unsigned)TP_TXN_RETAINED_ID_CAP + 1U, newest);
    tp_idset__test_probe_reset();
    tp_idset_put_reserved(&ids, newest); /* full-window FIFO eviction */
    TEST_ASSERT_TRUE(tp_idset_contains(&ids, newest));
    const size_t probes = tp_idset__test_probe_take();
    TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)TP_IDSET_TABLE_CAP * 4U, probes);

    char oldest[TP_IDSET_IDLEN + 1];
    char successor[TP_IDSET_IDLEN + 1];
    char at[TP_IDSET_IDLEN + 1];
    retention_id(1U, oldest);
    retention_id(2U, successor);
    TEST_ASSERT_FALSE(tp_idset_contains(&ids, oldest));
    TEST_ASSERT_TRUE(tp_idset_contains(&ids, successor));
    TEST_ASSERT_TRUE(tp_idset_format_at(&ids, 0, at));
    TEST_ASSERT_EQUAL_STRING(successor, at);
    TEST_ASSERT_TRUE(tp_idset_format_at(&ids, TP_TXN_RETAINED_ID_CAP - 1, at));
    TEST_ASSERT_EQUAL_STRING(newest, at);
    tp_idset_dispose(&ids);
}

void test_retained_id_eviction_is_durable_and_post_append(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x7c));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));

    char id[33];
    for (unsigned i = 0U; i < (unsigned)TP_TXN_RETAINED_ID_CAP; ++i) {
        retention_id(i + 1U, id);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_journal_append_txn(journal, id, (int64_t)i + 1, NULL, 0U, &err));
    }
    TEST_ASSERT_EQUAL_INT(TP_TXN_RETAINED_ID_CAP, tp_journal_id_count(journal));

    char oldest[33];
    char successor[33];
    char newest[33];
    retention_id(1U, oldest);
    retention_id(2U, successor);
    retention_id((unsigned)TP_TXN_RETAINED_ID_CAP + 1U, newest);

    tp_journal_io_memory__fail_next_writes(io, 1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_journal_append_txn(journal, newest, TP_TXN_RETAINED_ID_CAP + 1,
                                                NULL, 0U, &err));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, oldest));
    TEST_ASSERT_FALSE(tp_journal_contains(journal, newest));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal, newest, TP_TXN_RETAINED_ID_CAP + 1,
                                                NULL, 0U, &err));
    TEST_ASSERT_EQUAL_INT(TP_TXN_RETAINED_ID_CAP, tp_journal_id_count(journal));
    TEST_ASSERT_FALSE(tp_journal_contains(journal, oldest));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, successor));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, newest));

    /* Rebuild from durable record order and prove the same deterministic window. */
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_EQUAL_INT(TP_TXN_RETAINED_ID_CAP, tp_journal_id_count(journal));
    TEST_ASSERT_FALSE(tp_journal_contains(journal, oldest));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, successor));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, newest));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_recovery_retained_ids_publish_only_after_record_acceptance(void) {
    tp_id128 key = tp_test_id_of(0x7e);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    static const uint8_t snapshot[] = {'{', '}'};
    const char *id_a = "7e000000000000000000000000000001";
    const char *id_b = "7e000000000000000000000000000002";
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, snapshot, sizeof snapshot, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_append_txn(journal, id_a, 1, NULL, 0U, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_append_txn(journal, id_b, 2, NULL, 0U, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, snapshot, sizeof snapshot, 3, &err));

    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);
    const size_t bad_record = record_offset(bytes, bytes_len, 3);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, bad_record);
    const size_t payload = bad_record + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD;
    const uint32_t payload_len = tp_jrn_get_u32(bytes + bad_record + (size_t)TP_JRN_SYNC_FIELD);
    const size_t second_id = payload + (size_t)TP_JRN_CKPT_FIXED + (size_t)TP_JRN_IDLEN;
    bytes[second_id] = 'z'; /* semantic corruption, then repair the frame CRC */
    const size_t crc_span = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + (size_t)payload_len;
    tp_jrn_put_u32(bytes + bad_record + crc_span, tp_jrn_crc32(0, bytes + bad_record, crc_span));

    journal = tp_journal_create(io_from_bytes(bytes, bytes_len), key);
    free(bytes);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, recovery.status);
    TEST_ASSERT_TRUE(tp_journal_contains(journal, id_a));
    TEST_ASSERT_TRUE(tp_journal_contains(journal, id_b));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_journal_rejects_null_positive_payload_before_mutation(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x7f));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};

    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_journal_init_checkpoint(journal, NULL, 1U, 0, &err));
    TEST_ASSERT_EQUAL_INT64(0, io.length(io.ctx));

    static const uint8_t snapshot[] = {'o', 'k'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, snapshot, sizeof snapshot, 0, &err));
    const int64_t before = io.length(io.ctx);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_journal_append_txn(journal, "7f000000000000000000000000000001",
                                                1, NULL, 1U, &err));
    TEST_ASSERT_EQUAL_INT64(before, io.length(io.ctx));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_journal_compact(journal, NULL, 1U, 0, &err));
    TEST_ASSERT_EQUAL_INT64(before, io.length(io.ctx));
    tp_journal_destroy(journal);
}

void test_checkpoint_retained_id_count_limit_is_prepublication(void) {
    const uint32_t id_count = (uint32_t)TP_TXN_RETAINED_ID_CAP + 1U;
    const size_t payload_len = (size_t)TP_JRN_CKPT_FIXED + (size_t)id_count * (size_t)TP_JRN_IDLEN;
    const size_t record_len = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD +
                              payload_len + (size_t)TP_JRN_CRC_FIELD;
    const size_t total_len = (size_t)TP_JRN_HEADER_LEN + record_len;
    uint8_t *bytes = (uint8_t *)calloc(1U, total_len);
    TEST_ASSERT_NOT_NULL(bytes);
    const tp_id128 key = tp_test_id_of(0x6d);
    memcpy(bytes, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(bytes + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(bytes + TP_JRN_KEY_OFF, key.bytes, 16U);

    uint8_t *record = bytes + TP_JRN_HEADER_LEN;
    tp_jrn_put_u32(record, (uint32_t)TP_JRN_SYNC_WORD);
    tp_jrn_put_u32(record + TP_JRN_SYNC_FIELD, (uint32_t)payload_len);
    uint8_t *payload = record + TP_JRN_SYNC_FIELD + TP_JRN_LEN_FIELD;
    payload[0] = (uint8_t)TP_JRN_REC_CKPT;
    tp_jrn_put_i64(payload + 1, 0);
    tp_jrn_put_u32(payload + 9, id_count);
    for (uint32_t i = 0U; i < id_count; ++i) {
        char id[TP_JRN_IDLEN + 1];
        (void)snprintf(id, sizeof id, "%032x", i + 1U);
        memcpy(payload + TP_JRN_CKPT_FIXED + (size_t)i * TP_JRN_IDLEN, id, TP_JRN_IDLEN);
    }
    const size_t crc_span = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + payload_len;
    tp_jrn_put_u32(record + crc_span, tp_jrn_crc32(0, record, crc_span));

    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_peek(io_from_bytes(bytes, total_len), &peek, &err));

    tp_journal *journal = tp_journal_create(io_from_bytes(bytes, total_len), key);
    free(bytes);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, recovery.status);
    TEST_ASSERT_EQUAL_INT(recovery.status, peek.status);
    TEST_ASSERT_EQUAL_INT(recovery.records_recovered, peek.record_count);
    TEST_ASSERT_EQUAL_INT(0, tp_journal_id_count(journal));
    tp_journal_peek_free(&peek);
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_replay_window_limit_is_prewrite_and_checkpoint_resets_it(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x7d));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));

    char id[33];
    for (unsigned i = 0U; i < (unsigned)TP_JOURNAL_MAX_REPLAY_RECORDS; ++i) {
        retention_id(i + 1U, id);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_journal_append_txn(journal, id, (int64_t)i + 1,
                                                    NULL, 0U, &err));
    }
    /* Metadata is outside the replay window and must not reset its count. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(journal, 123, "", "window-full", &err));
    const int64_t full_length = io.length(io.ctx);
    TEST_ASSERT_GREATER_THAN_INT64(0, full_length);
    tp_journal_recovery at_limit;
    memset(&at_limit, 0, sizeof at_limit);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(journal, &at_limit, &err));
    TEST_ASSERT_EQUAL_UINT64(TP_JOURNAL_MAX_REPLAY_RECORDS, at_limit.op_count);
    TEST_ASSERT_TRUE(tp_journal__test_recovery_ops_borrow_raw(&at_limit));
    tp_journal_recovery_free(&at_limit);
    retention_id((unsigned)TP_JOURNAL_MAX_REPLAY_RECORDS + 1U, id);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_journal_append_txn(journal, id,
                                                (int64_t)TP_JOURNAL_MAX_REPLAY_RECORDS + 1,
                                                NULL, 0U, &err));
    TEST_ASSERT_EQUAL_INT64(full_length, io.length(io.ctx));
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "replay-window"));

    /* A retained retry remains an idempotent no-op even when the window is full. */
    retention_id((unsigned)TP_JOURNAL_MAX_REPLAY_RECORDS, id);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal, id,
                                                (int64_t)TP_JOURNAL_MAX_REPLAY_RECORDS,
                                                NULL, 0U, &err));
    TEST_ASSERT_EQUAL_INT64(full_length, io.length(io.ctx));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U,
                                                     (int64_t)TP_JOURNAL_MAX_REPLAY_RECORDS,
                                                     &err));
    retention_id((unsigned)TP_JOURNAL_MAX_REPLAY_RECORDS + 1U, id);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal, id,
                                                (int64_t)TP_JOURNAL_MAX_REPLAY_RECORDS + 1,
                                                NULL, 0U, &err));
    tp_journal_destroy(journal);
}

/* Apply one atlas.rename as a whole transaction; returns the apply status. */
static tp_status commit_rename(tp_model *m, const char *id_hex, int64_t expected_rev, const char *name) {
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_model_project(m)->atlases[0].id;
    op.u.atlas_rename.name = (char *)name;
    tp_txn_request req;
    memset(&req, 0, sizeof req);
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", id_hex);
    req.expected_revision = expected_rev;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    memset(&res, 0, sizeof res);
    tp_error err;
    tp_status st = tp_model_apply(m, &req, &res, &err);
    tp_txn_result_free(&res);
    return st;
}

static char *dense_rename_payload(tp_id128 atlas_id) {
    tp_operation *operations =
        (tp_operation *)calloc((size_t)TP_TXN_MAX_OPS, sizeof *operations);
    if (!operations) {
        return NULL;
    }
    for (int i = 0; i < TP_TXN_MAX_OPS; ++i) {
        operations[i].kind = TP_OP_ATLAS_RENAME;
        operations[i].atlas_id = atlas_id;
        operations[i].u.atlas_rename.name = (char *)((i & 1) == 0 ? "a" : "b");
    }
    tp_txn_request request;
    memset(&request, 0, sizeof request);
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   "d1000000000000000000000000000001");
    request.expected_revision = 0;
    request.ops = operations;
    request.op_count = TP_TXN_MAX_OPS;
    char *payload = tp_txn_request_encode(&request);
    free(operations); /* operation strings are borrowed literals */
    return payload;
}

static char *dense_rename_payload_with_duplicate_operations(tp_id128 atlas_id) {
    char *canonical = dense_rename_payload(atlas_id);
    if (!canonical) {
        return NULL;
    }
    char *array_end = strrchr(canonical, ']');
    static const char duplicate[] = ",\"operations\":[]";
    if (!array_end) {
        free(canonical);
        return NULL;
    }
    const size_t prefix = (size_t)(array_end - canonical) + 1U;
    const size_t suffix = strlen(array_end + 1);
    char *payload = (char *)malloc(prefix + sizeof duplicate - 1U + suffix + 1U);
    if (payload) {
        memcpy(payload, canonical, prefix);
        memcpy(payload + prefix, duplicate, sizeof duplicate - 1U);
        memcpy(payload + prefix + sizeof duplicate - 1U, array_end + 1,
               suffix + 1U);
    }
    free(canonical);
    return payload;
}

/* Commit a PARTIAL target.set (only out_path) on atlas[0]'s first target: exercises a mask-carrying op
 * through the journaled commit path. Format B serializes it via tp_txn_request_encode and replays it on
 * recovery, so the partial mask must survive that round-trip (R2a) -- not re-expand to full-replace. */
static tp_status commit_target_out_path(tp_model *m, const char *id_hex, int64_t expected_rev, const char *out_path) {
    tp_project *p = tp_model_project(m);
    tp_operation op;
    memset(&op, 0, sizeof op);
    op.kind = TP_OP_TARGET_SET;
    op.atlas_id = p->atlases[0].id;
    op.u.target_set.target_id = p->atlases[0].targets[0].id;
    op.u.target_set.mask = TP_TF_OUT_PATH;
    op.u.target_set.out_path = (char *)out_path;
    tp_txn_request req;
    memset(&req, 0, sizeof req);
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", id_hex);
    req.expected_revision = expected_rev;
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result res;
    memset(&res, 0, sizeof res);
    tp_error err;
    tp_status st = tp_model_apply(m, &req, &res, &err);
    tp_txn_result_free(&res);
    return st;
}

/* A model over base_project with a fresh in-memory journal keyed by `key`. The io
 * handle (shared ctx) is returned via *io_out so a test can snapshot the durable
 * bytes before destroying the model. */
static tp_model *model_with_journal(tp_id128 key, tp_journal_io *io_out) {
    tp_project *p = tp_test_base_project();
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_attach_journal(m, j, &err));
    *io_out = io;
    return m;
}

/* Snapshot a memory io's durable bytes into a fresh malloc'd buffer (may be NULL). */
static uint8_t *snapshot_io(tp_journal_io io, size_t *len) {
    uint8_t *b = NULL;
    TEST_ASSERT_EQUAL_INT(0, io.read_all(io.ctx, SIZE_MAX, &b, len));
    return b;
}

/* A fresh memory io pre-loaded with `bytes[0..len)` -- simulates the same durable
 * store after a process restart. */
static tp_journal_io io_from_bytes(const uint8_t *bytes, size_t len) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    if (len > 0) {
        TEST_ASSERT_EQUAL_INT64((int64_t)len, io.write(io.ctx, bytes, len));
    }
    return io;
}

/* ---- shared round-trip tail: recover/peek a snapshot and assert the outcome ---------------- */

/* Snapshot io's durable bytes and destroy the model that owned them -- the shared first half of
 * every "simulate a restart" round trip: capture what's durable, then drop the live model so the
 * next step proves it reconstructs state purely from the captured bytes. */
static uint8_t *snapshot_and_destroy(tp_journal_io io, tp_model *m, size_t *len) {
    uint8_t *bytes = snapshot_io(io, len);
    tp_model_destroy(m);
    return bytes;
}

/* Recover a model from `io` under `key`, asserting the API call itself returns `expected` (usually
 * TP_STATUS_OK -- the recovery MECHANISM succeeded, independent of the recovered OUTCOME reported
 * in *info->status, which the caller inspects separately for TRUNCATED/CORRUPT/STALE_KEY/etc).
 * Returns the recovered model (NULL for a rejected or stale recovery); the caller still asserts
 * NULL vs NOT_NULL and any info-specific fields itself. */
static tp_model *recover_expect(tp_journal_io io, tp_id128 key, tp_status expected,
                                tp_journal_recovery *info, tp_error *err) {
    tp_model *m = NULL;
    memset(info, 0, sizeof *info);
    TEST_ASSERT_EQUAL_INT(expected, tp_model_recover(io, key, &m, info, err));
    return m;
}

/* Peek `bytes` and assert it carries the expected cached recovery metadata (path + name), then
 * free the peek result. Does not free `bytes` -- ownership stays with the caller. */
static void assert_peek_meta(const uint8_t *bytes, size_t len, const char *path, const char *name) {
    tp_journal_peek_result peek = {0};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, len), &peek, &err));
    TEST_ASSERT_TRUE(peek.has_meta);
    TEST_ASSERT_EQUAL_STRING(path, peek.meta.path);
    TEST_ASSERT_EQUAL_STRING(name, peek.meta.name);
    tp_journal_peek_free(&peek);
}

void test_total_record_limit_bounds_all_frame_types_before_write_and_recovery(void) {
    tp_journal__test_set_record_limit(3U);
    const tp_id128 key = tp_test_id_of(0x79);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(journal, 1, "", "one", &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_append_txn(journal, "79000000000000000000000000000001",
                              1, NULL, 0U, &err));
    const int64_t at_limit = io.length(io.ctx);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_journal_set_metadata(journal, 2, "", "must-not-write", &err));
    TEST_ASSERT_EQUAL_INT64(at_limit, io.length(io.ctx));
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "total record limit"));

    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    tp_journal__test_set_record_limit(2U);
    journal = tp_journal_create(io_from_bytes(bytes, bytes_len), key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery = {0};
    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_EQUAL_size_t(0U, recovery.op_count);
    TEST_ASSERT_NULL(recovery._raw_record_buffer);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "total record limit"));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
    free(bytes);
}

/* A model commit must reject an exhausted total-frame slot before canonical encoding or project
 * cloning. The clone allocation counter is the observable staging boundary: zero means no mutable
 * candidate was built. */
void test_total_record_admission_precedes_commit_staging(void) {
    tp_journal_io model_io;
    tp_model *model = model_with_journal(tp_test_id_of(0x78), &model_io);
    tp_journal__test_set_record_limit(1U); /* the attach checkpoint owns the only slot */
    tp_project__test_set_clone_alloc_fail(-1);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        commit_rename(model, "78000000000000000000000000000001", 0,
                      "must-not-stage"));
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_clone_allocation_bytes());
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(model));
    tp_model_destroy(model);
}

/* Same total-frame-slot boundary, but for undo's staging clone rather than a commit's. */
void test_total_record_admission_precedes_undo_staging(void) {
    tp_error err = {0};
    tp_journal_io model_io;
    tp_journal__test_set_record_limit(2U);
    tp_model *model = model_with_journal(tp_test_id_of(0x76), &model_io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "76000000000000000000000000000001", 0,
                      "undo-candidate"));
    tp_project__test_set_clone_alloc_fail(-1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS, tp_model_undo(model, &err));
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_clone_allocation_bytes());
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(model));
    tp_model_destroy(model);
}

/* Rejected metadata must not replace the write-side cache. After restoring the limit, compaction
 * re-emits the last durable metadata ("before"). */
void test_total_record_admission_rejected_metadata_preserves_cache_for_compaction(void) {
    tp_error err = {0};
    tp_journal__test_set_record_limit(2U);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    const tp_id128 key = tp_test_id_of(0x77);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(journal, 1, "/before", "before", &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_journal_set_metadata(journal, 2, "/after", "must-not-cache", &err));
    tp_journal__test_set_record_limit(0U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_compact(journal, NULL, 0U, 0, &err));
    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    assert_peek_meta(bytes, bytes_len, "/before", "before");
    free(bytes);
}

void test_file_byte_admission_precedes_transaction_staging_and_metadata_cache_mutation(void) {
    tp_error err = {0};

    tp_journal_io model_io;
    tp_model *model = model_with_journal(tp_test_id_of(0x75), &model_io);
    const int64_t checkpoint_bytes = model_io.length(model_io.ctx);
    TEST_ASSERT_GREATER_THAN_INT64(0, checkpoint_bytes);

    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_model_project(model)->atlases[0].id;
    op.u.atlas_rename.name = "must-not-stage";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s",
                   "75000000000000000000000000000001");
    req.expected_revision = 0;
    req.ops = &op;
    req.op_count = 1;
    char *canonical = tp_txn_request_encode(&req);
    TEST_ASSERT_NOT_NULL(canonical);
    const size_t request_bytes = strlen(canonical);
    free(canonical);
    const size_t frame_bytes = (size_t)TP_JRN_SYNC_FIELD +
                               (size_t)TP_JRN_LEN_FIELD +
                               (size_t)TP_JRN_TXN_FIXED + request_bytes +
                               (size_t)TP_JRN_CRC_FIELD;

    /* One byte short of the exact canonical frame: reject after an exact
     * count-only pass, before the allocating encoder or any model staging. */
    tp_journal__test_set_file_limit((size_t)checkpoint_bytes + frame_bytes - 1U);
    tp_project__test_set_clone_alloc_fail(-1);
    tp_txn__test_encode_stats_reset();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_JOURNAL_FAILED,
        commit_rename(model, "75000000000000000000000000000001", 0,
                      "must-not-stage"));
    TEST_ASSERT_EQUAL_size_t(0U, tp_txn__test_last_measure_allocations());
    TEST_ASSERT_EQUAL_size_t(0U, tp_txn__test_request_encode_calls());
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_clone_allocation_bytes());
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(model));

    /* The exact boundary is admitted, proving the preflight is not a
     * conservative estimate that rejects a valid transaction. */
    tp_journal__test_set_file_limit((size_t)checkpoint_bytes + frame_bytes);
    tp_txn__test_encode_stats_reset();
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "75000000000000000000000000000001", 0,
                      "must-not-stage"));
    TEST_ASSERT_EQUAL_size_t(0U, tp_txn__test_last_measure_allocations());
    TEST_ASSERT_EQUAL_size_t(1U, tp_txn__test_request_encode_calls());
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(model));
    tp_model_destroy(model);

    tp_journal__test_set_file_limit(0U);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    const tp_id128 key = tp_test_id_of(0x74);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(journal, 1, "/before", "before", &err));
    const int64_t metadata_bytes = io.length(io.ctx);
    TEST_ASSERT_GREATER_THAN_INT64(0, metadata_bytes);
    tp_journal__test_set_file_limit((size_t)metadata_bytes + 1U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_JOURNAL_FAILED,
        tp_journal_set_metadata(journal, 2, "/after", "must-not-cache", &err));

    tp_journal__test_set_file_limit(0U);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_compact(journal, NULL, 0U, 0, &err));
    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    assert_peek_meta(bytes, bytes_len, "/before", "before");
    free(bytes);
}

/* ---- shared body: checkpoint-materialization byte-admission boundary --------------------------
 * 3 of the admission-cluster tests (attach / compact / undo+redo) share one shape: `exact - 1`
 * bytes must fail closed before any checkpoint I/O; exactly `exact` bytes must admit it and do the
 * I/O once. They differ only in frame-type (attach's checkpoint frame is the FIRST one written vs
 * compact/undo/redo's Nth), materialize-fn, and counter-set (undo/redo always clone the pending
 * candidate BEFORE the byte check, so their reject branch shows clone work instead of none). The
 * other cluster tests (record-count limits, a single pathological length, tp_journal_compact's own
 * file+record limit interplay, a txn-frame boundary) differ structurally and are left inline. */

typedef tp_status (*materialize_fn)(tp_model *, tp_journal *, tp_error *);

static tp_status materialize_compact(tp_model *m, tp_journal *j, tp_error *err) {
    (void)j;
    return tp_model_compact_journal(m, err);
}
static tp_status materialize_undo(tp_model *m, tp_journal *j, tp_error *err) {
    (void)j;
    return tp_model_undo(m, err);
}
static tp_status materialize_redo(tp_model *m, tp_journal *j, tp_error *err) {
    (void)j;
    return tp_model_redo(m, err);
}

/* Reject branch: `exact - 1` bytes must fail closed before any checkpoint I/O (save/serialize/load
 * counters all stay zero). `clone_always_happens` (undo/redo) selects the branch where the pending
 * candidate is already cloned by the time the byte budget is checked, so clone work and one
 * size-query call are expected instead of none; `revision_at_reject` is the model revision expected
 * while still rejected (ignored unless clone_always_happens). */
static void assert_materialization_rejects_below_exact(
    tp_model *model, tp_journal *journal, size_t exact, materialize_fn materialize, tp_error *err,
    bool clone_always_happens, int64_t revision_at_reject) {
    tp_journal__test_set_file_limit(exact - 1U);
    tp_project__test_serialization_stats_reset();
    tp_project__test_set_clone_alloc_fail(-1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, materialize(model, journal, err));
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_save_buffer_calls());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_serializer_allocations());
    TEST_ASSERT_EQUAL_size_t(0U, tp_project__test_load_buffer_calls());
    if (clone_always_happens) {
        TEST_ASSERT_GREATER_THAN_INT(0, tp_project__test_clone_alloc_count());
        TEST_ASSERT_EQUAL_size_t(1U, tp_project__test_size_query_calls());
        TEST_ASSERT_EQUAL_INT64(revision_at_reject, tp_model_revision(model));
    } else {
        TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    }
}

/* Accept branch: exactly `exact` bytes admits it and the checkpoint I/O happens once. */
static void assert_materialization_accepts_at_exact(
    tp_model *model, tp_journal *journal, size_t exact, materialize_fn materialize, tp_error *err) {
    tp_journal__test_set_file_limit(exact);
    tp_project__test_serialization_stats_reset();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, materialize(model, journal, err));
    TEST_ASSERT_EQUAL_size_t(1U, tp_project__test_save_buffer_calls());
    TEST_ASSERT_EQUAL_size_t(1U, tp_project__test_load_buffer_calls());
}

void test_checkpoint_byte_admission_precedes_attach_materialization_and_counts_ids(void) {
    tp_model *model = tp_model_wrap(tp_test_base_project());
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "73000000000000000000000000000001", 0,
                      "retained-before-attach"));

    size_t snapshot_bytes = 0U;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_checkpoint_serialized_size_bounded(
            tp_model_project(model), SIZE_MAX, &snapshot_bytes, &err));
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x73));
    TEST_ASSERT_NOT_NULL(journal);
    const size_t exact = checkpoint_store_bytes(
        (size_t)TP_JRN_HEADER_LEN, snapshot_bytes, 1);

    assert_materialization_rejects_below_exact(model, journal, exact,
                                               tp_model_attach_journal, &err,
                                               false, 0);
    TEST_ASSERT_EQUAL_INT64(0, io.length(io.ctx));

    assert_materialization_accepts_at_exact(model, journal, exact,
                                            tp_model_attach_journal, &err);
    TEST_ASSERT_EQUAL_INT64((int64_t)exact, io.length(io.ctx));
    TEST_ASSERT_GREATER_THAN_size_t(0U, tp_project__test_serializer_allocations());
    tp_model_destroy(model);
}

void test_checkpoint_pathological_length_rejects_before_write(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x74));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    const uint8_t sentinel = 0U;

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_JOURNAL_FAILED,
        tp_journal_init_checkpoint(journal, &sentinel, SIZE_MAX, 0, &err));
    TEST_ASSERT_EQUAL_INT64(0, io.length(io.ctx));
    tp_journal_destroy(journal);
}

void test_compact_admits_checkpoint_and_cached_metadata_as_one_layout(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x75));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    const uint8_t old_snapshot[] = {'o', 'l', 'd'};
    const uint8_t new_snapshot[] = {'n', 'e', 'w'};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_init_checkpoint(journal, old_snapshot,
                                   sizeof old_snapshot, 0, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_set_metadata(journal, 1700000075,
                                "/saved/project.ntpacker", "project", &err));
    const int64_t complete_before = io.length(io.ctx);
    TEST_ASSERT_GREATER_THAN_INT64(0, complete_before);

    const size_t checkpoint_only = checkpoint_store_bytes(
        (size_t)TP_JRN_HEADER_LEN, sizeof new_snapshot, 0);
    tp_journal__test_set_file_limit(checkpoint_only);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_JOURNAL_FAILED,
        tp_journal_compact(journal, new_snapshot, sizeof new_snapshot, 1,
                           &err));
    TEST_ASSERT_EQUAL_INT64(complete_before, io.length(io.ctx));

    tp_journal__test_set_file_limit(0U);
    tp_journal__test_set_record_limit(1U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_journal_compact(journal, new_snapshot, sizeof new_snapshot, 1,
                           &err));
    TEST_ASSERT_EQUAL_INT64(complete_before, io.length(io.ctx));

    tp_journal__test_set_record_limit(2U);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_compact(journal, new_snapshot, sizeof new_snapshot, 1,
                           &err));
    tp_journal_destroy(journal);
}

void test_checkpoint_byte_admission_precedes_compact_materialization(void) {
    tp_journal_io io;
    tp_model *model = model_with_journal(tp_test_id_of(0x72), &io);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "72000000000000000000000000000001", 0,
                      "retained-before-compact"));
    size_t snapshot_bytes = 0U;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_checkpoint_serialized_size_bounded(
            tp_model_project(model), SIZE_MAX, &snapshot_bytes, &err));
    const size_t exact = checkpoint_store_bytes(
        (size_t)TP_JRN_HEADER_LEN, snapshot_bytes,
        tp_journal_id_count(tp_model_journal(model)));

    assert_materialization_rejects_below_exact(model, tp_model_journal(model), exact,
                                               materialize_compact, &err, false, 0);

    assert_materialization_accepts_at_exact(model, tp_model_journal(model), exact,
                                            materialize_compact, &err);
    TEST_ASSERT_EQUAL_INT64((int64_t)exact, io.length(io.ctx));
    TEST_ASSERT_GREATER_THAN_size_t(0U, tp_project__test_serializer_allocations());
    tp_model_destroy(model);
}

void test_checkpoint_byte_admission_precedes_undo_redo_materialization(void) {
    tp_journal_io io;
    tp_model *model = model_with_journal(tp_test_id_of(0x71), &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "71000000000000000000000000000001", 0,
                      "a-name-long-enough-to-change-checkpoint-size"));
    tp_error err = {0};

    tp_project *undo_candidate = tp_project_clone(tp_model_project(model));
    TEST_ASSERT_NOT_NULL(undo_candidate);
    tp_diff_record *record = tp_history_undo_record(tp_model_history(model));
    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_diff_record_apply(undo_candidate, record, true,
                                               &err));
    size_t undo_bytes = 0U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_checkpoint_serialized_size_bounded(
            undo_candidate, SIZE_MAX, &undo_bytes, &err));
    tp_project_destroy(undo_candidate);
    const size_t undo_exact = checkpoint_store_bytes(
        (size_t)io.length(io.ctx), undo_bytes,
        tp_journal_id_count(tp_model_journal(model)));
    assert_materialization_rejects_below_exact(model, tp_model_journal(model), undo_exact,
                                               materialize_undo, &err, true, 1);
    assert_materialization_accepts_at_exact(model, tp_model_journal(model), undo_exact,
                                            materialize_undo, &err);

    tp_project *redo_candidate = tp_project_clone(tp_model_project(model));
    TEST_ASSERT_NOT_NULL(redo_candidate);
    record = tp_history_redo_record(tp_model_history(model));
    TEST_ASSERT_NOT_NULL(record);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_diff_record_apply(redo_candidate, record, false,
                                               &err));
    size_t redo_bytes = 0U;
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_checkpoint_serialized_size_bounded(
            redo_candidate, SIZE_MAX, &redo_bytes, &err));
    tp_project_destroy(redo_candidate);
    const size_t redo_exact = checkpoint_store_bytes(
        (size_t)io.length(io.ctx), redo_bytes,
        tp_journal_id_count(tp_model_journal(model)));
    assert_materialization_rejects_below_exact(model, tp_model_journal(model), redo_exact,
                                               materialize_redo, &err, true, 2);
    assert_materialization_accepts_at_exact(model, tp_model_journal(model), redo_exact,
                                            materialize_redo, &err);
    tp_model_destroy(model);
}

void test_save_stages_path_normalization_without_invalidating_history(void) {
    if (!g_dir) {
        return;
    }
    tp_project *project = tp_test_base_project();
    tp_project_source *source = &project->atlases[0].sources[0];
    char absolute_source[1024];
    char project_path[1024];
    (void)snprintf(absolute_source, sizeof absolute_source,
                   "%s/assets/hero.png", g_dir);
    (void)snprintf(project_path, sizeof project_path,
                   "%s/history-size-refresh.ntpacker", g_dir);
    char *source_copy = (char *)malloc(strlen(absolute_source) + 1U);
    TEST_ASSERT_NOT_NULL(source_copy);
    memcpy(source_copy, absolute_source, strlen(absolute_source) + 1U);
    free(source->path);
    source->path = source_copy;

    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x76));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_attach_journal(model, journal, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "76000000000000000000000000000001", 0,
                      "edited-before-save"));

    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_save(tp_model_project(model), project_path, &err));
    TEST_ASSERT_EQUAL_STRING(
        absolute_source,
        tp_model_project(model)->atlases[0].sources[0].path);
    tp_model_mark_saved(model);
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(model, &err));

    tp_model_destroy(model);
    (void)remove(project_path);
}

void test_save_as_compact_recovery_keeps_source_target_self_contained(void) {
    if (!g_dir) {
        return;
    }
    char old_path[1024];
    char new_path[1024];
    char new_dir[1024];
    (void)snprintf(old_path, sizeof old_path, "%s/recovery-old.ntpacker", g_dir);
    (void)snprintf(new_dir, sizeof new_dir, "%s", g_dir);
    char *slash = strrchr(new_dir, '/');
    if (!slash) {
        slash = strrchr(new_dir, '\\');
    }
    TEST_ASSERT_NOT_NULL(slash);
    *slash = '\0';
    (void)snprintf(new_path, sizeof new_path, "%s/recovery-new.ntpacker",
                   new_dir);

    tp_project *project = tp_test_base_project();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, old_path, &err));
    char target_before[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_source_path(
            project, project->atlases[0].sources[0].path, target_before,
            sizeof target_before));

    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, tp_test_id_of(0x79));
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_attach_journal(model, journal, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(tp_model_project(model), new_path,
                                          &err));
    tp_model_mark_saved(model);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_set_recovery_metadata(model, 1700000079, new_path,
                                       "recovery-new", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_compact_journal(model, &err));

    size_t journal_len = 0U;
    uint8_t *journal_bytes = snapshot_and_destroy(io, model, &journal_len);
    tp_journal_io recovered_io = io_from_bytes(journal_bytes, journal_len);
    free(journal_bytes);
    tp_journal_recovery recovery;
    tp_model *recovered = recover_expect(recovered_io, tp_test_id_of(0x79), TP_STATUS_OK, &recovery, &err);
    TEST_ASSERT_NOT_NULL(recovered);
    const tp_project *recovered_project = tp_model_project(recovered);
    char target_after[1024];
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_project_resolve_source_path(
            recovered_project, recovered_project->atlases[0].sources[0].path,
            target_after, sizeof target_after));
    TEST_ASSERT_EQUAL_STRING(target_before, target_after);

    tp_journal_recovery_free(&recovery);
    tp_model_destroy(recovered);
    (void)remove(new_path);
    (void)remove(old_path);
}

void test_mark_saved_preserves_deep_history_without_clone_work(void) {
    tp_model *model = tp_model_wrap(tp_test_base_project());
    TEST_ASSERT_NOT_NULL(model);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(model));
    for (int i = 0; i < 16; ++i) {
        char id[33];
        char name[64];
        (void)snprintf(id, sizeof id, "%032x", (unsigned)(0x7700 + i));
        (void)snprintf(name, sizeof name, "deep-history-%d", i);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              commit_rename(model, id, i, name));
    }
    TEST_ASSERT_TRUE(tp_model_dirty(model));
    tp_error err = {0};
    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(model, &err));
    }
    TEST_ASSERT_EQUAL_INT(12, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(4, tp_model_redo_depth(model));

    tp_project__test_set_clone_alloc_fail(-1);
    tp_model_mark_saved(model);
    TEST_ASSERT_FALSE(tp_model_dirty(model));
    TEST_ASSERT_EQUAL_INT(0, tp_project__test_clone_alloc_count());
    TEST_ASSERT_EQUAL_INT(12, tp_model_undo_depth(model));
    TEST_ASSERT_EQUAL_INT(4, tp_model_redo_depth(model));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(model, &err));
    tp_model_destroy(model);
}

void test_replay_operation_budget_is_prewrite_preclone_and_checkpoint_reset(void) {
    const tp_id128 key = tp_test_id_of(0x6c);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U, 0, &err));
    const char *at_limit_id = "6c000000000000000000000000000001";
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_append_txn_counted(
            journal, at_limit_id, 1, NULL, 0U,
            (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS, &err));
    const int64_t at_limit_bytes = io.length(io.ctx);
    TEST_ASSERT_GREATER_THAN_INT64(0, at_limit_bytes);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        tp_journal_append_txn_counted(
            journal, "6c000000000000000000000000000002", 2, NULL, 0U, 1U, &err));
    TEST_ASSERT_EQUAL_INT64(at_limit_bytes, io.length(io.ctx));
    TEST_ASSERT_FALSE(tp_journal_contains(journal,
                                          "6c000000000000000000000000000002"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_append_txn_counted(journal, at_limit_id, 1, NULL, 0U, 1U, &err));
    TEST_ASSERT_EQUAL_INT64(at_limit_bytes, io.length(io.ctx));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, NULL, 0U, 2, &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_append_txn_counted(
            journal, "6c000000000000000000000000000002", 3, NULL, 0U, 1U, &err));
    tp_journal_destroy(journal);

    tp_journal_io model_io;
    tp_model *model = model_with_journal(tp_test_id_of(0x6b), &model_io);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal__set_replay_operations(
            tp_model_journal(model), (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS, &err));
    const int64_t revision_before = tp_model_revision(model);
    char *project_before = serialize(tp_model_project(model));
    tp_project__test_set_clone_alloc_fail(0);
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OUT_OF_BOUNDS,
        commit_rename(model, "6b000000000000000000000000000001",
                      revision_before, "must-not-commit"));
    tp_project__test_set_clone_alloc_fail(-1);
    TEST_ASSERT_EQUAL_INT64(revision_before, tp_model_revision(model));
    char *project_after = serialize(tp_model_project(model));
    TEST_ASSERT_EQUAL_STRING(project_before, project_after);
    free(project_after);
    free(project_before);
    tp_model_destroy(model);
}

void test_recovery_rejects_aggregate_operation_overflow_before_apply(void) {
    const tp_id128 key = tp_test_id_of(0x6a);
    tp_project *project = tp_test_base_project();
    const tp_id128 atlas_id = project->atlases[0].id;
    char *snapshot = serialize(project);
    char *payload = dense_rename_payload(atlas_id);
    TEST_ASSERT_NOT_NULL(payload);
    const size_t payload_len = strlen(payload);
    TEST_ASSERT_TRUE(payload_len < (size_t)TP_TXN_MAX_REQUEST_BYTES);

    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot,
                                   strlen(snapshot), 0, &err));
    const size_t records =
        (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS / (size_t)TP_TXN_MAX_OPS + 1U;
    for (size_t i = 0U; i < records; ++i) {
        char id[33];
        (void)snprintf(id, sizeof id, "%032" PRIx64,
                       (uint64_t)i + UINT64_C(0x6a0000));
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_journal_append_txn(journal, id, (int64_t)i + 1,
                                  (const uint8_t *)payload, payload_len, &err));
    }
    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    tp_journal_recovery info;
    tp_op__test_apply_count_reset();
    tp_model *recovered = recover_expect(io_from_bytes(bytes, bytes_len), key,
                                         TP_STATUS_OUT_OF_BOUNDS, &info, &err);
    TEST_ASSERT_NULL(recovered);
    TEST_ASSERT_EQUAL_size_t(0U, tp_op__test_apply_count_take());
    TEST_ASSERT_EQUAL_size_t(records, info.op_count);
    TEST_ASSERT_TRUE(tp_journal__test_recovery_ops_borrow_raw(&info));
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "operation limit"));

    tp_journal_recovery_free(&info);
    free(bytes);
    free(payload);
    free(snapshot);
    tp_project_destroy(project);
}

void test_recovery_rejects_duplicate_operations_before_aggregate_apply(void) {
    const tp_id128 key = tp_test_id_of(0x69);
    tp_project *project = tp_test_base_project();
    char *snapshot = serialize(project);
    char *payload = dense_rename_payload_with_duplicate_operations(
        project->atlases[0].id);
    TEST_ASSERT_NOT_NULL(payload);
    const size_t payload_len = strlen(payload);
    TEST_ASSERT_TRUE(payload_len < (size_t)TP_TXN_MAX_REQUEST_BYTES);

    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_init_checkpoint(journal, (const uint8_t *)snapshot,
                                   strlen(snapshot), 0, &err));
    const size_t records =
        (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS / (size_t)TP_TXN_MAX_OPS + 1U;
    for (size_t i = 0U; i < records; ++i) {
        char id[33];
        (void)snprintf(id, sizeof id, "%032" PRIx64,
                       (uint64_t)i + UINT64_C(0x690000));
        TEST_ASSERT_EQUAL_INT(
            TP_STATUS_OK,
            tp_journal_append_txn(journal, id, (int64_t)i + 1,
                                  (const uint8_t *)payload, payload_len, &err));
    }
    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    tp_journal_recovery info;
    tp_op__test_apply_count_reset();
    tp_model *recovered = recover_expect(io_from_bytes(bytes, bytes_len), key,
                                         TP_STATUS_INVALID_ARGUMENT, &info, &err);
    TEST_ASSERT_NULL(recovered);
    TEST_ASSERT_EQUAL_size_t(0U, tp_op__test_apply_count_take());
    TEST_ASSERT_EQUAL_size_t(records, info.op_count);
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "duplicate operations"));

    tp_journal_recovery_free(&info);
    free(bytes);
    free(payload);
    free(snapshot);
    tp_project_destroy(project);
}

/* Byte offset of record #idx (0-based, after the 28-byte header) in a journal store,
 * or SIZE_MAX if the walk runs past the end. Lets a test corrupt a chosen mid-stream
 * record precisely (the length-prefix framing makes the walk trivial). */
static size_t record_offset(const uint8_t *buf, size_t len, int idx) {
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    for (int i = 0; i < idx; i++) {
        if (off + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD > len) {
            return SIZE_MAX;
        }
        uint32_t plen = tp_jrn_get_u32(buf + off + (size_t)TP_JRN_SYNC_FIELD); /* len follows the sync-word */
        off += (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + (size_t)plen + (size_t)TP_JRN_CRC_FIELD;
    }
    return off;
}

/* Test-only io wrapper over a memory io that fails EXACTLY the write whose 1-based index == fail_at
 * (0 = never). The memory-io __fail_next_writes seam is a from-the-next countdown, so it cannot let the
 * header + checkpoint writes SUCCEED and fail only the LATER metadata write inside tp_journal_compact
 * (write #3 after the truncate-to-0). This wrapper targets that specific write: an injected write returns
 * 0 (total failure, store untouched) so write_record's rollback truncate -- delegated to the inner
 * store -- still succeeds, exactly the [0] scenario (torn META tail rolled back, journal NOT poisoned). */
typedef struct {
    tp_journal_io inner; /* owned memory io */
    int write_count;
    int fail_at; /* 1-based index of the write to fail; 0 = never */
    int read_count;
    int64_t length_override; /* <0 delegates to inner */
    size_t read_len_override; /* SIZE_MAX delegates to inner */
} faulty_io_ctx;

static int64_t faulty_write(void *ctx, const uint8_t *data, size_t len) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    f->write_count++;
    if (f->fail_at != 0 && f->write_count == f->fail_at) {
        return 0; /* injected total failure of exactly this write (nothing written) */
    }
    return f->inner.write(f->inner.ctx, data, len);
}
static int64_t faulty_length(void *ctx) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    if (f->length_override >= 0) {
        return f->length_override;
    }
    return f->inner.length(f->inner.ctx);
}
static int faulty_truncate(void *ctx, size_t len) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    return f->inner.truncate(f->inner.ctx, len);
}
static int faulty_read_all(void *ctx, size_t max_len, uint8_t **out,
                           size_t *out_len) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    f->read_count++;
    const int status = f->inner.read_all(f->inner.ctx, max_len, out, out_len);
    if (status == 0 && f->read_len_override != SIZE_MAX) {
        *out_len = f->read_len_override;
    }
    return status;
}
static int faulty_sync(void *ctx) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    return f->inner.sync ? f->inner.sync(f->inner.ctx) : 0;
}
static void faulty_destroy(void *ctx) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    if (f) {
        if (f->inner.destroy) {
            f->inner.destroy(f->inner.ctx);
        }
        free(f);
    }
}
static tp_journal_io faulty_io(void) {
    faulty_io_ctx *f = (faulty_io_ctx *)calloc(1, sizeof *f);
    TEST_ASSERT_NOT_NULL(f);
    f->inner = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(f->inner.ctx);
    f->fail_at = 0;
    f->length_override = -1;
    f->read_len_override = SIZE_MAX;
    tp_journal_io io;
    io.ctx = f;
    io.write = faulty_write;
    io.length = faulty_length;
    io.truncate = faulty_truncate;
    io.read_all = faulty_read_all;
    io.sync = faulty_sync;
    io.destroy = faulty_destroy;
    return io;
}
/* Arm: fail the n-th write COUNTED FROM NOW (resets the counter), so a test can target a specific write
 * inside one operation regardless of how many writes preceded the arming. */
static void faulty_io_fail_at(tp_journal_io io, int n) {
    faulty_io_ctx *f = (faulty_io_ctx *)io.ctx;
    f->write_count = 0;
    f->fail_at = n;
}

/* ---- byte-identity: the journal is a sidecar ----------------------------- */

void test_journal_is_sidecar_byte_identical(void) {
    /* Same op on a journal-LESS and a journal-BACKED model -> identical project bytes. */
    tp_project *p0 = tp_test_base_project();
    tp_model *plain = tp_model_wrap(p0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(plain, "10000000000000000000000000000001", 0, "renamed"));
    char *plain_bytes = serialize(tp_model_project(plain));

    tp_journal_io io;
    tp_model *j = model_with_journal(tp_test_id_of(0x01), &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(j, "10000000000000000000000000000001", 0, "renamed"));
    char *j_bytes = serialize(tp_model_project(j));

    TEST_ASSERT_EQUAL_STRING(plain_bytes, j_bytes);
    TEST_ASSERT_EQUAL_INT64(tp_model_revision(plain), tp_model_revision(j));

    free(plain_bytes);
    free(j_bytes);
    tp_model_destroy(plain);
    tp_model_destroy(j);
}

/* ---- checkpoint + journal replay restores state + retained id set -------- */

void test_checkpoint_and_replay(void) {
    tp_id128 key = tp_test_id_of(0x11);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "aa000000000000000000000000000001", 0, "alpha"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "aa000000000000000000000000000002", 1, "beta"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(2, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_op__test_apply_count_reset();
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_size_t(2U, tp_op__test_apply_count_take());
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT(3, info.records_recovered); /* checkpoint + 2 txns */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));

    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(tp_model_journal(m2)));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "aa000000000000000000000000000001"));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "aa000000000000000000000000000002"));

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- R2 format-B linchpin: recovery REPLAYS op-payloads onto the checkpoint --- */

/* Format B (plan S18 R / R2): a TXN record stores the SERIALIZED OPERATION, not a snapshot;
 * recovery loads the last CHECKPOINT as a base and replays the post-checkpoint ops onto it.
 * Proven three ways: (1) the recovery info carries op_count == the post-checkpoint txns and a
 * base snapshot that does NOT already hold the final state (so a pass REQUIRES real replay, not
 * a lucky snapshot); (2) the replayed project reserializes byte-identically to the live committed
 * model (the same tp_operation_apply, same order, as commit); (3) the recovered revision is the
 * FINAL one. */
void test_format_b_replays_ops_onto_checkpoint(void) {
    tp_id128 key = tp_test_id_of(0x13);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io); /* checkpoint captured at rev 0 (atlas "atlas1") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ab000000000000000000000000000001", 0, "first"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ab000000000000000000000000000002", 1, "second"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(2, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);

    /* (1) format-B shape: a base checkpoint snapshot + exactly two post-checkpoint op-payloads. */
    TEST_ASSERT_EQUAL_INT(2, (int)info.op_count);
    TEST_ASSERT_NOT_NULL(info.snapshot);
    TEST_ASSERT_TRUE(span_contains(info.snapshot, info.snapshot_len, "atlas1"));
    TEST_ASSERT_FALSE(span_contains(info.snapshot, info.snapshot_len, "second"));
    TEST_ASSERT_EQUAL_INT64(1, info.ops[0].revision);
    TEST_ASSERT_EQUAL_INT64(2, info.ops[1].revision);

    /* (2)+(3): replaying the ops onto the base reaches the FINAL committed state, byte-identical. */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_STRING("second", tp_model_project(m2)->atlases[0].name);

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R2b/#4: a MASK-CARRYING op (partial target.set -- only out_path) survives the full format-B chain
 * end-to-end: commit -> journal (op-payload) -> recover (replay). The recovered project must be
 * byte-identical to the committed one -- so the partial mask applied as a PARTIAL edit, not a full
 * replace that would clobber exporter/enabled. Guards the masked-op round-trip R2a fixed (the rename-
 * only test above covers a plain op). */
void test_format_b_recovers_masked_target_set(void) {
    tp_id128 key = tp_test_id_of(0x14);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_target_out_path(m, "cd000000000000000000000000000001", 0, "out/changed.json"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(1, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);

    /* base checkpoint = the ORIGINAL out_path; the edit lives ONLY in the replayed op-payload */
    TEST_ASSERT_EQUAL_INT(1, (int)info.op_count);
    TEST_ASSERT_TRUE(span_contains(info.snapshot, info.snapshot_len, "out/a"));
    TEST_ASSERT_FALSE(span_contains(info.snapshot, info.snapshot_len, "out/changed.json"));

    /* replaying the masked op onto the base reaches the committed state, byte-identical */
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    TEST_ASSERT_EQUAL_STRING("out/changed.json", tp_model_project(m2)->atlases[0].targets[0].out_path);

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- duplicate retry after restart is de-duplicated (§7.2) --------------- */

void test_duplicate_retry_after_restart(void) {
    tp_id128 key = tp_test_id_of(0x12);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "bb000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    char *after_recover = serialize(tp_model_project(m2));

    /* Retry the acknowledged id -> DUPLICATE_ID, model unchanged. */
    tp_status dup = commit_rename(m2, "bb000000000000000000000000000001", tp_model_revision(m2), "one-again");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, dup);
    char *after_dup = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(after_recover, after_dup);

    /* A NEW id after restart commits and appends onto the recovered journal. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m2, "bb000000000000000000000000000002", tp_model_revision(m2), "two"));
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(tp_model_journal(m2)));

    free(after_recover);
    free(after_dup);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- append failure after apply -> exact rollback, no ack, retryable ----- */

void test_append_failure_rolls_back(void) {
    tp_id128 key = tp_test_id_of(0x22);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "cc000000000000000000000000000001", 0, "one"));
    char *before = serialize(tp_model_project(m));
    int64_t rev_before = tp_model_revision(m);

    /* Inject: the next durable write (the append) fails entirely. */
    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_status st = commit_rename(m, "cc000000000000000000000000000002", 1, "two");
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, st);

    /* Live model byte-unchanged, revision unchanged, id NOT retained. */
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_EQUAL_INT64(rev_before, tp_model_revision(m));
    TEST_ASSERT_FALSE(tp_journal_contains(tp_model_journal(m), "cc000000000000000000000000000002"));

    /* Retry the SAME id -> succeeds (the failed append left no torn tail). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "cc000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT64(rev_before + 1, tp_model_revision(m));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m), "cc000000000000000000000000000002"));

    /* And the retried txn is recoverable exactly once (not duplicated). */
    size_t blen = 0;
    char *committed = serialize(tp_model_project(m));
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(tp_model_journal(m2))); /* id01 + id02, once each */
    char *rec = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(committed, rec);

    free(before);
    free(after);
    free(committed);
    free(rec);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- append OOM (retained-id reserve fails) -> nothing durable, retryable - */

void test_append_oom_is_retryable(void) {
    /* Drive the journal directly: a reserve OOM must write nothing. Simulate by
     * failing the write and confirming no id is registered; then a clean append
     * registers exactly once. */
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0x23));
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    const uint8_t snap[] = {'x', 'y', 'z'};

    tp_journal_io_memory__fail_next_writes(io, 1); /* the header/record write fails */
    tp_status st = tp_journal_append_txn(j, "dd000000000000000000000000000001", 1, snap, sizeof snap, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, st);
    TEST_ASSERT_FALSE(tp_journal_contains(j, "dd000000000000000000000000000001"));
    TEST_ASSERT_EQUAL_INT(0, tp_journal_id_count(j));

    /* Retry succeeds and registers exactly once; a second identical append is a no-op. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "dd000000000000000000000000000001", 1, snap, sizeof snap, &err));
    TEST_ASSERT_TRUE(tp_journal_contains(j, "dd000000000000000000000000000001"));
    TEST_ASSERT_EQUAL_INT(1, tp_journal_id_count(j));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "dd000000000000000000000000000001", 1, snap, sizeof snap, &err));
    TEST_ASSERT_EQUAL_INT(1, tp_journal_id_count(j));

    tp_journal_destroy(j);
}

/* ---- short write at EVERY byte boundary: UB-clean prefix recovery -------- */

void test_short_write_every_boundary(void) {
    tp_id128 key = tp_test_id_of(0x33);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "b0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "b0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);
    TEST_ASSERT_TRUE(full_len > (size_t)TP_JRN_HEADER_LEN);

    int prev_records = -1;
    int max_records = 0;
    for (size_t n = 0; n <= full_len; n++) {
        tp_journal_io io2 = io_from_bytes(full, n);
        tp_journal_recovery info;
        tp_error err;
        tp_model *rm = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
        /* Never UB / never crash; the recovered prefix is monotone non-decreasing; the
         * stop offset is in bounds; the retained id count never exceeds recovered txns. */
        TEST_ASSERT_TRUE(info.records_recovered >= prev_records);
        TEST_ASSERT_TRUE(info.stop_offset <= n);
        prev_records = info.records_recovered;
        if (info.records_recovered > max_records) {
            max_records = info.records_recovered;
        }
        if (rm) {
            char *s = serialize(tp_model_project(rm)); /* recovered project must load + reserialize */
            free(s);
            TEST_ASSERT_TRUE(tp_journal_id_count(tp_model_journal(rm)) <= info.records_recovered);
            tp_model_destroy(rm);
        }
        tp_journal_recovery_free(&info);
    }
    TEST_ASSERT_EQUAL_INT(3, max_records); /* the full store recovers all 3 records */
    free(full);
}

/* ---- torn tail (payload truncated) -> invisible, no dup on retry --------- */

void test_torn_tail_invisible(void) {
    tp_id128 key = tp_test_id_of(0x34);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "c0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "c0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Truncate 3 bytes off the end -> the last record's tail is torn. */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, info.status);
    TEST_ASSERT_NOT_NULL(m2);
    /* The torn txn is invisible; the good prefix (checkpoint + txn01) survived. */
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "c0000000000000000000000000000001"));
    TEST_ASSERT_FALSE(tp_journal_contains(tp_model_journal(m2), "c0000000000000000000000000000002"));

    /* Retrying the unacknowledged (torn) txn applies it once -- no dup. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m2, "c0000000000000000000000000000002", tp_model_revision(m2), "two"));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "c0000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- checksum mismatch -> corruption boundary, safe fallback ------------- */

void test_checksum_mismatch(void) {
    tp_id128 key = tp_test_id_of(0x35);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "d0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "d0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Flip the last payload byte of the last record (before its 4-byte crc). */
    size_t at = full_len - TP_JRN_CRC_FIELD - 1;
    tp_journal_io io2 = io_from_bytes(full, full_len);
    tp_journal_io_memory__poke(io2, at, (uint8_t)(full[at] ^ 0xFFu));

    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, info.status);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "d0000000000000000000000000000001"));
    TEST_ASSERT_FALSE(tp_journal_contains(tp_model_journal(m2), "d0000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- stale journal for a moved project -> detected, not applied ---------- */

void test_stale_key_not_applied(void) {
    tp_id128 k1 = tp_test_id_of(0x44);
    tp_id128 k2 = tp_test_id_of(0x55);
    tp_journal_io io;
    tp_model *m = model_with_journal(k1, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ee000000000000000000000000000001", 0, "one"));
    size_t len = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &len);

    tp_journal_io io2 = io_from_bytes(bytes, len);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, k2, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NULL(m2); /* moved project: NOT misapplied */
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_STALE_KEY, info.status);
    tp_journal_recovery_free(&info);
}

/* ---- UB-clean on arbitrary / garbage / absurd-length bytes --------------- */

void test_recover_arbitrary_bytes(void) {
    tp_id128 key = tp_test_id_of(0x66);
    for (size_t len = 0; len <= 320; len++) {
        uint8_t *buf = (uint8_t *)malloc(len ? len : 1);
        TEST_ASSERT_NOT_NULL(buf);
        uint32_t s = (uint32_t)(len * 2654435761u + 1u);
        for (size_t i = 0; i < len; i++) {
            s = s * 1103515245u + 12345u;
            buf[i] = (uint8_t)(s >> 16);
        }
        tp_journal_io io = io_from_bytes(buf, len);
        free(buf);
        tp_journal *j = tp_journal_create(io, key);
        TEST_ASSERT_NOT_NULL(j);
        tp_journal_recovery rec;
        memset(&rec, 0, sizeof rec);
        tp_error err;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j, &rec, &err));
        TEST_ASSERT_TRUE(rec.status >= TP_JOURNAL_RECOVERY_OK && rec.status <= TP_JOURNAL_RECOVERY_STALE_KEY);
        tp_journal_recovery_free(&rec);
        tp_journal_destroy(j);
    }

    /* A valid header + a valid record sync-word followed by an absurdly huge length prefix:
     * the reader must bounds-check (TRUNCATED, nothing follows) and NEVER allocate the
     * claimed size. The sync-word makes this a genuine bloated-length frame (not a partial
     * header), exercising the P1-5 overrun path with no trailing record. */
    uint8_t hdr[TP_JRN_HEADER_LEN + TP_JRN_SYNC_FIELD + TP_JRN_LEN_FIELD];
    memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(hdr + TP_JRN_KEY_OFF, key.bytes, 16);
    tp_jrn_put_u32(hdr + TP_JRN_HEADER_LEN, (uint32_t)TP_JRN_SYNC_WORD);        /* valid record sync */
    tp_jrn_put_u32(hdr + TP_JRN_HEADER_LEN + TP_JRN_SYNC_FIELD, 0xFFFFFFF0u);   /* claims ~4 GB payload */
    tp_journal_io io = io_from_bytes(hdr, sizeof hdr);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, rec.status);
    TEST_ASSERT_EQUAL_INT(0, rec.records_recovered);
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j);
}

/* ---- bad header: BAD_MAGIC (foreign) vs VERSION_MISMATCH (our file, wrong format) ------ *
 * R5a plan item 7: the old conflated BAD_HEADER splits so the R5b scan distinguishes a foreign file
 * from an out-of-date recovery journal. Both keep the old semantics (nothing recovered, bytes on disk
 * preserved, return OK) -- only the classification splits. recover AND peek must agree per case. */
void test_bad_magic_vs_version_mismatch(void) {
    tp_id128 key = tp_test_id_of(0x67);
    tp_error err;

    /* (1) BAD_MAGIC: a complete header whose magic is not "NTPKJRNL" -> not our file. */
    uint8_t bad_magic[TP_JRN_HEADER_LEN];
    memset(bad_magic, 0, sizeof bad_magic);
    memcpy(bad_magic, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    bad_magic[0] = (uint8_t)(bad_magic[0] ^ 0xFFu); /* corrupt one magic byte */
    tp_jrn_put_u32(bad_magic + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(bad_magic + TP_JRN_KEY_OFF, key.bytes, 16);

    tp_journal_io io = io_from_bytes(bad_magic, sizeof bad_magic);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_BAD_MAGIC, rec.status);
    TEST_ASSERT_EQUAL_INT(0, rec.records_recovered); /* nothing recovered */
    TEST_ASSERT_EQUAL_INT64((int64_t)sizeof bad_magic, (int64_t)rec.bytes_total); /* bytes preserved */
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j);

    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bad_magic, sizeof bad_magic), &pk, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_BAD_MAGIC, pk.status);
    TEST_ASSERT_EQUAL_INT(0, pk.record_count);
    tp_journal_peek_free(&pk);

    /* (2) VERSION_MISMATCH: a valid "NTPKJRNL" header whose format_version is not the current one. */
    uint8_t bad_ver[TP_JRN_HEADER_LEN];
    memset(bad_ver, 0, sizeof bad_ver);
    memcpy(bad_ver, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(bad_ver + TP_JRN_MAGIC_LEN, 999u); /* unknown version -> never guess */
    memcpy(bad_ver + TP_JRN_KEY_OFF, key.bytes, 16);

    tp_journal_io io2 = io_from_bytes(bad_ver, sizeof bad_ver);
    tp_journal *j2 = tp_journal_create(io2, key);
    TEST_ASSERT_NOT_NULL(j2);
    tp_journal_recovery rec2;
    memset(&rec2, 0, sizeof rec2);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j2, &rec2, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_VERSION_MISMATCH, rec2.status);
    TEST_ASSERT_EQUAL_INT(0, rec2.records_recovered); /* nothing recovered */
    TEST_ASSERT_EQUAL_INT64((int64_t)sizeof bad_ver, (int64_t)rec2.bytes_total); /* bytes preserved */
    tp_journal_recovery_free(&rec2);
    tp_journal_destroy(j2);

    tp_journal_peek_result pk2;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bad_ver, sizeof bad_ver), &pk2, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_VERSION_MISMATCH, pk2.status);
    TEST_ASSERT_EQUAL_UINT32(999u, pk2.format_version); /* peek reports the header version */
    tp_journal_peek_free(&pk2);
}

/* ---- poison on failed rollback: a torn tail that cannot be truncated ----- */

void test_poison_on_truncate_failure(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0x68));
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    const uint8_t snap[] = {'a', 'b', 'c', 'd'};
    /* First a clean append so a header + one record exist. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "ff000000000000000000000000000001", 1, snap, sizeof snap, &err));
    /* Next append does a SHORT write (torn tail) and the rollback truncate FAILS -> poison. */
    tp_journal_io_memory__short_next_write(io, 3);
    tp_journal_io_memory__fail_next_truncate(io);
    tp_status st = tp_journal_append_txn(j, "ff000000000000000000000000000002", 2, snap, sizeof snap, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, st);
    TEST_ASSERT_FALSE(tp_journal_contains(j, "ff000000000000000000000000000002"));
    /* A poisoned journal refuses further appends (won't hide the good record behind the tail). */
    st = tp_journal_append_txn(j, "ff000000000000000000000000000003", 2, snap, sizeof snap, &err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, st);
    tp_journal_destroy(j);
}

/* ---- side-effect coordinator prepare/publish/abort ordering -------------- */

typedef struct {
    int prep, pub, abrt;
    tp_status prep_ret;
} coord_counts;

static tp_status cc_prepare(void *ctx, const tp_txn_request *req, tp_error *err) {
    (void)req;
    coord_counts *c = (coord_counts *)ctx;
    c->prep++;
    if (c->prep_ret != TP_STATUS_OK) {
        return tp_error_set(err, c->prep_ret, "prepare fault (injected)");
    }
    return TP_STATUS_OK;
}
static void cc_publish(void *ctx, const tp_txn_request *req) {
    (void)req;
    ((coord_counts *)ctx)->pub++;
}
static void cc_abort(void *ctx, const tp_txn_request *req) {
    (void)req;
    ((coord_counts *)ctx)->abrt++;
}

void test_coordinator_ordering(void) {
    coord_counts cnt;
    memset(&cnt, 0, sizeof cnt);
    cnt.prep_ret = TP_STATUS_OK;
    tp_side_effect_coordinator coord;
    coord.ctx = &cnt;
    coord.prepare = cc_prepare;
    coord.publish = cc_publish;
    coord.abort = cc_abort;

    tp_journal_io io;
    tp_model *m = model_with_journal(tp_test_id_of(0x77), &io);
    tp_model_set_coordinator(m, &coord);

    /* (a) success: prepare + publish, no abort. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "01000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(1, cnt.prep);
    TEST_ASSERT_EQUAL_INT(1, cnt.pub);
    TEST_ASSERT_EQUAL_INT(0, cnt.abrt);

    /* (b) append failure: prepare + abort, no publish. */
    tp_journal_io_memory__fail_next_writes(io, 1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, commit_rename(m, "01000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT(2, cnt.prep);
    TEST_ASSERT_EQUAL_INT(1, cnt.pub);
    TEST_ASSERT_EQUAL_INT(1, cnt.abrt);

    /* (c) prepare failure: reject, no gate, no publish, no abort. */
    cnt.prep_ret = TP_STATUS_INVALID_ARGUMENT;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          commit_rename(m, "01000000000000000000000000000003", 1, "three"));
    TEST_ASSERT_EQUAL_INT(3, cnt.prep);
    TEST_ASSERT_EQUAL_INT(1, cnt.pub);
    TEST_ASSERT_EQUAL_INT(1, cnt.abrt);

    tp_model_destroy(m);
}

void test_coordinator_noop(void) {
    tp_side_effect_coordinator noop = tp_side_effect_coordinator_noop();
    tp_journal_io io;
    tp_model *m = model_with_journal(tp_test_id_of(0x78), &io);
    tp_model_set_coordinator(m, &noop);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "02000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    tp_model_destroy(m);
}

/* ---- real on-disk file journal round-trip -------------------------------- */

void test_file_journal_roundtrip(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/roundtrip.journal", g_dir);
    remove(path);

    tp_id128 key = tp_test_id_of(0x79);
    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_project *p = tp_test_base_project();
    tp_model *m = tp_model_wrap(p);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_attach_journal(m, j, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "0a000000000000000000000000000001", 0, "disk-one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "0a000000000000000000000000000002", 1, "disk-two"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    tp_model_destroy(m); /* flush + close the file */

    /* Reopen the same file and recover. */
    tp_journal_io io2 = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io2.ctx);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(tp_model_journal(m2)));

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
    remove(path);
}

/* R5b-2 fix [3]: the READ-ONLY, NO-CREATE opener. A missing path -> ctx == NULL AND creates NO file (the
 * peek/adopt-source scan contract is strictly read-only; tp_journal_io_file would resurrect a stray empty
 * journal). An existing journal opens + reads (peek works over it: checkpoint + 1 txn => record_count 2). */
void test_io_file_read_no_create(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/ro_nocreate.journal", g_dir);
    remove(path); /* ensure absent */

    /* Missing path: ctx == NULL and NOTHING is created on disk. A re-open (still read-only) ALSO fails,
     * proving the first call materialized no file -- a "w+b" create fallback would make this second open
     * succeed. (Avoids a raw fopen(), which -Wdeprecated rejects under the MSVC-clang test build.) */
    tp_journal_io ro = tp_journal_io_file_read(path);
    TEST_ASSERT_NULL(ro.ctx);
    tp_journal_io ro_again = tp_journal_io_file_read(path);
    TEST_ASSERT_NULL(ro_again.ctx); /* still missing => the read-only opener created no file */

    /* Write a real journal (checkpoint + one txn) with the create-capable opener, then close it. */
    tp_id128 key = tp_test_id_of(0x7B);
    tp_journal_io wio = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(wio.ctx);
    tp_journal *j = tp_journal_create(wio, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    const uint8_t snap[] = {'r', 'o'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(j, snap, sizeof snap, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "7b000000000000000000000000000001", 1, snap, sizeof snap, &err));
    tp_journal_destroy(j); /* flush + close */

    /* The read-only opener now opens it, and peek reads it. */
    tp_journal_io ro2 = tp_journal_io_file_read(path);
    TEST_ASSERT_NOT_NULL(ro2.ctx);
    tp_journal_peek_result pk;
    memset(&pk, 0, sizeof pk);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(ro2, &pk, &err)); /* TAKES OWNERSHIP of ro2 */
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, pk.status);
    TEST_ASSERT_TRUE(pk.has_checkpoint);
    TEST_ASSERT_EQUAL_INT(2, pk.record_count);
    tp_journal_peek_free(&pk);

    remove(path);
}

void test_recovery_rejects_oversized_abstract_store_before_read(void) {
    tp_journal_io io = faulty_io();
    faulty_io_ctx *ctx = (faulty_io_ctx *)io.ctx;
    ctx->length_override = 9;
    tp_journal__test_set_file_limit(8U);
    tp_journal *journal = tp_journal_create(io, tp_id128_nil());
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_journal_recover(journal, &recovery, &error));
    TEST_ASSERT_EQUAL_INT(0, ctx->read_count);
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
    tp_journal__test_set_file_limit(0U);
}

void test_peek_rejects_post_read_oversize_from_abstract_store(void) {
    tp_journal_io io = faulty_io();
    faulty_io_ctx *ctx = (faulty_io_ctx *)io.ctx;
    const uint8_t byte = 0U;
    TEST_ASSERT_EQUAL_INT64(1, io.write(io.ctx, &byte, 1U));
    ctx->length_override = 1;
    ctx->read_len_override = 9U;
    tp_journal__test_set_file_limit(8U);
    tp_journal_peek_result peek = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_journal_peek(io, &peek, &error));
    tp_journal__test_set_file_limit(0U);
}

void test_recovery_backend_rejects_growth_before_copy(void) {
    tp_journal_io io = faulty_io();
    faulty_io_ctx *ctx = (faulty_io_ctx *)io.ctx;
    const uint8_t bytes[9] = {0};
    TEST_ASSERT_EQUAL_INT64(9, io.write(io.ctx, bytes, sizeof bytes));
    ctx->length_override = 1; /* preflight sees the old in-limit length */
    tp_journal__test_set_file_limit(8U);
    tp_journal *journal = tp_journal_create(io, tp_id128_nil());
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery = {0};
    tp_error error = {{0}};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_journal_recover(journal, &recovery, &error));
    TEST_ASSERT_EQUAL_INT(1, ctx->read_count);
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
    tp_journal__test_set_file_limit(0U);
}

void test_journal_crc32_matches_ieee_reference_vector(void) {
    static const uint8_t reference[] = "123456789";
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926u,
                            tp_jrn_crc32(0U, reference,
                                         sizeof reference - 1U));
}

void test_file_journal_rejects_oversize_before_read(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/oversize.journal", g_dir);
    remove(path);
    FILE *f = NULL;
#ifdef _WIN32
    (void)fopen_s(&f, path, "wb");
#else
    f = fopen(path, "wb");
#endif
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_EQUAL_INT(0, fseek(f, (long)TP_JOURNAL_MAX_FILE_BYTES, SEEK_SET));
    TEST_ASSERT_NOT_EQUAL(EOF, fputc(0, f));
    TEST_ASSERT_EQUAL_INT(0, fclose(f));

    tp_journal_peek_result pk;
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OUT_OF_BOUNDS,
                          tp_journal_peek(tp_journal_io_file_read(path), &pk, &err));
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "supported byte bound"));
    remove(path);
}

/* Writer/reader size contract: every record acknowledged by a file-backed journal must leave the
 * store readable by the same build. These tests use the public 64 MiB cap rather than a test-only
 * override so a future reader/writer drift cannot be hidden by separate test configuration. */
void test_file_journal_initial_checkpoint_cannot_exceed_reader_cap(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/cap-initial.journal", g_dir);
    remove(path);

    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0xC1));
    TEST_ASSERT_NOT_NULL(j);
    const size_t frame_bytes = (size_t)TP_JRN_HEADER_LEN + (size_t)TP_JRN_SYNC_FIELD +
                               (size_t)TP_JRN_LEN_FIELD + (size_t)TP_JRN_CRC_FIELD +
                               (size_t)TP_JRN_CKPT_FIXED;
    const size_t snapshot_len = (size_t)TP_JOURNAL_MAX_FILE_BYTES - frame_bytes + 1U;
    uint8_t *snapshot = (uint8_t *)malloc(snapshot_len);
    TEST_ASSERT_NOT_NULL(snapshot);

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_journal_init_checkpoint(j, snapshot, snapshot_len, 0, &err));
    TEST_ASSERT_EQUAL_INT64(0, io.length(io.ctx)); /* rejected before even writing the header */
    free(snapshot);
    tp_journal_destroy(j);
    remove(path);
}

void test_file_journal_transaction_rejected_before_crossing_reader_cap(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/cap-transaction.journal", g_dir);
    remove(path);

    tp_id128 key = tp_test_id_of(0xC2);
    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_project *p = tp_test_base_project();
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_attach_journal(m, j, &err));
    const int64_t valid_len = io.length(io.ctx);
    TEST_ASSERT_TRUE(valid_len > 0);
    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES - 1U));
    char *before = serialize(tp_model_project(m));

    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          commit_rename(m, "c2000000000000000000000000000001", 0, "must-not-commit"));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT64((int64_t)TP_JOURNAL_MAX_FILE_BYTES - 1, io.length(io.ctx));

    free(before);
    free(after);
    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)valid_len));
    /* Limit rejection retained neither bytes nor the id: the exact transaction is retryable. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m, "c2000000000000000000000000000001", 0, "must-not-commit"));
    TEST_ASSERT_EQUAL_STRING("must-not-commit", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    tp_model_destroy(m); /* owns and closes j/io */
    remove(path);
}

void test_file_journal_undo_rejected_before_crossing_reader_cap(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/cap-undo.journal", g_dir);
    remove(path);

    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0xC3));
    TEST_ASSERT_NOT_NULL(j);
    tp_model *m = tp_model_wrap(tp_test_base_project());
    TEST_ASSERT_NOT_NULL(m);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_attach_journal(m, j, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m, "c3000000000000000000000000000001", 0, "committed"));
    const int64_t valid_len = io.length(io.ctx);
    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES - 1U));
    const int undo_before = tp_model_undo_depth(m);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("committed", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(undo_before, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT64((int64_t)TP_JOURNAL_MAX_FILE_BYTES - 1, io.length(io.ctx));

    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)valid_len));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err)); /* same cursor move remains retryable */
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(2, tp_model_revision(m));
    tp_model_destroy(m);
    remove(path);
}

void test_file_journal_metadata_failure_at_reader_cap_is_reported(void) {
    if (!g_dir) {
        TEST_IGNORE_MESSAGE("no scratch dir (argv[1]) provided");
        return;
    }
    char path[1024];
    (void)snprintf(path, sizeof path, "%s/cap-metadata.journal", g_dir);
    remove(path);

    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0xC4));
    TEST_ASSERT_NOT_NULL(j);
    const uint8_t snapshot[] = {'o', 'k'};
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(j, snapshot, sizeof snapshot, 0, &err));
    const int64_t valid_len = io.length(io.ctx);
    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)TP_JOURNAL_MAX_FILE_BYTES - 1U));

    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK,
                          tp_journal_set_metadata(j, 123, "/cached/project.ntpacker_project", "cached", &err));
    TEST_ASSERT_EQUAL_INT64((int64_t)TP_JOURNAL_MAX_FILE_BYTES - 1, io.length(io.ctx));

    TEST_ASSERT_EQUAL_INT(0, io.truncate(io.ctx, (size_t)valid_len));
    /* Byte-cap admission is deterministic and happens before cache mutation.
     * Reclaiming space and compacting must not resurrect rejected metadata. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_compact(j, snapshot, sizeof snapshot, 0, &err));
    tp_journal_destroy(j);
    tp_journal_peek_result pk;
    memset(&pk, 0, sizeof pk);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(tp_journal_io_file_read(path), &pk, &err));
    TEST_ASSERT_FALSE(pk.has_meta);
    tp_journal_peek_free(&pk);
    remove(path);
}

/* ==== F2-04 FIX PASS: one genuine case per correctness fix (C1-C5) ========= */

/* C1: a journal attached AFTER journal-less commits must inherit the model's already-
 * retained ids, so a re-submit de-duplicates instead of double-applying (§7.2). */
void test_attach_migrates_retained_ids(void) {
    tp_id128 key = tp_test_id_of(0x6A);
    tp_project *p = tp_test_base_project();
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    /* Commit journal-LESS: the id lands only in the in-memory idstore. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1a000000000000000000000000000001", 0, "one"));

    /* Attach a journal AFTER the commit. C1: the pre-attach id migrates into the
     * journal's retained-id index (else a re-submit double-applies). */
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_attach_journal(m, j, &err));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m), "1a000000000000000000000000000001"));

    /* Re-submitting the pre-attach id now rejects as a duplicate (model unchanged). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_rename(m, "1a000000000000000000000000000001", tp_model_revision(m), "one-again"));

    /* Recovery sees the migrated id too (the initial checkpoint carried it durably). */
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "1a000000000000000000000000000001"));
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C2 (torn tail): an incomplete final record IS truncated back to the last good
 * record, and continued appends work (the journal stays healthy). */
void test_torn_tail_is_truncated(void) {
    tp_id128 key = tp_test_id_of(0x6D);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1d000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1d000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Tear 3 bytes off the tail -> the last record is incomplete (TRUNCATED). */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, info.status);
    TEST_ASSERT_FALSE(info.mid_stream_corrupt);
    TEST_ASSERT_NOT_NULL(m2);

    /* The torn tail IS truncated away: the store now ends at the last good record. */
    size_t after_len = 0;
    uint8_t *after = snapshot_io(io2, &after_len);
    TEST_ASSERT_EQUAL_INT64((int64_t)info.stop_offset, (int64_t)after_len);
    free(after);

    /* Re-append the unacknowledged txn -> succeeds (journal not poisoned), no dup. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m2, "1d000000000000000000000000000002", tp_model_revision(m2), "two"));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "1d000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C2 (mid-stream): a bad-CRC record with a valid record STILL after it must NOT be
 * truncated -- that would delete the trailing acknowledged record. Recover up to the
 * last good record, PRESERVE the file, and poison the journal against appends behind
 * the corruption. This is the crux fix: torn-tail (truncate) vs mid-stream (preserve). */
void test_midstream_corrupt_preserves_trailing(void) {
    tp_id128 key = tp_test_id_of(0x6E);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    /* Store layout: header | checkpoint(#0) | txn01(#1) | txn02(#2). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1e000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1e000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Corrupt a byte inside txn01 (record #1) -> mid-stream; txn02 (#2) stays intact. */
    size_t t1 = record_offset(full, full_len, 1);
    TEST_ASSERT_TRUE(t1 != SIZE_MAX && t1 < full_len);
    size_t at = t1 + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + 1; /* a payload byte of txn01 */
    tp_journal_io io2 = io_from_bytes(full, full_len);
    tp_journal_io_memory__poke(io2, at, (uint8_t)(full[at] ^ 0xFFu));

    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    /* Reported as CORRUPT + mid-stream; up-to-last-good recovered (the checkpoint, rev 0). */
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, info.status);
    TEST_ASSERT_TRUE(info.mid_stream_corrupt);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m2));

    /* The trailing acknowledged record was NOT deleted: the store length is unchanged. */
    size_t after_len = 0;
    uint8_t *after = snapshot_io(io2, &after_len);
    TEST_ASSERT_EQUAL_INT64((int64_t)full_len, (int64_t)after_len);
    free(after);

    /* The recovered journal refuses appends behind the corruption (poisoned). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          commit_rename(m2, "1e000000000000000000000000000003", tp_model_revision(m2), "three"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* P1-5 (plan S18 R): a mid-stream record whose LENGTH FIELD is corrupted to a bloated value
 * must NOT masquerade as a torn tail. Under the pre-v2 length-first framing the overrun
 * classified as TRUNCATED and the tail-clean truncate DELETED the acknowledged records after
 * it (violating C2 + ADR 0013). The per-record sync-word lets recovery find the valid
 * trailing record and classify this as CORRUPT + mid-stream: the file is preserved and the
 * journal poisoned. This is the exact regression the sync-word closes. */
void test_midstream_bloated_length_preserves_trailing(void) {
    tp_id128 key = tp_test_id_of(0x6F);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    /* Store layout: header | checkpoint(#0) | txn01(#1) | txn02(#2). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Bloat txn01's length field (the 4 bytes right after its sync-word) to ~4 GB. Under the
     * old framing this looked like a torn tail; the intact sync-word before txn02 proves an
     * acknowledged record still follows, so it is a mid-stream corruption, not a tail. */
    size_t t1 = record_offset(full, full_len, 1);
    TEST_ASSERT_TRUE(t1 != SIZE_MAX && t1 < full_len);
    tp_journal_io io2 = io_from_bytes(full, full_len);
    for (int b = 0; b < TP_JRN_LEN_FIELD; b++) {
        tp_journal_io_memory__poke(io2, t1 + (size_t)TP_JRN_SYNC_FIELD + (size_t)b, 0xFFu);
    }

    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, info.status);
    TEST_ASSERT_TRUE(info.mid_stream_corrupt);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT64(0, tp_model_revision(m2)); /* recovered up to the checkpoint (rev 0) */

    /* The trailing acknowledged record was NOT deleted: the store length is unchanged. */
    size_t after_len = 0;
    uint8_t *after = snapshot_io(io2, &after_len);
    TEST_ASSERT_EQUAL_INT64((int64_t)full_len, (int64_t)after_len);
    free(after);

    /* Poisoned: refuses appends behind the corruption. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          commit_rename(m2, "1f000000000000000000000000000003", tp_model_revision(m2), "three"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C3: if the recovery tail-clean truncate itself fails, the journal is poisoned -- a
 * still-present torn record must never hide a later acknowledged append. */
void test_truncate_failure_poisons_recovery(void) {
    tp_id128 key = tp_test_id_of(0x6F);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Torn tail + the recovery tail-clean truncate is injected to FAIL. */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_journal_io_memory__fail_next_truncate(io2);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, info.status);
    TEST_ASSERT_NOT_NULL(m2);

    /* The torn tail could not be cleaned -> the recovered journal is poisoned. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          commit_rename(m2, "1f000000000000000000000000000003", tp_model_revision(m2), "three"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C4: a torn PARTIAL header (crash mid initial 28-byte write) is re-initializable, not
 * a permanent brick -- while a COMPLETE but foreign header is still refused. */
void test_torn_header_reinitializable(void) {
    tp_id128 key = tp_test_id_of(0x70);
    tp_error err;
    /* A crash during the initial header write leaves a sub-header (10-byte) partial. */
    uint8_t junk[10] = {'N', 'T', 'P', 'K', 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    tp_journal_io io = io_from_bytes(junk, sizeof junk);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);

    /* ensure_header resets the torn partial header + writes a fresh one -> append works. */
    const uint8_t snap[] = {'h', 'i'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "20000000000000000000000000000001", 1, snap, sizeof snap, &err));
    TEST_ASSERT_TRUE(tp_journal_contains(j, "20000000000000000000000000000001"));

    /* The re-initialized store recovers cleanly. */
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_journal_destroy(j);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal *j2 = tp_journal_create(io2, key);
    TEST_ASSERT_NOT_NULL(j2);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j2, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, rec.status);
    TEST_ASSERT_TRUE(tp_journal_contains(j2, "20000000000000000000000000000001"));
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j2);

    /* Preserve the real protection: a COMPLETE but foreign header (wrong magic) is
     * REFUSED (BAD_MAGIC -- not our file), never reset. */
    uint8_t foreign[TP_JRN_HEADER_LEN];
    memset(foreign, 0, sizeof foreign); /* all-zero magic = not "NTPKJRNL" */
    tp_jrn_put_u32(foreign + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(foreign + TP_JRN_KEY_OFF, key.bytes, 16);
    tp_journal_io io3 = io_from_bytes(foreign, sizeof foreign);
    tp_journal *j3 = tp_journal_create(io3, key);
    TEST_ASSERT_NOT_NULL(j3);
    tp_journal_recovery rec3;
    memset(&rec3, 0, sizeof rec3);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j3, &rec3, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_BAD_MAGIC, rec3.status);
    tp_journal_recovery_free(&rec3);
    tp_journal_destroy(j3);
}

/* C5: a crash-recovered model is ahead of the on-disk project file -> it must report
 * dirty until an explicit Save re-baselines it (else save-on-dirty-shutdown loses it). */
void test_recovered_model_is_dirty(void) {
    tp_id128 key = tp_test_id_of(0x71);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "21000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_error err;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);

    /* Recovered committed state is ahead of any on-disk project file -> DIRTY. */
    TEST_ASSERT_TRUE(tp_model_dirty(m2));
    /* An explicit Save re-baselines it clean. */
    tp_model_mark_saved(m2);
    TEST_ASSERT_FALSE(tp_model_dirty(m2));

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ==== R3: compaction on Save (plan S18 R / spec §22.3) ==================== */

/* R3(a,b,d): after N commits, compaction resets the store to ONE checkpoint == the current
 * committed state. A recover then yields records_recovered==1 + op_count==0 (the store shrank to
 * a single checkpoint), the FINAL revision, the SAME serialized project, and the full retained-id
 * set re-persisted from the live index into the fresh checkpoint. */
void test_compaction_resets_to_one_checkpoint(void) {
    tp_id128 key = tp_test_id_of(0x80);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io); /* checkpoint at rev 0 */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "80000000000000000000000000000001", 0, "alpha"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "80000000000000000000000000000002", 1, "beta"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "80000000000000000000000000000003", 2, "gamma"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(3, exp_rev);

    /* Store before compaction: header + checkpoint + 3 txns. */
    size_t pre_len = 0;
    uint8_t *pre = snapshot_io(io, &pre_len);
    free(pre);

    /* Compact: the store becomes header + exactly one checkpoint == the current state. */
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    size_t post_len = 0;
    uint8_t *post = snapshot_io(io, &post_len);
    TEST_ASSERT_TRUE(post_len < pre_len); /* the 3 txn records were dropped */

    /* In-memory: the retained-id index survived compaction (idempotency authority intact). */
    TEST_ASSERT_EQUAL_INT(3, tp_journal_id_count(tp_model_journal(m)));
    tp_model_destroy(m);

    /* Recover from the compacted bytes: exactly one record (the checkpoint), no ops to replay. */
    tp_journal_io io2 = io_from_bytes(post, post_len);
    free(post);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT(1, info.records_recovered); /* (a) one checkpoint */
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);     /* (a) no post-checkpoint ops to replay */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2)); /* (b) revision preserved */

    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got); /* (d) recovers exactly the saved state */
    /* durable id set preserved: all three acknowledged ids recovered from the fresh checkpoint */
    TEST_ASSERT_EQUAL_INT(3, tp_journal_id_count(tp_model_journal(m2)));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "80000000000000000000000000000001"));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m2), "80000000000000000000000000000003"));

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R3(c): compaction PRESERVES the retained-id set -- a re-submit of an already-acknowledged id
 * AFTER compaction is still an idempotent no-op (DUPLICATE_ID), both in-memory and after a
 * crash-recover. Guards the "only the byte store is truncated, j->ids is kept" invariant: if
 * compaction reset the id index, an acked id would double-apply after Save (§7.2). */
void test_compaction_preserves_retained_ids(void) {
    tp_id128 key = tp_test_id_of(0x81);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "81000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "81000000000000000000000000000002", 1, "two"));
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));

    /* In-memory: a re-submit of an acked id post-compaction rejects as a duplicate (model unchanged). */
    char *before = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_rename(m, "81000000000000000000000000000001", tp_model_revision(m), "one-again"));
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    /* After a crash-recover from the compacted store the id set is STILL the idempotency authority. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(tp_model_journal(m2)));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_rename(m2, "81000000000000000000000000000002", tp_model_revision(m2), "two-again"));
    /* A NEW id still commits + appends onto the compacted journal (it is healthy, not poisoned). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m2, "81000000000000000000000000000003", tp_model_revision(m2), "three"));

    free(before);
    free(after);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R3(e): a commit AFTER compaction recovers as the compacted checkpoint + exactly ONE replay op
 * (the diff journal resumed from the fresh baseline). The base checkpoint holds the compacted
 * state; the post-compaction edit lives ONLY in the replayed op-payload. */
void test_compaction_then_commit_recovers_as_ckpt_plus_op(void) {
    tp_id128 key = tp_test_id_of(0x82);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "82000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "82000000000000000000000000000002", 1, "two"));
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err)); /* checkpoint == rev 2 ("two") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "82000000000000000000000000000003", 2, "three"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(3, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, info.records_recovered); /* compacted checkpoint + 1 txn */
    TEST_ASSERT_EQUAL_INT(1, (int)info.op_count);     /* (e) exactly one post-checkpoint op */
    TEST_ASSERT_TRUE(span_contains(info.snapshot, info.snapshot_len, "two"));
    TEST_ASSERT_FALSE(span_contains(info.snapshot, info.snapshot_len, "three"));
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R3: tp_model_compact_journal is a no-op (returns OK, mutates nothing) when no journal is
 * attached -- a journal-less model (recovery disabled) Saves without a compaction error. */
void test_compaction_no_journal_is_noop(void) {
    tp_project *p = tp_test_base_project();
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NULL(tp_model_journal(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "83000000000000000000000000000001", 0, "solo"));
    char *before = serialize(tp_model_project(m));
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err)); /* no journal -> no-op */
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_NULL(tp_model_journal(m));
    free(before);
    free(after);
    tp_model_destroy(m);
}

/* R3: compaction fails CLOSED on a truncate failure -- the store is left INTACT (the old
 * checkpoint + records survive) and TP_STATUS_JOURNAL_FAILED is returned, so a Save-time
 * compaction that cannot reset the store never silently loses the existing recovery log. The
 * journal stays healthy (a FAILED compaction does not poison it): continued appends work. */
void test_compaction_truncate_failure_is_fault(void) {
    tp_id128 key = tp_test_id_of(0x84);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "84000000000000000000000000000001", 0, "one"));
    size_t pre_len = 0;
    uint8_t *pre = snapshot_io(io, &pre_len);

    /* The compaction truncate-to-0 is injected to FAIL -> fault, store byte-for-byte unchanged. */
    tp_journal_io_memory__fail_next_truncate(io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_compact_journal(m, &err));
    size_t post_len = 0;
    uint8_t *post = snapshot_io(io, &post_len);
    TEST_ASSERT_EQUAL_INT64((int64_t)pre_len, (int64_t)post_len); /* store intact */
    TEST_ASSERT_EQUAL_MEMORY(pre, post, pre_len);

    /* Not poisoned by the FAILED compaction -- a normal append still works. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "84000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m), "84000000000000000000000000000002"));

    free(pre);
    free(post);
    tp_model_destroy(m);
}

/* R3 fail-closed (core): the truncate-OK / init-fail window. If the compaction truncate SUCCEEDS
 * (the old checkpoint + records are gone) but the fresh checkpoint then FAILS to write, the store is
 * left checkpoint-LESS. Appending onto it would recover to NOTHING after a crash -> silent loss. The
 * primitive MUST fail closed: poison the journal so further appends are refused. */
void test_compaction_init_failure_poisons(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0x85));
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    const uint8_t snap[] = {'s', 'n', 'a', 'p'};
    /* Seed a checkpoint + a txn so the store is non-empty before compaction. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(j, snap, sizeof snap, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "85000000000000000000000000000001", 1, snap, sizeof snap, &err));

    /* Compact: the truncate-to-0 SUCCEEDS, but the fresh checkpoint's header write is injected to FAIL
     * (fail-next-writes hits only write(), not the preceding truncate). */
    tp_journal_io_memory__fail_next_writes(io, 1);
    const uint8_t snap2[] = {'n', 'e', 'w'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_journal_compact(j, snap2, sizeof snap2, 2, &err));
    TEST_ASSERT_TRUE(tp_journal__is_poisoned(j)); /* (a) fail closed */

    /* The store is checkpoint-less: a recover finds nothing (no unrecoverable base to append onto). */
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_EMPTY, rec.status);
    TEST_ASSERT_EQUAL_INT(0, rec.records_recovered);
    TEST_ASSERT_NULL(rec.snapshot);
    tp_journal_recovery_free(&rec);

    /* Poisoned -> refuses further appends: no TXN is silently written onto the checkpoint-less store. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          tp_journal_append_txn(j, "85000000000000000000000000000002", 3, snap2, sizeof snap2, &err));
    tp_journal_destroy(j);
}

/* A failed replacement checkpoint must not silently remove recovery/idempotency authority. The
 * poisoned journal remains attached, retains acknowledged ids, and rejects later mutations with a
 * structured durability status until the owner explicitly repairs or replaces the authority. */
void test_compaction_broken_store_keeps_fail_closed_authority(void) {
    tp_id128 key = tp_test_id_of(0x86);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "86000000000000000000000000000001", 0, "one"));

    const char *acknowledged = "86000000000000000000000000000001";
    const char *blocked = "86000000000000000000000000000002";
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m), acknowledged));
    const int retained_before = tp_journal_id_count(tp_model_journal(m));

    /* Compaction: truncate OK, fresh-checkpoint write FAILS -> journal poisons itself but stays the
     * sole idempotency authority for this live model. */
    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_compact_journal(m, &err));
    TEST_ASSERT_NOT_NULL(tp_model_journal(m));
    TEST_ASSERT_TRUE(tp_journal__is_poisoned(tp_model_journal(m)));
    TEST_ASSERT_EQUAL_INT(retained_before, tp_journal_id_count(tp_model_journal(m)));
    TEST_ASSERT_TRUE(tp_journal_contains(tp_model_journal(m), acknowledged));

    tp_operation op = {0};
    op.kind = TP_OP_ATLAS_RENAME;
    op.atlas_id = tp_model_project(m)->atlases[0].id;
    op.u.atlas_rename.name = "two";
    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", blocked);
    req.expected_revision = tp_model_revision(m);
    req.ops = &op;
    req.op_count = 1;
    tp_txn_result result = {0};
    const int64_t revision_before = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_apply(m, &req, &result, &err));
    TEST_ASSERT_FALSE(result.committed);
    TEST_ASSERT_EQUAL_INT64(revision_before, result.revision);
    TEST_ASSERT_EQUAL_INT(1, result.error_count);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, result.errors[0].code);
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_FALSE(tp_journal_contains(tp_model_journal(m), blocked));
    TEST_ASSERT_EQUAL_INT(retained_before, tp_journal_id_count(tp_model_journal(m)));
    tp_txn_result_free(&result);

    /* A retry of an acknowledged id is still rejected as duplicate before touching the poisoned
     * store, proving the retained authority did not disappear with the failed checkpoint. */
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%s", acknowledged);
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, tp_model_apply(m, &req, &result, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID, result.errors[0].code);
    tp_txn_result_free(&result);

    tp_model_destroy(m);
}

/* ==== R4: journal-gated undo/redo checkpoints (P1-1) ====================== */

/* Undo is a commit, so its durable CHECKPOINT append is part of the SAME stage-then-commit gate as
 * the candidate project swap. A failed append must leave the live document, revision, history cursor,
 * and durable byte store unchanged; the one-shot failure then remains retryable. */
void test_journal_undo_append_failure_rolls_back_history_commit(void) {
    tp_id128 key = tp_test_id_of(0x87);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "87000000000000000000000000000001", 0, "one"));

    tp_project *project_ptr_before = tp_model_project(m);
    char *project_before = serialize(project_ptr_before);
    size_t journal_before_len = 0;
    uint8_t *journal_before = snapshot_io(io, &journal_before_len);
    const int64_t revision_before = tp_model_revision(m);
    const int undo_before = tp_model_undo_depth(m);
    const int redo_before = tp_model_redo_depth(m);

    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_undo(m, &err));

    char *project_after = serialize(tp_model_project(m));
    size_t journal_after_len = 0;
    uint8_t *journal_after = snapshot_io(io, &journal_after_len);
    TEST_ASSERT_EQUAL_PTR(project_ptr_before, tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(project_before, project_after);
    TEST_ASSERT_EQUAL_INT64(revision_before, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(undo_before, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT(redo_before, tp_model_redo_depth(m));
    TEST_ASSERT_EQUAL_INT64((int64_t)journal_before_len, (int64_t)journal_after_len);
    TEST_ASSERT_EQUAL_MEMORY(journal_before, journal_after, journal_before_len);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(revision_before + 1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(undo_before - 1, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT(redo_before + 1, tp_model_redo_depth(m));

    free(project_before);
    free(project_after);
    free(journal_before);
    free(journal_after);
    tp_model_destroy(m);
}

void test_peek_and_recover_share_retained_id_policy(void) {
    const tp_id128 key = tp_test_id_of(0xa6);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    tp_error err = {0};
    static const uint8_t snapshot[] = {'b', 'a', 's', 'e'};
    static const uint8_t op[] = {'{', '}'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(journal, snapshot, sizeof snapshot, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal, "a6000000000000000000000000000001",
                                                1, op, sizeof op, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(journal, "a6000000000000000000000000000002",
                                                2, op, sizeof op, &err));
    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_io(io, &bytes_len);
    tp_journal_destroy(journal);

    const size_t first = record_offset(bytes, bytes_len, 1);
    const size_t second = record_offset(bytes, bytes_len, 2);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, first);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, second);
    const size_t header = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD;
    memcpy(bytes + second + header + 1U, bytes + first + header + 1U, TP_JRN_IDLEN);
    const uint32_t payload_len = tp_jrn_get_u32(bytes + second + TP_JRN_SYNC_FIELD);
    const size_t crc_span = header + (size_t)payload_len;
    tp_jrn_put_u32(bytes + second + crc_span, tp_jrn_crc32(0, bytes + second, crc_span));

    tp_journal *recover_journal = tp_journal_create(io_from_bytes(bytes, bytes_len), key);
    TEST_ASSERT_NOT_NULL(recover_journal);
    tp_journal_recovery recovery = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_recover(recover_journal, &recovery, &err));
    tp_journal_peek_result peek = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_peek(io_from_bytes(bytes, bytes_len), &peek, &err));
    free(bytes);

    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, recovery.status);
    TEST_ASSERT_EQUAL_INT(recovery.status, peek.status);
    TEST_ASSERT_EQUAL_INT(recovery.records_recovered, peek.record_count);
    TEST_ASSERT_EQUAL_INT(2, recovery.records_recovered);
    tp_journal_recovery_free(&recovery);
    tp_journal_peek_free(&peek);
    tp_journal_destroy(recover_journal);
}

/* Redo has the identical durable gate: if its checkpoint append fails, the already-undone live
 * state and cursor stay exactly where they were. */
void test_journal_redo_append_failure_rolls_back_history_commit(void) {
    tp_id128 key = tp_test_id_of(0x88);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "88000000000000000000000000000001", 0, "one"));
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));

    tp_project *project_ptr_before = tp_model_project(m);
    char *project_before = serialize(project_ptr_before);
    size_t journal_before_len = 0;
    uint8_t *journal_before = snapshot_io(io, &journal_before_len);
    const int64_t revision_before = tp_model_revision(m);
    const int undo_before = tp_model_undo_depth(m);
    const int redo_before = tp_model_redo_depth(m);

    tp_journal_io_memory__fail_next_writes(io, 1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_redo(m, &err));

    char *project_after = serialize(tp_model_project(m));
    size_t journal_after_len = 0;
    uint8_t *journal_after = snapshot_io(io, &journal_after_len);
    TEST_ASSERT_EQUAL_PTR(project_ptr_before, tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(project_before, project_after);
    TEST_ASSERT_EQUAL_INT64(revision_before, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(undo_before, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT(redo_before, tp_model_redo_depth(m));
    TEST_ASSERT_EQUAL_INT64((int64_t)journal_before_len, (int64_t)journal_after_len);
    TEST_ASSERT_EQUAL_MEMORY(journal_before, journal_after, journal_before_len);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(revision_before + 1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(undo_before + 1, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_INT(redo_before - 1, tp_model_redo_depth(m));

    free(project_before);
    free(project_after);
    free(journal_before);
    free(journal_after);
    tp_model_destroy(m);
}

/* A full step budget needs an eviction plan, but the old record must remain owned
 * until the replacement transaction's journal append is acknowledged. */
void test_history_eviction_waits_for_journal_append_ack(void) {
    tp_history__test_set_limits(1, TP_HISTORY_MAX_BYTES, TP_HISTORY_MAX_RECORD_BYTES);
    tp_journal_io io;
    tp_model *m = model_with_journal(tp_test_id_of(0x89), &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "89000000000000000000000000000001", 0, "one"));
    const size_t history_bytes = tp_model_history(m)->bytes;
    size_t journal_before_len = 0U;
    uint8_t *journal_before = snapshot_io(io, &journal_before_len);

    tp_journal_io_memory__fail_next_writes(io, 1);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED,
                          commit_rename(m, "89000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(m));
    TEST_ASSERT_EQUAL_UINT64(history_bytes, tp_model_history(m)->bytes);
    size_t journal_after_len = 0U;
    uint8_t *journal_after = snapshot_io(io, &journal_after_len);
    TEST_ASSERT_EQUAL_UINT64(journal_before_len, journal_after_len);
    TEST_ASSERT_EQUAL_MEMORY(journal_before, journal_after, journal_before_len);

    /* The identical id remains retryable. Only the successful ACK replaces the
     * retained Undo record, so Undo still restores the previous committed state. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "89000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT(1, tp_model_undo_depth(m));
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_FALSE(tp_model_can_undo(m));

    free(journal_before);
    free(journal_after);
    tp_model_destroy(m);
}

void test_revision_max_rejects_commit_and_history_without_overflow(void) {
    tp_model *m = tp_model_wrap(tp_test_base_project());
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m, "8f000000000000000000000000000001", 0, "one"));
    const int undo_before = tp_model_undo_depth(m);
    const char *name_before = tp_model_project(m)->atlases[0].name;
    char name_copy[64];
    (void)snprintf(name_copy, sizeof name_copy, "%s", name_before);

    tp_model__test_set_revision(m, INT64_MAX); /* a CRC-valid hostile recovery record can carry any i64 */
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION,
                          commit_rename(m, "8f000000000000000000000000000002", INT64_MAX, "two"));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, tp_model_revision(m));
    TEST_ASSERT_EQUAL_STRING(name_copy, tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(undo_before, tp_model_undo_depth(m));

    memset(&err, 0, sizeof err);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_REVISION, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_INT64(INT64_MAX, tp_model_revision(m));
    TEST_ASSERT_EQUAL_STRING(name_copy, tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(undo_before, tp_model_undo_depth(m));
    tp_model_destroy(m);
}

void test_recovery_rejects_invalid_or_nonmonotonic_revisions(void) {
    tp_error err = {0};
    const uint8_t snap[] = {'{', '}'};

    tp_journal_io io = tp_journal_io_memory();
    tp_journal *j = tp_journal_create(io, tp_test_id_of(0x8E));
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(j, snap, sizeof snap, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "8e000000000000000000000000000001", 0,
                                                snap, sizeof snap, &err));
    size_t len = 0;
    uint8_t *bytes = snapshot_io(io, &len);
    tp_journal_destroy(j);
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, len), &peek, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, peek.status);
    TEST_ASSERT_EQUAL_INT(1, peek.record_count); /* retain only the valid revision-0 checkpoint */
    tp_journal_peek_free(&peek);
    free(bytes);

    io = tp_journal_io_memory();
    j = tp_journal_create(io, tp_test_id_of(0x8D));
    TEST_ASSERT_NOT_NULL(j);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_init_checkpoint(j, snap, sizeof snap, INT64_MAX, &err));
    bytes = snapshot_io(io, &len);
    tp_journal_destroy(j);
    memset(&peek, 0, sizeof peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, len), &peek, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, peek.status);
    TEST_ASSERT_EQUAL_INT(0, peek.record_count);
    tp_journal_peek_free(&peek);
    free(bytes);
}

/* Undo appends (never truncates) a full checkpoint from the candidate state before publishing it.
 * Compact first to model a clean Save baseline: one history checkpoint must take record_count 1->2,
 * which is exactly the startup scan's adoptable-unsaved threshold, and crash recovery must load the
 * undone state without a GUI post-history hook. */
void test_journal_undo_recovers_undone_state(void) {
    tp_id128 key = tp_test_id_of(0x89);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);          /* checkpoint at rev 0 (atlas "atlas1") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m)); /* undo needs the diff history */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "89000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    size_t saved_len = 0;
    uint8_t *saved_bytes = snapshot_io(io, &saved_len);
    tp_journal_peek_result saved_peek;
    memset(&saved_peek, 0, sizeof saved_peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(saved_bytes, saved_len), &saved_peek, &err));
    TEST_ASSERT_EQUAL_INT(1, saved_peek.record_count);
    tp_journal_peek_free(&saved_peek);
    free(saved_bytes);

    /* UNDO -> back to "atlas1"; the durable append is inside tp_model_undo. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_model_project(m)->atlases[0].name);
    int64_t undone_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(2, undone_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &peek, &err));
    TEST_ASSERT_EQUAL_INT(2, peek.record_count); /* clean checkpoint + undo checkpoint => scan candidate */
    tp_journal_peek_free(&peek);
    tp_model_destroy(m);

    /* Recover: the last checkpoint is the UNDONE state, no ops to replay after it. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, info.records_recovered);
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);     /* nothing to replay -> cannot resurrect "two" */
    TEST_ASSERT_EQUAL_INT64(undone_rev, tp_model_revision(m2));
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_model_project(m2)->atlases[0].name);

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* Redo is symmetric. Compact the successfully undone state to one clean checkpoint, then redo must
 * append a second checkpoint (scan candidate) and recover the redone document after a crash. */
void test_journal_redo_recovers_redone_state(void) {
    tp_id128 key = tp_test_id_of(0x8a);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "8a000000000000000000000000000001", 0, "one"));

    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err)); /* -> "atlas1" (rev 2) */
    TEST_ASSERT_EQUAL_STRING("atlas1", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err)); /* -> "one" (rev 3) */
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    int64_t redone_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(3, redone_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_journal_peek_result peek;
    memset(&peek, 0, sizeof peek);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &peek, &err));
    TEST_ASSERT_EQUAL_INT(2, peek.record_count); /* clean checkpoint + redo checkpoint => scan candidate */
    tp_journal_peek_free(&peek);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, info.records_recovered);
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);
    TEST_ASSERT_EQUAL_INT64(redone_rev, tp_model_revision(m2));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m2)->atlases[0].name); /* the REDONE state */

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ==== R5a: journal metadata record + peek API ============================== */

/* R5a: a metadata record {timestamp, path, name} round-trips through recovery, and does NOT disturb
 * replay -- the rename still applies and the revision is right (META is captured-and-skipped). */
void test_metadata_roundtrip(void) {
    tp_id128 key = tp_test_id_of(0x90);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700000000, "/foo/bar.ntpacker", "bar", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "90000000000000000000000000000001", 0, "renamed"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(1, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);

    /* metadata recovered exactly */
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(1700000000, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/foo/bar.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("bar", info.metadata.name);

    /* state still recovers correctly (META did not disturb the txn replay) */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_STRING("renamed", tp_model_project(m2)->atlases[0].name);

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* Recovery Save-to-original needs the exact saved-file baseline that existed when the
 * journal metadata was recorded. The optional v3 metadata trailer carries that fingerprint
 * without invalidating older v3 records, and compaction must preserve it. */
void test_metadata_fingerprint_roundtrip_and_compaction(void) {
    tp_id128 key = tp_test_id_of(0x9b);
    tp_id128 fingerprint = tp_test_id_of(0xc7);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_set_metadata_ex(tp_model_journal(m), 1700012345, "/fingerprint/p.ntpacker_project", "p",
                                   &fingerprint, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_peek_result pk;
    memset(&pk, 0, sizeof pk);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &pk, &err));
    TEST_ASSERT_TRUE(pk.has_meta);
    TEST_ASSERT_TRUE(pk.meta.has_file_fingerprint);
    TEST_ASSERT_EQUAL_MEMORY(fingerprint.bytes, pk.meta.file_fingerprint.bytes, 16);
    tp_journal_peek_free(&pk);

    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io_from_bytes(bytes, blen), key, TP_STATUS_OK, &info, &err);
    free(bytes);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_TRUE(info.metadata.has_file_fingerprint);
    TEST_ASSERT_EQUAL_MEMORY(fingerprint.bytes, info.metadata.file_fingerprint.bytes, 16);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

void test_recovery_metadata_oom_cleans_materialized_descriptors(void) {
    const tp_id128 key = tp_test_id_of(0x91);
    tp_journal_io io;
    tp_model *model = model_with_journal(key, &io);
    tp_error err = {{0}};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_set_metadata(tp_model_journal(model), 1700000001,
                                "/foo/oom.ntpacker", "oom", &err));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        commit_rename(model, "91000000000000000000000000000001", 0,
                      "renamed"));
    size_t length = 0U;
    uint8_t *bytes = snapshot_and_destroy(io, model, &length);

    tp_journal *journal = tp_journal_create(io_from_bytes(bytes, length), key);
    free(bytes);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    tp_journal__test_fail_next_metadata_materialize();
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OOM,
                          tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_NULL(recovery.ops);
    TEST_ASSERT_EQUAL_UINT64(0U, recovery.op_count);
    TEST_ASSERT_NULL(recovery.snapshot);
    TEST_ASSERT_NULL(recovery._raw_record_buffer);
    TEST_ASSERT_FALSE(recovery.has_metadata);

    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

/* A relative source key accepted after a checkpoint must be journaled with the
 * same project-relative identity that apply stores. Recovery loads a self-contained
 * checkpoint (no project_dir), so replay proves that the durable payload is already
 * canonical rather than depending on process CWD. Exercise both ADD and REPLACE. */
void test_compaction_relative_source_ops_recover_canonical_paths(void) {
    if (!g_dir) {
        return;
    }
    char project_path[1024];
    (void)snprintf(project_path, sizeof project_path,
                   "%s/relative-source-replay.ntpacker", g_dir);
    tp_project *project = tp_test_base_project();
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_save(project, project_path, &err));
    tp_model *model = tp_model_wrap(project);
    TEST_ASSERT_NOT_NULL(model);
    const tp_id128 key = tp_test_id_of(0xc2);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_attach_journal(model, journal, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_compact_journal(model, &err));

    tp_operation add = {0};
    add.kind = TP_OP_SOURCE_ADD;
    add.atlas_id = project->atlases[0].id;
    add.u.source_add.source_id = tp_test_id_of(0x31);
    add.u.source_add.kind = TP_SOURCE_KIND_FOLDER;
    add.u.source_add.key = (char *)"future/add";
    tp_txn_request request = {0};
    request.schema = TP_TXN_SCHEMA;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   "c2000000000000000000000000000001");
    request.expected_revision = 0;
    request.ops = &add;
    request.op_count = 1;
    tp_txn_result result = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &err));
    tp_txn_result_free(&result);

    tp_operation replace = {0};
    replace.kind = TP_OP_SOURCE_REPLACE;
    replace.atlas_id = add.atlas_id;
    replace.u.source_ref.source_id = add.u.source_add.source_id;
    replace.u.source_ref.key = (char *)"future/replaced";
    request.ops = &replace;
    request.expected_revision = 1;
    (void)snprintf(request.id_hex, sizeof request.id_hex, "%s",
                   "c2000000000000000000000000000002");
    memset(&result, 0, sizeof result);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_apply(model, &request, &result, &err));
    tp_txn_result_free(&result);

    const tp_project *live_project = tp_model_project(model);
    const tp_project_source *live =
        tp_project_atlas_find_source_by_id(&live_project->atlases[0],
                                           add.u.source_add.source_id);
    TEST_ASSERT_NOT_NULL(live);
    char expected[4096];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_project_resolve_path(live_project, "future/replaced",
                                                  expected, sizeof expected));
    TEST_ASSERT_EQUAL_STRING(expected, live->path);
    const tp_id128 expected_identity = tp_semantic_identity(live_project);

    size_t bytes_len = 0U;
    uint8_t *bytes = snapshot_and_destroy(io, model, &bytes_len);
    tp_journal_recovery info;
    tp_model *recovered = recover_expect(io_from_bytes(bytes, bytes_len), key, TP_STATUS_OK, &info, &err);
    free(bytes);
    TEST_ASSERT_NOT_NULL(recovered);
    const tp_project *rp = tp_model_project(recovered);
    const tp_project_source *replayed =
        tp_project_atlas_find_source_by_id(&rp->atlases[0],
                                           add.u.source_add.source_id);
    TEST_ASSERT_NOT_NULL(replayed);
    TEST_ASSERT_EQUAL_STRING(expected, replayed->path);
    TEST_ASSERT_TRUE(tp_id128_eq(expected_identity,
                                tp_semantic_identity(rp)));
    tp_journal_recovery_free(&info);
    tp_model_destroy(recovered);
    (void)remove(project_path);
}

void test_checkpoint_only_recovery_borrows_raw_storage_without_payload_copy(void) {
    const tp_id128 key = tp_test_id_of(0x12);
    tp_journal_io io;
    tp_model *model = model_with_journal(key, &io);
    size_t byte_count = 0U;
    uint8_t *bytes = snapshot_and_destroy(io, model, &byte_count);

    tp_journal *journal = tp_journal_create(io_from_bytes(bytes, byte_count), key);
    free(bytes);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery recovery;
    memset(&recovery, 0, sizeof recovery);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_recover(journal, &recovery, &err));
    TEST_ASSERT_NOT_NULL(recovery.snapshot);
    TEST_ASSERT_EQUAL_size_t(0U, recovery.op_count);

    tp_journal_recovery_copy_stats copies;
    tp_journal__test_recovery_copy_stats(&recovery, &copies);
    TEST_ASSERT_EQUAL_size_t(1U, copies.raw_storage_copies);
    TEST_ASSERT_EQUAL_size_t(byte_count, copies.raw_storage_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, copies.checkpoint_payload_copies);
    TEST_ASSERT_EQUAL_size_t(0U, copies.checkpoint_payload_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, copies.operation_payload_copies);
    TEST_ASSERT_EQUAL_size_t(0U, copies.operation_payload_bytes);

    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_recovery_borrows_all_op_payloads_from_one_buffer(void) {
    tp_id128 key = tp_test_id_of(0xc1);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    for (int i = 0; i < 100; ++i) {
        char id[33];
        char name[32];
        (void)snprintf(id, sizeof id, "%032x", i + 1);
        (void)snprintf(name, sizeof name, "recovery-%d", i);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, id, i, name));
    }

    size_t bytes_len = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &bytes_len);

    tp_journal *journal = tp_journal_create(io_from_bytes(bytes, bytes_len), key);
    free(bytes);
    TEST_ASSERT_NOT_NULL(journal);
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(journal, &info, &err));
    TEST_ASSERT_EQUAL_UINT64(100, info.op_count);
    TEST_ASSERT_TRUE(tp_journal__test_recovery_ops_borrow_raw(&info));
    tp_journal_recovery_copy_stats copies;
    tp_journal__test_recovery_copy_stats(&info, &copies);
    TEST_ASSERT_EQUAL_size_t(1U, copies.raw_storage_copies);
    TEST_ASSERT_EQUAL_size_t(bytes_len, copies.raw_storage_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, copies.checkpoint_payload_copies);
    TEST_ASSERT_EQUAL_size_t(0U, copies.checkpoint_payload_bytes);
    TEST_ASSERT_EQUAL_size_t(0U, copies.operation_payload_copies);
    TEST_ASSERT_EQUAL_size_t(0U, copies.operation_payload_bytes);
    for (size_t i = 0; i < info.op_count; ++i) {
        tp_txn_request *request = NULL;
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                              tp_txn_request_decode_n(info.ops[i].payload,
                                                      info.ops[i].payload_len,
                                                      &request, &err));
        TEST_ASSERT_NOT_NULL(request);
        tp_txn_request_free(request);
    }
    tp_journal_recovery_free(&info);
    tp_journal_destroy(journal);
}

void test_recovery_rejects_oversize_borrowed_op_before_parse(void) {
    tp_id128 key = tp_test_id_of(0xc2);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    const size_t payload_len = (size_t)TP_TXN_MAX_REQUEST_BYTES + 1U;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    TEST_ASSERT_NOT_NULL(payload);
    memset(payload, '{', payload_len);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_journal_append_txn(tp_model_journal(m), "c2000000000000000000000000000001", 1,
                              payload, payload_len, &err));
    free(payload);

    size_t bytes_len = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &bytes_len);
    tp_journal_recovery info;
    memset(&err, 0, sizeof err);
    tp_model *recovered = recover_expect(io_from_bytes(bytes, bytes_len), key,
                                         TP_STATUS_OUT_OF_BOUNDS, &info, &err);
    free(bytes);
    TEST_ASSERT_NULL(recovered);
    TEST_ASSERT_EQUAL_UINT64(1, info.op_count);
    TEST_ASSERT_TRUE(tp_journal__test_recovery_ops_borrow_raw(&info));
    TEST_ASSERT_NOT_NULL(strstr(err.msg, "maximum"));
    tp_journal_recovery_free(&info);
}

void test_corrupt_resync_crc_work_is_linear(void) {
    tp_id128 key = tp_test_id_of(0x70);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "20000000000000000000000000000001", 0, "one"));
    size_t base_len = 0;
    uint8_t *base = snapshot_and_destroy(io, m, &base_len);

    const size_t bad = record_offset(base, base_len, 1);
    TEST_ASSERT_NOT_EQUAL(SIZE_MAX, bad);
    base[bad + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + 1U] ^= 0xFFu;

    const size_t dense_len = 1024U * 1024U;
    uint8_t *dense = (uint8_t *)calloc(1U, dense_len);
    TEST_ASSERT_NOT_NULL(dense);
    memcpy(dense, base, base_len);
    for (size_t p = base_len; p + 65548U <= dense_len; p += 12U) {
        tp_jrn_put_u32(dense + p, TP_JRN_SYNC_WORD);
        tp_jrn_put_u32(dense + p + (size_t)TP_JRN_SYNC_FIELD, 65536U);
        /* CRC stays zero/invalid; embedded sync candidates force repeated probes. */
    }

    tp_journal_recovery info;
    tp_error err = {0};
    size_t crc_work = 0;
    TEST_ASSERT_TRUE(tp_journal__test_has_valid_record_after(dense, dense_len, base_len, &crc_work));
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(dense_len, crc_work);
    tp_model *recovered = recover_expect(io_from_bytes(dense, dense_len), key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, info.status);

    free(base);
    free(dense);
    tp_journal_recovery_free(&info);
    tp_model_destroy(recovered);
}

/* R5a: an untitled project (empty path + name) round-trips as empty strings, not NULL. */
void test_metadata_empty_path(void) {
    tp_id128 key = tp_test_id_of(0x91);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(tp_model_journal(m), 42, "", "", &err));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(42, info.metadata.timestamp);
    TEST_ASSERT_NOT_NULL(info.metadata.path);
    TEST_ASSERT_NOT_NULL(info.metadata.name);
    TEST_ASSERT_EQUAL_STRING("", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("", info.metadata.name);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R5a: two metadata records -> recovery yields the SECOND (last-wins), no leak of the first. */
void test_metadata_last_wins(void) {
    tp_id128 key = tp_test_id_of(0x92);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(tp_model_journal(m), 100, "/old/path.ntpacker", "old", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(tp_model_journal(m), 200, "/new/path.ntpacker", "new", &err));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(200, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/new/path.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("new", info.metadata.name);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R5a: metadata survives compaction -- compact re-emits the cached META record, so recovery from the
 * compacted store still carries it AND recovers the post-compaction state. */
void test_metadata_survives_compaction(void) {
    tp_id128 key = tp_test_id_of(0x93);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700001234, "/proj/p.ntpacker", "p", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    /* compacted store = header + one checkpoint + the re-emitted META (no post-checkpoint ops) */
    TEST_ASSERT_EQUAL_INT(1, info.records_recovered);
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);
    /* metadata still present after compaction */
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(1700001234, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/proj/p.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("p", info.metadata.name);
    /* state == post-compaction state */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R5a: peek reads header + metadata + status WITHOUT building a model. A journal with metadata + a
 * checkpoint + a txn -> status OK, has_checkpoint true, record_count 2 (META excluded), key matches,
 * meta exact. */
void test_peek_metadata(void) {
    tp_id128 key = tp_test_id_of(0x94);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700005555, "/scan/proj.ntpacker", "proj", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "94000000000000000000000000000001", 0, "renamed"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &pk, &err));
    free(bytes);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, pk.status);
    TEST_ASSERT_TRUE(pk.has_checkpoint);
    TEST_ASSERT_EQUAL_INT(2, pk.record_count); /* checkpoint + txn; META not counted */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_JOURNAL_FORMAT_VERSION, pk.format_version);
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, pk.key, 16);
    TEST_ASSERT_TRUE(pk.has_meta);
    TEST_ASSERT_EQUAL_INT64(1700005555, pk.meta.timestamp);
    TEST_ASSERT_EQUAL_STRING("/scan/proj.ntpacker", pk.meta.path);
    TEST_ASSERT_EQUAL_STRING("proj", pk.meta.name);
    tp_journal_peek_free(&pk);
}

/* R5a: peek and recover agree on the same bytes -- same status + same metadata (peek shares recover's
 * header-validate + frame-walk). */
void test_peek_and_recover_agree(void) {
    tp_id128 key = tp_test_id_of(0x95);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700009999, "/agree/a.ntpacker", "a", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "95000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    /* recover */
    tp_journal_io io_r = io_from_bytes(bytes, blen);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io_r, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);

    /* peek */
    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &pk, &err));
    free(bytes);

    /* same status + same metadata */
    TEST_ASSERT_EQUAL_INT(info.status, pk.status);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, pk.status);
    TEST_ASSERT_EQUAL_INT(info.has_metadata ? 1 : 0, pk.has_meta ? 1 : 0);
    TEST_ASSERT_TRUE(pk.has_meta);
    TEST_ASSERT_EQUAL_INT64(info.metadata.timestamp, pk.meta.timestamp);
    TEST_ASSERT_EQUAL_STRING(info.metadata.path, pk.meta.path);
    TEST_ASSERT_EQUAL_STRING(info.metadata.name, pk.meta.name);

    tp_journal_peek_free(&pk);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R5a: peek a header-only journal -> EMPTY, has_checkpoint false, no metadata. */
void test_peek_empty(void) {
    tp_id128 key = tp_test_id_of(0x96);
    uint8_t hdr[TP_JRN_HEADER_LEN];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(hdr + TP_JRN_KEY_OFF, key.bytes, 16);

    tp_error err;
    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(hdr, sizeof hdr), &pk, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_EMPTY, pk.status);
    TEST_ASSERT_FALSE(pk.has_checkpoint);
    TEST_ASSERT_EQUAL_INT(0, pk.record_count);
    TEST_ASSERT_FALSE(pk.has_meta);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)TP_JOURNAL_FORMAT_VERSION, pk.format_version);
    TEST_ASSERT_EQUAL_MEMORY(key.bytes, pk.key, 16);
    tp_journal_peek_free(&pk);
}

/* ==== R5a FIX ROUND: adversarial-review findings [0][1][2][3][5] ========== */

/* A checkpoint without its canonical path/fingerprint is not complete Save-Original authority. A rolled-
 * back META re-emit leaves document bytes recoverable but poisons this incomplete replacement authority. */
void test_compact_meta_reemit_failure_is_reported(void) {
    tp_id128 key = tp_test_id_of(0xA0);
    tp_journal_io io = faulty_io();
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_error err;
    const uint8_t snap[] = {'s', 'n', 'a', 'p'};
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_init_checkpoint(j, snap, sizeof snap, 0, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_append_txn(j, "a0000000000000000000000000000001", 1, snap, sizeof snap, &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(j, 1700000000, "/scan/p.ntpacker", "p", &err));

    /* Fail the 3rd write of the upcoming compact -- the META re-emit (after header + fresh checkpoint). */
    faulty_io_fail_at(io, 3);
    const uint8_t snap2[] = {'n', 'e', 'w'};
    TEST_ASSERT_NOT_EQUAL(TP_STATUS_OK, tp_journal_compact(j, snap2, sizeof snap2, 2, &err));
    TEST_ASSERT_TRUE(tp_journal__is_poisoned(j));

    /* Recovery INTACT: the fresh checkpoint recovered; metadata simply absent (re-emit was swallowed). */
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_journal_destroy(j);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal *j2 = tp_journal_create(io2, key);
    TEST_ASSERT_NOT_NULL(j2);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j2, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, rec.status);
    TEST_ASSERT_EQUAL_INT(1, rec.records_recovered); /* the compacted checkpoint */
    TEST_ASSERT_EQUAL_INT(0, (int)rec.op_count);
    TEST_ASSERT_NOT_NULL(rec.snapshot);
    TEST_ASSERT_TRUE(span_contains(rec.snapshot, rec.snapshot_len, "new")); /* post-compaction state */
    TEST_ASSERT_FALSE(rec.has_metadata);               /* metadata absent, recovery still works */
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j2);
}

/* A healthy rollback preserves the cache for a possible compaction, but the authority caller still sees
 * the failed durable META append and can detach the slot before stale metadata becomes destructive. */
void test_set_metadata_write_failure_is_reported_but_still_caches(void) {
    tp_id128 key = tp_test_id_of(0xA1);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io); /* header + initial checkpoint durably written */
    tp_error err;

    tp_journal_io_memory__fail_next_writes(io, 1); /* fail ONLY the META durable write (header present) */
    TEST_ASSERT_NOT_EQUAL(
        TP_STATUS_OK,
        tp_journal_set_metadata(tp_model_journal(m), 1700000000, "/still/caches.ntpacker", "cached", &err));
    TEST_ASSERT_FALSE(tp_journal__is_poisoned(tp_model_journal(m))); /* torn META rolled back -> journal healthy */

    /* Fault exhausted -> compaction re-emits the cached (authoritative) metadata durably. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata); /* the cache was authoritative -> compaction persisted it */
    TEST_ASSERT_EQUAL_INT64(1700000000, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/still/caches.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("cached", info.metadata.name);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* [5]/parity: peek and recover share ONE frame walker, so a TORN TAIL classifies identically (same
 * status + same CKPT/TXN count). A metadata record is present so both walkers exercise the shared
 * capture-and-skip too. */
void test_peek_agrees_recover_torn_tail(void) {
    tp_id128 key = tp_test_id_of(0xA4);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700003333, "/parity/torn.ntpacker", "torn", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a4000000000000000000000000000001", 0, "one"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);
    TEST_ASSERT_TRUE(full_len > (size_t)TP_JRN_HEADER_LEN + 3);
    size_t torn_len = full_len - 3; /* tear the final frame's tail */

    tp_journal_io io_r = io_from_bytes(full, torn_len);
    tp_journal *jr = tp_journal_create(io_r, key);
    TEST_ASSERT_NOT_NULL(jr);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(jr, &rec, &err));

    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(full, torn_len), &pk, &err));

    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, rec.status);
    TEST_ASSERT_EQUAL_INT(rec.status, pk.status);                  /* same status */
    TEST_ASSERT_EQUAL_INT(rec.records_recovered, pk.record_count); /* same CKPT/TXN count (META excluded) */

    free(full);
    tp_journal_recovery_free(&rec);
    tp_journal_peek_free(&pk);
    tp_journal_destroy(jr);
}

/* [5]/parity: a MID-STREAM corrupt record followed by a valid one classifies as CORRUPT by BOTH peek and
 * recover (shared walker + shared has_valid_record_after boundary decision). */
void test_peek_agrees_recover_midstream_corrupt(void) {
    tp_id128 key = tp_test_id_of(0xA5);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    /* header | ckpt(#0) | txn01(#1) | txn02(#2) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a5000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a5000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_and_destroy(io, m, &full_len);

    /* Flip a payload byte inside txn01 (#1); txn02 (#2) stays intact -> a mid-stream corruption. */
    size_t t1 = record_offset(full, full_len, 1);
    TEST_ASSERT_TRUE(t1 != SIZE_MAX && t1 < full_len);
    size_t at = t1 + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + 1;
    full[at] = (uint8_t)(full[at] ^ 0xFFu);

    tp_journal_io io_r = io_from_bytes(full, full_len);
    tp_journal *jr = tp_journal_create(io_r, key);
    TEST_ASSERT_NOT_NULL(jr);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(jr, &rec, &err));

    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(full, full_len), &pk, &err));

    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, rec.status);
    TEST_ASSERT_EQUAL_INT(rec.status, pk.status);                  /* both CORRUPT */
    TEST_ASSERT_EQUAL_INT(rec.records_recovered, pk.record_count); /* same recovered count (the ckpt) */

    free(full);
    tp_journal_recovery_free(&rec);
    tp_journal_peek_free(&pk);
    tp_journal_destroy(jr);
}

/* [1] crash-recover seeds the journal's write-side metadata cache, so a subsequent compaction (Save/undo)
 * RE-EMITS the metadata instead of erasing the recovered project's scan label. Core crash->reattach->Save
 * cycle: recover populates j->meta -> compact re-emits -> a second recover still sees it. FAILS pre-fix
 * (recover never seeded the cache, so has_meta==false and the recompaction dropped the metadata). */
void test_recovered_metadata_survives_recompaction(void) {
    tp_id128 key = tp_test_id_of(0xA3);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(tp_model_journal(m), 1700007777, "/recov/keepme.ntpacker", "keepme", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a3000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    /* First recover: the recovered journal's write-side cache is seeded from the metadata record. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    tp_journal_recovery_free(&info);

    /* Compact the RECOVERED journal (the Save/undo hook). Pre-fix has_meta==false here -> no re-emit. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m2, &err));
    size_t blen2 = 0;
    uint8_t *bytes2 = snapshot_and_destroy(io2, m2, &blen2);

    /* Second recover from the recompacted store: metadata STILL present (path/name/time intact). */
    tp_journal_io io3 = io_from_bytes(bytes2, blen2);
    free(bytes2);
    tp_journal_recovery info2;
    tp_model *m3 = recover_expect(io3, key, TP_STATUS_OK, &info2, &err);
    TEST_ASSERT_NOT_NULL(m3);
    TEST_ASSERT_TRUE(info2.has_metadata); /* survived the recovered-journal recompaction */
    TEST_ASSERT_EQUAL_INT64(1700007777, info2.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/recov/keepme.ntpacker", info2.metadata.path);
    TEST_ASSERT_EQUAL_STRING("keepme", info2.metadata.name);
    tp_journal_recovery_free(&info2);
    tp_model_destroy(m3);
}

/* [3] version bump: a journal whose header version field is 2 (the now-previous version) is surfaced as
 * VERSION_MISMATCH by BOTH recover and peek -- never silently mis-replayed as if the new META record type
 * did not exist, never BAD_MAGIC. Bytes preserved. FAILS pre-bump (version 2 == current -> accepted). */
void test_v2_header_reads_version_mismatch(void) {
    tp_id128 key = tp_test_id_of(0xA2);
    tp_error err;
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a2000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);
    /* Overwrite the 4-byte BE format-version field (right after the 8-byte magic) with the old value 2. */
    tp_jrn_put_u32(bytes + TP_JRN_MAGIC_LEN, 2u);

    tp_journal_io io_r = io_from_bytes(bytes, blen);
    tp_journal *jr = tp_journal_create(io_r, key);
    TEST_ASSERT_NOT_NULL(jr);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(jr, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_VERSION_MISMATCH, rec.status);
    TEST_ASSERT_EQUAL_INT(0, rec.records_recovered);                       /* nothing recovered */
    TEST_ASSERT_EQUAL_INT64((int64_t)blen, (int64_t)rec.bytes_total);      /* bytes preserved */
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(jr);

    tp_journal_peek_result pk;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &pk, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_VERSION_MISMATCH, pk.status);
    TEST_ASSERT_EQUAL_UINT32(2u, pk.format_version); /* peek reports the header version */
    tp_journal_peek_free(&pk);

    free(bytes);
}

/* R5b-1: the model-level glue tp_model_set_recovery_metadata forwards {timestamp, path, name} to the
 * attached journal (the GUI calls it at the set_path identity chokepoint), and it round-trips through
 * recovery without disturbing the ordinary txn replay. */
void test_model_set_recovery_metadata_glue_roundtrips_through_recovery(void) {
    tp_id128 key = tp_test_id_of(0x93);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000001", 0, "renamed"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_set_recovery_metadata(m, 1700000000, "/x/proj.ntpacker", "proj.ntpacker", &err));

    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_journal_recovery info;
    tp_model *m2 = recover_expect(io2, key, TP_STATUS_OK, &info, &err);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(1700000000, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/x/proj.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("proj.ntpacker", info.metadata.name);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m2)); /* the committed rename still replayed (META is skipped) */
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* The no-journal (recovery off / journal-less) case is a safe no-op success -- no crash, nothing
 * durable. Recovery metadata is optional, so a bare model must not reject it. */
void test_model_set_recovery_metadata_glue_is_noop_without_journal(void) {
    tp_error err;
    tp_model *bare = tp_model_wrap(tp_test_base_project());
    TEST_ASSERT_NOT_NULL(bare);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_set_recovery_metadata(bare, 42, "/ignored", "ignored", &err));
    tp_model_destroy(bare);
}

/* A NULL model is INVALID_ARGUMENT, not a crash. */
void test_model_set_recovery_metadata_rejects_null_model(void) {
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_model_set_recovery_metadata(NULL, 0, "", "", &err));
}

/* tp_model_detach_journal is idempotent (a second detach on an already-bare model is a no-op)
 * and NULL-safe (never crashes on a NULL model). */
void test_model_detach_journal_is_idempotent_and_null_safe(void) {
    tp_journal_io detach_io;
    tp_model *detached = model_with_journal(tp_test_id_of(0x94), &detach_io);
    TEST_ASSERT_TRUE(tp_model_has_journal(detached));
    tp_model_detach_journal(detached);
    TEST_ASSERT_FALSE(tp_model_has_journal(detached));
    tp_model_detach_journal(detached); /* idempotent */
    tp_model_detach_journal(NULL);     /* NULL-safe */
    tp_model_destroy(detached);
}

void test_model_set_recovery_metadata_fingerprint_glue(void) {
    tp_id128 key = tp_test_id_of(0xbd);
    tp_id128 fingerprint = tp_test_id_of(0x4e);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err = {0};
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_set_recovery_metadata_ex(m, 1700000100, "/x/fp.ntpacker_project", "fp", &fingerprint, &err));
    size_t blen = 0;
    uint8_t *bytes = snapshot_and_destroy(io, m, &blen);

    tp_journal_peek_result pk;
    memset(&pk, 0, sizeof pk);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_peek(io_from_bytes(bytes, blen), &pk, &err));
    free(bytes);
    TEST_ASSERT_TRUE(pk.has_meta);
    TEST_ASSERT_TRUE(pk.meta.has_file_fingerprint);
    TEST_ASSERT_EQUAL_MEMORY(fingerprint.bytes, pk.meta.file_fingerprint.bytes, 16);
    tp_journal_peek_free(&pk);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    UNITY_BEGIN();
    RUN_TEST(test_journal_is_sidecar_byte_identical);
    RUN_TEST(test_journal_crc32_matches_ieee_reference_vector);
    RUN_TEST(test_idset_backward_shift_eviction_has_linear_probe_bound);
    RUN_TEST(test_checkpoint_and_replay);
    RUN_TEST(test_checkpoint_only_recovery_borrows_raw_storage_without_payload_copy);
    RUN_TEST(test_format_b_replays_ops_onto_checkpoint);
    RUN_TEST(test_recovery_borrows_all_op_payloads_from_one_buffer);
    RUN_TEST(test_recovery_rejects_oversize_borrowed_op_before_parse);
    RUN_TEST(test_format_b_recovers_masked_target_set);
    RUN_TEST(test_duplicate_retry_after_restart);
    RUN_TEST(test_append_failure_rolls_back);
    RUN_TEST(test_append_oom_is_retryable);
    RUN_TEST(test_retained_id_eviction_is_durable_and_post_append);
    RUN_TEST(test_recovery_retained_ids_publish_only_after_record_acceptance);
    RUN_TEST(test_journal_rejects_null_positive_payload_before_mutation);
    RUN_TEST(test_checkpoint_retained_id_count_limit_is_prepublication);
    RUN_TEST(test_replay_window_limit_is_prewrite_and_checkpoint_resets_it);
    RUN_TEST(test_total_record_limit_bounds_all_frame_types_before_write_and_recovery);
    RUN_TEST(test_total_record_admission_precedes_commit_staging);
    RUN_TEST(test_total_record_admission_precedes_undo_staging);
    RUN_TEST(test_total_record_admission_rejected_metadata_preserves_cache_for_compaction);
    RUN_TEST(test_file_byte_admission_precedes_transaction_staging_and_metadata_cache_mutation);
    RUN_TEST(test_checkpoint_byte_admission_precedes_attach_materialization_and_counts_ids);
    RUN_TEST(test_checkpoint_pathological_length_rejects_before_write);
    RUN_TEST(test_compact_admits_checkpoint_and_cached_metadata_as_one_layout);
    RUN_TEST(test_checkpoint_byte_admission_precedes_compact_materialization);
    RUN_TEST(test_checkpoint_byte_admission_precedes_undo_redo_materialization);
    RUN_TEST(test_save_stages_path_normalization_without_invalidating_history);
    RUN_TEST(test_save_as_compact_recovery_keeps_source_target_self_contained);
    RUN_TEST(test_mark_saved_preserves_deep_history_without_clone_work);
    RUN_TEST(test_replay_operation_budget_is_prewrite_preclone_and_checkpoint_reset);
    RUN_TEST(test_recovery_rejects_aggregate_operation_overflow_before_apply);
    RUN_TEST(test_recovery_rejects_duplicate_operations_before_aggregate_apply);
    RUN_TEST(test_short_write_every_boundary);
    RUN_TEST(test_torn_tail_invisible);
    RUN_TEST(test_checksum_mismatch);
    RUN_TEST(test_stale_key_not_applied);
    RUN_TEST(test_recover_arbitrary_bytes);
    RUN_TEST(test_bad_magic_vs_version_mismatch);
    RUN_TEST(test_poison_on_truncate_failure);
    RUN_TEST(test_coordinator_ordering);
    RUN_TEST(test_coordinator_noop);
    RUN_TEST(test_file_journal_roundtrip);
    RUN_TEST(test_io_file_read_no_create); /* R5b-2 fix [3]: read-only, no-create opener */
    RUN_TEST(test_file_journal_rejects_oversize_before_read);
    RUN_TEST(test_recovery_rejects_oversized_abstract_store_before_read);
    RUN_TEST(test_peek_rejects_post_read_oversize_from_abstract_store);
    RUN_TEST(test_recovery_backend_rejects_growth_before_copy);
    RUN_TEST(test_file_journal_initial_checkpoint_cannot_exceed_reader_cap);
    RUN_TEST(test_file_journal_transaction_rejected_before_crossing_reader_cap);
    RUN_TEST(test_file_journal_undo_rejected_before_crossing_reader_cap);
    RUN_TEST(test_file_journal_metadata_failure_at_reader_cap_is_reported);
    /* F2-04 fix pass */
    RUN_TEST(test_attach_migrates_retained_ids);
    RUN_TEST(test_torn_tail_is_truncated);
    RUN_TEST(test_midstream_corrupt_preserves_trailing);
    RUN_TEST(test_midstream_bloated_length_preserves_trailing);
    RUN_TEST(test_corrupt_resync_crc_work_is_linear);
    RUN_TEST(test_truncate_failure_poisons_recovery);
    RUN_TEST(test_torn_header_reinitializable);
    RUN_TEST(test_recovered_model_is_dirty);
    /* R3: compaction on Save */
    RUN_TEST(test_compaction_resets_to_one_checkpoint);
    RUN_TEST(test_compaction_preserves_retained_ids);
    RUN_TEST(test_compaction_then_commit_recovers_as_ckpt_plus_op);
    RUN_TEST(test_compaction_relative_source_ops_recover_canonical_paths);
    RUN_TEST(test_compaction_no_journal_is_noop);
    RUN_TEST(test_compaction_truncate_failure_is_fault);
    RUN_TEST(test_compaction_init_failure_poisons);
    RUN_TEST(test_compaction_broken_store_keeps_fail_closed_authority);
    /* R4: journal-gated undo/redo checkpoint commits (P1-1) */
    RUN_TEST(test_journal_undo_append_failure_rolls_back_history_commit);
    RUN_TEST(test_journal_redo_append_failure_rolls_back_history_commit);
    RUN_TEST(test_history_eviction_waits_for_journal_append_ack);
    RUN_TEST(test_revision_max_rejects_commit_and_history_without_overflow);
    RUN_TEST(test_recovery_rejects_invalid_or_nonmonotonic_revisions);
    RUN_TEST(test_journal_undo_recovers_undone_state);
    RUN_TEST(test_journal_redo_recovers_redone_state);
    /* R5a: metadata record + peek API + BAD_HEADER split */
    RUN_TEST(test_metadata_roundtrip);
    RUN_TEST(test_recovery_metadata_oom_cleans_materialized_descriptors);
    RUN_TEST(test_metadata_fingerprint_roundtrip_and_compaction);
    RUN_TEST(test_metadata_empty_path);
    RUN_TEST(test_metadata_last_wins);
    RUN_TEST(test_metadata_survives_compaction);
    RUN_TEST(test_peek_metadata);
    RUN_TEST(test_peek_and_recover_agree);
    RUN_TEST(test_peek_empty);
    /* R5a fix round: adversarial-review findings [0][1][2][3][5] */
    RUN_TEST(test_compact_meta_reemit_failure_is_reported);
    RUN_TEST(test_set_metadata_write_failure_is_reported_but_still_caches);
    RUN_TEST(test_peek_agrees_recover_torn_tail);
    RUN_TEST(test_peek_agrees_recover_midstream_corrupt);
    RUN_TEST(test_peek_and_recover_share_retained_id_policy);
    RUN_TEST(test_recovered_metadata_survives_recompaction);
    RUN_TEST(test_v2_header_reads_version_mismatch);
    /* R5b-1: model-level metadata glue the GUI calls at the set_path identity chokepoint */
    RUN_TEST(test_model_set_recovery_metadata_glue_roundtrips_through_recovery);
    RUN_TEST(test_model_set_recovery_metadata_glue_is_noop_without_journal);
    RUN_TEST(test_model_set_recovery_metadata_rejects_null_model);
    RUN_TEST(test_model_detach_journal_is_idempotent_and_null_safe);
    RUN_TEST(test_model_set_recovery_metadata_fingerprint_glue);
    return UNITY_END();
}
