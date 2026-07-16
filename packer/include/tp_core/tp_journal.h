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
 * opaque record payloads (a CHECKPOINT carries a project snapshot; a committed TXN
 * carries its serialized operation -- format B) + 32-hex transaction ids and knows
 * nothing about tp_project. The model<->journal glue (attach / commit append / recover-to-model)
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
 *   payload: rec_type u8 (1=TXN, 2=CHECKPOINT, 3=METADATA) then --
 *            TXN        : tx_id[32] hex | revision i64 BE | op_payload[rest]
 *            CHECKPOINT : revision i64 BE | id_count u32 BE | id[32]*id_count | snapshot[rest]
 *            METADATA   : timestamp i64 BE | path_len u32 BE | path | name_len u32 BE | name (v3, R5a)
 *
 * FORMAT B (plan S18 R / R2): a TXN record's payload is the SERIALIZED OPERATION REQUEST
 * (a tp_txn_request_encode blob), NOT a full snapshot. Only CHECKPOINT records carry a
 * project snapshot. Recovery loads the last checkpoint's snapshot as a base and REPLAYS the
 * post-checkpoint TXN op-payloads onto it in commit order (via the model<->journal glue) --
 * a diff journal, so a committed transaction costs its op bytes, not a whole re-snapshot.
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

/* Current on-disk journal format version -- the only version this build reads. v2 added the
 * per-record sync-word (plan S18 R / P1-5 framing robustness); v3 adds the METADATA record type
 * (R5a). A journal written by a different format version is treated as a VERSION_MISMATCH (our
 * file, wrong format), distinct from a BAD_MAGIC foreign file (R5a). NO back-compat: a pre-v3
 * journal is surfaced as VERSION_MISMATCH (never silently mis-replayed as if the new record type
 * did not exist). The sidecar is ephemeral and compacted away on Save. */
#define TP_JOURNAL_FORMAT_VERSION 3

/* Shared writer/reader bound for one sidecar. Every durable append is rejected before record allocation/write
 * when its framed result would cross this limit, so an acknowledged operation can never create a journal
 * that startup recovery refuses. Recovery also rejects foreign/legacy oversized files before allocation.
 * Normal Save compaction reclaims the append budget. */
#define TP_JOURNAL_MAX_FILE_BYTES (64U * 1024U * 1024U)

/* Explicit component budgets within the file bound. They prevent hostile tiny
 * records from turning byte-bounded recovery into an unbounded descriptor
 * allocation while preserving the calibrated 64 MiB recovery fixture. */
/* 28-byte header + 12-byte record frame leave this exact payload maximum. */
#define TP_JOURNAL_MAX_RECORD_BYTES (TP_JOURNAL_MAX_FILE_BYTES - 40U)
/* Total CRC-valid frames of every type in one sidecar. This is independent of
 * the post-checkpoint replay window: repeated META/CKPT frames are bounded too. */
#define TP_JOURNAL_MAX_RECORDS 524288U
/* Independent post-checkpoint replay budgets. The record limit bounds descriptor
 * count; the operation limit bounds the aggregate decoded work across those
 * records. Both reset only after a durable checkpoint. */
#define TP_JOURNAL_MAX_REPLAY_RECORDS 262144U
#define TP_JOURNAL_MAX_REPLAY_OPERATIONS 262144U

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
    /* Read the WHOLE store into a fresh malloc'd buffer (*out, *out_len), but
     * reject before allocation/copy when its live length exceeds `max_len`.
     * Caller frees *out. 0 on success (empty store -> *out NULL, *out_len 0,
     * return 0); negative on failure. */
    int (*read_all)(void *ctx, size_t max_len, uint8_t **out,
                    size_t *out_len);
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

/* R5b-2 fix [3]: READ-ONLY, NO-CREATE file backing store at `path` -- opens "rb" ONLY and NEVER
 * creates the file. On a missing/unopenable path returns a {0} io (ctx == NULL) so a racing/vanished
 * candidate is skipped rather than resurrected as a stray zero-byte journal (the peek + adopt-source
 * scan contract is strictly read-only). read_all/length are real; write/truncate are failing stubs
 * (they never write), so tp_journal_peek + tp_model_recover -- which only read, plus one best-effort
 * tail-truncate whose failure is harmless when the recovered model is cloned-off and discarded -- work
 * over it while any attempt to durably append fails closed. sync is NULL (nothing to flush). */
tp_journal_io tp_journal_io_file_read(const char *path);

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

/* R3 (plan S18 R / spec §22.3): COMPACT the store to a single fresh CHECKPOINT -- the
 * Save-window reset. Truncates the backing store to 0 (dropping the old checkpoint + every
 * post-checkpoint TXN record + any poisoned/corrupt record) and re-emits header + one
 * CHECKPOINT capturing `snapshot`/`revision` and the journal's CURRENT retained-id set, so
 * recovery after a Save replays from exactly the saved state. The compaction PRESERVES the
 * retained-id set + revision (only the byte store is reset; the in-memory id index is kept so
 * an already-acknowledged transaction id still de-duplicates after a Save -- §7.2). On a
 * successful truncate the poison flag is cleared (nothing left to hide behind); if the truncate
 * FAILS the store + poison are left intact and TP_STATUS_JOURNAL_FAILED is returned (fail-closed
 * -- nothing partially compacted). NULL journal -> TP_STATUS_INVALID_ARGUMENT. */
tp_status tp_journal_compact(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision, tp_error *err);

/* R5a: record the owning project's metadata {timestamp, path, name} so a startup scan can list
 * this journal's crashed project by path + name + time. The values are cached on the journal for
 * compaction and appended durably immediately. EVERY append failure is returned, even when the
 * partially-written record was rolled back and the journal remains healthy: a caller that uses the
 * metadata as Save-Original authority must detach/disable that recovery slot rather than let a crash
 * expose an older path/fingerprint. `path`/`name` are UTF-8 and may be empty (untitled project ->
 * path ""); NULL is treated as "". `timestamp` is a caller-supplied unix-seconds value (core stays
 * deterministic -- it never calls time()). NULL journal -> INVALID_ARGUMENT. */
tp_status tp_journal_set_metadata(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                  tp_error *err);

/* Extended metadata for recovery conflict detection. `file_fingerprint` is the exact-byte
 * fingerprint of the original saved project at the time this metadata is recorded; NULL means
 * no saved-file baseline (untitled or legacy caller). The fingerprint is an optional trailer in
 * the existing journal format, so older v3 metadata remains readable. */
tp_status tp_journal_set_metadata_ex(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                     const tp_id128 *file_fingerprint, tp_error *err);

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
    /* R5a (plan item 7): the old conflated BAD_HEADER split two ways so the R5b startup scan can
     * distinguish a foreign file from an out-of-date recovery journal (shown, not lost silently). */
    TP_JOURNAL_RECOVERY_BAD_MAGIC,        /* magic != "NTPKJRNL": not our file; nothing recovered */
    TP_JOURNAL_RECOVERY_VERSION_MISMATCH, /* magic OK but format_version != current: our file, wrong format */
    TP_JOURNAL_RECOVERY_STALE_KEY   /* header valid but key != expected (moved project); NOT applied */
} tp_journal_recovery_status;

/* R5a: project metadata a recovery journal carries so a startup scan can list crashed projects by
 * path + name + time WITHOUT reconstructing the model. `path`/`name` are owned, UTF-8, NUL-terminated
 * copies (an untitled project has path == ""); both NULL when no metadata record was present. */
typedef struct tp_journal_meta {
    int64_t timestamp; /* caller-supplied unix-seconds when the metadata was recorded */
    char *path;        /* owned; the project's on-disk path ("" if untitled), or NULL when absent */
    char *name;        /* owned; the project's display name, or NULL when absent */
    tp_id128 file_fingerprint; /* exact bytes of the saved original when metadata was recorded */
    bool has_file_fingerprint; /* false for legacy metadata and untitled projects */
} tp_journal_meta;

/* Format B (R2): one recovered post-checkpoint committed transaction. `payload` is a read-only
 * borrowed span into the recovery result's single bounded raw-record buffer. It is NOT required
 * to be NUL-terminated and remains valid until tp_journal_recovery_free. The model<->journal glue
 * must decode exactly payload_len bytes and re-apply the operations to reach the committed state,
 * so the journal itself stays payload-agnostic (no tp_project/tp_operation dependency). */
typedef struct tp_journal_recovered_op {
    const char *payload; /* borrowed, read-only serialized tp_txn_request bytes */
    size_t payload_len;  /* exact payload length in bytes */
    int64_t revision;    /* the committed revision this transaction produced */
} tp_journal_recovered_op;

typedef struct tp_journal_recovery {
    tp_journal_recovery_status status;
    size_t bytes_total;    /* total bytes in the backing store */
    size_t stop_offset;    /* byte offset where decoding stopped (end of last good record) */
    bool mid_stream_corrupt; /* CORRUPT with a decodable/complete record STILL PRESENT after the
                              * bad one: the corruption is mid-log, NOT a tail. The tail-cleanup
                              * truncation is UNSAFE here (it would delete the trailing acknowledged
                              * records); recovery preserves the file and poisons the journal. A
                              * torn tail or a single trailing corrupt record leaves this false. */
    int records_recovered; /* count of good records recovered (checkpoint + txns) */
    int64_t revision;      /* revision of the FINAL recovered state (last good record's revision; 0 if none) */
    /* Format B REPLAY BASELINE: `snapshot` is the LAST CHECKPOINT's project bytes -- the base
     * that recovery loads and then replays `ops` onto. (Under the old format-A journal this was
     * the last good record's snapshot; format-B checkpoints are the only snapshot-bearing records.)
     * It is a read-only borrowed length-delimited span into `_raw_record_buffer`, is NOT required
     * to be NUL-terminated, and remains valid until tp_journal_recovery_free. NULL if no usable
     * checkpoint was recovered. */
    const char *snapshot;
    size_t snapshot_len;
    /* Format B: the post-checkpoint committed transactions, in commit order. Each payload is a
     * borrowed span owned by this recovery result and replayed onto `snapshot` to reach `revision`.
     * Empty (NULL/0) when no txn followed the last checkpoint. Freed by
     * tp_journal_recovery_free. */
    tp_journal_recovered_op *ops;
    size_t op_count;
    /* R5a: the LAST metadata record seen (last-wins), owned. A META record is NOT a txn/ckpt:
     * it never affects records_recovered / op replay / the retained-id set. `has_metadata` is
     * false and `metadata` is zeroed when no metadata record was recovered. Strings freed by
     * tp_journal_recovery_free. */
    tp_journal_meta metadata;
    bool has_metadata;
    /* Internal owner for snapshot and every borrowed ops[i].payload span. Callers must not access
     * or free it; tp_journal_recovery_free releases it after the span list. Kept in the result so
     * spans remain valid when ownership is transferred out of tp_model_recover. */
    uint8_t *_raw_record_buffer;
    size_t _raw_record_buffer_len;
} tp_journal_recovery;

void tp_journal_recovery_free(tp_journal_recovery *r);

/* Replay the backing store: validate the header (magic/version/key), decode records
 * front-to-back, STOP at the first undecodable record (never guessing corrupt
 * content), populate the journal's retained-id index from the good records, and
 * fill *out. Format B: *out carries the LAST CHECKPOINT snapshot as `snapshot` (the
 * replay base) plus the ordered post-checkpoint TXN op-payloads in `ops` (the glue
 * replays them onto the base to reach `revision`). UB-clean on arbitrary/corrupt/short/
 * torn bytes. Returns TP_STATUS_OK whenever replay ran (corruption is reported in
 * out->status); non-OK only on a hard fault (io read failure / OOM building the result). */
tp_status tp_journal_recover(tp_journal *j, tp_journal_recovery *out, tp_error *err);

/* ---- peek: header + metadata + status, no model reconstruction (R5a) ------ *
 * What the R5b startup scan reads per journal file: it classifies the header, reports the header
 * key + format version + metadata, counts the well-formed records and notes whether a checkpoint
 * (a recoverable base) is present -- WITHOUT loading a tp_project or replaying any op. It shares the
 * header-validate + frame-walk with tp_journal_recover (same BAD_MAGIC / VERSION_MISMATCH / TRUNCATED
 * / CORRUPT classification), differing only in that it builds no model and REPORTS the header key
 * rather than comparing one (peek has no key to compare against). */
typedef struct tp_journal_peek_result {
    tp_journal_recovery_status status; /* OK/EMPTY/TRUNCATED/CORRUPT/BAD_MAGIC/VERSION_MISMATCH */
    uint32_t format_version;           /* format version read from the header (0 if none present) */
    uint8_t key[16];                   /* the header's key bytes (zeroed if no complete header) */
    bool has_checkpoint;               /* a CKPT record was seen -> a recoverable base exists */
    int record_count;                  /* well-formed CKPT/TXN records before any torn/corrupt tail */
    tp_journal_meta meta;              /* last metadata record (owned; freed by tp_journal_peek_free) */
    bool has_meta;
} tp_journal_peek_result;

/* Free the owned strings in a peek result. NULL-safe; safe to call on a zeroed result. */
void tp_journal_peek_free(tp_journal_peek_result *r);

/* Read `io`'s header + metadata + status without reconstructing the model. TAKES OWNERSHIP of io
 * (destroys it before returning), exactly like tp_journal_recover. Returns TP_STATUS_OK whenever
 * the store was read (classification is in out->status); non-OK only on a hard fault (io read
 * failure / OOM / invalid io). UB-clean on arbitrary/corrupt/short/torn bytes. */
tp_status tp_journal_peek(tp_journal_io io, tp_journal_peek_result *out, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_JOURNAL_H */
