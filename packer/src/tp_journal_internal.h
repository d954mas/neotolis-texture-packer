#ifndef TP_CORE_SRC_TP_JOURNAL_INTERNAL_H
#define TP_CORE_SRC_TP_JOURNAL_INTERNAL_H

/*
 * F2-04 journal internals shared between tp_journal.c / tp_journal_io.c and the
 * fault suite (test_journal.c includes this from src/ the same way test_diff.c
 * includes tp_diff_internal.h). Exposes the byte-exact record layout constants,
 * the endian-stable CRC-32, and the memory-io fault-injection seams so the suite
 * can craft/corrupt records precisely and drive deterministic write failures.
 */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_journal.h"

/* ---- byte-exact on-disk layout (all #define; never a const array bound, which
 * macOS -Wgnu-folding-constant rejects as a VLA) ------------------------------ */
#define TP_JRN_MAGIC_LEN 8
#define TP_JRN_HEADER_LEN 28  /* MAGIC[8] + version u32 + key[16] */
#define TP_JRN_KEY_OFF 12     /* key[16] starts here in the header */
#define TP_JRN_IDLEN 32       /* a transaction id is 32 lowercase-hex chars (no NUL on disk) */
#define TP_JRN_SYNC_FIELD 4   /* record sync-word u32 BE -- first field of every record frame (v2) */
#define TP_JRN_LEN_FIELD 4    /* payload_len u32 BE */
#define TP_JRN_CRC_FIELD 4    /* crc32 u32 BE */

/* F2-04 v2 (plan S18 R / P1-5): every RECORD frame begins with this fixed sync-word so
 * recovery can tell a genuinely torn TAIL from a mid-stream record whose length field was
 * corrupted to a bloated value -- which would otherwise masquerade as a torn tail and get
 * the acknowledged records AFTER it deleted. On any framing/CRC break, recovery scans
 * forward for the next sync-word that begins a CRC-valid record: found => mid-stream
 * corruption (preserve the file + poison), not found => a truncatable tail. A sync-word
 * that collides with payload bytes self-rejects on the CRC check, so it is only a hint. */
#define TP_JRN_SYNC_WORD 0xA5C31E7Bu
#define TP_JRN_REC_TXN 1      /* payload rec_type: committed transaction */
#define TP_JRN_REC_CKPT 2     /* payload rec_type: checkpoint (state + retained id set) */
#define TP_JRN_REC_META 3     /* payload rec_type: project metadata {timestamp, path, name} (R5) */
#define TP_JRN_TXN_FIXED 41   /* TXN payload fixed prefix: type(1) + tx_id(32) + revision(8) */
#define TP_JRN_CKPT_FIXED 13  /* CKPT payload fixed prefix: type(1) + revision(8) + id_count(4) */
#define TP_JRN_META_FIXED 13  /* META payload fixed prefix: type(1) + timestamp(8) + path_len(4) */

/* The 8 magic bytes "NTPKJRNL" -- brace-init (NUL-free) so it is exactly 8 bytes. */
extern const uint8_t tp_jrn_magic[TP_JRN_MAGIC_LEN];

/* Endian-stable CRC-32 (IEEE 0xEDB88320), byte-at-a-time, table-less: same output
 * on every OS. Detects a flipped byte or a torn tail. Seed with 0. */
uint32_t tp_jrn_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Byte-at-a-time big-endian codecs (no memcpy of multi-byte ints, no host-endian
 * punning). Writers assume the caller sized the buffer; readers are bounds-checked
 * by the record parser BEFORE they are called. */
void tp_jrn_put_u32(uint8_t *p, uint32_t v);
void tp_jrn_put_i64(uint8_t *p, int64_t v);
uint32_t tp_jrn_get_u32(const uint8_t *p);
int64_t tp_jrn_get_i64(const uint8_t *p);

/* ---- recovery glue seams (shared with tp_txn_apply.c's tp_model_recover) -------- */

/* F2-04 fix C3: mark the journal poisoned so it refuses further appends. Called by the
 * recovery glue when a torn tail could NOT be truncated away -- a still-present bad
 * record must never hide a later acknowledged append. NULL-safe. */
void tp_journal__poison(tp_journal *j);

/* R3: true iff the journal is poisoned (refuses further appends). The compaction glue reads it to
 * distinguish a broken-store compaction (truncate OK but the fresh checkpoint could not be written
 * -> poisoned -> detach + run journal-less) from a benign truncate failure (store intact, journal
 * healthy -> keep it). NULL-safe (false). */
bool tp_journal__is_poisoned(const tp_journal *j);

/* F2-04 fix C1: register an already-durable retained id into the in-memory index
 * WITHOUT writing a record. tp_model_attach_journal calls it to migrate ids the model
 * committed journal-less, BEFORE the initial checkpoint (so the checkpoint id-list AND
 * the live index both carry them). OOM -> non-OK; a bad/NULL arg -> INVALID_ARGUMENT. */
tp_status tp_journal_seed_retained_id(tp_journal *j, const char *id_hex);

/* ---- memory-io fault seams (test-only; default off) ---------------------- *
 * Operate on an io returned by tp_journal_io_memory. A NULL/foreign io is a no-op. */

/* Make the next `n` write() calls fail entirely (return 0 bytes). */
void tp_journal_io_memory__fail_next_writes(tp_journal_io io, int n);

/* Make the NEXT write() a SHORT write of exactly `n` bytes (simulates a torn
 * record); the partial bytes ARE appended so the store holds a torn tail. -1 off. */
void tp_journal_io_memory__short_next_write(tp_journal_io io, int64_t n);

/* Make the next truncate() call fail (return -1). Proves the poison-on-truncate-
 * failure path. */
void tp_journal_io_memory__fail_next_truncate(tp_journal_io io);

/* Overwrite one byte of the backing store in place (flip a byte / corrupt a record
 * for the checksum-mismatch test). No-op if `at` is out of range. */
void tp_journal_io_memory__poke(tp_journal_io io, size_t at, uint8_t val);

/* Force the backing store's length to `len` (truncate to any byte boundary for the
 * short-write-at-every-boundary test). No-op if `len` exceeds the current length. */
void tp_journal_io_memory__set_len(tp_journal_io io, size_t len);

#endif /* TP_CORE_SRC_TP_JOURNAL_INTERNAL_H */
