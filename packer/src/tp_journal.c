/*
 * F2-04 minimum recovery journal -- a PURE durable log over the tp_journal_io seam
 * (master spec §7.1-7.2, §22.3). It deals in opaque project-snapshot blobs + 32-hex
 * transaction ids and knows nothing about tp_project; the model<->journal glue lives
 * in tp_txn_apply.c.
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

const uint8_t tp_jrn_magic[TP_JRN_MAGIC_LEN] = {'N', 'T', 'P', 'K', 'J', 'R', 'N', 'L'};

/* ---- endian-stable CRC-32 + byte-at-a-time integer codecs ---------------- */

uint32_t tp_jrn_crc32(uint32_t crc, const uint8_t *data, size_t len) {
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

void tp_jrn_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)((v >> 24) & 0xFFu);
    p[1] = (uint8_t)((v >> 16) & 0xFFu);
    p[2] = (uint8_t)((v >> 8) & 0xFFu);
    p[3] = (uint8_t)(v & 0xFFu);
}

void tp_jrn_put_i64(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v;
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (56 - i * 8)) & 0xFFu);
    }
}

uint32_t tp_jrn_get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

int64_t tp_jrn_get_i64(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u = (u << 8) | (uint64_t)p[i];
    }
    return (int64_t)u; /* implementation-defined (never UB) for the full u64 range */
}

/* ---- journal object ------------------------------------------------------ */

struct tp_journal {
    tp_journal_io io;  /* owned */
    tp_id128 key;
    bool poisoned;     /* a failed append/tail-clean: refuse further appends */
    tp_idset ids;      /* retained-id index (shared set); mirrors the durable records */
    /* R5a: cached project metadata (owned copies). Set by tp_journal_set_metadata and re-emitted by
     * tp_journal_compact so it survives compaction. has_meta stays false until first set. */
    int64_t meta_time;
    char *meta_path;   /* owned; NULL until set (then never NULL, may be "") */
    char *meta_name;   /* owned; NULL until set (then never NULL, may be "") */
    bool has_meta;
};

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
static char *jrn_dup_bytes(const uint8_t *p, size_t n) {
    char *d = (char *)malloc(n + 1);
    if (!d) {
        return NULL;
    }
    if (n) {
        memcpy(d, p, n);
    }
    d[n] = '\0';
    return d;
}

static tp_status journal_fail(tp_error *err, const char *msg) {
    return tp_error_set(err, TP_STATUS_JOURNAL_FAILED, "%s", msg);
}

void tp_journal__poison(tp_journal *j) {
    if (j) {
        j->poisoned = true;
    }
}

bool tp_journal__is_poisoned(const tp_journal *j) { return j && j->poisoned; }

tp_status tp_journal_seed_retained_id(tp_journal *j, const char *id_hex) {
    if (!j || !id_hex) {
        return TP_STATUS_INVALID_ARGUMENT;
    }
    return tp_idset_add(&j->ids, id_hex); /* dedup + grow; OOM -> non-OK, no durable write */
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
 * write so no torn tail persists. */
static tp_status write_record(tp_journal *j, const uint8_t *payload, size_t payload_len, tp_error *err) {
    if (j->poisoned) {
        return journal_fail(err, "journal is poisoned: a prior append could not be rolled back");
    }
    tp_status hs = ensure_header(j, err);
    if (hs != TP_STATUS_OK) {
        return hs;
    }
    if (payload_len > 0xFFFFFFFFu) {
        return journal_fail(err, "journal record exceeds the maximum record size");
    }
    int64_t prior = j->io.length(j->io.ctx);
    if (prior < 0) {
        return journal_fail(err, "journal length query failed");
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
    if (j->io.sync) {
        (void)j->io.sync(j->io.ctx);
    }
    return TP_STATUS_OK;
}

/* R5a: durably append a META record built from the passed-in args (payload:
 * type(1)=3 | timestamp i64 | path_len u32 | path | name_len u32 | name -- all BE, no NUL on wire).
 * Takes explicit args (NOT j->meta_*) so a caller can write DURABLY BEFORE committing the cache
 * (finding [2]: set_metadata) and compaction can re-emit the cache (finding [0]). path/name may be
 * "" (never NULL from our callers; a NULL is defensively treated as ""). Reuses write_record so it
 * inherits the fail-closed / poison / rollback discipline. */
static tp_status write_meta_record(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                   tp_error *err) {
    size_t plen = path ? strlen(path) : 0;
    size_t nlen = name ? strlen(name) : 0;
    size_t payload_len = (size_t)TP_JRN_META_FIXED + plen + (size_t)TP_JRN_LEN_FIELD + nlen;
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
    }
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    return st;
}

tp_status tp_journal_set_metadata(tp_journal *j, int64_t timestamp, const char *path, const char *name,
                                  tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    /* R5a fix [2] (write-first, mirror tp_journal_append_txn): pre-allocate the cache dups, write the
     * durable record FROM THE ARGS, and commit the cache ONLY on a successful durable write. A failed
     * durable write leaves j->meta_* + has_meta UNTOUCHED, so the metadata the API reported as failed is
     * never later re-emitted by compaction (NULL -> "", both may be empty). */
    char *pdup = jrn_strdup(path ? path : "");
    if (!pdup) {
        return tp_error_set(err, TP_STATUS_OOM, "journal metadata path allocation failed");
    }
    char *ndup = jrn_strdup(name ? name : "");
    if (!ndup) {
        free(pdup);
        return tp_error_set(err, TP_STATUS_OOM, "journal metadata name allocation failed");
    }
    tp_status st = write_meta_record(j, timestamp, path ? path : "", name ? name : "", err);
    if (st != TP_STATUS_OK) {
        free(pdup);
        free(ndup);
        return st; /* cache + has_meta UNTOUCHED on failure */
    }
    free(j->meta_path);
    free(j->meta_name);
    j->meta_path = pdup;
    j->meta_name = ndup;
    j->meta_time = timestamp;
    j->has_meta = true;
    return TP_STATUS_OK;
}

tp_status tp_journal_init_checkpoint(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision,
                                     tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    int id_count = tp_idset_count(&j->ids);
    size_t ids_bytes = (size_t)id_count * (size_t)TP_JRN_IDLEN;
    size_t payload_len = (size_t)TP_JRN_CKPT_FIXED + ids_bytes + len;
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
        memcpy(payload + off, tp_idset_at(&j->ids, i), TP_JRN_IDLEN);
        off += TP_JRN_IDLEN;
    }
    if (len) {
        memcpy(payload + off, snapshot, len);
    }
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    return st;
}

tp_status tp_journal_compact(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision, tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    /* R3 (plan S18 R): compact the store to a SINGLE fresh checkpoint (Save-window reset).
     * Truncate the BYTE STORE to 0 -- this physically removes the old checkpoint, every
     * post-checkpoint txn record, AND any mid-stream-corrupt record a prior recovery poisoned
     * against; nothing is left to hide behind, so a successful truncate clears the poison flag.
     * If the truncate FAILS, keep the store + poison intact and return a fault (fail-closed,
     * the same pattern as tp_journal__poison / a failed tail-clean): a compaction that cannot
     * reset the store must not pretend it did. CRITICAL: ONLY the byte store is reset -- the
     * in-memory retained-id index (j->ids) is left INTACT so tp_journal_init_checkpoint below
     * re-persists the SAME live id set into the fresh checkpoint; clearing it would let an
     * already-acknowledged txn id double-apply after Save (§7.2 idempotency). After truncate-to-0
     * ensure_header re-emits a fresh v3 header, so the compacted store = header + one CHECKPOINT. */
    if (j->io.truncate(j->io.ctx, 0) != 0) {
        /* Store byte-intact (nothing removed) + poison state UNCHANGED: the journal is still fully
         * usable (its old checkpoint + records survive, appends continue). Fail closed on the compaction
         * itself, but do not brick a healthy journal. */
        return journal_fail(err, "journal compaction truncate failed (store unchanged, journal preserved)");
    }
    j->poisoned = false; /* the truncate-to-0 removed any offending record: appends are safe again */
    tp_status cs = tp_journal_init_checkpoint(j, snapshot, len, revision, err);
    if (cs != TP_STATUS_OK) {
        /* The truncate SUCCEEDED (the old checkpoint + records are gone) but the fresh checkpoint could
         * NOT be written (OOM / I/O; init_checkpoint rolled its store back to length 0). The store is now
         * checkpoint-LESS: appending TXN op-payloads onto it would recover to NOTHING (no base snapshot)
         * after a crash -- silent loss of every post-Save edit. FAIL CLOSED: poison the journal so
         * write_record refuses further appends; the glue then detaches it and continues journal-less. */
        j->poisoned = true;
        return cs;
    }
    /* R5a fix [0]: re-emit the cached metadata BEST-EFFORT. Metadata is INFORMATIONAL (scan list only),
     * NOT recovery-critical -- write_record already rolled a torn META tail back to the durable
     * checkpoint, so the store stays fully recoverable; do NOT poison a healthy checkpoint over a lost
     * scan label (poison here would make the glue detach the journal -> crash recovery silently OFF for
     * the session even though a valid checkpoint is on disk). Use a LOCAL error so a swallowed best-effort
     * failure never pollutes the caller's err. ONLY if write_record could not roll back (it self-poisoned,
     * store genuinely corrupt) do we propagate so the glue detaches. */
    if (j->has_meta) {
        tp_error meta_err = {0};
        tp_status ms = write_meta_record(j, j->meta_time, j->meta_path, j->meta_name, &meta_err);
        if (ms != TP_STATUS_OK && j->poisoned) {
            /* rollback failed -> store corrupt -> surface via the caller's err and fail the compaction */
            return tp_error_set(err, ms, "%s",
                                meta_err.msg[0] ? meta_err.msg
                                                : "journal metadata re-emit failed after compaction");
        }
        /* else: checkpoint durable + store recoverable -> swallow; metadata simply absent until next compaction */
    }
    return TP_STATUS_OK;
}

tp_status tp_journal_append_txn(tp_journal *j, const char *id_hex, int64_t revision, const uint8_t *snapshot,
                                size_t len, tp_error *err) {
    if (!j || !id_hex) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal or id");
    }
    if (strlen(id_hex) != TP_JRN_IDLEN) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "journal id must be 32 hex characters");
    }
    /* (1) reserve the retained-id slot BEFORE the durable write so step (3) is
     * allocation-free. An OOM here writes nothing (id stays retryable). */
    if (tp_idset_contains(&j->ids, id_hex)) {
        return TP_STATUS_OK; /* already retained: an idempotent no-op (never double-appends) */
    }
    tp_status rs = tp_idset_reserve(&j->ids);
    if (rs != TP_STATUS_OK) {
        return tp_error_set(err, rs, "journal retained-id reserve failed (out of memory)");
    }
    size_t payload_len = (size_t)TP_JRN_TXN_FIXED + len;
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
    return TP_STATUS_OK;
}

/* ---- recovery ------------------------------------------------------------ */

void tp_journal_recovery_free(tp_journal_recovery *r) {
    if (!r) {
        return;
    }
    free(r->snapshot);
    r->snapshot = NULL;
    r->snapshot_len = 0;
    for (size_t i = 0; i < r->op_count; i++) { /* format B: free each replay op-payload */
        free(r->ops[i].payload);
    }
    free(r->ops);
    r->ops = NULL;
    r->op_count = 0;
    free(r->metadata.path); /* R5a: owned metadata strings */
    free(r->metadata.name);
    r->metadata.path = NULL;
    r->metadata.name = NULL;
    r->has_metadata = false;
}

/* R5a: parse a META (type 3) payload -- type(1) | timestamp i64 | path_len u32 | path | name_len u32
 * | name (all BE) -- into owned NUL-terminated strdup'd `*path`/`*name` copies + `*ts`. Every field
 * is bounds-checked with size_t math before it is read. Returns TP_STATUS_OK (caller owns the
 * strings), TP_STATUS_OUT_OF_BOUNDS for a malformed/short payload (a corruption boundary), or
 * TP_STATUS_OOM. On any non-OK return the out strings are NULL (nothing leaks). Shared by recover + peek. */
static tp_status parse_meta(const uint8_t *pl, size_t plen, int64_t *ts, char **path, char **name) {
    *ts = 0;
    *path = NULL;
    *name = NULL;
    if (plen < (size_t)TP_JRN_META_FIXED) {
        return TP_STATUS_OUT_OF_BOUNDS; /* too short for type + timestamp + path_len */
    }
    *ts = tp_jrn_get_i64(pl + 1);
    uint32_t path_len = tp_jrn_get_u32(pl + 9);
    size_t off = (size_t)TP_JRN_META_FIXED;
    if (plen - off < (size_t)path_len) {
        return TP_STATUS_OUT_OF_BOUNDS; /* path overruns the payload */
    }
    char *p = jrn_dup_bytes(pl + off, (size_t)path_len);
    if (!p) {
        return TP_STATUS_OOM;
    }
    off += (size_t)path_len;
    if (plen - off < (size_t)TP_JRN_LEN_FIELD) {
        free(p);
        return TP_STATUS_OUT_OF_BOUNDS; /* no room for name_len */
    }
    uint32_t name_len = tp_jrn_get_u32(pl + off);
    off += (size_t)TP_JRN_LEN_FIELD;
    if (plen - off < (size_t)name_len) {
        free(p);
        return TP_STATUS_OUT_OF_BOUNDS; /* name overruns the payload */
    }
    char *n = jrn_dup_bytes(pl + off, (size_t)name_len);
    if (!n) {
        free(p);
        return TP_STATUS_OOM;
    }
    *path = p;
    *name = n;
    return TP_STATUS_OK;
}

/* R5a fix [4]: capture a META payload into *dst (last-wins: frees any prior strings). Returns
 * TP_STATUS_OK, TP_STATUS_OOM (hard fault -> caller aborts), or TP_STATUS_OUT_OF_BOUNDS (malformed ->
 * corruption boundary). Shared by the recover + peek frame walks so their META handling cannot drift. */
static tp_status capture_meta(const uint8_t *pl, size_t plen, tp_journal_meta *dst, bool *has_dst) {
    int64_t mts = 0;
    char *mpath = NULL, *mname = NULL;
    tp_status ms = parse_meta(pl, plen, &mts, &mpath, &mname);
    if (ms != TP_STATUS_OK) {
        return ms; /* OOM or OUT_OF_BOUNDS; parse_meta guarantees mpath/mname NULL on non-OK (no leak) */
    }
    free(dst->path); /* last-wins: drop any earlier metadata record */
    free(dst->name);
    dst->timestamp = mts;
    dst->path = mpath;
    dst->name = mname;
    *has_dst = true;
    return TP_STATUS_OK;
}

/* Decode one payload; when `ids` is non-NULL register its ids into it; output the record type, the
 * payload SLICE offset (relative to payload start) + length + revision. For a CHECKPOINT the slice is
 * the project snapshot; for a TXN (format B) it is the serialized op-payload; a META record has no
 * slice (0/0). `ids` NULL = classify + bounds-check only, no registration (peek shares this decoder,
 * having no idset). Returns TP_STATUS_OK on a good record, TP_STATUS_OOM on a hard fault (abort), or
 * TP_STATUS_OUT_OF_BOUNDS for a malformed/unknown payload (a corruption boundary). All reads
 * bounds-checked. */
static tp_status decode_payload(tp_idset *ids, const uint8_t *pl, size_t plen, uint8_t *rec_type, size_t *snap_off,
                                size_t *snap_len, int64_t *rev) {
    if (plen < 1) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    uint8_t type = pl[0];
    *rec_type = type;
    if (type == (uint8_t)TP_JRN_REC_TXN) {
        if (plen < (size_t)TP_JRN_TXN_FIXED) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        char idhex[33];
        memcpy(idhex, pl + 1, TP_JRN_IDLEN);
        idhex[TP_JRN_IDLEN] = '\0';
        *rev = tp_jrn_get_i64(pl + 1 + TP_JRN_IDLEN);
        *snap_off = (size_t)TP_JRN_TXN_FIXED;
        *snap_len = plen - (size_t)TP_JRN_TXN_FIXED;
        return ids ? tp_idset_add(ids, idhex) : TP_STATUS_OK;
    }
    if (type == (uint8_t)TP_JRN_REC_CKPT) {
        if (plen < (size_t)TP_JRN_CKPT_FIXED) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        *rev = tp_jrn_get_i64(pl + 1);
        uint32_t idc = tp_jrn_get_u32(pl + 9);
        size_t avail = plen - (size_t)TP_JRN_CKPT_FIXED;
        if ((size_t)idc > avail / (size_t)TP_JRN_IDLEN) {
            return TP_STATUS_OUT_OF_BOUNDS; /* overflow-safe: id list would exceed the payload */
        }
        size_t ids_bytes = (size_t)idc * (size_t)TP_JRN_IDLEN;
        if (ids) {
            tp_idset_reset(ids); /* a checkpoint RESETS the retained set to its captured id list */
            for (uint32_t i = 0; i < idc; i++) {
                char idhex[33];
                memcpy(idhex, pl + (size_t)TP_JRN_CKPT_FIXED + (size_t)i * (size_t)TP_JRN_IDLEN, TP_JRN_IDLEN);
                idhex[TP_JRN_IDLEN] = '\0';
                tp_status r = tp_idset_add(ids, idhex);
                if (r != TP_STATUS_OK) {
                    return r;
                }
            }
        }
        *snap_off = (size_t)TP_JRN_CKPT_FIXED + ids_bytes;
        *snap_len = plen - *snap_off;
        return TP_STATUS_OK;
    }
    if (type == (uint8_t)TP_JRN_REC_META) {
        /* R5a: a META record has no snapshot slice + registers no id; the walk parses its fields via
         * parse_meta. Decoding here just accepts it as a well-formed (skippable) record so a CRC-valid
         * META interleaved with TXNs is not mistaken for corruption. */
        *snap_off = 0;
        *snap_len = 0;
        return TP_STATUS_OK;
    }
    return TP_STATUS_OUT_OF_BOUNDS; /* unknown record type => corruption */
}

/* R5a: classify the fixed header (magic + version), SHARED by recover + peek so both split BAD_MAGIC
 * vs VERSION_MISMATCH identically. Returns true when the header is well-formed (caller may walk
 * records) and sets *version + *key (a 16-byte pointer into buf). Otherwise sets *status to the
 * classified failure (EMPTY / TRUNCATED torn-header / BAD_MAGIC / VERSION_MISMATCH) and returns false.
 * *version is best-effort populated from the header bytes when present (0 otherwise); *key is NULL
 * unless a complete header is present. */
static bool header_ok(const uint8_t *buf, size_t len, tp_journal_recovery_status *status, uint32_t *version,
                      const uint8_t **key) {
    *version = 0;
    *key = NULL;
    if (len == 0) {
        *status = TP_JOURNAL_RECOVERY_EMPTY;
        return false;
    }
    if (len < (size_t)TP_JRN_HEADER_LEN) {
        /* C4: a sub-header-length store is a torn/incomplete header write, not a foreign file, and
         * carries no committed record -- a truncatable tail so the glue resets it to empty. */
        *status = TP_JOURNAL_RECOVERY_TRUNCATED;
        return false;
    }
    *version = tp_jrn_get_u32(buf + TP_JRN_MAGIC_LEN);
    if (memcmp(buf, tp_jrn_magic, TP_JRN_MAGIC_LEN) != 0) {
        *status = TP_JOURNAL_RECOVERY_BAD_MAGIC; /* not our file */
        return false;
    }
    if (*version != (uint32_t)TP_JOURNAL_FORMAT_VERSION) {
        *status = TP_JOURNAL_RECOVERY_VERSION_MISMATCH; /* our file, wrong format version */
        return false;
    }
    *key = buf + TP_JRN_KEY_OFF;
    return true;
}

/* Why a front-to-back frame walk stopped: a clean EOF, an INCOMPLETE/short frame (a torn tail OR a
 * bloated length field), or a definite CORRUPT boundary (bad sync-word / bad CRC / undecodable
 * payload). Shared by recover + peek. */
typedef enum { TP_JRN_STOP_EOF, TP_JRN_STOP_INCOMPLETE, TP_JRN_STOP_CORRUPT } tp_jrn_stop;

/* R5a: map a walk outcome to a recovery status, SHARED by recover + peek so both agree on
 * OK/EMPTY/TRUNCATED/CORRUPT. `records` counts good CKPT/TXN records (META excluded); `more_after`
 * is true iff a CRC-valid record still follows the boundary (mid-stream corruption, not a tail). */
static tp_journal_recovery_status classify_stop(tp_jrn_stop stop, int records, bool more_after) {
    if (stop == TP_JRN_STOP_EOF) {
        return records > 0 ? TP_JOURNAL_RECOVERY_OK : TP_JOURNAL_RECOVERY_EMPTY;
    }
    if (stop == TP_JRN_STOP_CORRUPT || more_after) {
        return TP_JOURNAL_RECOVERY_CORRUPT;
    }
    return TP_JOURNAL_RECOVERY_TRUNCATED;
}

/* One record frame's parse+validate outcome. FRAME_OK => well-formed and CRC-valid;
 * FRAME_INCOMPLETE => too few bytes (short header, payload overrun, or short crc) -- a torn
 * tail OR a bloated length field; FRAME_BAD => a present-but-invalid frame (bad sync-word or
 * a CRC mismatch on a complete record). */
typedef enum { FRAME_OK, FRAME_INCOMPLETE, FRAME_BAD } tp_frame_status;

/* Parse + CRC-validate the record frame at offset `p` -- the SINGLE source of truth for the
 * v3 frame geometry [sync u32 | len u32 | payload | crc u32] (unchanged since the v2 sync-word).
 * On FRAME_OK sets *payload_off
 * (payload start), *payload_len, and *frame_end (byte just past the crc). Bounds-checked /
 * UB-clean on arbitrary bytes: no read is issued past `len` and a bloated length never
 * allocates. Both the recovery walk AND has_valid_record_after go through this so the
 * truncate-vs-preserve decision can never desync from how records are actually parsed. */
static tp_frame_status frame_parse_at(const uint8_t *buf, size_t len, size_t p, size_t *payload_off,
                                      size_t *payload_len, size_t *frame_end) {
    if (p > len || len - p < (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD) {
        return FRAME_INCOMPLETE; /* not enough bytes for a frame header (sync + len) */
    }
    if (tp_jrn_get_u32(buf + p) != (uint32_t)TP_JRN_SYNC_WORD) {
        return FRAME_BAD; /* framing lost: this offset is not a record boundary */
    }
    size_t len_off = p + (size_t)TP_JRN_SYNC_FIELD;
    uint32_t plen = tp_jrn_get_u32(buf + len_off);
    size_t after_len = len_off + (size_t)TP_JRN_LEN_FIELD;
    if (len - after_len < (size_t)plen) {
        return FRAME_INCOMPLETE; /* payload short: a torn tail OR a bloated length field */
    }
    size_t crc_off = after_len + (size_t)plen;
    if (len - crc_off < (size_t)TP_JRN_CRC_FIELD) {
        return FRAME_INCOMPLETE; /* checksum short */
    }
    uint32_t want = tp_jrn_get_u32(buf + crc_off);
    uint32_t got = tp_jrn_crc32(0, buf + p, (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + (size_t)plen);
    if (got != want) {
        return FRAME_BAD; /* a flipped byte / tampering on a complete record */
    }
    *payload_off = after_len;
    *payload_len = (size_t)plen;
    *frame_end = crc_off + (size_t)TP_JRN_CRC_FIELD;
    return FRAME_OK;
}

/* P1-5 (plan S18 R): is there a CRC-valid record starting at any offset >= `from`? Recovery
 * calls this at a framing/CRC boundary to decide truncate-vs-preserve: a valid record STILL
 * ahead => mid-stream corruption (a bloated length field, a lost sync-word, or a flipped
 * byte), so the file must be PRESERVED (truncating would delete the trailing acknowledged
 * records) and the journal poisoned; none => a torn tail, safe to truncate. A PURE probe --
 * it never registers ids or applies a payload (recovery never guesses past a corruption). A
 * sync-word that collides with payload bytes self-rejects on the CRC check, so it is a hint. */
static bool has_valid_record_after(const uint8_t *buf, size_t len, size_t from) {
    size_t frame_min = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + (size_t)TP_JRN_CRC_FIELD;
    if (len < frame_min) {
        return false;
    }
    for (size_t p = from; p <= len - frame_min; p++) {
        size_t payload_off = 0, payload_len = 0, frame_end = 0;
        if (frame_parse_at(buf, len, p, &payload_off, &payload_len, &frame_end) == FRAME_OK) {
            return true;
        }
    }
    return false;
}

/* R5a fix [5]: per-record callback for the SHARED front-to-back walk. Invoked ONCE per good CKPT/TXN
 * record (META is handled inside the walker, never dispatched here). It accumulates the type-specific
 * result (recover: base slice + ordered op-refs; peek: has_checkpoint). Returns TP_STATUS_OK, or
 * TP_STATUS_OOM for a hard fault (a refs realloc failure) which the walker propagates as an abort --
 * NOT a corruption boundary. `buf` + the slice offsets locate the payload inside the raw buffer. */
typedef tp_status (*tp_jrn_on_record)(void *ctx, uint8_t rtype, const uint8_t *buf, size_t payload_off,
                                      size_t snap_off, size_t snap_len, int64_t rev);

/* R5a fix [5]: the SINGLE front-to-back frame walk driven by BOTH recover + peek so their status
 * classification can never diverge. For each frame: validate geometry (frame_parse_at); decode +
 * (when ids != NULL) register ids via decode_payload; capture META (last-wins) into *meta + *has_meta;
 * and for each good CKPT/TXN record invoke on_record. Counts good CKPT/TXN records into *records (META
 * excluded), tracks *last_rev (last good record's revision) + *have_any, and stops at the first
 * torn/corrupt boundary (sets *stop). Always sets *stop_off to where the walk stopped. Returns
 * TP_STATUS_OOM on a hard fault (a decode/parse/callback OOM -> caller aborts); else TP_STATUS_OK
 * (corruption is reported via *stop, not the return). The caller does the post-walk
 * has_valid_record_after + classify_stop + (recover only) poison/mid_stream + materialization. */
static tp_status walk_records(const uint8_t *buf, size_t len, tp_idset *ids, tp_journal_meta *meta,
                              bool *has_meta, tp_jrn_on_record on_record, void *cb_ctx, size_t *stop_off,
                              tp_jrn_stop *stop, int *records, int64_t *last_rev, bool *have_any) {
    *records = 0;
    *last_rev = 0;
    *have_any = false;
    *stop = TP_JRN_STOP_EOF;
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    while (off < len) {
        size_t payload_off = 0, payload_len = 0, frame_end = 0;
        tp_frame_status fs = frame_parse_at(buf, len, off, &payload_off, &payload_len, &frame_end);
        if (fs == FRAME_INCOMPLETE) {
            *stop = TP_JRN_STOP_INCOMPLETE; /* a torn tail OR a bloated length field */
            break;
        }
        if (fs == FRAME_BAD) {
            *stop = TP_JRN_STOP_CORRUPT; /* bad sync-word or a CRC mismatch on a complete record */
            break;
        }
        /* FRAME_OK: sync + length + crc all check out; decode the payload semantics. */
        uint8_t rtype = 0;
        size_t snap_off = 0, snap_len = 0;
        int64_t rev = 0;
        tp_status ds = decode_payload(ids, buf + payload_off, payload_len, &rtype, &snap_off, &snap_len, &rev);
        if (ds == TP_STATUS_OOM) {
            *stop_off = off;
            return TP_STATUS_OOM;
        }
        if (ds != TP_STATUS_OK) {
            *stop = TP_JRN_STOP_CORRUPT; /* CRC-valid but undecodable payload => corruption boundary */
            break;
        }
        if (rtype == (uint8_t)TP_JRN_REC_META) {
            /* A META record is captured (last-wins) but never counted / dispatched to on_record: it is
             * not a txn/ckpt. A malformed META (CRC-valid but bad internals) is a corruption boundary;
             * an OOM strdup is a hard fault. */
            tp_status ms = capture_meta(buf + payload_off, payload_len, meta, has_meta);
            if (ms == TP_STATUS_OOM) {
                *stop_off = off;
                return TP_STATUS_OOM;
            }
            if (ms != TP_STATUS_OK) {
                *stop = TP_JRN_STOP_CORRUPT;
                break;
            }
            off = frame_end;
            continue;
        }
        /* A good CKPT/TXN record: hand it to the type-specific accumulator (a realloc OOM in the
         * callback is a HARD fault, not a CORRUPT stop). */
        tp_status cs = on_record(cb_ctx, rtype, buf, payload_off, snap_off, snap_len, rev);
        if (cs == TP_STATUS_OOM) {
            *stop_off = off;
            return TP_STATUS_OOM;
        }
        *last_rev = rev;
        *have_any = true;
        *records += 1;
        off = frame_end;
    }
    *stop_off = off;
    return TP_STATUS_OK;
}

/* A post-checkpoint TXN op-payload located in the raw recovery buffer (offset+len+rev).
 * The walk records these lazily and materializes them into owned tp_journal_recovered_op
 * copies once framing is fully classified -- a checkpoint mid-walk just resets the count
 * (a fresh baseline supersedes prior ops) with no owned allocations to unwind (format B). */
typedef struct {
    size_t off;
    size_t len;
    int64_t rev;
} tp_recop_ref;

/* R5a fix [5]: recover's on_record accumulator. A CKPT sets a fresh replay base + drops any prior
 * post-checkpoint op-refs (a checkpoint supersedes them); a TXN appends its op-payload slice (a realloc
 * OOM is a HARD fault the walker aborts on). */
typedef struct {
    size_t base_off, base_len;
    bool have_base;
    tp_recop_ref *refs; /* post-(last-)checkpoint TXN op-payload refs, in commit order */
    size_t ref_count, ref_cap;
} recover_walk_ctx;

static tp_status recover_on_record(void *ctx, uint8_t rtype, const uint8_t *buf, size_t payload_off,
                                   size_t snap_off, size_t snap_len, int64_t rev) {
    (void)buf; /* recover stores slice OFFSETS + materializes from buf later, in the caller */
    recover_walk_ctx *c = (recover_walk_ctx *)ctx;
    if (rtype == (uint8_t)TP_JRN_REC_CKPT) {
        /* A checkpoint is a FRESH BASELINE: it supersedes every prior post-checkpoint op. */
        c->base_off = payload_off + snap_off;
        c->base_len = snap_len;
        c->have_base = true;
        c->ref_count = 0; /* refs are plain offsets -- no owned allocations to unwind */
        return TP_STATUS_OK;
    }
    /* Format B TXN: record its op-payload slice to replay onto the base, in commit order. */
    if (c->ref_count == c->ref_cap) {
        size_t new_cap = c->ref_cap ? c->ref_cap * 2 : 8;
        tp_recop_ref *grown = (tp_recop_ref *)realloc(c->refs, new_cap * sizeof *grown);
        if (!grown) {
            return TP_STATUS_OOM; /* hard fault: the walker aborts, the caller frees c->refs */
        }
        c->refs = grown;
        c->ref_cap = new_cap;
    }
    c->refs[c->ref_count].off = payload_off + snap_off;
    c->refs[c->ref_count].len = snap_len;
    c->refs[c->ref_count].rev = rev;
    c->ref_count++;
    return TP_STATUS_OK;
}

tp_status tp_journal_recover(tp_journal *j, tp_journal_recovery *out, tp_error *err) {
    if (!j || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal or recovery out");
    }
    memset(out, 0, sizeof *out);
    uint8_t *buf = NULL;
    size_t len = 0;
    if (j->io.read_all(j->io.ctx, &buf, &len) != 0) {
        return journal_fail(err, "journal read failed");
    }
    out->bytes_total = len;
    /* Recovery rebuilds BOTH in-memory caches from the store so a reused journal reflects ONLY what the
     * store actually holds: the retained-id index AND (R5a fix [0]) the write-side metadata cache. Clearing
     * the meta cache here -- symmetric with tp_idset_reset -- means recovering a metadata-LESS store on a
     * journal that once had set_metadata called drops the stale label instead of re-emitting a ghost label
     * (that it never stored) on the next compaction. The post-walk seed below repopulates it iff the store
     * carried a META record. */
    tp_idset_reset(&j->ids);
    free(j->meta_path);
    free(j->meta_name);
    j->meta_path = NULL;
    j->meta_name = NULL;
    j->meta_time = 0;
    j->has_meta = false;

    /* Validate the fixed header via the shared classifier (EMPTY / torn-header TRUNCATED /
     * BAD_MAGIC foreign / VERSION_MISMATCH). A COMPLETE but foreign/incompatible header is
     * refused and NOT reset (bytes preserved on disk). */
    tp_journal_recovery_status hstatus;
    uint32_t hversion = 0;
    const uint8_t *hkey = NULL;
    if (!header_ok(buf, len, &hstatus, &hversion, &hkey)) {
        out->status = hstatus;
        free(buf);
        return TP_STATUS_OK;
    }
    if (memcmp(hkey, j->key.bytes, 16) != 0) {
        out->status = TP_JOURNAL_RECOVERY_STALE_KEY;
        free(buf);
        return TP_STATUS_OK;
    }

    /* R5a fix [5]: drive the SHARED front-to-back frame walk (identical to peek's) with recover's
     * accumulator. It registers ids into j->ids, captures META into out->metadata (last-wins), and
     * feeds each good CKPT/TXN record to recover_on_record. `off`/`stop` mark where + why the walk
     * stopped; `last_rev`/`have_any` carry the FINAL recovered revision; a returned OOM is a hard fault. */
    recover_walk_ctx wc;
    memset(&wc, 0, sizeof wc);
    int records = 0;
    int64_t last_rev = 0;
    bool have_any = false;
    tp_jrn_stop stop = TP_JRN_STOP_EOF;
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    tp_status hard = walk_records(buf, len, &j->ids, &out->metadata, &out->has_metadata, recover_on_record,
                                  &wc, &off, &stop, &records, &last_rev, &have_any);
    if (hard != TP_STATUS_OK) {
        free(wc.refs);
        free(buf);
        tp_journal_recovery_free(out); /* frees any captured metadata (nothing else set yet) */
        return tp_error_set(err, hard, "journal recovery ran out of memory");
    }

    out->records_recovered = records;
    out->stop_offset = off;
    out->revision = have_any ? last_rev : 0;

    /* P1-5 / C2: the truncate-vs-preserve decision reduces to ONE question -- is there a
     * CRC-valid record STILL after the boundary? (Scan from off+1 so the corrupt record at
     * off never self-matches.) If so the break is a MID-STREAM corruption -- a bloated length
     * field, a lost sync-word, or a flipped byte -- and truncating would delete the trailing
     * acknowledged records, so preserve the file and poison the journal against appends behind
     * it. If not, the tail is genuinely torn/corrupt and is safe to truncate away. A bloated
     * length that used to masquerade as a clean torn tail (TRUNCATED) is now caught here. */
    bool more_after = (stop != TP_JRN_STOP_EOF) && has_valid_record_after(buf, len, off + 1);
    out->status = classify_stop(stop, out->records_recovered, more_after);
    if (more_after) {
        out->mid_stream_corrupt = true;
        j->poisoned = true;
    }

    /* Format B materialization: copy the BASE checkpoint snapshot + the ordered op-payloads out of
     * `buf` into owned buffers (buf is freed below). Any OOM here frees what was built + returns a
     * hard fault (tp_journal_recovery_free leaves *out clean for the caller). */
    if (wc.have_base && wc.base_len > 0) {
        char *snap = (char *)malloc(wc.base_len + 1);
        if (!snap) {
            free(wc.refs);
            free(buf);
            tp_journal_recovery_free(out); /* frees any captured metadata */
            return tp_error_set(err, TP_STATUS_OOM, "journal snapshot copy failed (out of memory)");
        }
        memcpy(snap, buf + wc.base_off, wc.base_len);
        snap[wc.base_len] = '\0';
        out->snapshot = snap;
        out->snapshot_len = wc.base_len;
    }
    if (wc.ref_count > 0) {
        tp_journal_recovered_op *ops = (tp_journal_recovered_op *)calloc(wc.ref_count, sizeof *ops);
        if (!ops) {
            free(wc.refs);
            free(buf);
            tp_journal_recovery_free(out); /* frees the base snapshot already set */
            return tp_error_set(err, TP_STATUS_OOM, "journal recovery op-list allocation failed (out of memory)");
        }
        for (size_t i = 0; i < wc.ref_count; i++) {
            char *payload = (char *)malloc(wc.refs[i].len + 1);
            if (!payload) {
                for (size_t k = 0; k < i; k++) {
                    free(ops[k].payload);
                }
                free(ops);
                free(wc.refs);
                free(buf);
                tp_journal_recovery_free(out); /* frees the base snapshot already set */
                return tp_error_set(err, TP_STATUS_OOM, "journal recovery op-payload copy failed (out of memory)");
            }
            memcpy(payload, buf + wc.refs[i].off, wc.refs[i].len);
            payload[wc.refs[i].len] = '\0';
            ops[i].payload = payload;
            ops[i].payload_len = wc.refs[i].len;
            ops[i].revision = wc.refs[i].rev;
        }
        out->ops = ops;
        out->op_count = wc.ref_count;
    }

    /* R5a fix [1]: seed the journal's WRITE-side metadata cache from the recovered metadata so a later
     * compaction (R3 Save / R4 undo) on THIS journal RE-EMITS it. Use INDEPENDENT copies (out->metadata's
     * strings are freed separately by tp_journal_recovery_free). The cache was cleared up-front (above), so
     * the no-metadata case needs no else. Non-fatal on OOM: the cache is informational, recovery must still
     * succeed (has_meta simply stays false). Runs only on the SUCCESS path, so no double-free on any error path.
     * NOTE (R5b, review finding [1]): the shipping GUI flow (try_adopt_recovered) CLONES the recovered state
     * and wraps a FRESH journal, discarding THIS seeded journal -- so R5b must carry out->metadata onto
     * whatever journal it makes live for the recovered project (reuse this journal, OR call
     * tp_journal_set_metadata on the fresh one). This seed is the correct core invariant either way. */
    if (out->has_metadata) {
        char *cp = jrn_strdup(out->metadata.path ? out->metadata.path : "");
        char *cn = jrn_strdup(out->metadata.name ? out->metadata.name : "");
        if (cp && cn) {
            free(j->meta_path);
            free(j->meta_name);
            j->meta_path = cp;
            j->meta_name = cn;
            j->meta_time = out->metadata.timestamp;
            j->has_meta = true;
        } else { /* OOM: leave the cache untouched (has_meta stays false) -- informational only */
            free(cp);
            free(cn);
        }
    }

    free(wc.refs);
    free(buf);
    return TP_STATUS_OK;
}

/* ---- peek (R5a): header + metadata + status, no model reconstruction ------ */

void tp_journal_peek_free(tp_journal_peek_result *r) {
    if (!r) {
        return;
    }
    free(r->meta.path);
    free(r->meta.name);
    r->meta.path = NULL;
    r->meta.name = NULL;
    r->has_meta = false;
}

/* R5a fix [5]: peek's on_record accumulator -- it only needs to note that a CKPT (a recoverable base)
 * was seen; the walker itself counts records + captures META. Never returns OOM (no allocation). */
typedef struct {
    bool has_checkpoint;
} peek_walk_ctx;

static tp_status peek_on_record(void *ctx, uint8_t rtype, const uint8_t *buf, size_t payload_off,
                                size_t snap_off, size_t snap_len, int64_t rev) {
    (void)buf;
    (void)payload_off;
    (void)snap_off;
    (void)snap_len;
    (void)rev;
    if (rtype == (uint8_t)TP_JRN_REC_CKPT) {
        ((peek_walk_ctx *)ctx)->has_checkpoint = true;
    }
    return TP_STATUS_OK;
}

tp_status tp_journal_peek(tp_journal_io io, tp_journal_peek_result *out, tp_error *err) {
    if (!out) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null peek out");
    }
    memset(out, 0, sizeof *out);
    if (!io.ctx || !io.read_all) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid journal io for peek");
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    int rc = io.read_all(io.ctx, &buf, &len);
    if (io.destroy) {
        io.destroy(io.ctx); /* peek TAKES OWNERSHIP of io; the bytes are now in buf */
    }
    if (rc != 0) {
        free(buf);
        return journal_fail(err, "journal read failed");
    }

    /* Header: shared classifier (same BAD_MAGIC / VERSION_MISMATCH / EMPTY / torn-header split as
     * recover). peek has NO key to compare -- it REPORTS the header key instead. */
    tp_journal_recovery_status hstatus;
    uint32_t hversion = 0;
    const uint8_t *hkey = NULL;
    if (!header_ok(buf, len, &hstatus, &hversion, &hkey)) {
        out->status = hstatus;
        out->format_version = hversion;
        free(buf);
        return TP_STATUS_OK;
    }
    out->format_version = hversion;
    memcpy(out->key, hkey, 16);

    /* R5a fix [5]: drive the SAME shared frame walk as recover (with a NULL idset so nothing is
     * registered and no model is built), capturing META into out->meta and noting a checkpoint via
     * peek_on_record. `records`/`last_rev`/`have_any` are populated by the walker; peek uses only the
     * record count + the stop classification (no materialization). */
    peek_walk_ctx pc;
    memset(&pc, 0, sizeof pc);
    int records = 0;
    int64_t last_rev = 0;
    bool have_any = false;
    tp_jrn_stop stop = TP_JRN_STOP_EOF;
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    tp_status hard = walk_records(buf, len, NULL, &out->meta, &out->has_meta, peek_on_record, &pc, &off,
                                  &stop, &records, &last_rev, &have_any);
    if (hard != TP_STATUS_OK) {
        free(buf);
        tp_journal_peek_free(out);
        return tp_error_set(err, hard, "journal peek ran out of memory");
    }
    out->has_checkpoint = pc.has_checkpoint;
    out->record_count = records;
    bool more_after = (stop != TP_JRN_STOP_EOF) && has_valid_record_after(buf, len, off + 1);
    out->status = classify_stop(stop, out->record_count, more_after);
    free(buf);
    return TP_STATUS_OK;
}
