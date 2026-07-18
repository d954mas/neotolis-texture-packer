#ifndef TP_CORE_SRC_TP_JOURNAL_INTERNAL_H
#define TP_CORE_SRC_TP_JOURNAL_INTERNAL_H

/*
 * Journal internals shared between tp_journal.c / tp_journal_io.c and the
 * fault suite (test_journal.c includes this from src/ the same way test_diff.c
 * includes tp_diff_internal.h). Exposes the byte-exact record layout constants,
 * the endian-stable CRC-32, and the memory-io fault-injection seams so the suite
 * can craft/corrupt records precisely and drive deterministic write failures.
 */

#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_journal.h"
#include "tp_idset_internal.h"

struct tp_journal {
    tp_journal_io io;
    tp_id128 key;
    bool poisoned;
    size_t record_count;
    size_t replay_count;
    size_t replay_operations;
    tp_idset ids;
    int64_t meta_time;
    char *meta_path;
    char *meta_name;
    tp_id128 meta_file_fingerprint;
    bool meta_has_file_fingerprint;
    bool has_meta;
};

size_t tp_journal__record_limit(void);
size_t tp_journal__file_limit(void);

/* Filesystem owners that already opened a regular file with their required
 * no-follow/create-new policy transfer that native descriptor here. The
 * returned io owns and closes it. `native_fd` is a CRT fd on every platform. */
tp_journal_io tp_journal_io_file_adopt_fd(int native_fd);
tp_journal_io tp_journal_io_file_adopt_fd_read(int native_fd);

/* ---- byte-exact on-disk layout (all #define; never a const array bound, which
 * macOS -Wgnu-folding-constant rejects as a VLA) ------------------------------ */
#define TP_JRN_MAGIC_LEN 8
#define TP_JRN_HEADER_LEN 28  /* MAGIC[8] + version u32 + key[16] */
#define TP_JRN_KEY_OFF 12     /* key[16] starts here in the header */
#define TP_JRN_IDLEN 32       /* a transaction id is 32 lowercase-hex chars (no NUL on disk) */
#define TP_JRN_SYNC_FIELD 4   /* record sync-word u32 BE -- first field of every record frame (v2) */
#define TP_JRN_LEN_FIELD 4    /* payload_len u32 BE */
#define TP_JRN_CRC_FIELD 4    /* crc32 u32 BE */

/* v2 framing: every RECORD frame begins with this fixed sync-word so
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
#define TP_JRN_REC_HISTORY 4  /* payload rec_type: compact Undo/Redo transition */
#define TP_JRN_TXN_FIXED 41   /* TXN payload fixed prefix: type(1) + tx_id(32) + revision(8) */
#define TP_JRN_CKPT_FIXED 13  /* CKPT payload fixed prefix: type(1) + revision(8) + id_count(4) */
#define TP_JRN_META_FIXED 13  /* META payload fixed prefix: type(1) + timestamp(8) + path_len(4) */
#define TP_JRN_HISTORY_FIXED 9 /* HISTORY fixed prefix: type(1) + revision(8) */

/* The 8 magic bytes "NTPKJRNL" -- brace-init (NUL-free) so it is exactly 8 bytes. */
extern const uint8_t tp_jrn_magic[TP_JRN_MAGIC_LEN];

/* Endian-stable CRC-32 (IEEE 0xEDB88320), byte-at-a-time with an immutable
 * 256-entry byte table: same output on every OS. Detects a flipped byte or a
 * torn tail. Seed with 0. */
uint32_t tp_jrn_crc32(uint32_t crc, const uint8_t *data, size_t len);

/* Byte-at-a-time big-endian codecs (no memcpy of multi-byte ints, no host-endian
 * punning). Writers assume the caller sized the buffer; readers are bounds-checked
 * by the record parser BEFORE they are called. */
void tp_jrn_put_u32(uint8_t *p, uint32_t v);
void tp_jrn_put_i64(uint8_t *p, int64_t v);
uint32_t tp_jrn_get_u32(const uint8_t *p);
int64_t tp_jrn_get_i64(const uint8_t *p);

/* ---- recovery glue seams (shared with tp_txn_apply.c's tp_model_recover) -------- */

/* Mark the journal poisoned so it refuses further appends. Called by the
 * recovery glue when a torn tail could NOT be truncated away -- a still-present bad
 * record must never hide a later acknowledged append. NULL-safe. */
void tp_journal__poison(tp_journal *j);

/* Test/recovery diagnostic: true iff the journal is poisoned and refuses further
 * appends. A poisoned authority remains attached and fails closed. NULL-safe. */
bool tp_journal__is_poisoned(const tp_journal *j);

/* Aggregate post-checkpoint operation admission shared by the model's pre-clone
 * gate and the counted durable append. Recovery seeds the exact allocation-free
 * pre-count before handing the journal back to a live model. */
tp_status tp_journal__check_replay_operations(const tp_journal *j, size_t add,
                                              tp_error *err);
/* Allocation-free writer admission. The model calls this before request
 * encoding/cloning; the counted append calls it again immediately before
 * durable staging. Retained duplicate ids are filtered before this seam. */
tp_status tp_journal__check_append_admission(const tp_journal *j,
                                             size_t replay_operations,
                                             tp_error *err);
/* Allocation-free byte-cap admission for model transactions. The minimum and
 * exact count-only checks both run before canonical encoding and all mutable
 * model/history staging. */
tp_status tp_journal__check_txn_min_bytes(const tp_journal *j, tp_error *err);
tp_status tp_journal__check_txn_bytes(const tp_journal *j, size_t request_bytes,
                                      tp_error *err);
/* Exact checkpoint admission including fixed framing and the journal's retained
 * ID set. Append uses the current store length and total-frame slot; compact
 * uses a fresh header because the old store will be replaced. Both are
 * allocation-free and are repeated by the final journal writer. */
tp_status tp_journal__check_checkpoint_append_bytes(const tp_journal *j,
                                                     size_t snapshot_bytes,
                                                     tp_error *err);
tp_status tp_journal__check_checkpoint_compact_bytes(const tp_journal *j,
                                                      size_t snapshot_bytes,
                                                      tp_error *err);
/* Exact compact-HISTORY frame admission. The history layer calls this after
 * its count-only codec pass and before staging a candidate project. */
tp_status tp_journal__check_history_append_bytes(const tp_journal *j,
                                                 size_t transition_bytes,
                                                 size_t replay_operations,
                                                 tp_error *err);
tp_status tp_journal__set_replay_operations(tp_journal *j, size_t count,
                                            tp_error *err);

/* The durable transaction append is not a client-facing journal API. The model
 * owns the counted acknowledgement gate; recovery codec tests use the uncounted
 * fixture form for deliberately non-transaction payloads. Both remain internal
 * so no frontend can invent or omit replay-operation accounting. */
tp_status tp_journal_append_txn_counted(tp_journal *j, const char *id_hex,
                                        int64_t revision, const uint8_t *payload,
                                        size_t len, size_t replay_operations,
                                        tp_error *err);
tp_status tp_journal_append_txn(tp_journal *j, const char *id_hex,
                                int64_t revision, const uint8_t *fixture_payload,
                                size_t len, tp_error *err);
tp_status tp_journal_append_history_counted(tp_journal *j, int64_t revision,
                                            const uint8_t *payload, size_t len,
                                            size_t replay_operations,
                                            tp_error *err);

/* Test-only direct probe for the corruption resync scanner. Returns the same
 * conservative "valid record or budget exhausted" decision as production and
 * reports aggregate CRC bytes without shared mutable state. */
bool tp_journal__test_has_valid_record_after(const uint8_t *buf, size_t len, size_t from,
                                             size_t *crc_work_out);

/* Test-only ownership probe: true iff every recovered op payload is a bounded span inside the
 * result's one raw-record owner (therefore no per-operation payload allocation/copy exists). */
bool tp_journal__test_recovery_ops_borrow_raw(const tp_journal_recovery *recovery);

typedef struct tp_journal_recovery_copy_stats {
    size_t raw_storage_copies;
    size_t raw_storage_bytes;
    size_t checkpoint_payload_copies;
    size_t checkpoint_payload_bytes;
    size_t operation_payload_copies;
    size_t operation_payload_bytes;
} tp_journal_recovery_copy_stats;

/* Test/benchmark accounting derived from the result's actual ownership graph.
 * One read_all materialization is the raw-storage copy. Any checkpoint/op span
 * outside that owner is reported as an additional payload copy. */
void tp_journal__test_recovery_copy_stats(
    const tp_journal_recovery *recovery,
    tp_journal_recovery_copy_stats *out);

/* Test-only deterministic override for the total CRC-valid frame budget.
 * Zero restores TP_JOURNAL_MAX_RECORDS. */
void tp_journal__test_set_record_limit(size_t limit);
/* Zero restores TP_JOURNAL_MAX_FILE_BYTES. */
void tp_journal__test_set_file_limit(size_t limit);
/* Fail the next recovery metadata materialization before it allocates. */
void tp_journal__test_fail_next_metadata_materialize(void);

/* Register an already-durable retained id into the in-memory index
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

/* Make the next durability barrier fail. The record bytes may have reached the
 * backing store, but they are never acknowledged; write_record rolls back its
 * append and poisons the authority. */
void tp_journal_io_memory__fail_next_sync(tp_journal_io io);

/* Number of durability-barrier calls observed by the memory backend. This lets
 * the fault suite prove a failed append also attempts to durably confirm its
 * successful rollback. */
size_t tp_journal_io_memory__sync_count(tp_journal_io io);

/* Overwrite one byte of the backing store in place (flip a byte / corrupt a record
 * for the checksum-mismatch test). No-op if `at` is out of range. */
void tp_journal_io_memory__poke(tp_journal_io io, size_t at, uint8_t val);

/* Force the backing store's length to `len` (truncate to any byte boundary for the
 * short-write-at-every-boundary test). No-op if `len` exceeds the current length. */
void tp_journal_io_memory__set_len(tp_journal_io io, size_t len);

#endif /* TP_CORE_SRC_TP_JOURNAL_INTERNAL_H */
