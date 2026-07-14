#ifndef TP_CORE_TP_JOURNAL_H
#define TP_CORE_TP_JOURNAL_H

/*
 * F2-04 minimum recovery journal (master spec §7.1-7.2, §22.3, §59 items 19,
 * 46-49, 52). A commit is not acknowledged as successful until its recovery
 * record is durably appended; the retained transaction-ID set is recoverable
 * from the journal after a process restart so an acknowledged transaction is
 * never applied twice (§7.2 idempotency).
 *
 * SCOPE: this is a PURE durable log over an injectable I/O seam -- it deals in
 * opaque project-SNAPSHOT blobs + 32-hex transaction ids and knows nothing about
 * tp_project. The model<->journal glue (attach / commit append / recover-to-model)
 * lives in tp_transaction.h, which owns tp_model. The journal is a SIDECAR: it is
 * NOT part of `.ntpacker_project` and does not change project serialization (the
 * byte-identity goldens depend on this).
 *
 * ON-DISK FORMAT (byte-deterministic, endian-stable, self-describing, checksummed):
 *   header : MAGIC[8] "NTPKJRNL" | format_version u32 BE | key[16]
 *   record : sync u32 BE (TP_JRN_SYNC_WORD) | payload_len u32 BE | payload[payload_len] | crc32 u32 BE
 *            crc32 covers (sync ++ payload_len ++ payload). All multi-byte integers are
 *            written byte-at-a-time big-endian (no host-endian punning). The fixed
 *            per-record sync-word lets recovery re-synchronise after a corrupt length
 *            field (plan S18 R / P1-5): a torn TAIL (no valid record follows) is truncated,
 *            a mid-stream corruption (a valid record still follows) is preserved.
 *   payload: rec_type u8 (1=TXN, 2=CHECKPOINT) then --
 *            TXN        : tx_id[32] hex | revision i64 BE | snapshot[rest]
 *            CHECKPOINT : revision i64 BE | id_count u32 BE | id[32]*id_count | snapshot[rest]
 *
 * The reader is UB-clean on arbitrary/corrupt/short/torn input: every field is
 * bounds-checked (size_t math) before it is touched, and replay STOPS at the first
 * undecodable record -- it never guesses corrupt content (§60 corruption policy).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h" /* tp_id128: the 16-byte journal key (path hash / session id) */

#ifdef __cplusplus
extern "C" {
#endif

/* Current on-disk journal format version -- the only version this build reads. v2 adds
 * the per-record sync-word (plan S18 R / P1-5 framing robustness); a v1 journal is treated
 * as an incompatible BAD_HEADER (the sidecar is ephemeral and compacted away on Save). */
#define TP_JOURNAL_FORMAT_VERSION 2

/* ---- injectable I/O seam ------------------------------------------------- *
 * The journal never calls the filesystem directly: all durability goes through
 * this seam, so the fault suite drives an in-memory backing store with a write-
 * failure countdown and the reader is exercised on crafted/corrupt bytes without
 * real-fs flakiness. int64_t (not long) avoids the Windows 32-bit-long trap. */
typedef struct tp_journal_io {
    void *ctx;
    /* Append exactly `len` bytes at the end of the store. Returns bytes written;
     * a value < len (or negative) is a short write / failure. */
    int64_t (*write)(void *ctx, const uint8_t *data, size_t len);
    /* Current store length in bytes, or negative on error. */
    int64_t (*length)(void *ctx);
    /* Truncate the store to `len` bytes (rolls a failed append back). 0 / <0. */
    int (*truncate)(void *ctx, size_t len);
    /* Read the WHOLE store into a fresh malloc'd buffer (*out, *out_len); caller
     * frees *out. 0 on success (empty store -> *out NULL, *out_len 0, return 0);
     * negative on failure. */
    int (*read_all)(void *ctx, uint8_t **out, size_t *out_len);
    /* Best-effort flush to stable storage; may be NULL. 0 / <0. */
    int (*sync)(void *ctx);
    /* Free ctx; may be NULL for a borrowed store. */
    void (*destroy)(void *ctx);
} tp_journal_io;

/* In-memory backing store (a growable byte buffer). Deterministic on every OS --
 * this is what the fault suite drives. On OOM the returned io has ctx == NULL
 * (tp_journal_create then fails cleanly). */
tp_journal_io tp_journal_io_memory(void);

/* File backing store at `path`, opened for read + append (created if absent). Real
 * on-disk durability for production wiring (F2-05). On open failure ctx == NULL. */
tp_journal_io tp_journal_io_file(const char *path);

/* ---- the journal object (opaque) ----------------------------------------- */
typedef struct tp_journal tp_journal;

/* Create a journal over `io` (TAKES OWNERSHIP of io -- destroy calls io.destroy).
 * `key` is stored in the file header and checked on recovery so a stale journal
 * for a moved project is detected, not misapplied. Writes NOTHING yet. NULL on OOM
 * (io is destroyed). */
tp_journal *tp_journal_create(tp_journal_io io, tp_id128 key);

/* Frees the journal and its owned io. NULL-safe. */
void tp_journal_destroy(tp_journal *j);

/* Write the header (once) + a CHECKPOINT record capturing the committed `snapshot`
 * (project bytes) at `revision` together with the journal's CURRENT retained-id set
 * -- the durable baseline recovery replays from, and the compaction primitive. On
 * I/O failure the store is rolled back to its prior length and TP_STATUS_JOURNAL_FAILED
 * is returned (nothing partially durable). */
tp_status tp_journal_init_checkpoint(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision,
                                     tp_error *err);

/* Append one committed transaction record {id_hex(32 hex), revision, snapshot}.
 * This is the ACKNOWLEDGEMENT gate: a transaction is committed only once this
 * returns OK. It is the LAST fallible step of a commit -- everything after a
 * successful append is allocation-free. Internally, in order:
 *   (1) reserve an in-memory retained-id slot (OOM -> TP_STATUS_OOM, nothing written);
 *   (2) durably write the framed record (short write / I/O error -> the store is
 *       truncated back to its prior length, TP_STATUS_JOURNAL_FAILED);
 *   (3) infallibly register id_hex in the reserved slot.
 * On ANY non-OK return nothing was durably retained and id_hex is NOT registered,
 * so the same transaction id stays retryable (never poisoned to DUPLICATE_ID). */
tp_status tp_journal_append_txn(tp_journal *j, const char *id_hex, int64_t revision, const uint8_t *snapshot,
                                size_t len, tp_error *err);

/* Idempotency query: true iff `id_hex` is in the retained-id index. NULL-safe. */
bool tp_journal_contains(const tp_journal *j, const char *id_hex);

/* Number of retained transaction ids currently indexed. */
int tp_journal_id_count(const tp_journal *j);

/* ---- recovery ------------------------------------------------------------ *
 * How replay classified the backing store. Corruption is EXPECTED and reported
 * here (not an error return): the caller falls back to the last good record and
 * never guesses corrupt content. */
typedef enum tp_journal_recovery_status {
    TP_JOURNAL_RECOVERY_OK = 0,     /* decoded cleanly to EOF, no torn/corrupt tail */
    TP_JOURNAL_RECOVERY_EMPTY,      /* valid header, zero records (or empty store) */
    TP_JOURNAL_RECOVERY_TRUNCATED,  /* torn/short tail ignored; recovered up to last good record */
    TP_JOURNAL_RECOVERY_CORRUPT,    /* a record failed its checksum mid-stream; stopped, recovered prior */
    TP_JOURNAL_RECOVERY_BAD_HEADER, /* magic/version invalid; nothing recovered */
    TP_JOURNAL_RECOVERY_STALE_KEY   /* header valid but key != expected (moved project); NOT applied */
} tp_journal_recovery_status;

typedef struct tp_journal_recovery {
    tp_journal_recovery_status status;
    size_t bytes_total;    /* total bytes in the backing store */
    size_t stop_offset;    /* byte offset where decoding stopped (end of last good record) */
    bool mid_stream_corrupt; /* CORRUPT with a decodable/complete record STILL PRESENT after the
                              * bad one: the corruption is mid-log, NOT a tail. The tail-cleanup
                              * truncation is UNSAFE here (it would delete the trailing acknowledged
                              * records); recovery preserves the file and poisons the journal. A
                              * torn tail or a single trailing corrupt record leaves this false. */
    int records_recovered; /* count of good records applied (checkpoint + txn) */
    int64_t revision;      /* revision of the recovered committed state (0 if none) */
    char *snapshot;        /* owned: recovered project bytes of the LAST good record, NUL-terminated
                            * (*snapshot_len excludes the NUL), or NULL if none. Caller frees via
                            * tp_journal_recovery_free (or moves it out). */
    size_t snapshot_len;
} tp_journal_recovery;

void tp_journal_recovery_free(tp_journal_recovery *r);

/* Replay the backing store: validate the header (magic/version/key), decode records
 * front-to-back, STOP at the first undecodable record (never guessing corrupt
 * content), populate the journal's retained-id index from the good records, and
 * fill *out (the last good record's snapshot + revision + a structured status).
 * UB-clean on arbitrary/corrupt/short/torn bytes. Returns TP_STATUS_OK whenever
 * replay ran (corruption is reported in out->status); non-OK only on a hard fault
 * (io read failure / OOM building the result). */
tp_status tp_journal_recover(tp_journal *j, tp_journal_recovery *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_JOURNAL_H */
