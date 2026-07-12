#ifndef TP_C0_JOURNAL_H
#define TP_C0_JOURNAL_H

/*
 * C0-03 task 1: minimum recovery-journal record/checkpoint framing, payload
 * checksum, truncation recovery, and the idempotency retention seam.
 *
 * Master spec §7.1 (commit is not visible until appended to the recovery
 * journal), §7.2 (the retained transaction-id set must be recoverable from the
 * journal so an acknowledged transaction is not duplicated after restart),
 * §22.1/§22.3 (local journal + periodic checkpoints; committed model
 * transactions survive process restart; power loss without fsync is not
 * guaranteed). This is a CONTRACT SPIKE: it pins the on-disk RECORD FRAMING that
 * a real journal writer/reader must agree on. Checkpoint cadence, compaction,
 * dedup retention window, corruption policy beyond torn-tail/bad-checksum
 * detection, and optional fsync modes stay OPEN per §60 item 1 / §52.5.
 *
 * Determinism: every field is fixed-width little-endian, written byte-by-byte
 * (no host endianness / long-width leakage), and the checksum reuses the
 * portable C0-01 FNV-1a/128 (tp_c0_hash128). A golden byte vector therefore
 * round-trips byte-identically on Linux/macOS/Windows.
 *
 * On-disk record layout (all integers LITTLE-ENDIAN):
 *
 *   offset size field
 *   0      4    magic       = 'T','P','J','1' (TP_C0_JOURNAL_MAGIC0..3)
 *   4      2    version     = TP_C0_JOURNAL_VERSION
 *   6      2    kind        = tp_c0_journal_kind
 *   8      4    payload_len = length of the payload that follows
 *   12     16   checksum    = tp_c0_hash128(payload) (16 raw bytes, bytes[0] MSB)
 *   28     N    payload     = payload_len bytes
 *
 * The checksum covers the PAYLOAD only (per the task). The per-record magic +
 * bounds guard the header; a length that stays in-bounds but is wrong makes the
 * hashed bytes differ -> journal_bad_checksum. A checkpoint compacts/obsoletes
 * earlier records but that policy is out of the spike (§60 item 1).
 *
 * Payload shapes the framing layer understands (so recovery can rebuild the
 * idempotency set without parsing the opaque transaction body):
 *
 *   TXN payload:        txn_id[16] | revision_after (int64 LE, 8) | body[...]
 *   CHECKPOINT payload: revision (int64 LE, 8) | state_hash[16] | body[...]
 *
 * `body` is opaque to the framing layer (a future binary transaction encoding or
 * the C0-02 JSON); the framing only stores its length and checksum.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_c0/tp_c0_error.h"
#include "tp_c0/tp_c0_id.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TP_C0_JOURNAL_MAGIC0 'T'
#define TP_C0_JOURNAL_MAGIC1 'P'
#define TP_C0_JOURNAL_MAGIC2 'J'
#define TP_C0_JOURNAL_MAGIC3 '1'
#define TP_C0_JOURNAL_VERSION 1u

/* Fixed record header: magic(4)+version(2)+kind(2)+payload_len(4)+checksum(16). */
#define TP_C0_JOURNAL_HEADER_SIZE 28u

/* Minimum payload sizes for the framing-known record shapes. */
#define TP_C0_JOURNAL_TXN_PREFIX 24u        /* txn_id[16] + revision_after(8) */
#define TP_C0_JOURNAL_CHECKPOINT_PREFIX 24u /* revision(8) + state_hash[16] */

typedef enum tp_c0_journal_kind {
    TP_C0_JREC_INVALID = 0,
    TP_C0_JREC_TXN = 1,        /* one committed transaction (idempotency seam) */
    TP_C0_JREC_CHECKPOINT = 2, /* periodic/full checkpoint marker */
    TP_C0_JREC_KIND_COUNT      /* append new kinds before this; never reorder */
} tp_c0_journal_kind;

/* One decoded record (view into the caller's buffer; `body` points INTO it). */
typedef struct tp_c0_journal_record {
    tp_c0_journal_kind kind;
    /* TXN fields (valid when kind == TP_C0_JREC_TXN). */
    tp_c0_id128 txn_id;
    int64_t revision;       /* TXN: revision_after; CHECKPOINT: checkpoint revision */
    tp_c0_id128 state_hash; /* CHECKPOINT only: semantic-state identity placeholder */
    const uint8_t *body;    /* opaque tail after the framing-known prefix (may be NULL) */
    size_t body_len;
    size_t record_len; /* total on-disk bytes consumed by this record */
} tp_c0_journal_record;

/* ---- encode one record into a caller buffer ----------------------------- */

/* Encode a committed-transaction record. `body`/`body_len` is the opaque tail
 * (may be NULL/0). Writes the full record to `out`; sets *written. A buffer that
 * cannot hold the record returns buffer_too_small (drives the ack APPEND_FAIL
 * path). nil txn_id is rejected (id_nil). A body whose framed size would overflow
 * the u32 on-disk length field or the size_t byte math is rejected with
 * journal_too_large BEFORE any copy (no truncated length, no memcpy past cap). */
tp_c0_detail tp_c0_journal_encode_txn(tp_c0_id128 txn_id, int64_t revision_after, const void *body, size_t body_len,
                                      uint8_t *out, size_t cap, size_t *written, tp_error *err);

/* Encode a checkpoint record. `state_hash` is the semantic-state identity anchor
 * captured at `revision` (a placeholder in the spike). */
tp_c0_detail tp_c0_journal_encode_checkpoint(int64_t revision, tp_c0_id128 state_hash, const void *body, size_t body_len,
                                             uint8_t *out, size_t cap, size_t *written, tp_error *err);

/* ---- decode one record from a byte buffer ------------------------------- */

/* Decode the single record at the front of [buf, buf+len). On success fills
 * *out (with out->record_len set) and returns TP_C0_OK. Faults:
 *   journal_short        -> fewer bytes than a full header/payload (torn/short write)
 *   journal_bad_magic    -> header magic mismatch
 *   journal_bad_version  -> unsupported framing version
 *   journal_bad_kind     -> unknown record kind, or payload shorter than the
 *                           framing-known prefix for a known kind
 *   journal_bad_checksum -> payload checksum mismatch
 * Never reads out of bounds and never aborts. */
tp_c0_detail tp_c0_journal_decode(const uint8_t *buf, size_t len, tp_c0_journal_record *out, tp_error *err);

/* ---- recovery scan + idempotency retention set -------------------------- */

/* Spike cap on recovered transaction ids (fixed for determinism; a production
 * journal uses dynamic storage so the retained set is ALWAYS complete, §60 item
 * 1). Hitting this cap is a spike artifact, reported as the distinct
 * journal_retention_full outcome (NOT corruption, NOT a torn tail). */
#define TP_C0_JOURNAL_MAX_TXNS 64

/* Result of scanning a journal byte buffer from the start. Records before the
 * first torn/corrupt record (or the retention cap) are recovered; the scan then
 * stops (spike policy: stop at first bad record rather than resync past a gap).
 *
 * `stop_reason` is the SINGLE SOURCE OF TRUTH for why the scan stopped; the
 * truncated / corrupt / capped predicates below are derived from it, so there are
 * no hand-synced bool fields that can drift out of agreement with the reason. */
typedef struct tp_c0_journal_recovery {
    tp_c0_id128 txns[TP_C0_JOURNAL_MAX_TXNS]; /* committed txn ids, in journal order */
    int64_t txn_revisions[TP_C0_JOURNAL_MAX_TXNS];
    int txn_count;
    int64_t last_revision;      /* highest revision seen (txn or checkpoint); -1 if none */
    int64_t checkpoint_revision;/* revision of the last checkpoint; -1 if none */
    size_t valid_bytes;         /* bytes consumed by cleanly recovered records */
    tp_c0_detail stop_reason;   /* TP_C0_OK if clean EOF, else the fault/cap that stopped the scan */
} tp_c0_journal_recovery;

/* Scan [buf, buf+len) from the start, recovering every clean record until EOF, the
 * first bad record, or the retention cap. Always returns TP_C0_OK (a torn tail is
 * EXPECTED after a crash, not a caller error); inspect stop_reason / the predicates
 * for what happened. A NULL buf with len 0 is an empty (clean) journal. If more
 * than TP_C0_JOURNAL_MAX_TXNS clean txns are present the scan stops with
 * stop_reason == journal_retention_full and the recovered id set is PARTIAL --
 * callers MUST treat tp_c0_journal_recovery_capped() as "cannot safely dedup"
 * (production uses dynamic storage and never caps). */
tp_c0_detail tp_c0_journal_recover(const uint8_t *buf, size_t len, tp_c0_journal_recovery *out, tp_error *err);

/* ---- derived recovery predicates (over stop_reason, the single source of truth) */

/* Stopped on a torn/short FINAL record -- the EXPECTED crash case; the clean
 * prefix is fully recovered and this is not data loss. */
static inline bool tp_c0_journal_recovery_truncated(const tp_c0_journal_recovery *rec) {
    return rec && rec->stop_reason == TP_C0_ERR_JOURNAL_SHORT;
}

/* Stopped on bad magic/version/kind/checksum -- unexpected corruption (records
 * after the clean prefix are lost). */
static inline bool tp_c0_journal_recovery_corrupt(const tp_c0_journal_recovery *rec) {
    if (!rec) {
        return false;
    }
    switch (rec->stop_reason) {
        case TP_C0_ERR_JOURNAL_BAD_MAGIC:
        case TP_C0_ERR_JOURNAL_BAD_VERSION:
        case TP_C0_ERR_JOURNAL_BAD_KIND:
        case TP_C0_ERR_JOURNAL_BAD_CHECKSUM:
            return true;
        default:
            return false;
    }
}

/* Stopped because the fixed spike retention set filled -- a spike CAP, NOT
 * corruption and NOT a torn tail. The recovered id set is PARTIAL: a retried txn
 * beyond the cap would not be seen as a duplicate, so the caller cannot safely
 * dedup and must fail safe. Production uses dynamic storage and never caps. */
static inline bool tp_c0_journal_recovery_capped(const tp_c0_journal_recovery *rec) {
    return rec && rec->stop_reason == TP_C0_ERR_JOURNAL_RETENTION_FULL;
}

/* Idempotency retention seam (§7.2): true iff `txn_id` was already committed in
 * the recovered set -- a duplicate append after restart is a no-op, not a second
 * apply. Callers seed the C0-02 tp_c0_txn_idset from this so a retried txn is
 * detected as txn_duplicate_id. */
bool tp_c0_journal_recovery_has_txn(const tp_c0_journal_recovery *rec, tp_c0_id128 txn_id);

#ifdef __cplusplus
}
#endif

#endif /* TP_C0_JOURNAL_H */
