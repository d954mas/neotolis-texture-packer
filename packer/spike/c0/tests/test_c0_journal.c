/* C0-03 task 1: recovery-journal record framing + recovery. Golden BINARY
 * vectors (hex byte arrays) pin the on-disk framing byte-for-byte; the fault
 * cases pin every recovery failure to a distinct structured tp_c0_detail token.
 * The executable form of C0-03-contract.md §1. */

#include "tp_c0/tp_c0_journal.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* ------------------------------------------------------------------------- *
 * Golden vectors. Bytes are little-endian per the frame layout; the 16-byte
 * checksum is FNV-1a/128 over the payload (portable, cross-checked below and
 * against an independent Python reference during authoring). Identical on
 * Linux/macOS/Windows by construction.
 * ------------------------------------------------------------------------- */

/* TXN: txn_id = 0x00..0x0f, revision_after = 7, body = "op". */
static const uint8_t k_txn_golden[] = {
    0x54, 0x50, 0x4a, 0x31,                         /* magic 'T','P','J','1' */
    0x01, 0x00,                                     /* version = 1 (LE) */
    0x01, 0x00,                                     /* kind = TXN (LE) */
    0x1a, 0x00, 0x00, 0x00,                         /* payload_len = 26 (LE) */
    0xc7, 0x5c, 0x56, 0x83, 0xd5, 0xa4, 0xd6, 0x17, /* checksum[0..7] */
    0x83, 0x59, 0x05, 0x1b, 0xd7, 0x88, 0x22, 0xbd, /* checksum[8..15] */
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, /* txn_id[0..7] */
    0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, /* txn_id[8..15] */
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* revision_after = 7 (LE) */
    0x6f, 0x70,                                     /* body "op" */
};

/* CHECKPOINT: revision = 5, state_hash = 0xa0..0xaf, no body. */
static const uint8_t k_ckp_golden[] = {
    0x54, 0x50, 0x4a, 0x31,                         /* magic */
    0x01, 0x00,                                     /* version = 1 */
    0x02, 0x00,                                     /* kind = CHECKPOINT */
    0x18, 0x00, 0x00, 0x00,                         /* payload_len = 24 */
    0xd2, 0x15, 0xcc, 0x43, 0x2d, 0x60, 0xef, 0x3b, /* checksum[0..7] */
    0xda, 0x11, 0xdd, 0x8d, 0x35, 0x26, 0x54, 0x88, /* checksum[8..15] */
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* revision = 5 (LE) */
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, /* state_hash[0..7] */
    0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, /* state_hash[8..15] */
};

static tp_c0_id128 id_seq(uint8_t base) {
    tp_c0_id128 id;
    for (int i = 0; i < 16; i++) {
        id.bytes[i] = (uint8_t)(base + i);
    }
    return id;
}

/* ---- golden encode ------------------------------------------------------- */

void test_txn_encode_matches_golden(void) {
    uint8_t out[64];
    size_t written = 0;
    tp_error err;
    tp_c0_detail d = tp_c0_journal_encode_txn(id_seq(0x00), 7, "op", 2, out, sizeof out, &written, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_EQUAL_UINT(sizeof k_txn_golden, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_txn_golden, out, sizeof k_txn_golden);
}

void test_checkpoint_encode_matches_golden(void) {
    uint8_t out[64];
    size_t written = 0;
    tp_c0_detail d = tp_c0_journal_encode_checkpoint(5, id_seq(0xa0), NULL, 0, out, sizeof out, &written, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_EQUAL_UINT(sizeof k_ckp_golden, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_ckp_golden, out, sizeof k_ckp_golden);
}

/* The embedded checksum really is FNV-1a/128 of the payload (not a transcription
 * error): recompute it from the golden's own payload region. */
void test_golden_checksum_is_real_fnv(void) {
    const uint8_t *payload = k_txn_golden + TP_C0_JOURNAL_HEADER_SIZE;
    size_t payload_len = sizeof k_txn_golden - TP_C0_JOURNAL_HEADER_SIZE;
    tp_c0_id128 sum = tp_c0_hash128(payload, payload_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(k_txn_golden + 12, sum.bytes, 16);
}

/* ---- golden decode ------------------------------------------------------- */

void test_txn_decode_golden(void) {
    tp_c0_journal_record rec;
    tp_c0_detail d = tp_c0_journal_decode(k_txn_golden, sizeof k_txn_golden, &rec, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_EQUAL_INT(TP_C0_JREC_TXN, rec.kind);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(rec.txn_id, id_seq(0x00)));
    TEST_ASSERT_EQUAL_INT64(7, rec.revision);
    TEST_ASSERT_EQUAL_UINT(2, rec.body_len);
    TEST_ASSERT_EQUAL_MEMORY("op", rec.body, 2);
    TEST_ASSERT_EQUAL_UINT(sizeof k_txn_golden, rec.record_len);
}

void test_checkpoint_decode_golden(void) {
    tp_c0_journal_record rec;
    tp_c0_detail d = tp_c0_journal_decode(k_ckp_golden, sizeof k_ckp_golden, &rec, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_EQUAL_INT(TP_C0_JREC_CHECKPOINT, rec.kind);
    TEST_ASSERT_EQUAL_INT64(5, rec.revision);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(rec.state_hash, id_seq(0xa0)));
    TEST_ASSERT_EQUAL_UINT(0, rec.body_len);
    TEST_ASSERT_NULL(rec.body);
}

void test_txn_roundtrip_with_body(void) {
    uint8_t out[128];
    size_t written = 0;
    const char *body = "transaction-body-blob";
    tp_c0_detail d = tp_c0_journal_encode_txn(id_seq(0x40), 123456789012345LL, body, strlen(body), out, sizeof out,
                                              &written, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    tp_c0_journal_record rec;
    d = tp_c0_journal_decode(out, written, &rec, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(rec.txn_id, id_seq(0x40)));
    TEST_ASSERT_EQUAL_INT64(123456789012345LL, rec.revision);
    TEST_ASSERT_EQUAL_UINT(strlen(body), rec.body_len);
    TEST_ASSERT_EQUAL_MEMORY(body, rec.body, strlen(body));
}

/* ---- encode faults ------------------------------------------------------- */

void test_encode_buffer_too_small_is_append_fail(void) {
    uint8_t out[10]; /* < HEADER_SIZE */
    size_t written = 99;
    tp_error err;
    tp_c0_detail d = tp_c0_journal_encode_txn(id_seq(0x00), 1, NULL, 0, out, sizeof out, &written, &err);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_BUFFER_TOO_SMALL, d);
    TEST_ASSERT_EQUAL_UINT(0, written); /* nothing written -> drives ack APPEND_FAIL */
}

void test_encode_nil_txn_id_rejected(void) {
    uint8_t out[64];
    tp_c0_detail d = tp_c0_journal_encode_txn(tp_c0_id128_nil(), 1, NULL, 0, out, sizeof out, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_ID_NIL, d);
}

/* ---- decode faults (fault injection) ------------------------------------- */

void test_decode_short_header(void) {
    tp_c0_journal_record rec;
    tp_c0_detail d = tp_c0_journal_decode(k_txn_golden, 10, &rec, NULL); /* < 28-byte header */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_SHORT, d);
}

void test_decode_truncated_payload_short_write(void) {
    /* Full 28-byte header present, but payload cut off (a short/torn write). */
    tp_c0_journal_record rec;
    tp_c0_detail d = tp_c0_journal_decode(k_txn_golden, 40, &rec, NULL); /* 40 < 54 total */
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_SHORT, d);
}

void test_decode_bad_magic(void) {
    uint8_t buf[sizeof k_txn_golden];
    memcpy(buf, k_txn_golden, sizeof buf);
    buf[0] = 0x00;
    tp_c0_journal_record rec;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_MAGIC, tp_c0_journal_decode(buf, sizeof buf, &rec, NULL));
}

void test_decode_bad_version(void) {
    uint8_t buf[sizeof k_txn_golden];
    memcpy(buf, k_txn_golden, sizeof buf);
    buf[4] = 0x02; /* version 2 */
    tp_c0_journal_record rec;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_VERSION, tp_c0_journal_decode(buf, sizeof buf, &rec, NULL));
}

void test_decode_bad_checksum(void) {
    uint8_t buf[sizeof k_txn_golden];
    memcpy(buf, k_txn_golden, sizeof buf);
    buf[sizeof buf - 1] ^= 0xFF; /* flip a payload byte -> checksum mismatch */
    tp_c0_journal_record rec;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_CHECKSUM, tp_c0_journal_decode(buf, sizeof buf, &rec, NULL));
}

void test_decode_bad_kind_unknown(void) {
    uint8_t buf[sizeof k_txn_golden];
    memcpy(buf, k_txn_golden, sizeof buf);
    buf[6] = 0x63; /* kind = 99: unknown; checksum is over payload so still valid */
    tp_c0_journal_record rec;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_KIND, tp_c0_journal_decode(buf, sizeof buf, &rec, NULL));
}

/* A known kind whose payload is shorter than its framing prefix -> bad_kind. */
void test_decode_kind_payload_too_short(void) {
    uint8_t payload[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    uint8_t buf[TP_C0_JOURNAL_HEADER_SIZE + 10];
    buf[0] = 0x54;
    buf[1] = 0x50;
    buf[2] = 0x4a;
    buf[3] = 0x31;
    buf[4] = 0x01;
    buf[5] = 0x00; /* version 1 */
    buf[6] = 0x01;
    buf[7] = 0x00;                                                        /* kind TXN */
    buf[8] = 0x0a;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x00; /* payload_len 10 (< 24 prefix) */
    tp_c0_id128 sum = tp_c0_hash128(payload, sizeof payload);
    memcpy(buf + 12, sum.bytes, 16);
    memcpy(buf + TP_C0_JOURNAL_HEADER_SIZE, payload, sizeof payload);
    tp_c0_journal_record rec;
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_KIND, tp_c0_journal_decode(buf, sizeof buf, &rec, NULL));
}

/* ---- recovery ------------------------------------------------------------ */

/* Append helper: encode a record onto the end of `buf` at *off. */
static void append_txn(uint8_t *buf, size_t *off, size_t cap, tp_c0_id128 id, int64_t rev) {
    size_t w = 0;
    tp_c0_detail d = tp_c0_journal_encode_txn(id, rev, NULL, 0, buf + *off, cap - *off, &w, NULL);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, d);
    *off += w;
}

void test_recover_clean_stream(void) {
    uint8_t buf[512];
    size_t off = 0;
    append_txn(buf, &off, sizeof buf, id_seq(0x10), 1);
    append_txn(buf, &off, sizeof buf, id_seq(0x20), 2);
    size_t ckp = 0;
    tp_c0_journal_encode_checkpoint(2, id_seq(0xb0), NULL, 0, buf + off, sizeof buf - off, &ckp, NULL);
    off += ckp;

    tp_c0_journal_recovery r;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_journal_recover(buf, off, &r, NULL));
    TEST_ASSERT_EQUAL_INT(2, r.txn_count);
    TEST_ASSERT_TRUE(tp_c0_id128_eq(r.txns[0], id_seq(0x10)));
    TEST_ASSERT_TRUE(tp_c0_id128_eq(r.txns[1], id_seq(0x20)));
    TEST_ASSERT_EQUAL_INT64(2, r.last_revision);
    TEST_ASSERT_EQUAL_INT64(2, r.checkpoint_revision);
    TEST_ASSERT_EQUAL_UINT(off, r.valid_bytes);
    TEST_ASSERT_FALSE(r.truncated_tail);
    TEST_ASSERT_FALSE(r.corrupt);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, r.stop_reason);
}

void test_recover_torn_final_record(void) {
    uint8_t buf[512];
    size_t off = 0;
    append_txn(buf, &off, sizeof buf, id_seq(0x10), 1);
    append_txn(buf, &off, sizeof buf, id_seq(0x20), 2);
    size_t good = off;
    append_txn(buf, &off, sizeof buf, id_seq(0x30), 3);
    /* Chop the last 5 bytes of the final record (a partial/torn append). */
    size_t torn_len = off - 5;

    tp_c0_journal_recovery r;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_journal_recover(buf, torn_len, &r, NULL));
    TEST_ASSERT_EQUAL_INT(2, r.txn_count); /* torn tail dropped; prefix recovered */
    TEST_ASSERT_EQUAL_UINT(good, r.valid_bytes);
    TEST_ASSERT_TRUE(r.truncated_tail);
    TEST_ASSERT_FALSE(r.corrupt);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_SHORT, r.stop_reason);
}

void test_recover_bad_checksum_midstream(void) {
    uint8_t buf[512];
    size_t off = 0;
    append_txn(buf, &off, sizeof buf, id_seq(0x10), 1);
    size_t first = off;
    append_txn(buf, &off, sizeof buf, id_seq(0x20), 2);
    buf[off - 1] ^= 0xFF; /* corrupt the 2nd record's payload */

    tp_c0_journal_recovery r;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_journal_recover(buf, off, &r, NULL));
    TEST_ASSERT_EQUAL_INT(1, r.txn_count); /* only the clean first record */
    TEST_ASSERT_EQUAL_UINT(first, r.valid_bytes);
    TEST_ASSERT_FALSE(r.truncated_tail);
    TEST_ASSERT_TRUE(r.corrupt);
    TEST_ASSERT_EQUAL_INT(TP_C0_ERR_JOURNAL_BAD_CHECKSUM, r.stop_reason);
}

void test_recover_empty_is_clean(void) {
    tp_c0_journal_recovery r;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_journal_recover(NULL, 0, &r, NULL));
    TEST_ASSERT_EQUAL_INT(0, r.txn_count);
    TEST_ASSERT_EQUAL_INT64(-1, r.last_revision);
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, r.stop_reason);
}

/* Idempotency retention seam (§7.2): the recovered id set lets a duplicate
 * transaction be detected after a simulated restart (not applied twice). */
void test_recover_idempotency_duplicate_after_restart(void) {
    uint8_t buf[256];
    size_t off = 0;
    append_txn(buf, &off, sizeof buf, id_seq(0x10), 1);
    append_txn(buf, &off, sizeof buf, id_seq(0x20), 2);

    tp_c0_journal_recovery r;
    TEST_ASSERT_EQUAL_INT(TP_C0_OK, tp_c0_journal_recover(buf, off, &r, NULL));
    /* A retried (already-committed) txn is a known id -> duplicate, no re-apply. */
    TEST_ASSERT_TRUE(tp_c0_journal_recovery_has_txn(&r, id_seq(0x10)));
    TEST_ASSERT_TRUE(tp_c0_journal_recovery_has_txn(&r, id_seq(0x20)));
    /* A fresh txn id is not in the set -> would be applied. */
    TEST_ASSERT_FALSE(tp_c0_journal_recovery_has_txn(&r, id_seq(0x99)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_txn_encode_matches_golden);
    RUN_TEST(test_checkpoint_encode_matches_golden);
    RUN_TEST(test_golden_checksum_is_real_fnv);
    RUN_TEST(test_txn_decode_golden);
    RUN_TEST(test_checkpoint_decode_golden);
    RUN_TEST(test_txn_roundtrip_with_body);
    RUN_TEST(test_encode_buffer_too_small_is_append_fail);
    RUN_TEST(test_encode_nil_txn_id_rejected);
    RUN_TEST(test_decode_short_header);
    RUN_TEST(test_decode_truncated_payload_short_write);
    RUN_TEST(test_decode_bad_magic);
    RUN_TEST(test_decode_bad_version);
    RUN_TEST(test_decode_bad_checksum);
    RUN_TEST(test_decode_bad_kind_unknown);
    RUN_TEST(test_decode_kind_payload_too_short);
    RUN_TEST(test_recover_clean_stream);
    RUN_TEST(test_recover_torn_final_record);
    RUN_TEST(test_recover_bad_checksum_midstream);
    RUN_TEST(test_recover_empty_is_clean);
    RUN_TEST(test_recover_idempotency_duplicate_after_restart);
    return UNITY_END();
}
