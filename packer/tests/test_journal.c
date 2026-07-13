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

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_export.h" /* TP_EXPORTER_ID_JSON_NEOTOLIS */
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h" /* tp_project_promote_ids */
#include "tp_core/tp_transaction.h"
#include "tp_journal_internal.h" /* format constants + memory-io fault seams */
#include "unity.h"

static const char *g_dir = NULL; /* scratch dir for the on-disk file journal test */

void setUp(void) {}
void tearDown(void) {}

/* ---- fixtures (mirror test_transaction.c) -------------------------------- */

static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}

static tp_project *base_project(void) {
    tp_project *p = tp_project_create();
    TEST_ASSERT_NOT_NULL(p);
    tp_project_atlas *a = &p->atlases[0];
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_source(a, "sprites"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_atlas_add_target(a, TP_EXPORTER_ID_JSON_NEOTOLIS, "out/a", NULL));
    uint8_t ctr = 1;
    tp_rng rng = {det_fill, &ctr};
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_promote_ids(p, &rng, &err));
    return p;
}

static char *serialize(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_project_save_buffer(p, &buf, &len, &err));
    return buf;
}

static tp_id128 key_of(uint8_t b) {
    tp_id128 x;
    for (int i = 0; i < 16; i++) {
        x.bytes[i] = b;
    }
    return x;
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

/* A model over base_project with a fresh in-memory journal keyed by `key`. The io
 * handle (shared ctx) is returned via *io_out so a test can snapshot the durable
 * bytes before destroying the model. */
static tp_model *model_with_journal(tp_id128 key, tp_journal_io *io_out) {
    tp_project *p = base_project();
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
    TEST_ASSERT_EQUAL_INT(0, io.read_all(io.ctx, &b, len));
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

/* Byte offset of record #idx (0-based, after the 28-byte header) in a journal store,
 * or SIZE_MAX if the walk runs past the end. Lets a test corrupt a chosen mid-stream
 * record precisely (the length-prefix framing makes the walk trivial). */
static size_t record_offset(const uint8_t *buf, size_t len, int idx) {
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    for (int i = 0; i < idx; i++) {
        if (off + (size_t)TP_JRN_LEN_FIELD > len) {
            return SIZE_MAX;
        }
        uint32_t plen = tp_jrn_get_u32(buf + off);
        off += (size_t)TP_JRN_LEN_FIELD + (size_t)plen + (size_t)TP_JRN_CRC_FIELD;
    }
    return off;
}

/* ---- byte-identity: the journal is a sidecar ----------------------------- */

void test_journal_is_sidecar_byte_identical(void) {
    /* Same op on a journal-LESS and a journal-BACKED model -> identical project bytes. */
    tp_project *p0 = base_project();
    tp_model *plain = tp_model_wrap(p0);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(plain, "10000000000000000000000000000001", 0, "renamed"));
    char *plain_bytes = serialize(tp_model_project(plain));

    tp_journal_io io;
    tp_model *j = model_with_journal(key_of(0x01), &io);
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
    tp_id128 key = key_of(0x11);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "aa000000000000000000000000000001", 0, "alpha"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "aa000000000000000000000000000002", 1, "beta"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(2, exp_rev);

    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT(3, info.records_recovered); /* checkpoint + 2 txns */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));

    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(m2->journal));
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "aa000000000000000000000000000001"));
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "aa000000000000000000000000000002"));

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- duplicate retry after restart is de-duplicated (§7.2) --------------- */

void test_duplicate_retry_after_restart(void) {
    tp_id128 key = key_of(0x12);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "bb000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
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
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(m2->journal));

    free(after_recover);
    free(after_dup);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- append failure after apply -> exact rollback, no ack, retryable ----- */

void test_append_failure_rolls_back(void) {
    tp_id128 key = key_of(0x22);
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
    TEST_ASSERT_FALSE(tp_journal_contains(m->journal, "cc000000000000000000000000000002"));

    /* Retry the SAME id -> succeeds (the failed append left no torn tail). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "cc000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT64(rev_before + 1, tp_model_revision(m));
    TEST_ASSERT_TRUE(tp_journal_contains(m->journal, "cc000000000000000000000000000002"));

    /* And the retried txn is recoverable exactly once (not duplicated). */
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    char *committed = serialize(tp_model_project(m));
    tp_model_destroy(m);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(m2->journal)); /* id01 + id02, once each */
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
    tp_journal *j = tp_journal_create(io, key_of(0x23));
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
    tp_id128 key = key_of(0x33);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "b0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "b0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);
    TEST_ASSERT_TRUE(full_len > (size_t)TP_JRN_HEADER_LEN);

    int prev_records = -1;
    int max_records = 0;
    for (size_t n = 0; n <= full_len; n++) {
        tp_journal_io io2 = io_from_bytes(full, n);
        tp_model *rm = NULL;
        tp_journal_recovery info;
        memset(&info, 0, sizeof info);
        tp_error err;
        tp_status st = tp_model_recover(io2, key, &rm, &info, &err);
        TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, st);
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
            TEST_ASSERT_TRUE(tp_journal_id_count(rm->journal) <= info.records_recovered);
            tp_model_destroy(rm);
        }
        tp_journal_recovery_free(&info);
    }
    TEST_ASSERT_EQUAL_INT(3, max_records); /* the full store recovers all 3 records */
    free(full);
}

/* ---- torn tail (payload truncated) -> invisible, no dup on retry --------- */

void test_torn_tail_invisible(void) {
    tp_id128 key = key_of(0x34);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "c0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "c0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Truncate 3 bytes off the end -> the last record's tail is torn. */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, info.status);
    TEST_ASSERT_NOT_NULL(m2);
    /* The torn txn is invisible; the good prefix (checkpoint + txn01) survived. */
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "c0000000000000000000000000000001"));
    TEST_ASSERT_FALSE(tp_journal_contains(m2->journal, "c0000000000000000000000000000002"));

    /* Retrying the unacknowledged (torn) txn applies it once -- no dup. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_rename(m2, "c0000000000000000000000000000002", tp_model_revision(m2), "two"));
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "c0000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- checksum mismatch -> corruption boundary, safe fallback ------------- */

void test_checksum_mismatch(void) {
    tp_id128 key = key_of(0x35);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "d0000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "d0000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Flip the last payload byte of the last record (before its 4-byte crc). */
    size_t at = full_len - TP_JRN_CRC_FIELD - 1;
    tp_journal_io io2 = io_from_bytes(full, full_len);
    tp_journal_io_memory__poke(io2, at, (uint8_t)(full[at] ^ 0xFFu));

    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, info.status);
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "d0000000000000000000000000000001"));
    TEST_ASSERT_FALSE(tp_journal_contains(m2->journal, "d0000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ---- stale journal for a moved project -> detected, not applied ---------- */

void test_stale_key_not_applied(void) {
    tp_id128 k1 = key_of(0x44);
    tp_id128 k2 = key_of(0x55);
    tp_journal_io io;
    tp_model *m = model_with_journal(k1, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ee000000000000000000000000000001", 0, "one"));
    size_t len = 0;
    uint8_t *bytes = snapshot_io(io, &len);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, len);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, k2, &m2, &info, &err));
    TEST_ASSERT_NULL(m2); /* moved project: NOT misapplied */
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_STALE_KEY, info.status);
    tp_journal_recovery_free(&info);
}

/* ---- UB-clean on arbitrary / garbage / absurd-length bytes --------------- */

void test_recover_arbitrary_bytes(void) {
    tp_id128 key = key_of(0x66);
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

    /* A valid header followed by a record whose length prefix is absurdly huge: the
     * reader must bounds-check (TRUNCATED) and NEVER allocate the claimed size. */
    uint8_t hdr[TP_JRN_HEADER_LEN + TP_JRN_LEN_FIELD];
    memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(hdr + TP_JRN_KEY_OFF, key.bytes, 16);
    tp_jrn_put_u32(hdr + TP_JRN_HEADER_LEN, 0xFFFFFFF0u); /* claims ~4 GB payload */
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

/* ---- bad header (wrong magic / version) ---------------------------------- */

void test_bad_header(void) {
    tp_id128 key = key_of(0x67);
    uint8_t hdr[TP_JRN_HEADER_LEN];
    memset(hdr, 0, sizeof hdr);
    memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, 999u); /* unknown version -> never guess */
    memcpy(hdr + TP_JRN_KEY_OFF, key.bytes, 16);
    tp_journal_io io = io_from_bytes(hdr, sizeof hdr);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_journal_recovery rec;
    memset(&rec, 0, sizeof rec);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_recover(j, &rec, &err));
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_BAD_HEADER, rec.status);
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j);
}

/* ---- poison on failed rollback: a torn tail that cannot be truncated ----- */

void test_poison_on_truncate_failure(void) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key_of(0x68));
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
    tp_model *m = model_with_journal(key_of(0x77), &io);
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
    tp_model *m = model_with_journal(key_of(0x78), &io);
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

    tp_id128 key = key_of(0x79);
    tp_journal_io io = tp_journal_io_file(path);
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *j = tp_journal_create(io, key);
    TEST_ASSERT_NOT_NULL(j);
    tp_project *p = base_project();
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
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2));
    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(m2->journal));

    free(expected);
    free(got);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
    remove(path);
}

/* ==== F2-04 FIX PASS: one genuine case per correctness fix (C1-C5) ========= */

/* C1: a journal attached AFTER journal-less commits must inherit the model's already-
 * retained ids, so a re-submit de-duplicates instead of double-applying (§7.2). */
void test_attach_migrates_retained_ids(void) {
    tp_id128 key = key_of(0x6A);
    tp_project *p = base_project();
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
    TEST_ASSERT_TRUE(tp_journal_contains(m->journal, "1a000000000000000000000000000001"));

    /* Re-submitting the pre-attach id now rejects as a duplicate (model unchanged). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_DUPLICATE_ID,
                          commit_rename(m, "1a000000000000000000000000000001", tp_model_revision(m), "one-again"));

    /* Recovery sees the migrated id too (the initial checkpoint carried it durably). */
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "1a000000000000000000000000000001"));
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C2 (torn tail): an incomplete final record IS truncated back to the last good
 * record, and continued appends work (the journal stays healthy). */
void test_torn_tail_is_truncated(void) {
    tp_id128 key = key_of(0x6D);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1d000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1d000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Tear 3 bytes off the tail -> the last record is incomplete (TRUNCATED). */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
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
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "1d000000000000000000000000000002"));

    free(full);
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* C2 (mid-stream): a bad-CRC record with a valid record STILL after it must NOT be
 * truncated -- that would delete the trailing acknowledged record. Recover up to the
 * last good record, PRESERVE the file, and poison the journal against appends behind
 * the corruption. This is the crux fix: torn-tail (truncate) vs mid-stream (preserve). */
void test_midstream_corrupt_preserves_trailing(void) {
    tp_id128 key = key_of(0x6E);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    /* Store layout: header | checkpoint(#0) | txn01(#1) | txn02(#2). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1e000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1e000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Corrupt a byte inside txn01 (record #1) -> mid-stream; txn02 (#2) stays intact. */
    size_t t1 = record_offset(full, full_len, 1);
    TEST_ASSERT_TRUE(t1 != SIZE_MAX && t1 < full_len);
    size_t at = t1 + (size_t)TP_JRN_LEN_FIELD + 1; /* a payload byte of txn01 */
    tp_journal_io io2 = io_from_bytes(full, full_len);
    tp_journal_io_memory__poke(io2, at, (uint8_t)(full[at] ^ 0xFFu));

    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
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

/* C3: if the recovery tail-clean truncate itself fails, the journal is poisoned -- a
 * still-present torn record must never hide a later acknowledged append. */
void test_truncate_failure_poisons_recovery(void) {
    tp_id128 key = key_of(0x6F);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Torn tail + the recovery tail-clean truncate is injected to FAIL. */
    tp_journal_io io2 = io_from_bytes(full, full_len - 3);
    tp_journal_io_memory__fail_next_truncate(io2);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
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
    tp_id128 key = key_of(0x70);
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
     * REFUSED (BAD_HEADER), never reset. */
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
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_BAD_HEADER, rec3.status);
    tp_journal_recovery_free(&rec3);
    tp_journal_destroy(j3);
}

/* C5: a crash-recovered model is ahead of the on-disk project file -> it must report
 * dirty until an explicit Save re-baselines it (else save-on-dirty-shutdown loses it). */
void test_recovered_model_is_dirty(void) {
    tp_id128 key = key_of(0x71);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "21000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);

    /* Recovered committed state is ahead of any on-disk project file -> DIRTY. */
    TEST_ASSERT_TRUE(tp_model_dirty(m2));
    /* An explicit Save re-baselines it clean. */
    tp_model_mark_saved(m2);
    TEST_ASSERT_FALSE(tp_model_dirty(m2));

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    UNITY_BEGIN();
    RUN_TEST(test_journal_is_sidecar_byte_identical);
    RUN_TEST(test_checkpoint_and_replay);
    RUN_TEST(test_duplicate_retry_after_restart);
    RUN_TEST(test_append_failure_rolls_back);
    RUN_TEST(test_append_oom_is_retryable);
    RUN_TEST(test_short_write_every_boundary);
    RUN_TEST(test_torn_tail_invisible);
    RUN_TEST(test_checksum_mismatch);
    RUN_TEST(test_stale_key_not_applied);
    RUN_TEST(test_recover_arbitrary_bytes);
    RUN_TEST(test_bad_header);
    RUN_TEST(test_poison_on_truncate_failure);
    RUN_TEST(test_coordinator_ordering);
    RUN_TEST(test_coordinator_noop);
    RUN_TEST(test_file_journal_roundtrip);
    /* F2-04 fix pass */
    RUN_TEST(test_attach_migrates_retained_ids);
    RUN_TEST(test_torn_tail_is_truncated);
    RUN_TEST(test_midstream_corrupt_preserves_trailing);
    RUN_TEST(test_truncate_failure_poisons_recovery);
    RUN_TEST(test_torn_header_reinitializable);
    RUN_TEST(test_recovered_model_is_dirty);
    return UNITY_END();
}
