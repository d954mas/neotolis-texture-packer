/* Independent v4 journal wire oracle. The checked-in fixture contains a fixed
 * header and one CHECKPOINT, META, TXN and HISTORY frame. Its big-endian fields
 * and CRC values are literal; no production journal helper builds them. */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_journal.h"
#include "tp_journal_internal.h"
#include "unity.h"

enum {
    FIXTURE_BYTES = 171,
    CHECKPOINT_END = 55,
    META_LENGTH_OFFSET = 59,
    META_CRC_OFFSET = 83,
    HISTORY_FRAME_OFFSET = 142
};

static const char *g_fixture_path;

void setUp(void) {}
void tearDown(void) {}

static tp_id128 id_last(unsigned char last_byte) {
    tp_id128 id = {{0U}};
    id.bytes[15] = last_byte;
    return id;
}

static int hex_digit(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static uint8_t *read_fixture(size_t *out_len) {
    FILE *file = fopen(g_fixture_path, "rb");
    TEST_ASSERT_NOT_NULL(file);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_END));
    const long file_len = ftell(file);
    TEST_ASSERT_TRUE(file_len >= 0);
    TEST_ASSERT_EQUAL_INT(0, fseek(file, 0, SEEK_SET));
    char *text = (char *)malloc((size_t)file_len + 1U);
    TEST_ASSERT_NOT_NULL(text);
    TEST_ASSERT_EQUAL_size_t((size_t)file_len,
                             fread(text, 1U, (size_t)file_len, file));
    TEST_ASSERT_EQUAL_INT(0, fclose(file));
    text[file_len] = '\0';
    uint8_t *bytes = (uint8_t *)malloc((size_t)file_len / 2U + 1U);
    TEST_ASSERT_NOT_NULL(bytes);
    int high = -1;
    size_t count = 0U;
    for (long i = 0; i < file_len; i++) {
        const int digit = hex_digit((unsigned char)text[i]);
        if (digit < 0) {
            TEST_ASSERT_TRUE(text[i] == ' ' || text[i] == '\t' ||
                             text[i] == '\r' || text[i] == '\n');
            continue;
        }
        if (high < 0) {
            high = digit;
        } else {
            bytes[count++] = (uint8_t)((unsigned)high * 16U +
                                       (unsigned)digit);
            high = -1;
        }
    }
    TEST_ASSERT_EQUAL_INT(-1, high);
    TEST_ASSERT_EQUAL_size_t((size_t)FIXTURE_BYTES, count);
    free(text);
    *out_len = count;
    return bytes;
}

static tp_journal *journal_from_bytes(const uint8_t *bytes, size_t len,
                                      tp_journal_io *out_io) {
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    TEST_ASSERT_EQUAL_INT64((int64_t)len, io.write(io.ctx, bytes, len));
    tp_journal *journal = tp_journal_create(io, id_last(0xAAU));
    TEST_ASSERT_NOT_NULL(journal);
    if (out_io) *out_io = io;
    return journal;
}

static uint8_t *snapshot_io(tp_journal_io io, size_t *out_len) {
    uint8_t *bytes = NULL;
    TEST_ASSERT_EQUAL_INT(0, io.read_all(io.ctx, SIZE_MAX, &bytes, out_len));
    return bytes;
}

void test_writer_matches_complete_fixed_journal_bytes(void) {
    size_t expected_len = 0U;
    uint8_t *expected = read_fixture(&expected_len);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    tp_journal *journal = tp_journal_create(io, id_last(0xAAU));
    TEST_ASSERT_NOT_NULL(journal);
    tp_error error = {0};
    static const uint8_t snapshot[] = {'{', '}'};
    static const uint8_t txn_payload[] = {0xDEU, 0xADU};
    static const uint8_t history_transition[] = {
        0x00U, 0x00U, 0x00U, 0x01U, /* transition version */
        0x00U, 0x00U, 0x00U, 0x00U  /* zero operations */};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_journal_init_checkpoint(journal, snapshot, sizeof snapshot, 1,
                                   &error), error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_journal_set_metadata(journal, 2, "/p", "n", &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_journal_append_txn(journal,
                              "11111111111111111111111111111111", 3,
                              txn_payload, sizeof txn_payload, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK,
        tp_journal_append_history_counted(
            journal, 4, history_transition, sizeof history_transition, 0U,
            &error), error.msg);
    size_t actual_len = 0U;
    uint8_t *actual = snapshot_io(io, &actual_len);
    TEST_ASSERT_EQUAL_size_t(expected_len, actual_len);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, expected_len);
    free(expected);
    free(actual);
    tp_journal_destroy(journal);
}

void test_reader_recovers_fixed_mixed_sequence(void) {
    size_t len = 0U;
    uint8_t *bytes = read_fixture(&len);
    tp_journal *journal = journal_from_bytes(bytes, len, NULL);
    free(bytes);
    tp_journal_recovery recovery = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_journal_recover(journal, &recovery, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, recovery.status);
    TEST_ASSERT_EQUAL_size_t(len, recovery.bytes_total);
    TEST_ASSERT_EQUAL_size_t(len, recovery.stop_offset);
    TEST_ASSERT_FALSE(recovery.mid_stream_corrupt);
    TEST_ASSERT_EQUAL_INT(3, recovery.records_recovered);
    TEST_ASSERT_EQUAL_INT64(4, recovery.revision);
    TEST_ASSERT_EQUAL_size_t(2U, recovery.snapshot_len);
    TEST_ASSERT_EQUAL_MEMORY("{}", recovery.snapshot, 2U);
    TEST_ASSERT_EQUAL_size_t(2U, recovery.op_count);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_REPLAY_TXN, recovery.ops[0].kind);
    TEST_ASSERT_EQUAL_INT64(3, recovery.ops[0].revision);
    TEST_ASSERT_EQUAL_size_t(2U, recovery.ops[0].payload_len);
    static const uint8_t txn_payload[] = {0xDEU, 0xADU};
    TEST_ASSERT_EQUAL_MEMORY(txn_payload, recovery.ops[0].payload,
                             sizeof txn_payload);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_REPLAY_HISTORY, recovery.ops[1].kind);
    TEST_ASSERT_EQUAL_INT64(4, recovery.ops[1].revision);
    TEST_ASSERT_EQUAL_size_t(8U, recovery.ops[1].payload_len);
    static const uint8_t history_transition[] = {
        0x00U, 0x00U, 0x00U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U};
    TEST_ASSERT_EQUAL_MEMORY(history_transition, recovery.ops[1].payload,
                             sizeof history_transition);
    TEST_ASSERT_TRUE(recovery.has_metadata);
    TEST_ASSERT_EQUAL_INT64(2, recovery.metadata.timestamp);
    TEST_ASSERT_EQUAL_STRING("/p", recovery.metadata.path);
    TEST_ASSERT_EQUAL_STRING("n", recovery.metadata.name);
    TEST_ASSERT_FALSE(recovery.metadata.has_file_fingerprint);
    TEST_ASSERT_TRUE(tp_journal_contains(
        journal, "11111111111111111111111111111111"));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_peek_classifies_the_same_fixed_mixed_sequence(void) {
    size_t len = 0U;
    uint8_t *bytes = read_fixture(&len);
    tp_journal_io io = tp_journal_io_memory();
    TEST_ASSERT_NOT_NULL(io.ctx);
    TEST_ASSERT_EQUAL_INT64((int64_t)len, io.write(io.ctx, bytes, len));
    free(bytes);
    tp_journal_peek_result peek = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(TP_STATUS_OK,
                                  tp_journal_peek(io, &peek, &error),
                                  error.msg);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_OK, peek.status);
    TEST_ASSERT_EQUAL_UINT32(4U, peek.format_version);
    for (int i = 0; i < 15; i++) TEST_ASSERT_EQUAL_UINT8(0U, peek.key[i]);
    TEST_ASSERT_EQUAL_UINT8(0xAAU, peek.key[15]);
    TEST_ASSERT_TRUE(peek.has_checkpoint);
    TEST_ASSERT_EQUAL_INT(3, peek.record_count);
    TEST_ASSERT_TRUE(peek.has_meta);
    TEST_ASSERT_EQUAL_INT64(2, peek.meta.timestamp);
    TEST_ASSERT_EQUAL_STRING("/p", peek.meta.path);
    TEST_ASSERT_EQUAL_STRING("n", peek.meta.name);
    tp_journal_peek_free(&peek);
}

static void assert_corrupt_meta_preserves_checkpoint_prefix(uint8_t *bytes,
                                                            size_t len) {
    tp_journal *journal = journal_from_bytes(bytes, len, NULL);
    tp_journal_recovery recovery = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_journal_recover(journal, &recovery, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_CORRUPT, recovery.status);
    TEST_ASSERT_TRUE(recovery.mid_stream_corrupt);
    TEST_ASSERT_EQUAL_size_t((size_t)CHECKPOINT_END, recovery.stop_offset);
    TEST_ASSERT_EQUAL_INT(1, recovery.records_recovered);
    TEST_ASSERT_EQUAL_INT64(1, recovery.revision);
    TEST_ASSERT_EQUAL_size_t(2U, recovery.snapshot_len);
    TEST_ASSERT_EQUAL_MEMORY("{}", recovery.snapshot, 2U);
    TEST_ASSERT_EQUAL_size_t(0U, recovery.op_count);
    TEST_ASSERT_FALSE(recovery.has_metadata);
    TEST_ASSERT_FALSE(tp_journal_contains(
        journal, "11111111111111111111111111111111"));
    TEST_ASSERT_TRUE(tp_journal__is_poisoned(journal));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

void test_bad_checksum_with_valid_suffix_preserves_committed_prefix(void) {
    size_t len = 0U;
    uint8_t *bytes = read_fixture(&len);
    bytes[META_CRC_OFFSET] ^= 0x01U;
    assert_corrupt_meta_preserves_checkpoint_prefix(bytes, len);
    free(bytes);
}

void test_bad_length_with_valid_suffix_preserves_committed_prefix(void) {
    size_t len = 0U;
    uint8_t *bytes = read_fixture(&len);
    bytes[META_LENGTH_OFFSET + 0] = 0x7fU;
    bytes[META_LENGTH_OFFSET + 1] = 0xffU;
    bytes[META_LENGTH_OFFSET + 2] = 0xffU;
    bytes[META_LENGTH_OFFSET + 3] = 0xffU;
    assert_corrupt_meta_preserves_checkpoint_prefix(bytes, len);
    free(bytes);
}

void test_torn_history_tail_keeps_all_complete_prior_records(void) {
    size_t len = 0U;
    uint8_t *bytes = read_fixture(&len);
    tp_journal_io io = {0};
    tp_journal *journal = journal_from_bytes(bytes, len - 1U, &io);
    free(bytes);
    tp_journal_recovery recovery = {0};
    tp_error error = {0};
    TEST_ASSERT_EQUAL_INT_MESSAGE(
        TP_STATUS_OK, tp_journal_recover(journal, &recovery, &error),
        error.msg);
    TEST_ASSERT_EQUAL_INT(TP_JOURNAL_RECOVERY_TRUNCATED, recovery.status);
    TEST_ASSERT_FALSE(recovery.mid_stream_corrupt);
    TEST_ASSERT_EQUAL_size_t((size_t)HISTORY_FRAME_OFFSET,
                             recovery.stop_offset);
    TEST_ASSERT_EQUAL_INT(2, recovery.records_recovered);
    TEST_ASSERT_EQUAL_INT64(3, recovery.revision);
    TEST_ASSERT_EQUAL_size_t(1U, recovery.op_count);
    TEST_ASSERT_TRUE(recovery.has_metadata);
    TEST_ASSERT_FALSE(tp_journal__is_poisoned(journal));
    /* The low-level reader classifies and ignores the tail without mutating its
     * store; the model recovery coordinator owns any later safe truncation. */
    TEST_ASSERT_EQUAL_INT64((int64_t)(len - 1U), io.length(io.ctx));
    tp_journal_recovery_free(&recovery);
    tp_journal_destroy(journal);
}

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    g_fixture_path = argv[1];
    UNITY_BEGIN();
    RUN_TEST(test_writer_matches_complete_fixed_journal_bytes);
    RUN_TEST(test_reader_recovers_fixed_mixed_sequence);
    RUN_TEST(test_peek_classifies_the_same_fixed_mixed_sequence);
    RUN_TEST(test_bad_checksum_with_valid_suffix_preserves_committed_prefix);
    RUN_TEST(test_bad_length_with_valid_suffix_preserves_committed_prefix);
    RUN_TEST(test_torn_history_tail_keeps_all_complete_prior_records);
    return UNITY_END();
}
