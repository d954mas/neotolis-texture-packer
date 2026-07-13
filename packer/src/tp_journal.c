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
 * The retained-id set + last committed snapshot are recovered up to the last good record.
 */

#include "tp_core/tp_journal.h"

#include <stdlib.h>
#include <string.h>

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
    bool poisoned;     /* a failed append could not be rolled back: refuse further appends */
    char (*ids)[33];   /* retained-id index (32 hex + NUL); mirrors the durable records */
    int id_count;
    int id_cap;
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
    free(j->ids);
    free(j);
}

static tp_status journal_fail(tp_error *err, const char *msg) {
    return tp_error_set(err, TP_STATUS_JOURNAL_FAILED, "%s", msg);
}

/* Retained-id index: dedup + append (grows). OOM is a hard fault. */
static bool id_index_contains(const tp_journal *j, const char *id_hex) {
    for (int i = 0; i < j->id_count; i++) {
        if (memcmp(j->ids[i], id_hex, TP_JRN_IDLEN) == 0 && j->ids[i][TP_JRN_IDLEN] == '\0') {
            return true;
        }
    }
    return false;
}

static tp_status id_index_reserve(tp_journal *j) {
    if (j->id_count < j->id_cap) {
        return TP_STATUS_OK;
    }
    int ncap = (j->id_cap == 0) ? 16 : (j->id_cap * 2);
    char(*n)[33] = (char(*)[33])realloc(j->ids, (size_t)ncap * sizeof(*n));
    if (!n) {
        return TP_STATUS_OOM;
    }
    j->ids = n;
    j->id_cap = ncap;
    return TP_STATUS_OK;
}

/* Insert into a slot guaranteed present by a prior reserve (alloc-free). */
static void id_index_put_reserved(tp_journal *j, const char *id_hex) {
    memcpy(j->ids[j->id_count], id_hex, TP_JRN_IDLEN);
    j->ids[j->id_count][TP_JRN_IDLEN] = '\0';
    j->id_count++;
}

static tp_status id_index_register(tp_journal *j, const char *id_hex) {
    if (id_index_contains(j, id_hex)) {
        return TP_STATUS_OK; /* dedup: a checkpoint id also seen as a txn id */
    }
    tp_status rs = id_index_reserve(j);
    if (rs != TP_STATUS_OK) {
        return rs;
    }
    id_index_put_reserved(j, id_hex);
    return TP_STATUS_OK;
}

bool tp_journal_contains(const tp_journal *j, const char *id_hex) {
    if (!j || !id_hex) {
        return false;
    }
    return id_index_contains(j, id_hex);
}

int tp_journal_id_count(const tp_journal *j) { return j ? j->id_count : 0; }

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
        j->poisoned = true; /* a torn partial header: never write a second one over it */
        return journal_fail(err, "journal header is truncated");
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
    size_t reclen = (size_t)TP_JRN_LEN_FIELD + payload_len + (size_t)TP_JRN_CRC_FIELD;
    uint8_t *rec = (uint8_t *)malloc(reclen);
    if (!rec) {
        return tp_error_set(err, TP_STATUS_OOM, "journal record buffer allocation failed");
    }
    tp_jrn_put_u32(rec, (uint32_t)payload_len);
    memcpy(rec + TP_JRN_LEN_FIELD, payload, payload_len);
    uint32_t crc = tp_jrn_crc32(0, rec, (size_t)TP_JRN_LEN_FIELD + payload_len);
    tp_jrn_put_u32(rec + (size_t)TP_JRN_LEN_FIELD + payload_len, crc);
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

tp_status tp_journal_init_checkpoint(tp_journal *j, const uint8_t *snapshot, size_t len, int64_t revision,
                                     tp_error *err) {
    if (!j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null journal");
    }
    size_t ids_bytes = (size_t)j->id_count * (size_t)TP_JRN_IDLEN;
    size_t payload_len = (size_t)TP_JRN_CKPT_FIXED + ids_bytes + len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) {
        return tp_error_set(err, TP_STATUS_OOM, "journal checkpoint payload allocation failed");
    }
    size_t off = 0;
    payload[off++] = (uint8_t)TP_JRN_REC_CKPT;
    tp_jrn_put_i64(payload + off, revision);
    off += 8;
    tp_jrn_put_u32(payload + off, (uint32_t)j->id_count);
    off += 4;
    for (int i = 0; i < j->id_count; i++) {
        memcpy(payload + off, j->ids[i], TP_JRN_IDLEN);
        off += TP_JRN_IDLEN;
    }
    if (len) {
        memcpy(payload + off, snapshot, len);
    }
    tp_status st = write_record(j, payload, payload_len, err);
    free(payload);
    return st;
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
    if (id_index_contains(j, id_hex)) {
        return TP_STATUS_OK; /* already retained: an idempotent no-op (never double-appends) */
    }
    tp_status rs = id_index_reserve(j);
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
    id_index_put_reserved(j, id_hex);
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
}

/* Decode one payload; register its ids into j; output the snapshot slice offset
 * (relative to payload start) + length + revision. Returns TP_STATUS_OK on a good
 * record, TP_STATUS_OOM on a hard fault (abort recovery), or TP_STATUS_OUT_OF_BOUNDS
 * for a malformed payload (a corruption boundary). All reads bounds-checked. */
static tp_status decode_payload(tp_journal *j, const uint8_t *pl, size_t plen, size_t *snap_off, size_t *snap_len,
                                int64_t *rev) {
    if (plen < 1) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    uint8_t type = pl[0];
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
        return id_index_register(j, idhex);
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
        j->id_count = 0; /* a checkpoint RESETS the retained set to its captured id list */
        for (uint32_t i = 0; i < idc; i++) {
            char idhex[33];
            memcpy(idhex, pl + (size_t)TP_JRN_CKPT_FIXED + (size_t)i * (size_t)TP_JRN_IDLEN, TP_JRN_IDLEN);
            idhex[TP_JRN_IDLEN] = '\0';
            tp_status r = id_index_register(j, idhex);
            if (r != TP_STATUS_OK) {
                return r;
            }
        }
        *snap_off = (size_t)TP_JRN_CKPT_FIXED + ids_bytes;
        *snap_len = plen - *snap_off;
        return TP_STATUS_OK;
    }
    return TP_STATUS_OUT_OF_BOUNDS; /* unknown record type => corruption */
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
    j->id_count = 0; /* recovery rebuilds the retained index from scratch */

    if (len == 0) {
        out->status = TP_JOURNAL_RECOVERY_EMPTY;
        free(buf);
        return TP_STATUS_OK;
    }
    if (len < (size_t)TP_JRN_HEADER_LEN || memcmp(buf, tp_jrn_magic, TP_JRN_MAGIC_LEN) != 0 ||
        tp_jrn_get_u32(buf + TP_JRN_MAGIC_LEN) != (uint32_t)TP_JOURNAL_FORMAT_VERSION) {
        out->status = TP_JOURNAL_RECOVERY_BAD_HEADER;
        free(buf);
        return TP_STATUS_OK;
    }
    if (memcmp(buf + TP_JRN_KEY_OFF, j->key.bytes, 16) != 0) {
        out->status = TP_JOURNAL_RECOVERY_STALE_KEY;
        free(buf);
        return TP_STATUS_OK;
    }

    size_t off = (size_t)TP_JRN_HEADER_LEN;
    size_t last_snap_off = 0, last_snap_len = 0;
    int64_t last_rev = 0;
    bool have_snap = false;
    tp_journal_recovery_status final = TP_JOURNAL_RECOVERY_OK;
    tp_status hard = TP_STATUS_OK;

    while (off < len) {
        if (len - off < (size_t)TP_JRN_LEN_FIELD) {
            final = TP_JOURNAL_RECOVERY_TRUNCATED;
            break;
        }
        uint32_t plen = tp_jrn_get_u32(buf + off);
        size_t after_len = off + (size_t)TP_JRN_LEN_FIELD;
        if (len - after_len < (size_t)plen) {
            final = TP_JOURNAL_RECOVERY_TRUNCATED; /* payload short */
            break;
        }
        size_t crc_off = after_len + (size_t)plen;
        if (len - crc_off < (size_t)TP_JRN_CRC_FIELD) {
            final = TP_JOURNAL_RECOVERY_TRUNCATED; /* checksum short */
            break;
        }
        uint32_t want = tp_jrn_get_u32(buf + crc_off);
        uint32_t got = tp_jrn_crc32(0, buf + off, (size_t)TP_JRN_LEN_FIELD + (size_t)plen);
        if (got != want) {
            final = TP_JOURNAL_RECOVERY_CORRUPT; /* a flipped byte / tampering */
            break;
        }
        size_t snap_off = 0, snap_len = 0;
        int64_t rev = 0;
        tp_status ds = decode_payload(j, buf + after_len, (size_t)plen, &snap_off, &snap_len, &rev);
        if (ds == TP_STATUS_OOM) {
            hard = ds;
            break;
        }
        if (ds != TP_STATUS_OK) {
            final = TP_JOURNAL_RECOVERY_CORRUPT; /* malformed payload => corruption boundary */
            break;
        }
        last_snap_off = after_len + snap_off;
        last_snap_len = snap_len;
        last_rev = rev;
        have_snap = true;
        out->records_recovered++;
        off = crc_off + (size_t)TP_JRN_CRC_FIELD;
    }

    if (hard != TP_STATUS_OK) {
        free(buf);
        return tp_error_set(err, hard, "journal recovery ran out of memory");
    }

    out->stop_offset = off;
    out->revision = have_snap ? last_rev : 0;
    if (final == TP_JOURNAL_RECOVERY_OK) {
        out->status = (out->records_recovered > 0) ? TP_JOURNAL_RECOVERY_OK : TP_JOURNAL_RECOVERY_EMPTY;
    } else {
        out->status = final;
    }

    if (have_snap && last_snap_len > 0) {
        char *snap = (char *)malloc(last_snap_len + 1);
        if (!snap) {
            free(buf);
            return tp_error_set(err, TP_STATUS_OOM, "journal snapshot copy failed (out of memory)");
        }
        memcpy(snap, buf + last_snap_off, last_snap_len);
        snap[last_snap_len] = '\0';
        out->snapshot = snap;
        out->snapshot_len = last_snap_len;
    }
    free(buf);
    return TP_STATUS_OK;
}
