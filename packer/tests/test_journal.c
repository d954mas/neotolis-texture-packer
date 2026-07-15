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

#include "tp_core/tp_diff.h"   /* R4: tp_model_enable_history / tp_model_undo / tp_model_redo */
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
    return f->inner.length(f->inner.ctx);
}
static int faulty_truncate(void *ctx, size_t len) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    return f->inner.truncate(f->inner.ctx, len);
}
static int faulty_read_all(void *ctx, uint8_t **out, size_t *out_len) {
    faulty_io_ctx *f = (faulty_io_ctx *)ctx;
    return f->inner.read_all(f->inner.ctx, out, out_len);
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

/* ---- R2 format-B linchpin: recovery REPLAYS op-payloads onto the checkpoint --- */

/* Format B (plan S18 R / R2): a TXN record stores the SERIALIZED OPERATION, not a snapshot;
 * recovery loads the last CHECKPOINT as a base and replays the post-checkpoint ops onto it.
 * Proven three ways: (1) the recovery info carries op_count == the post-checkpoint txns and a
 * base snapshot that does NOT already hold the final state (so a pass REQUIRES real replay, not
 * a lucky snapshot); (2) the replayed project reserializes byte-identically to the live committed
 * model (the same tp_operation_apply, same order, as commit); (3) the recovered revision is the
 * FINAL one. */
void test_format_b_replays_ops_onto_checkpoint(void) {
    tp_id128 key = key_of(0x13);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io); /* checkpoint captured at rev 0 (atlas "atlas1") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ab000000000000000000000000000001", 0, "first"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "ab000000000000000000000000000002", 1, "second"));
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

    /* (1) format-B shape: a base checkpoint snapshot + exactly two post-checkpoint op-payloads. */
    TEST_ASSERT_EQUAL_INT(2, (int)info.op_count);
    TEST_ASSERT_NOT_NULL(info.snapshot);
    TEST_ASSERT_NOT_NULL(strstr(info.snapshot, "atlas1")); /* the base IS the rev-0 checkpoint */
    TEST_ASSERT_NULL(strstr(info.snapshot, "second"));     /* the base does NOT hold the final state */
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
    tp_id128 key = key_of(0x14);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          commit_target_out_path(m, "cd000000000000000000000000000001", 0, "out/changed.json"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(1, exp_rev);

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

    /* base checkpoint = the ORIGINAL out_path; the edit lives ONLY in the replayed op-payload */
    TEST_ASSERT_EQUAL_INT(1, (int)info.op_count);
    TEST_ASSERT_NOT_NULL(strstr(info.snapshot, "out/a"));
    TEST_ASSERT_NULL(strstr(info.snapshot, "out/changed.json"));

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
    tp_id128 key = key_of(0x67);
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
    size_t at = t1 + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + 1; /* a payload byte of txn01 */
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

/* P1-5 (plan S18 R): a mid-stream record whose LENGTH FIELD is corrupted to a bloated value
 * must NOT masquerade as a torn tail. Under the pre-v2 length-first framing the overrun
 * classified as TRUNCATED and the tail-clean truncate DELETED the acknowledged records after
 * it (violating C2 + ADR 0013). The per-record sync-word lets recovery find the valid
 * trailing record and classify this as CORRUPT + mid-stream: the file is preserved and the
 * journal poisoned. This is the exact regression the sync-word closes. */
void test_midstream_bloated_length_preserves_trailing(void) {
    tp_id128 key = key_of(0x6F);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    /* Store layout: header | checkpoint(#0) | txn01(#1) | txn02(#2). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "1f000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

    /* Bloat txn01's length field (the 4 bytes right after its sync-word) to ~4 GB. Under the
     * old framing this looked like a torn tail; the intact sync-word before txn02 proves an
     * acknowledged record still follows, so it is a mid-stream corruption, not a tail. */
    size_t t1 = record_offset(full, full_len, 1);
    TEST_ASSERT_TRUE(t1 != SIZE_MAX && t1 < full_len);
    tp_journal_io io2 = io_from_bytes(full, full_len);
    for (int b = 0; b < TP_JRN_LEN_FIELD; b++) {
        tp_journal_io_memory__poke(io2, t1 + (size_t)TP_JRN_SYNC_FIELD + (size_t)b, 0xFFu);
    }

    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
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

/* ==== R3: compaction on Save (plan S18 R / spec §22.3) ==================== */

/* R3(a,b,d): after N commits, compaction resets the store to ONE checkpoint == the current
 * committed state. A recover then yields records_recovered==1 + op_count==0 (the store shrank to
 * a single checkpoint), the FINAL revision, the SAME serialized project, and the full retained-id
 * set re-persisted from the live index into the fresh checkpoint. */
void test_compaction_resets_to_one_checkpoint(void) {
    tp_id128 key = key_of(0x80);
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
    TEST_ASSERT_EQUAL_INT(3, tp_journal_id_count(m->journal));
    tp_model_destroy(m);

    /* Recover from the compacted bytes: exactly one record (the checkpoint), no ops to replay. */
    tp_journal_io io2 = io_from_bytes(post, post_len);
    free(post);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, info.status);
    TEST_ASSERT_EQUAL_INT(1, info.records_recovered); /* (a) one checkpoint */
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);     /* (a) no post-checkpoint ops to replay */
    TEST_ASSERT_EQUAL_INT64(exp_rev, tp_model_revision(m2)); /* (b) revision preserved */

    char *got = serialize(tp_model_project(m2));
    TEST_ASSERT_EQUAL_STRING(expected, got); /* (d) recovers exactly the saved state */
    /* durable id set preserved: all three acknowledged ids recovered from the fresh checkpoint */
    TEST_ASSERT_EQUAL_INT(3, tp_journal_id_count(m2->journal));
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "80000000000000000000000000000001"));
    TEST_ASSERT_TRUE(tp_journal_contains(m2->journal, "80000000000000000000000000000003"));

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
    tp_id128 key = key_of(0x81);
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
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    /* After a crash-recover from the compacted store the id set is STILL the idempotency authority. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, tp_journal_id_count(m2->journal));
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
    tp_id128 key = key_of(0x82);
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
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(2, info.records_recovered); /* compacted checkpoint + 1 txn */
    TEST_ASSERT_EQUAL_INT(1, (int)info.op_count);     /* (e) exactly one post-checkpoint op */
    TEST_ASSERT_NOT_NULL(strstr(info.snapshot, "two"));   /* base = the compacted "two" state */
    TEST_ASSERT_NULL(strstr(info.snapshot, "three"));     /* the final edit is NOT in the base */
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
    tp_project *p = base_project();
    tp_model *m = tp_model_wrap(p);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_NULL(m->journal);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "83000000000000000000000000000001", 0, "solo"));
    char *before = serialize(tp_model_project(m));
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err)); /* no journal -> no-op */
    char *after = serialize(tp_model_project(m));
    TEST_ASSERT_EQUAL_STRING(before, after);
    TEST_ASSERT_NULL(m->journal);
    free(before);
    free(after);
    tp_model_destroy(m);
}

/* R3: compaction fails CLOSED on a truncate failure -- the store is left INTACT (the old
 * checkpoint + records survive) and TP_STATUS_JOURNAL_FAILED is returned, so a Save-time
 * compaction that cannot reset the store never silently loses the existing recovery log. The
 * journal stays healthy (a FAILED compaction does not poison it): continued appends work. */
void test_compaction_truncate_failure_is_fault(void) {
    tp_id128 key = key_of(0x84);
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
    TEST_ASSERT_TRUE(tp_journal_contains(m->journal, "84000000000000000000000000000002"));

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
    tp_journal *j = tp_journal_create(io, key_of(0x85));
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

/* R3 fail-closed (glue): a broken-store compaction poisons the journal; tp_model_compact_journal must
 * DETACH it so the session continues JOURNAL-LESS (still editable, recovery disabled) rather than leave
 * a poisoned journal that would REJECT every subsequent commit. Proves (b) detach + edits continue and
 * (c) no journal is left accumulating unrecoverable appends. */
void test_compaction_broken_store_detaches_to_journal_less(void) {
    tp_id128 key = key_of(0x86);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "86000000000000000000000000000001", 0, "one"));

    /* Compaction: truncate OK, fresh-checkpoint write FAILS -> the journal poisons itself; the glue must
     * DETACH it (the journal + its io are destroyed here -- do NOT touch `io` afterward). */
    tp_journal_io_memory__fail_next_writes(io, 1);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_JOURNAL_FAILED, tp_model_compact_journal(m, &err));
    TEST_ASSERT_NULL(m->journal); /* (b) detached -> session runs journal-less */

    /* A subsequent commit still SUCCEEDS journal-less, not rejected by a poisoned journal. */
    int64_t rev_before = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "86000000000000000000000000000002", rev_before, "two"));
    TEST_ASSERT_EQUAL_INT64(rev_before + 1, tp_model_revision(m)); /* (c) edits continue, nothing appended to a dead journal */

    tp_model_destroy(m);
}

/* ==== R4: journal undo/redo via checkpoint-on-undo (P1-1) ================== */

/* R4/P1-1: undo swaps the live project to the UNDONE state but does NOT touch the journal, whose
 * post-checkpoint op-payloads still describe the reverted transaction. Left unchecked, a crash after
 * an undo would replay those ops and RESURRECT the undone edit. The fix checkpoints the post-undo
 * state (tp_model_compact_journal -- the same mechanism the GUI undo hook runs), so recovery loads the
 * undone state directly. This test proves it: commit two renames ("one"->"two"), UNDO back to "one",
 * compact, recover -> the recovered atlas is "one", NOT "two". Without the checkpoint-on-undo the
 * journal would still replay both renames and wrongly recover "two". */
void test_journal_undo_recovers_undone_state(void) {
    tp_id128 key = key_of(0x87);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);          /* checkpoint at rev 0 (atlas "atlas1") */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m)); /* undo needs the diff history */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "87000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "87000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_STRING("two", tp_model_project(m)->atlases[0].name);

    /* UNDO -> back to "one"; the undo bumps the revision forward (a new committed state). */
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    int64_t undone_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(3, undone_rev); /* rev0 -> +1 (one) -> +1 (two) -> +1 (undo) */

    /* The fix: checkpoint the post-undo state into the journal (the GUI hook runs exactly this). */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));

    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    /* Recover: the store is now one checkpoint == the UNDONE state, no ops to replay. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_EQUAL_INT(1, info.records_recovered); /* one checkpoint (the undone state) */
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);     /* nothing to replay -> cannot resurrect "two" */
    TEST_ASSERT_EQUAL_INT64(undone_rev, tp_model_revision(m2));
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m2)->atlases[0].name); /* the UNDONE state, NOT "two" */

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* R4/P1-1 (redo): the symmetric case -- a redo re-applies the reverted edit and is likewise
 * checkpointed, so recovery loads the REDONE state. Commit "one"->"two", UNDO (->"one"), REDO
 * (->"two"), compact, recover -> the recovered atlas is "two" at the post-redo revision. */
void test_journal_redo_recovers_redone_state(void) {
    tp_id128 key = key_of(0x88);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_enable_history(m));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "88000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "88000000000000000000000000000002", 1, "two"));

    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_undo(m, &err)); /* -> "one" (rev 3) */
    TEST_ASSERT_EQUAL_STRING("one", tp_model_project(m)->atlases[0].name);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_redo(m, &err)); /* -> "two" (rev 4) */
    TEST_ASSERT_EQUAL_STRING("two", tp_model_project(m)->atlases[0].name);
    int64_t redone_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(4, redone_rev);

    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));

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
    TEST_ASSERT_EQUAL_INT(1, info.records_recovered);
    TEST_ASSERT_EQUAL_INT(0, (int)info.op_count);
    TEST_ASSERT_EQUAL_INT64(redone_rev, tp_model_revision(m2));
    TEST_ASSERT_EQUAL_STRING("two", tp_model_project(m2)->atlases[0].name); /* the REDONE state */

    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);
}

/* ==== R5a: journal metadata record + peek API ============================== */

/* R5a: a metadata record {timestamp, path, name} round-trips through recovery, and does NOT disturb
 * replay -- the rename still applies and the revision is right (META is captured-and-skipped). */
void test_metadata_roundtrip(void) {
    tp_id128 key = key_of(0x90);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700000000, "/foo/bar.ntpacker", "bar", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "90000000000000000000000000000001", 0, "renamed"));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);
    TEST_ASSERT_EQUAL_INT64(1, exp_rev);

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

/* R5a: an untitled project (empty path + name) round-trips as empty strings, not NULL. */
void test_metadata_empty_path(void) {
    tp_id128 key = key_of(0x91);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(m->journal, 42, "", "", &err));
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
    tp_id128 key = key_of(0x92);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(m->journal, 100, "/old/path.ntpacker", "old", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_set_metadata(m->journal, 200, "/new/path.ntpacker", "new", &err));
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
    tp_id128 key = key_of(0x93);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700001234, "/proj/p.ntpacker", "p", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000002", 1, "two"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
    char *expected = serialize(tp_model_project(m));
    int64_t exp_rev = tp_model_revision(m);

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
    tp_id128 key = key_of(0x94);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700005555, "/scan/proj.ntpacker", "proj", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "94000000000000000000000000000001", 0, "renamed"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

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
    tp_id128 key = key_of(0x95);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700009999, "/agree/a.ntpacker", "a", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "95000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    /* recover */
    tp_journal_io io_r = io_from_bytes(bytes, blen);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io_r, key, &m2, &info, &err));
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
    tp_id128 key = key_of(0x96);
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

/* [0] compact must NOT poison a healthy checkpoint on a metadata re-emit failure. Arm the faulty io to
 * fail ONLY the META write inside compact (write #3 after truncate-to-0: header, checkpoint, META);
 * write_record rolls the torn META tail back to the durable checkpoint. Assert compact returns OK (not a
 * hard failure the glue would detach on), the journal is NOT poisoned, and recover yields the
 * post-compaction document (checkpoint recovered, records_recovered==1) -- recovery intact, metadata
 * simply absent. FAILS pre-fix (compact poisoned + returned JOURNAL_FAILED over the lost scan label). */
void test_compact_meta_reemit_failure_keeps_recovery(void) {
    tp_id128 key = key_of(0xA0);
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
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_journal_compact(j, snap2, sizeof snap2, 2, &err)); /* swallowed */
    TEST_ASSERT_FALSE(tp_journal__is_poisoned(j)); /* healthy checkpoint NOT poisoned over a lost label */

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
    TEST_ASSERT_NOT_NULL(strstr(rec.snapshot, "new")); /* == the post-compaction state */
    TEST_ASSERT_FALSE(rec.has_metadata);               /* metadata absent, recovery still works */
    tp_journal_recovery_free(&rec);
    tp_journal_destroy(j2);
}

/* [2] R5b-1 fix (FLIPPED contract): metadata is CACHE-AUTHORITATIVE + best-effort durable. A HEALTHY
 * journal whose durable META write transiently fails must SWALLOW the failure -- return TP_STATUS_OK,
 * stay NOT poisoned, and KEEP the identity in the cache -- so the NEXT compaction persists it (the cache
 * is the source of truth, the durable append a best-effort bonus). Fail exactly the META write (rollback
 * is a truncate -> journal not poisoned), assert set_metadata returns OK + not poisoned; then (fault
 * exhausted) compact + recover -> the metadata IS present. Proves the corrected best-effort contract
 * (the OLD write-first contract returned JOURNAL_FAILED here and dropped the metadata). */
void test_set_metadata_write_failure_still_caches(void) {
    tp_id128 key = key_of(0xA1);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io); /* header + initial checkpoint durably written */
    tp_error err;

    tp_journal_io_memory__fail_next_writes(io, 1); /* fail ONLY the META durable write (header present) */
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK, /* healthy-journal swallow: cache committed, durable append best-effort */
        tp_journal_set_metadata(m->journal, 1700000000, "/still/caches.ntpacker", "cached", &err));
    TEST_ASSERT_FALSE(tp_journal__is_poisoned(m->journal)); /* torn META rolled back -> journal healthy */

    /* Fault exhausted -> compaction re-emits the cached (authoritative) metadata durably. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m, &err));
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
    tp_id128 key = key_of(0xA4);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700003333, "/parity/torn.ntpacker", "torn", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a4000000000000000000000000000001", 0, "one"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);
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
    tp_id128 key = key_of(0xA5);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    /* header | ckpt(#0) | txn01(#1) | txn02(#2) */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a5000000000000000000000000000001", 0, "one"));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a5000000000000000000000000000002", 1, "two"));
    size_t full_len = 0;
    uint8_t *full = snapshot_io(io, &full_len);
    tp_model_destroy(m);

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
    tp_id128 key = key_of(0xA3);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_journal_set_metadata(m->journal, 1700007777, "/recov/keepme.ntpacker", "keepme", &err));
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a3000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);

    /* First recover: the recovered journal's write-side cache is seeded from the metadata record. */
    tp_journal_io io2 = io_from_bytes(bytes, blen);
    free(bytes);
    tp_model *m2 = NULL;
    tp_journal_recovery info;
    memset(&info, 0, sizeof info);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io2, key, &m2, &info, &err));
    TEST_ASSERT_NOT_NULL(m2);
    TEST_ASSERT_TRUE(info.has_metadata);
    tp_journal_recovery_free(&info);

    /* Compact the RECOVERED journal (the Save/undo hook). Pre-fix has_meta==false here -> no re-emit. */
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_compact_journal(m2, &err));
    size_t blen2 = 0;
    uint8_t *bytes2 = snapshot_io(io2, &blen2);
    tp_model_destroy(m2);

    /* Second recover from the recompacted store: metadata STILL present (path/name/time intact). */
    tp_journal_io io3 = io_from_bytes(bytes2, blen2);
    free(bytes2);
    tp_model *m3 = NULL;
    tp_journal_recovery info2;
    memset(&info2, 0, sizeof info2);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, tp_model_recover(io3, key, &m3, &info2, &err));
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
    tp_id128 key = key_of(0xA2);
    tp_error err;
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "a2000000000000000000000000000001", 0, "one"));
    size_t blen = 0;
    uint8_t *bytes = snapshot_io(io, &blen);
    tp_model_destroy(m);
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
 * attached journal (the GUI calls it at the set_path identity chokepoint). The metadata round-trips
 * through recovery, and the no-journal case is a safe no-op success (recovery is optional). */
void test_model_set_recovery_metadata_glue(void) {
    tp_id128 key = key_of(0x93);
    tp_journal_io io;
    tp_model *m = model_with_journal(key, &io);
    tp_error err;
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK, commit_rename(m, "93000000000000000000000000000001", 0, "renamed"));
    TEST_ASSERT_EQUAL_INT(
        TP_STATUS_OK,
        tp_model_set_recovery_metadata(m, 1700000000, "/x/proj.ntpacker", "proj.ntpacker", &err));

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
    TEST_ASSERT_TRUE(info.has_metadata);
    TEST_ASSERT_EQUAL_INT64(1700000000, info.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/x/proj.ntpacker", info.metadata.path);
    TEST_ASSERT_EQUAL_STRING("proj.ntpacker", info.metadata.name);
    TEST_ASSERT_EQUAL_INT64(1, tp_model_revision(m2)); /* the committed rename still replayed (META is skipped) */
    tp_journal_recovery_free(&info);
    tp_model_destroy(m2);

    /* No-journal (recovery off / journal-less) model: the glue is a no-op success -- no crash, nothing
     * durable. A NULL model is INVALID_ARGUMENT. */
    tp_model *bare = tp_model_wrap(base_project());
    TEST_ASSERT_NOT_NULL(bare);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_OK,
                          tp_model_set_recovery_metadata(bare, 42, "/ignored", "ignored", &err));
    tp_model_destroy(bare);
    TEST_ASSERT_EQUAL_INT(TP_STATUS_INVALID_ARGUMENT,
                          tp_model_set_recovery_metadata(NULL, 0, "", "", &err));
}

int main(int argc, char **argv) {
    if (argc > 1) {
        g_dir = argv[1];
    }
    UNITY_BEGIN();
    RUN_TEST(test_journal_is_sidecar_byte_identical);
    RUN_TEST(test_checkpoint_and_replay);
    RUN_TEST(test_format_b_replays_ops_onto_checkpoint);
    RUN_TEST(test_format_b_recovers_masked_target_set);
    RUN_TEST(test_duplicate_retry_after_restart);
    RUN_TEST(test_append_failure_rolls_back);
    RUN_TEST(test_append_oom_is_retryable);
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
    /* F2-04 fix pass */
    RUN_TEST(test_attach_migrates_retained_ids);
    RUN_TEST(test_torn_tail_is_truncated);
    RUN_TEST(test_midstream_corrupt_preserves_trailing);
    RUN_TEST(test_midstream_bloated_length_preserves_trailing);
    RUN_TEST(test_truncate_failure_poisons_recovery);
    RUN_TEST(test_torn_header_reinitializable);
    RUN_TEST(test_recovered_model_is_dirty);
    /* R3: compaction on Save */
    RUN_TEST(test_compaction_resets_to_one_checkpoint);
    RUN_TEST(test_compaction_preserves_retained_ids);
    RUN_TEST(test_compaction_then_commit_recovers_as_ckpt_plus_op);
    RUN_TEST(test_compaction_no_journal_is_noop);
    RUN_TEST(test_compaction_truncate_failure_is_fault);
    RUN_TEST(test_compaction_init_failure_poisons);
    RUN_TEST(test_compaction_broken_store_detaches_to_journal_less);
    /* R4: journal undo/redo via checkpoint-on-undo (P1-1) */
    RUN_TEST(test_journal_undo_recovers_undone_state);
    RUN_TEST(test_journal_redo_recovers_redone_state);
    /* R5a: metadata record + peek API + BAD_HEADER split */
    RUN_TEST(test_metadata_roundtrip);
    RUN_TEST(test_metadata_empty_path);
    RUN_TEST(test_metadata_last_wins);
    RUN_TEST(test_metadata_survives_compaction);
    RUN_TEST(test_peek_metadata);
    RUN_TEST(test_peek_and_recover_agree);
    RUN_TEST(test_peek_empty);
    /* R5a fix round: adversarial-review findings [0][1][2][3][5] */
    RUN_TEST(test_compact_meta_reemit_failure_keeps_recovery);
    RUN_TEST(test_set_metadata_write_failure_still_caches);
    RUN_TEST(test_peek_agrees_recover_torn_tail);
    RUN_TEST(test_peek_agrees_recover_midstream_corrupt);
    RUN_TEST(test_recovered_metadata_survives_recompaction);
    RUN_TEST(test_v2_header_reads_version_mismatch);
    /* R5b-1: model-level metadata glue the GUI calls at the set_path identity chokepoint */
    RUN_TEST(test_model_set_recovery_metadata_glue);
    return UNITY_END();
}
