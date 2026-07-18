/*
 * Minimum recovery journal -- a PURE durable log over the tp_journal_io seam
 * (master spec §7.1-7.2, §22.3). It deals in opaque project-snapshot blobs + 32-hex
 * transaction ids and knows nothing about tp_project; the model<->journal glue lives
 * in tp_model_journal.c.
 *
 * DURABILITY / ACKNOWLEDGEMENT: tp_journal_append_txn is the commit acknowledgement
 * gate (§7.1). It (1) reserves an in-memory retained-id slot, (2) durably writes the
 * framed+checksummed record, (3) infallibly registers the id. A failure at (1) or (2)
 * leaves NOTHING durable and the id UN-registered, so the same transaction id stays
 * retryable -- the "id recorded => commit cannot fail" invariant, now anchored on the
 * durable append instead of an in-memory set. A short/failed write is rolled back to
 * the prior length so no torn tail persists (best-effort; if truncation also fails the
 * journal is poisoned and refuses further appends rather than risk hiding good records
 * behind a mid-stream torn record).
 *
 * READING UNTRUSTED BYTES: tp_journal_recover replays arbitrary/corrupt/short/torn
 * input UB-cleanly -- every field is bounds-checked with size_t math BEFORE it is read,
 * and replay STOPS at the first undecodable record (it never guesses corrupt content).
 * The retained-id set + last committed snapshot are recovered up to the last good
 * record. A benign torn TAIL is safe to truncate away; a mid-stream corruption (a bad
 * record with valid records STILL after it) is NOT -- truncating would delete those
 * trailing acknowledged records, so recovery preserves the file and poisons the journal.
 */

#include "tp_core/tp_journal.h"

#include <stdlib.h>
#include <string.h>

#include "tp_idset_internal.h"
#include "tp_journal_internal.h"

/* ---- journal object ------------------------------------------------------ */

static _Thread_local size_t s_test_record_limit;
static _Thread_local size_t s_test_file_limit;

size_t tp_journal__record_limit(void) {
    return s_test_record_limit ? s_test_record_limit
                               : (size_t)TP_JOURNAL_MAX_RECORDS;
}

size_t tp_journal__file_limit(void) {
    return s_test_file_limit ? s_test_file_limit
                             : (size_t)TP_JOURNAL_MAX_FILE_BYTES;
}

static tp_status journal_fail(tp_error *err, const char *msg);

void tp_journal__test_set_record_limit(size_t limit) {
    s_test_record_limit = limit;
}

void tp_journal__test_set_file_limit(size_t limit) {
    s_test_file_limit = limit;
}

tp_journal *tp_journal_create(tp_journal_io io, tp_id128 key) {
    if (!io.ctx || !io.write || !io.length || !io.truncate || !io.read_all) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        return NULL;
    }
    tp_journal *j = (tp_journal *)calloc(1, sizeof *j);
    if (!j) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        return NULL;
    }
    j->io = io;
    j->key = key;
    if (tp_idset_reserve(&j->ids) != TP_STATUS_OK) {
        if (j->io.destroy) {
            j->io.destroy(j->io.ctx);
        }
        free(j);
        return NULL;
    }
    return j;
}

void tp_journal_destroy(tp_journal *j) {
    if (!j) {
        return;
    }
    if (j->io.destroy) {
        j->io.destroy(j->io.ctx);
    }
    tp_idset_dispose(&j->ids);
    free(j->meta_path);
    free(j->meta_name);
    free(j);
}

/* ---- small owned-string helpers (portable strdup; no POSIX strndup dependency) ---- */

static char *jrn_strdup(const char *s) {
    size_t n = strlen(s);
    char *d = (char *)malloc(n + 1);
    if (d) {
        memcpy(d, s, n + 1);
    }
    return d;
}

/* Copy `n` raw bytes into a fresh NUL-terminated string (n may be 0 -> ""). NULL on OOM. */
static tp_status journal_fail(tp_error *err, const char *msg) {
    return tp_error_set(err, TP_STATUS_JOURNAL_FAILED, "%s", msg);
}

/* Writer/reader size invariant: a successful append must never create a store that
 * file_read_all will reject. `store_len < header` models ensure_header's eventual
 * reset/write of one complete header. This check is deliberately allocation-free so
 * oversized checkpoint/transaction/metadata payloads fail before building copies. */
static tp_status record_limit_check_at(int64_t store_len, size_t payload_len, tp_error *err) {
    if (store_len < 0) {
        return journal_fail(err, "journal length query failed");
    }
    const uint64_t base = (store_len < (int64_t)TP_JRN_HEADER_LEN)
                              ? (uint64_t)TP_JRN_HEADER_LEN
                              : (uint64_t)store_len;
    const uint64_t frame = (uint64_t)TP_JRN_SYNC_FIELD + (uint64_t)TP_JRN_LEN_FIELD +
                           (uint64_t)payload_len + (uint64_t)TP_JRN_CRC_FIELD;
    const uint64_t file_limit = (uint64_t)tp_journal__file_limit();
    if ((uint64_t)payload_len > UINT32_MAX ||
        payload_len > (size_t)TP_JOURNAL_MAX_RECORD_BYTES || frame > SIZE_MAX ||
        base > file_limit || frame > file_limit - base) {
        return journal_fail(err, "journal append would exceed the recoverable file-size limit");
    }
    return TP_STATUS_OK;
}

static tp_status record_limit_check(const tp_journal *j, size_t payload_len, tp_error *err) {
    return record_limit_check_at(j->io.length(j->io.ctx), payload_len, err);
}

/* Total-frame admission is deliberately separate from byte-size admission: it
 * needs no payload and therefore runs before metadata copies, checkpoint
 * assembly, transaction encoding, or model cloning. write_record repeats it at
 * the final durable boundary so external store changes cannot bypass the cap. */
static tp_status record_slot_check(const tp_journal *j, tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    if (j->poisoned) {
        return journal_fail(err, "journal is poisoned: a prior append could not be rolled back");
    }
    if (j->record_count >= tp_journal__record_limit()) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "journal total record limit reached; save/compact is required");
    }
    return TP_STATUS_OK;
}

void tp_journal__poison(tp_journal *j) {
    if (j) {
        j->poisoned = true;
    }
}

bool tp_journal__is_poisoned(const tp_journal *j) { return j && j->poisoned; }

tp_status tp_journal__check_replay_operations(const tp_journal *j, size_t add,
                                              tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    if (add > (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS ||
        j->replay_operations >
            (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS - add) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "journal replay operation limit reached; save/compact is required");
    }
    return TP_STATUS_OK;
}

tp_status tp_journal__check_append_admission(const tp_journal *j,
                                             size_t replay_operations,
                                             tp_error *err) {
    tp_status status = record_slot_check(j, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    if (j->replay_count >= (size_t)TP_JOURNAL_MAX_REPLAY_RECORDS) {
        return journal_fail(err,
                            "journal replay-window record limit reached; save/compact is required");
    }
    return tp_journal__check_replay_operations(j, replay_operations, err);
}

tp_status tp_journal__check_txn_bytes(const tp_journal *j, size_t request_bytes,
                                      tp_error *err) {
    if (!j || request_bytes > SIZE_MAX - (size_t)TP_JRN_TXN_FIXED) {
        return tp_error_set(err, j ? TP_STATUS_OUT_OF_BOUNDS
                                   : TP_STATUS_INVALID_ARGUMENT,
                            j ? "journal transaction record size overflow"
                              : "null journal");
    }
    return record_limit_check(j, (size_t)TP_JRN_TXN_FIXED + request_bytes, err);
}

tp_status tp_journal__check_txn_min_bytes(const tp_journal *j, tp_error *err) {
    return tp_journal__check_txn_bytes(j, 0U, err);
}

tp_status tp_journal__check_history_append_bytes(
    const tp_journal *j, size_t transition_bytes, size_t replay_operations,
    tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    if (transition_bytes > SIZE_MAX - (size_t)TP_JRN_HISTORY_FIXED) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "journal history record size overflow");
    }
    tp_status admission =
        tp_journal__check_append_admission(j, replay_operations, err);
    if (admission != TP_STATUS_OK) {
        return admission;
    }
    return record_limit_check(
        j, (size_t)TP_JRN_HISTORY_FIXED + transition_bytes, err);
}

tp_status tp_journal__set_replay_operations(tp_journal *j, size_t count,
                                            tp_error *err) {
    if (!j || count > (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS) {
        return tp_error_set(err, j ? TP_STATUS_OUT_OF_BOUNDS : TP_STATUS_INVALID_ARGUMENT,
                            "journal replay operation count is outside the supported bound");
    }
    j->replay_operations = count;
    return TP_STATUS_OK;
}

tp_status tp_journal_seed_retained_id(tp_journal *j, const char *id_hex) {
    if (!j || !id_hex) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return tp_idset_add(&j->ids, id_hex); /* bounded dedup/FIFO policy shared with recovery */
}

bool tp_journal_contains(const tp_journal *j, const char *id_hex) {
    if (!j || !id_hex) {
        return false;
    }
    return tp_idset_contains(&j->ids, id_hex);
}

int tp_journal_id_count(const tp_journal *j) { return j ? tp_idset_count(&j->ids) : 0; }

/* ---- durable writing ----------------------------------------------------- */

/* Write the 28-byte header exactly once (when the store is empty). */
static tp_status ensure_header(tp_journal *j, tp_error *err) {
    int64_t len = j->io.length(j->io.ctx);
    if (len < 0) {
        return journal_fail(err, "journal length query failed");
    }
    if (len >= TP_JRN_HEADER_LEN) {
        return TP_STATUS_OK;
    }
    if (len != 0) {
        /* C4: a sub-header-length store is a torn/incomplete header write (a crash
         * during the initial 28-byte header), never a foreign file. Reset it to empty
         * and write a fresh header so journaling can re-initialize -- a torn header must
         * not be a permanent brick. (A COMPLETE but foreign header, len >= 28, is caught
         * on the recovery path and never reaches an append.) */
        if (j->io.truncate(j->io.ctx, 0) != 0) {
            j->poisoned = true; /* could not reset the torn header: refuse appends */
            return journal_fail(err, "could not reset a torn journal header");
        }
        j->record_count = 0U;
    }
    uint8_t hdr[TP_JRN_HEADER_LEN];
    memcpy(hdr, tp_jrn_magic, TP_JRN_MAGIC_LEN);
    tp_jrn_put_u32(hdr + TP_JRN_MAGIC_LEN, (uint32_t)TP_JOURNAL_FORMAT_VERSION);
    memcpy(hdr + TP_JRN_KEY_OFF, j->key.bytes, 16);
    int64_t w = j->io.write(j->io.ctx, hdr, TP_JRN_HEADER_LEN);
    if (w != (int64_t)TP_JRN_HEADER_LEN) {
        if (j->io.truncate(j->io.ctx, 0) != 0) {
            j->poisoned = true;
        }
        return journal_fail(err, "journal header write failed");
    }
    return TP_STATUS_OK;
}

/* Frame [len|payload|crc], append durably, and roll the store back on a failed
 * write OR durability barrier so no unacknowledged tail persists. After a
 * barrier failure, a successful truncate gets its own best-effort durability
 * barrier: this can confirm removal for a transient one-shot failure. The
 * authority is poisoned regardless, because callers must never treat the
 * original unconfirmed append as retryable acknowledgement. */
static tp_status write_record(tp_journal *j, const uint8_t *payload, size_t payload_len, tp_error *err) {
    tp_status slot = record_slot_check(j, err);
    if (slot != TP_STATUS_OK) {
        return slot;
    }
    /* First check leaves even an empty/torn-header store byte-unchanged on a limit rejection. */
    tp_status limit = record_limit_check(j, payload_len, err);
    if (limit != TP_STATUS_OK) {
        return limit;
    }
    if (!j->io.sync) {
        return journal_fail(err,
                            "journal durability barrier is unavailable");
    }
    tp_status hs = ensure_header(j, err);
    if (hs != TP_STATUS_OK) {
        return hs;
    }
    int64_t prior = j->io.length(j->io.ctx);
    if (prior < 0) {
        return journal_fail(err, "journal length query failed");
    }
    /* Re-check after ensure_header (and against a concurrent external extension) so
     * the write itself can never cross the reader cap. */
    limit = record_limit_check_at(prior, payload_len, err);
    if (limit != TP_STATUS_OK) {
        return limit;
    }
    size_t reclen = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + payload_len + (size_t)TP_JRN_CRC_FIELD;
    uint8_t *rec = (uint8_t *)malloc(reclen);
    if (!rec) {
        return tp_error_set(err, TP_STATUS_OOM, "journal record buffer allocation failed");
    }
    tp_jrn_put_u32(rec, (uint32_t)TP_JRN_SYNC_WORD);
    tp_jrn_put_u32(rec + TP_JRN_SYNC_FIELD, (uint32_t)payload_len);
    memcpy(rec + (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD, payload, payload_len);
    size_t crc_span = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + payload_len;
    uint32_t crc = tp_jrn_crc32(0, rec, crc_span);
    tp_jrn_put_u32(rec + crc_span, crc);
    int64_t w = j->io.write(j->io.ctx, rec, reclen);
    free(rec);
    if (w != (int64_t)reclen) {
        if (j->io.truncate(j->io.ctx, (size_t)prior) != 0) {
            j->poisoned = true; /* could not clean the torn tail: no further appends */
        }
        return journal_fail(err, "journal record append failed (rolled back)");
    }
    if (j->io.sync(j->io.ctx) != 0) {
        const int rollback = j->io.truncate(j->io.ctx, (size_t)prior);
        if (rollback != 0) {
            j->poisoned = true;
            return journal_fail(err,
                                "journal durability barrier failed and rollback failed; tail is unknown");
        }
        /* A failed initial barrier leaves append durability unknown. Try to
         * durably establish the rollback, but retain fail-closed poison even
         * if it succeeds; a second failure likewise leaves bytes unknown and
         * cannot be repaired in this authority. */
        const int rollback_sync = j->io.sync(j->io.ctx);
        j->poisoned = true;
        if (rollback_sync != 0) {
            return journal_fail(err,
                                "journal durability barrier failed and rollback was not durable; tail is unknown");
        }
        return journal_fail(err,
                            "journal durability barrier failed; rollback completed but append was not acknowledged");
    }
    j->record_count++;
    return TP_STATUS_OK;
}

/* META payload helpers take explicit values so normal writes and compaction share
 * one sizing/encoding path. NULL path/name values encode as empty strings. */
static tp_status metadata_payload_size(const char *path, const char *name,
                                       const tp_id128 *file_fingerprint,
                                       size_t *out, tp_error *err) {
    size_t plen = path ? strlen(path) : 0;
    size_t nlen = name ? strlen(name) : 0;
    const size_t fingerprint_len = file_fingerprint ? sizeof file_fingerprint->bytes : 0;
    const uint64_t payload64 = (uint64_t)TP_JRN_META_FIXED + (uint64_t)plen +
                               (uint64_t)TP_JRN_LEN_FIELD + (uint64_t)nlen +
                               (uint64_t)fingerprint_len;
    if (payload64 > SIZE_MAX) {
        return journal_fail(err, "journal metadata record size overflow");
    }
    *out = (size_t)payload64;
    return TP_STATUS_OK;
}

static tp_status write_meta_record(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                   const tp_id128 *file_fingerprint, tp_error *err) {
    const size_t plen = path ? strlen(path) : 0U;
    const size_t nlen = name ? strlen(name) : 0U;
    size_t payload_len = 0U;
    tp_status size_status = metadata_payload_size(path, name, file_fingerprint,
                                                  &payload_len, err);
    if (size_status != TP_STATUS_OK) {
        return size_status;
    }
    tp_status limit = record_limit_check(j, payload_len, err);
    if (limit != TP_STATUS_OK) {
        return limit;
    }
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM, "journal metadata payload allocation failed");
    }
    size_t off = 0;
    payload[off++] = (uint8_t)TP_JRN_REC_META;
    tp_jrn_put_i64(payload + off, timestamp);
    off += 8;
    tp_jrn_put_u32(payload + off, (uint32_t)plen);
    off += 4;
    if (plen) {
        memcpy(payload + off, path, plen);
        off += plen;
    }
    tp_jrn_put_u32(payload + off, (uint32_t)nlen);
    off += 4;
    if (nlen) {
        memcpy(payload + off, name, nlen);
        off += nlen;
    }
    if (file_fingerprint) {
        memcpy(payload + off, file_fingerprint->bytes, sizeof file_fingerprint->bytes);
    }
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    return st;
}

tp_status tp_journal_set_metadata(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                  tp_error *err) {
    return tp_journal_set_metadata_ex(j, timestamp, path, name, NULL, err);
}

tp_status tp_journal_set_metadata_ex(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                     const tp_id128 *file_fingerprint, tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    tp_status slot = record_slot_check(j, err);
    if (slot != TP_STATUS_OK) {
        return slot;
    }
    /* strlen-based sizing is allocation-free. Reject an exhausted byte budget
     * before duplicating or replacing the write-side metadata cache. */
    size_t payload_len = 0U;
    tp_status payload_status = metadata_payload_size(
        path, name, file_fingerprint, &payload_len, err);
    if (payload_status != TP_STATUS_OK) {
        return payload_status;
    }
    tp_status byte_status = record_limit_check(j, payload_len, err);
    if (byte_status != TP_STATUS_OK) {
        return byte_status;
    }
    /* Commit the cache first, then append the authoritative recovery metadata. The cache remains
     * useful for a later compaction even when the append was rolled back cleanly, but the caller MUST
     * see every durable-write failure: until a META record reaches the store, a crash may recover the
     * previous path/fingerprint and offer an unsafe Save Original. */
    char *pdup = jrn_strdup(path ? path : "");
    if (!pdup) {
        return tp_error_set(err, TP_STATUS_OOM, "journal metadata path allocation failed");
    }
    char *ndup = jrn_strdup(name ? name : "");
    if (!ndup) {
        free(pdup);
        return tp_error_set(err, TP_STATUS_OOM, "journal metadata name allocation failed");
    }
    free(j->meta_path);
    free(j->meta_name);
    j->meta_path = pdup;
    j->meta_name = ndup;
    j->meta_time = timestamp;
    memset(&j->meta_file_fingerprint, 0, sizeof j->meta_file_fingerprint);
    if (file_fingerprint) {
        j->meta_file_fingerprint = *file_fingerprint;
    }
    j->meta_has_file_fingerprint = file_fingerprint != NULL;
    j->has_meta = true;
    tp_error mde = {0};
    tp_status ms = write_meta_record(j, timestamp, pdup, ndup, file_fingerprint, &mde);
    if (ms != TP_STATUS_OK) {
        return tp_error_set(err, ms, "%s", mde.msg[0] ? mde.msg : "journal metadata durable write failed");
    }
    return TP_STATUS_OK;
}

static tp_status checkpoint_payload_size(const tp_journal *j, size_t snapshot_len, size_t *out,
                                         tp_error *err) {
    if (!j || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid checkpoint size query");
    }
    const size_t id_count = (size_t)tp_idset_count(&j->ids);
    if (id_count > (SIZE_MAX - (size_t)TP_JRN_CKPT_FIXED) /
                       (size_t)TP_JRN_IDLEN) {
        return journal_fail(err, "journal checkpoint record size overflow");
    }
    const size_t prefix = (size_t)TP_JRN_CKPT_FIXED +
                          id_count * (size_t)TP_JRN_IDLEN;
    if (snapshot_len > SIZE_MAX - prefix) {
        return journal_fail(err, "journal checkpoint record size overflow");
    }
    *out = prefix + snapshot_len;
    return TP_STATUS_OK;
}

tp_status tp_journal__check_checkpoint_append_bytes(const tp_journal *j,
                                                     size_t snapshot_bytes,
                                                     tp_error *err) {
    tp_status status = record_slot_check(j, err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    size_t payload_len = 0U;
    status = checkpoint_payload_size(j, snapshot_bytes, &payload_len, err);
    return status == TP_STATUS_OK ? record_limit_check(j, payload_len, err)
                                  : status;
}

tp_status tp_journal__check_checkpoint_compact_bytes(const tp_journal *j,
                                                      size_t snapshot_bytes,
                                                      tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    const size_t replacement_records = j->has_meta ? 2U : 1U;
    if (replacement_records > tp_journal__record_limit()) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "compacted journal replacement exceeds the total record limit");
    }
    size_t payload_len = 0U;
    tp_status status = checkpoint_payload_size(j, snapshot_bytes, &payload_len,
                                               err);
    if (status != TP_STATUS_OK) {
        return status;
    }
    status = record_limit_check_at((int64_t)TP_JRN_HEADER_LEN, payload_len,
                                   err);
    if (status != TP_STATUS_OK || !j->has_meta) {
        return status;
    }
    const uint64_t checkpoint_end = (uint64_t)TP_JRN_HEADER_LEN +
                                    (uint64_t)TP_JRN_SYNC_FIELD +
                                    (uint64_t)TP_JRN_LEN_FIELD +
                                    (uint64_t)payload_len +
                                    (uint64_t)TP_JRN_CRC_FIELD;
    if (checkpoint_end > INT64_MAX) {
        return journal_fail(err, "compacted journal replacement size overflow");
    }
    size_t metadata_len = 0U;
    status = metadata_payload_size(
        j->meta_path, j->meta_name,
        j->meta_has_file_fingerprint ? &j->meta_file_fingerprint : NULL,
        &metadata_len, err);
    return status == TP_STATUS_OK
               ? record_limit_check_at((int64_t)checkpoint_end, metadata_len,
                                       err)
               : status;
}

tp_status tp_journal_init_checkpoint(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision,
                                     tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    if (len > 0U && !snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null checkpoint payload with positive length");
    }
    tp_status slot = tp_journal__check_checkpoint_append_bytes(j, len, err);
    if (slot != TP_STATUS_OK) {
        return slot;
    }
    size_t payload_len = 0;
    tp_status ps = checkpoint_payload_size(j, len, &payload_len, err);
    if (ps != TP_STATUS_OK) {
        return ps;
    }
    int id_count = tp_idset_count(&j->ids);
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM, "journal checkpoint payload allocation failed");
    }
    size_t off = 0;
    payload[off++] = (uint8_t)TP_JRN_REC_CKPT;
    tp_jrn_put_i64(payload + off, revision);
    off += 8;
    tp_jrn_put_u32(payload + off, (uint32_t)id_count);
    off += 4;
    for (int i = 0; i < id_count; i++) {
        char id_hex[TP_JRN_IDLEN + 1];
        if (!tp_idset_format_at(&j->ids, i, id_hex)) {
            free(payload);
            return journal_fail(err, "journal retained-id ordering is invalid");
        }
        memcpy(payload + off, id_hex, TP_JRN_IDLEN);
        off += TP_JRN_IDLEN;
    }
    if (len) {
        memcpy(payload + off, snapshot, len);
    }
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    if (st == TP_STATUS_OK) {
        j->replay_count = 0U;
        j->replay_operations = 0U;
    }
    return st;
}

tp_status tp_journal_compact(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision, tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    if (len > 0U && !snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null checkpoint payload with positive length");
    }
    /* Reject an intrinsically oversized fresh checkpoint BEFORE truncate. Otherwise
     * compaction would destroy the current recoverable log and only then discover that
     * the replacement can never be read under this build's cap. */
    tp_status limit = tp_journal__check_checkpoint_compact_bytes(j, len, err);
    if (limit != TP_STATUS_OK) {
        return limit;
    }
    /* Compact the store to a single fresh checkpoint (Save-window reset).
     * Truncate the BYTE STORE to 0 -- this physically removes the old checkpoint, every
     * post-checkpoint txn record, AND any mid-stream-corrupt record a prior recovery poisoned
     * against; nothing is left to hide behind, so a successful truncate clears the poison flag.
     * If the truncate FAILS, keep the store + poison intact and return a fault (fail-closed,
     * the same pattern as tp_journal__poison / a failed tail-clean): a compaction that cannot
     * reset the store must not pretend it did. CRITICAL: ONLY the byte store is reset -- the
     * in-memory retained-id index (j->ids) is left INTACT so tp_journal_init_checkpoint below
     * re-persists the SAME live id set into the fresh checkpoint; clearing it would let an
     * already-acknowledged txn id double-apply after Save (§7.2 idempotency). After truncate-to-0
     * ensure_header re-emits the current-format header, so the compacted store = header + one CHECKPOINT. */
    if (j->io.truncate(j->io.ctx, 0) != 0) {
        /* Store byte-intact (nothing removed) + poison state UNCHANGED: the journal is still fully
         * usable (its old checkpoint + records survive, appends continue). Fail closed on the compaction
         * itself, but do not brick a healthy journal. */
        return journal_fail(err, "journal compaction truncate failed (store unchanged, journal preserved)");
    }
    j->record_count = 0U;
    j->poisoned = false; /* the truncate-to-0 removed any offending record: appends are safe again */
    tp_status cs = tp_journal_init_checkpoint(j, snapshot, len, revision, err);
    if (cs != TP_STATUS_OK) {
        /* The truncate SUCCEEDED (the old checkpoint + records are gone) but the fresh checkpoint could
         * NOT be written (OOM / I/O; init_checkpoint rolled its store back to length 0). The store is now
         * checkpoint-LESS: appending TXN op-payloads onto it would recover to NOTHING (no base snapshot)
         * after a crash -- silent loss of every post-Save edit. FAIL CLOSED: poison the journal so
         * write_record refuses further appends. The model keeps this authority attached rather than
         * silently falling back to a second journal-less source of truth. */
        j->poisoned = true;
        return cs;
    }
    /* Metadata carries the canonical path/fingerprint used by Save Original, so a fresh checkpoint
     * without its matching META record is not a complete recovery authority. Propagate every re-emit
     * failure; the host disables/removes this slot rather than later exposing an identity-less recovery. */
    if (j->has_meta) {
        tp_error meta_err = {0};
        const tp_id128 *fingerprint = j->meta_has_file_fingerprint ? &j->meta_file_fingerprint : NULL;
        tp_status ms = write_meta_record(j, j->meta_time, j->meta_path, j->meta_name, fingerprint, &meta_err);
        if (ms != TP_STATUS_OK) {
            j->poisoned = true;
            return tp_error_set(err, ms, "%s",
                                meta_err.msg[0] ? meta_err.msg
                                                : "journal metadata re-emit failed after compaction");
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_journal_append_txn_counted(tp_journal *j, const char *id_hex, int64_t revision,
                                        const uint8_t *snapshot, size_t len,
                                        size_t replay_operations, tp_error *err) {
    if (!j || !id_hex) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal or id");
    }
    if (len > 0U && !snapshot) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null transaction payload with positive length");
    }
    if (!tp_idset_valid_hex(id_hex)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "journal id must be 32 lowercase hex characters");
    }
    /* (1) reserve the retained-id slot BEFORE the durable write so step (3) is
     * allocation-free. An OOM here writes nothing (id stays retryable). */
    if (tp_idset_contains(&j->ids, id_hex)) {
        return TP_STATUS_OK; /* already retained: an idempotent no-op (never double-appends) */
    }
    tp_status admission =
        tp_journal__check_append_admission(j, replay_operations, err);
    if (admission != TP_STATUS_OK) {
        return admission;
    }
    tp_status rs = tp_idset_reserve(&j->ids);
    if (rs != TP_STATUS_OK) {
        return tp_error_set(err, rs, "journal retained-id storage is unavailable");
    }
    const uint64_t payload64 = (uint64_t)TP_JRN_TXN_FIXED + (uint64_t)len;
    if (payload64 > SIZE_MAX) {
        return journal_fail(err, "journal transaction record size overflow");
    }
    size_t payload_len = (size_t)payload64;
    tp_status limit = record_limit_check(j, payload_len, err);
    if (limit != TP_STATUS_OK) {
        return limit;
    }
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM, "journal transaction payload allocation failed");
    }
    size_t off = 0;
    payload[off++] = (uint8_t)TP_JRN_REC_TXN;
    memcpy(payload + off, id_hex, TP_JRN_IDLEN);
    off += TP_JRN_IDLEN;
    tp_jrn_put_i64(payload + off, revision);
    off += 8;
    if (len) {
        memcpy(payload + off, snapshot, len);
    }
    /* (2) the durable acknowledgement gate. */
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    if (st != TP_STATUS_OK) {
        return st; /* nothing durable; id NOT registered -> retryable */
    }
    /* (3) infallibly register the id in the reserved slot. */
    tp_idset_put_reserved(&j->ids, id_hex);
    j->replay_count++;
    j->replay_operations += replay_operations;
    return TP_STATUS_OK;
}

tp_status tp_journal_append_txn(tp_journal *j, const char *id_hex, int64_t revision,
                                const uint8_t *snapshot, size_t len, tp_error *err) {
    return tp_journal_append_txn_counted(j, id_hex, revision, snapshot, len, 0U, err);
}

tp_status tp_journal_append_history_counted(tp_journal *j, int64_t revision,
                                            const uint8_t *transition,
                                            size_t len,
                                            size_t replay_operations,
                                            tp_error *err) {
    if (!j || (len > 0U && !transition)) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "invalid journal history transition");
    }
    tp_status admission = tp_journal__check_history_append_bytes(
        j, len, replay_operations, err);
    if (admission != TP_STATUS_OK) return admission;
    const size_t payload_len = (size_t)TP_JRN_HISTORY_FIXED + len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "journal history payload allocation failed");
    }
    payload[0] = (uint8_t)TP_JRN_REC_HISTORY;
    tp_jrn_put_i64(payload + 1, revision);
    if (len) memcpy(payload + TP_JRN_HISTORY_FIXED, transition, len);
    tp_status status = write_record(j, payload, payload_len, err);
    free(payload);
    if (status == TP_STATUS_OK) {
        j->replay_count++;
        j->replay_operations += replay_operations;
    }
    return status;
}
