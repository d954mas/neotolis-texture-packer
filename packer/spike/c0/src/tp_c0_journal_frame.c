/* C0-03 task 1: recovery-journal record framing -- encode/decode one record plus
 * its FNV-1a/128 payload checksum. All integers little-endian, written byte by
 * byte so the on-disk bytes are identical on every OS (no host-endianness or
 * long-width leakage). See tp_c0_journal.h for the layout and the contract. */

#include "tp_c0/tp_c0_journal.h"

#include <string.h>

/* ---- fixed-width little-endian byte helpers ----------------------------- */

static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void put_i64le(uint8_t *p, int64_t v) {
    uint64_t u = (uint64_t)v; /* two's-complement bit pattern; portable round-trip */
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((u >> (8 * i)) & 0xFFu);
    }
}

static uint16_t get_u16le(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); }

static uint32_t get_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t get_i64le(const uint8_t *p) {
    uint64_t u = 0;
    for (int i = 0; i < 8; i++) {
        u |= (uint64_t)p[i] << (8 * i);
    }
    return (int64_t)u;
}

/* Both encoders lay their payload out in-place at `out + HEADER`, then call this
 * to stamp the header and the checksum over that in-place payload -- no separate
 * payload buffer and no self-overlapping copy. */
static void finish_header(uint8_t *out, tp_c0_journal_kind kind, size_t payload_len) {
    out[0] = (uint8_t)TP_C0_JOURNAL_MAGIC0;
    out[1] = (uint8_t)TP_C0_JOURNAL_MAGIC1;
    out[2] = (uint8_t)TP_C0_JOURNAL_MAGIC2;
    out[3] = (uint8_t)TP_C0_JOURNAL_MAGIC3;
    put_u16le(out + 4, (uint16_t)TP_C0_JOURNAL_VERSION);
    put_u16le(out + 6, (uint16_t)kind);
    put_u32le(out + 8, (uint32_t)payload_len);
    tp_c0_id128 sum = tp_c0_hash128(out + TP_C0_JOURNAL_HEADER_SIZE, payload_len);
    memcpy(out + 12, sum.bytes, sizeof sum.bytes);
}

static tp_c0_detail check_out(const uint8_t *out, size_t cap, size_t need, size_t *written, tp_error *err) {
    if (written) {
        *written = 0;
    }
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal encode: out is NULL");
    }
    if (cap < need) {
        return tp_c0_fail(err, TP_C0_ERR_BUFFER_TOO_SMALL, "journal encode: need %zu bytes, have %zu", need, cap);
    }
    return TP_C0_OK;
}

/* ---- encode ------------------------------------------------------------- */

tp_c0_detail tp_c0_journal_encode_txn(tp_c0_id128 txn_id, int64_t revision_after, const void *body, size_t body_len,
                                      uint8_t *out, size_t cap, size_t *written, tp_error *err) {
    if (written) {
        *written = 0;
    }
    if (tp_c0_id128_is_nil(txn_id)) {
        return tp_c0_fail(err, TP_C0_ERR_ID_NIL, "journal txn: transaction id is nil");
    }
    if (body_len > 0 && !body) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal txn: body is NULL but body_len > 0");
    }
    size_t payload_len = (size_t)TP_C0_JOURNAL_TXN_PREFIX + body_len;
    size_t need = (size_t)TP_C0_JOURNAL_HEADER_SIZE + payload_len;
    tp_c0_detail d = check_out(out, cap, need, written, err);
    if (d != TP_C0_OK) {
        return d;
    }
    uint8_t *payload = out + TP_C0_JOURNAL_HEADER_SIZE;
    memcpy(payload, txn_id.bytes, sizeof txn_id.bytes);
    put_i64le(payload + 16, revision_after);
    if (body_len > 0) {
        memcpy(payload + TP_C0_JOURNAL_TXN_PREFIX, body, body_len);
    }
    finish_header(out, TP_C0_JREC_TXN, payload_len);
    if (written) {
        *written = need;
    }
    return TP_C0_OK;
}

tp_c0_detail tp_c0_journal_encode_checkpoint(int64_t revision, tp_c0_id128 state_hash, const void *body, size_t body_len,
                                             uint8_t *out, size_t cap, size_t *written, tp_error *err) {
    if (written) {
        *written = 0;
    }
    if (body_len > 0 && !body) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal checkpoint: body is NULL but body_len > 0");
    }
    size_t payload_len = (size_t)TP_C0_JOURNAL_CHECKPOINT_PREFIX + body_len;
    size_t need = (size_t)TP_C0_JOURNAL_HEADER_SIZE + payload_len;
    tp_c0_detail d = check_out(out, cap, need, written, err);
    if (d != TP_C0_OK) {
        return d;
    }
    uint8_t *payload = out + TP_C0_JOURNAL_HEADER_SIZE;
    put_i64le(payload, revision);
    memcpy(payload + 8, state_hash.bytes, sizeof state_hash.bytes);
    if (body_len > 0) {
        memcpy(payload + TP_C0_JOURNAL_CHECKPOINT_PREFIX, body, body_len);
    }
    finish_header(out, TP_C0_JREC_CHECKPOINT, payload_len);
    if (written) {
        *written = need;
    }
    return TP_C0_OK;
}

/* ---- decode ------------------------------------------------------------- */

tp_c0_detail tp_c0_journal_decode(const uint8_t *buf, size_t len, tp_c0_journal_record *out, tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!buf || !out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal decode: buf/out is NULL");
    }
    if (len < TP_C0_JOURNAL_HEADER_SIZE) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_SHORT, "journal decode: %zu bytes < %u header", len,
                          TP_C0_JOURNAL_HEADER_SIZE);
    }
    if (buf[0] != (uint8_t)TP_C0_JOURNAL_MAGIC0 || buf[1] != (uint8_t)TP_C0_JOURNAL_MAGIC1 ||
        buf[2] != (uint8_t)TP_C0_JOURNAL_MAGIC2 || buf[3] != (uint8_t)TP_C0_JOURNAL_MAGIC3) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_BAD_MAGIC, "journal decode: bad record magic");
    }
    uint16_t version = get_u16le(buf + 4);
    if (version != (uint16_t)TP_C0_JOURNAL_VERSION) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_BAD_VERSION, "journal decode: version %u unsupported", version);
    }
    uint16_t kind = get_u16le(buf + 6);
    uint32_t payload_len = get_u32le(buf + 8);
    /* A length past the buffer is a torn/short record (payload never fully written). */
    if (payload_len > len - TP_C0_JOURNAL_HEADER_SIZE) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_SHORT, "journal decode: payload %u exceeds %zu remaining", payload_len,
                          len - TP_C0_JOURNAL_HEADER_SIZE);
    }
    const uint8_t *payload = buf + TP_C0_JOURNAL_HEADER_SIZE;
    tp_c0_id128 want;
    memcpy(want.bytes, buf + 12, sizeof want.bytes);
    tp_c0_id128 got = tp_c0_hash128(payload, payload_len);
    if (!tp_c0_id128_eq(want, got)) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_BAD_CHECKSUM, "journal decode: payload checksum mismatch");
    }

    if (kind != (uint16_t)TP_C0_JREC_TXN && kind != (uint16_t)TP_C0_JREC_CHECKPOINT) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_BAD_KIND, "journal decode: unknown record kind %u", kind);
    }
    uint32_t prefix = (kind == (uint16_t)TP_C0_JREC_TXN) ? TP_C0_JOURNAL_TXN_PREFIX : TP_C0_JOURNAL_CHECKPOINT_PREFIX;
    if (payload_len < prefix) {
        return tp_c0_fail(err, TP_C0_ERR_JOURNAL_BAD_KIND, "journal decode: payload %u shorter than %u prefix",
                          payload_len, prefix);
    }

    out->kind = (tp_c0_journal_kind)kind;
    out->record_len = (size_t)TP_C0_JOURNAL_HEADER_SIZE + payload_len;
    out->body = (payload_len > prefix) ? payload + prefix : NULL;
    out->body_len = payload_len - prefix;
    if (kind == (uint16_t)TP_C0_JREC_TXN) {
        memcpy(out->txn_id.bytes, payload, sizeof out->txn_id.bytes);
        out->revision = get_i64le(payload + 16);
    } else {
        out->revision = get_i64le(payload);
        memcpy(out->state_hash.bytes, payload + 8, sizeof out->state_hash.bytes);
    }
    return TP_C0_OK;
}
