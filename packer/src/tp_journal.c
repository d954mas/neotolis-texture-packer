/*
 * Minimum recovery journal -- a PURE durable log over the tp_journal_io seam
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
    /* Reflected IEEE CRC-32, one immutable/thread-safe lookup per byte. */
    static const uint32_t table[256] = {
        0x00000000u, 0x77073096u, 0xEE0E612Cu, 0x990951BAu,
        0x076DC419u, 0x706AF48Fu, 0xE963A535u, 0x9E6495A3u,
        0x0EDB8832u, 0x79DCB8A4u, 0xE0D5E91Eu, 0x97D2D988u,
        0x09B64C2Bu, 0x7EB17CBDu, 0xE7B82D07u, 0x90BF1D91u,
        0x1DB71064u, 0x6AB020F2u, 0xF3B97148u, 0x84BE41DEu,
        0x1ADAD47Du, 0x6DDDE4EBu, 0xF4D4B551u, 0x83D385C7u,
        0x136C9856u, 0x646BA8C0u, 0xFD62F97Au, 0x8A65C9ECu,
        0x14015C4Fu, 0x63066CD9u, 0xFA0F3D63u, 0x8D080DF5u,
        0x3B6E20C8u, 0x4C69105Eu, 0xD56041E4u, 0xA2677172u,
        0x3C03E4D1u, 0x4B04D447u, 0xD20D85FDu, 0xA50AB56Bu,
        0x35B5A8FAu, 0x42B2986Cu, 0xDBBBC9D6u, 0xACBCF940u,
        0x32D86CE3u, 0x45DF5C75u, 0xDCD60DCFu, 0xABD13D59u,
        0x26D930ACu, 0x51DE003Au, 0xC8D75180u, 0xBFD06116u,
        0x21B4F4B5u, 0x56B3C423u, 0xCFBA9599u, 0xB8BDA50Fu,
        0x2802B89Eu, 0x5F058808u, 0xC60CD9B2u, 0xB10BE924u,
        0x2F6F7C87u, 0x58684C11u, 0xC1611DABu, 0xB6662D3Du,
        0x76DC4190u, 0x01DB7106u, 0x98D220BCu, 0xEFD5102Au,
        0x71B18589u, 0x06B6B51Fu, 0x9FBFE4A5u, 0xE8B8D433u,
        0x7807C9A2u, 0x0F00F934u, 0x9609A88Eu, 0xE10E9818u,
        0x7F6A0DBBu, 0x086D3D2Du, 0x91646C97u, 0xE6635C01u,
        0x6B6B51F4u, 0x1C6C6162u, 0x856530D8u, 0xF262004Eu,
        0x6C0695EDu, 0x1B01A57Bu, 0x8208F4C1u, 0xF50FC457u,
        0x65B0D9C6u, 0x12B7E950u, 0x8BBEB8EAu, 0xFCB9887Cu,
        0x62DD1DDFu, 0x15DA2D49u, 0x8CD37CF3u, 0xFBD44C65u,
        0x4DB26158u, 0x3AB551CEu, 0xA3BC0074u, 0xD4BB30E2u,
        0x4ADFA541u, 0x3DD895D7u, 0xA4D1C46Du, 0xD3D6F4FBu,
        0x4369E96Au, 0x346ED9FCu, 0xAD678846u, 0xDA60B8D0u,
        0x44042D73u, 0x33031DE5u, 0xAA0A4C5Fu, 0xDD0D7CC9u,
        0x5005713Cu, 0x270241AAu, 0xBE0B1010u, 0xC90C2086u,
        0x5768B525u, 0x206F85B3u, 0xB966D409u, 0xCE61E49Fu,
        0x5EDEF90Eu, 0x29D9C998u, 0xB0D09822u, 0xC7D7A8B4u,
        0x59B33D17u, 0x2EB40D81u, 0xB7BD5C3Bu, 0xC0BA6CADu,
        0xEDB88320u, 0x9ABFB3B6u, 0x03B6E20Cu, 0x74B1D29Au,
        0xEAD54739u, 0x9DD277AFu, 0x04DB2615u, 0x73DC1683u,
        0xE3630B12u, 0x94643B84u, 0x0D6D6A3Eu, 0x7A6A5AA8u,
        0xE40ECF0Bu, 0x9309FF9Du, 0x0A00AE27u, 0x7D079EB1u,
        0xF00F9344u, 0x8708A3D2u, 0x1E01F268u, 0x6906C2FEu,
        0xF762575Du, 0x806567CBu, 0x196C3671u, 0x6E6B06E7u,
        0xFED41B76u, 0x89D32BE0u, 0x10DA7A5Au, 0x67DD4ACCu,
        0xF9B9DF6Fu, 0x8EBEEFF9u, 0x17B7BE43u, 0x60B08ED5u,
        0xD6D6A3E8u, 0xA1D1937Eu, 0x38D8C2C4u, 0x4FDFF252u,
        0xD1BB67F1u, 0xA6BC5767u, 0x3FB506DDu, 0x48B2364Bu,
        0xD80D2BDAu, 0xAF0A1B4Cu, 0x36034AF6u, 0x41047A60u,
        0xDF60EFC3u, 0xA867DF55u, 0x316E8EEFu, 0x4669BE79u,
        0xCB61B38Cu, 0xBC66831Au, 0x256FD2A0u, 0x5268E236u,
        0xCC0C7795u, 0xBB0B4703u, 0x220216B9u, 0x5505262Fu,
        0xC5BA3BBEu, 0xB2BD0B28u, 0x2BB45A92u, 0x5CB36A04u,
        0xC2D7FFA7u, 0xB5D0CF31u, 0x2CD99E8Bu, 0x5BDEAE1Du,
        0x9B64C2B0u, 0xEC63F226u, 0x756AA39Cu, 0x026D930Au,
        0x9C0906A9u, 0xEB0E363Fu, 0x72076785u, 0x05005713u,
        0x95BF4A82u, 0xE2B87A14u, 0x7BB12BAEu, 0x0CB61B38u,
        0x92D28E9Bu, 0xE5D5BE0Du, 0x7CDCEFB7u, 0x0BDBDF21u,
        0x86D3D2D4u, 0xF1D4E242u, 0x68DDB3F8u, 0x1FDA836Eu,
        0x81BE16CDu, 0xF6B9265Bu, 0x6FB077E1u, 0x18B74777u,
        0x88085AE6u, 0xFF0F6A70u, 0x66063BCAu, 0x11010B5Cu,
        0x8F659EFFu, 0xF862AE69u, 0x616BFFD3u, 0x166CCF45u,
        0xA00AE278u, 0xD70DD2EEu, 0x4E048354u, 0x3903B3C2u,
        0xA7672661u, 0xD06016F7u, 0x4969474Du, 0x3E6E77DBu,
        0xAED16A4Au, 0xD9D65ADCu, 0x40DF0B66u, 0x37D83BF0u,
        0xA9BCAE53u, 0xDEBB9EC5u, 0x47B2CF7Fu, 0x30B5FFE9u,
        0xBDBDF21Cu, 0xCABAC28Au, 0x53B39330u, 0x24B4A3A6u,
        0xBAD03605u, 0xCDD70693u, 0x54DE5729u, 0x23D967BFu,
        0xB3667A2Eu, 0xC4614AB8u, 0x5D681B02u, 0x2A6F2B94u,
        0xB40BBE37u, 0xC30C8EA1u, 0x5A05DF1Bu, 0x2D02EF8Du,
    };
    crc = crc ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        crc = (crc >> 8) ^ table[crc & 0xFFu];
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
    size_t record_count; /* all durable CKPT/TXN/HISTORY/META frames in the store */
    size_t replay_count; /* TXN/HISTORY refs after the latest durable checkpoint */
    size_t replay_operations; /* decoded operations across those records */
    tp_idset ids;      /* retained-id index (shared set); mirrors the durable records */
    /* Cached project metadata (owned copies). Set by tp_journal_set_metadata and re-emitted by
     * tp_journal_compact so it survives compaction. has_meta stays false until first set. */
    int64_t meta_time;
    char *meta_path;   /* owned; NULL until set (then never NULL, may be "") */
    char *meta_name;   /* owned; NULL until set (then never NULL, may be "") */
    tp_id128 meta_file_fingerprint;
    bool meta_has_file_fingerprint;
    bool has_meta;
};

static _Thread_local size_t s_test_record_limit;
static _Thread_local size_t s_test_file_limit;

static size_t journal_record_limit(void) {
    return s_test_record_limit ? s_test_record_limit
                               : (size_t)TP_JOURNAL_MAX_RECORDS;
}

static size_t journal_file_limit(void) {
    return s_test_file_limit ? s_test_file_limit
                             : (size_t)TP_JOURNAL_MAX_FILE_BYTES;
}

static tp_status journal_fail(tp_error *err, const char *msg);

static tp_status journal_read_all_bounded(tp_journal_io *io, uint8_t **out,
                                          size_t *out_len, tp_error *err) {
    *out = NULL;
    *out_len = 0U;
    const int64_t advertised = io->length(io->ctx);
    if (advertised < 0) {
        return journal_fail(err, "journal length query failed");
    }
    if ((uint64_t)advertised > (uint64_t)journal_file_limit()) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "journal exceeds the supported byte bound");
    }
    const size_t limit = journal_file_limit();
    if (io->read_all(io->ctx, limit, out, out_len) != 0) {
        return journal_fail(err, "journal read failed");
    }
    if (*out_len > limit) {
        free(*out);
        *out = NULL;
        *out_len = 0U;
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "journal grew beyond the supported byte bound while reading");
    }
    return TP_STATUS_OK;
}

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
    const uint64_t file_limit = (uint64_t)journal_file_limit();
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
    if (j->record_count >= journal_record_limit()) {
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
    if (replacement_records > journal_record_limit()) {
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

/* ---- recovery ------------------------------------------------------------ */

void tp_journal_recovery_free(tp_journal_recovery *r) {
    if (!r) {
        return;
    }
    r->snapshot = NULL;
    r->snapshot_len = 0;
    free(r->ops);
    r->ops = NULL;
    r->op_count = 0;
    free(r->_raw_record_buffer);
    r->_raw_record_buffer = NULL;
    r->_raw_record_buffer_len = 0;
    free(r->metadata.path); /* R5a: owned metadata strings */
    free(r->metadata.name);
    r->metadata.path = NULL;
    r->metadata.name = NULL;
    r->has_metadata = false;
}

/* Metadata is common in long-lived sessions. Recovery therefore validates each
 * frame into a borrowed last-wins descriptor and allocates the two result strings
 * once, after the walk. Repeated META frames remain O(1) auxiliary memory and do
 * not perform two malloc/free pairs per frame. */
typedef struct tp_jrn_meta_ref {
    int64_t timestamp;
    const uint8_t *path;
    size_t path_len;
    const uint8_t *name;
    size_t name_len;
    tp_id128 file_fingerprint;
    bool has_file_fingerprint;
} tp_jrn_meta_ref;

static tp_status parse_meta_ref(const uint8_t *pl, size_t plen,
                                tp_jrn_meta_ref *out) {
    memset(out, 0, sizeof *out);
    if (plen < (size_t)TP_JRN_META_FIXED) {
        return TP_STATUS_OUT_OF_BOUNDS; /* too short for type + timestamp + path_len */
    }
    out->timestamp = tp_jrn_get_i64(pl + 1);
    uint32_t path_len = tp_jrn_get_u32(pl + 9);
    size_t off = (size_t)TP_JRN_META_FIXED;
    if (plen - off < (size_t)path_len) {
        return TP_STATUS_OUT_OF_BOUNDS; /* path overruns the payload */
    }
    out->path = pl + off;
    out->path_len = (size_t)path_len;
    off += (size_t)path_len;
    if (plen - off < (size_t)TP_JRN_LEN_FIELD) {
        return TP_STATUS_OUT_OF_BOUNDS; /* no room for name_len */
    }
    uint32_t name_len = tp_jrn_get_u32(pl + off);
    off += (size_t)TP_JRN_LEN_FIELD;
    if (plen - off < (size_t)name_len) {
        return TP_STATUS_OUT_OF_BOUNDS; /* name overruns the payload */
    }
    out->name = pl + off;
    out->name_len = (size_t)name_len;
    off += (size_t)name_len;
    const size_t trailing = plen - off;
    if (trailing != 0 && trailing != sizeof out->file_fingerprint.bytes) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    if (trailing == sizeof out->file_fingerprint.bytes) {
        memcpy(out->file_fingerprint.bytes, pl + off,
               sizeof out->file_fingerprint.bytes);
        out->has_file_fingerprint = true;
    }
    return TP_STATUS_OK;
}

static _Thread_local bool s_fail_next_metadata_materialize;

void tp_journal__test_fail_next_metadata_materialize(void) {
    s_fail_next_metadata_materialize = true;
}

static tp_status materialize_meta(const tp_jrn_meta_ref *ref,
                                  tp_journal_meta *dst) {
    if (s_fail_next_metadata_materialize) {
        s_fail_next_metadata_materialize = false;
        return TP_STATUS_OOM;
    }
    char *path = jrn_dup_bytes(ref->path, ref->path_len);
    if (!path) {
        return TP_STATUS_OOM;
    }
    char *name = jrn_dup_bytes(ref->name, ref->name_len);
    if (!name) {
        free(path);
        return TP_STATUS_OOM;
    }
    dst->timestamp = ref->timestamp;
    dst->path = path;
    dst->name = name;
    dst->file_fingerprint = ref->file_fingerprint;
    dst->has_file_fingerprint = ref->has_file_fingerprint;
    return TP_STATUS_OK;
}

/* Decode one payload without publishing its ids; output the record type, the
 * payload SLICE offset (relative to payload start) + length + revision. For a CHECKPOINT the slice is
 * the project snapshot; for a TXN (format B) it is the serialized op-payload; a META record has no
 * slice (0/0). When `ids` is present it is read only to reject a retained duplicate transaction;
 * accepted ids are published by the walker only after its record callback succeeds. `ids` NULL means
 * classify + bounds-check only (peek shares this decoder, having no idset). Returns TP_STATUS_OK on a
 * good record, TP_STATUS_OOM on a hard fault (abort), or
 * TP_STATUS_OUT_OF_BOUNDS for a malformed/unknown payload (a corruption boundary). All reads
 * bounds-checked. */
static tp_status decode_payload(const tp_idset *ids, const uint8_t *pl, size_t plen, uint8_t *rec_type, size_t *snap_off,
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
        if (!tp_idset_valid_hex(idhex)) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        *rev = tp_jrn_get_i64(pl + 1 + TP_JRN_IDLEN);
        *snap_off = (size_t)TP_JRN_TXN_FIXED;
        *snap_len = plen - (size_t)TP_JRN_TXN_FIXED;
        if (!ids) {
            return TP_STATUS_OK;
        }
        /* A retained duplicate could not have been emitted by the writer. Reject
         * it before replay instead of applying the same acknowledged operation twice. */
        return tp_idset_contains(ids, idhex) ? TP_STATUS_OUT_OF_BOUNDS : TP_STATUS_OK;
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
        if (idc > (uint32_t)TP_TXN_RETAINED_ID_CAP) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        size_t ids_bytes = (size_t)idc * (size_t)TP_JRN_IDLEN;
        tp_idset checkpoint_ids = {0};
        if (idc > 0U && tp_idset_reserve(&checkpoint_ids) != TP_STATUS_OK) {
            return TP_STATUS_OOM;
        }
        for (uint32_t i = 0; i < idc; i++) {
            char idhex[33];
            memcpy(idhex, pl + (size_t)TP_JRN_CKPT_FIXED + (size_t)i * (size_t)TP_JRN_IDLEN, TP_JRN_IDLEN);
            idhex[TP_JRN_IDLEN] = '\0';
            if (!tp_idset_valid_hex(idhex)) {
                tp_idset_dispose(&checkpoint_ids);
                return TP_STATUS_OUT_OF_BOUNDS;
            }
            if (tp_idset_contains(&checkpoint_ids, idhex)) {
                tp_idset_dispose(&checkpoint_ids);
                return TP_STATUS_OUT_OF_BOUNDS;
            }
            tp_idset_put_reserved(&checkpoint_ids, idhex);
        }
        tp_idset_dispose(&checkpoint_ids);
        *snap_off = (size_t)TP_JRN_CKPT_FIXED + ids_bytes;
        *snap_len = plen - *snap_off;
        return TP_STATUS_OK;
    }
    if (type == (uint8_t)TP_JRN_REC_META) {
        /* A META record has no snapshot slice and registers no id; the walk parses its fields via
         * parse_meta. Decoding here just accepts it as a well-formed (skippable) record so a CRC-valid
         * META interleaved with TXNs is not mistaken for corruption. */
        *snap_off = 0;
        *snap_len = 0;
        return TP_STATUS_OK;
    }
    if (type == (uint8_t)TP_JRN_REC_HISTORY) {
        if (plen < (size_t)TP_JRN_HISTORY_FIXED + 8U) {
            return TP_STATUS_OUT_OF_BOUNDS;
        }
        *rev = tp_jrn_get_i64(pl + 1);
        *snap_off = (size_t)TP_JRN_HISTORY_FIXED;
        *snap_len = plen - (size_t)TP_JRN_HISTORY_FIXED;
        return TP_STATUS_OK;
    }
    return TP_STATUS_OUT_OF_BOUNDS; /* unknown record type => corruption */
}

/* Classify the fixed header once for recover and peek so both split BAD_MAGIC
 * vs VERSION_MISMATCH identically. Returns true when the header is well-formed (caller may walk
 * records) and sets *version + *key (a 16-byte pointer into buf). Otherwise sets *status to the
 * classified failure (EMPTY / TRUNCATED torn-header / BAD_MAGIC / VERSION_MISMATCH) and returns false.
 * *version is best-effort populated from the header bytes when present (0 otherwise); *key points
 * into every complete same-magic header, including a version mismatch, so store classification can
 * distinguish this application's legacy journal from a foreign key without parsing records. */
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
    *key = buf + TP_JRN_KEY_OFF;
    if (*version != (uint32_t)TP_JOURNAL_FORMAT_VERSION) {
        *status = TP_JOURNAL_RECOVERY_VERSION_MISMATCH; /* our file, wrong format version */
        return false;
    }
    return true;
}

/* Why a front-to-back frame walk stopped: a clean EOF, an INCOMPLETE/short frame (a torn tail OR a
 * bloated length field), or a definite CORRUPT boundary (bad sync-word / bad CRC / undecodable
 * payload). Shared by recover + peek. */
typedef enum { TP_JRN_STOP_EOF, TP_JRN_STOP_INCOMPLETE, TP_JRN_STOP_CORRUPT } tp_jrn_stop;

/* Map a walk outcome to a recovery status so recover and peek agree on
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

/* Is there a CRC-valid record starting at any offset >= `from`? Recovery
 * calls this at a framing/CRC boundary to decide truncate-vs-preserve: a valid record STILL
 * ahead => mid-stream corruption (a bloated length field, a lost sync-word, or a flipped
 * byte), so the file must be PRESERVED (truncating would delete the trailing acknowledged
 * records) and the journal poisoned; none => a torn tail, safe to truncate. A PURE probe --
 * it never registers ids or applies a payload (recovery never guesses past a corruption). A
 * sync-word that collides with payload bytes self-rejects on the CRC check, so it is a hint. */
static bool has_valid_record_after(const uint8_t *buf, size_t len, size_t from, size_t *crc_work_out) {
    size_t frame_min = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD + (size_t)TP_JRN_CRC_FIELD;
    size_t crc_work_done = 0;
    if (crc_work_out) {
        *crc_work_out = 0;
    }
    if (len < frame_min) {
        return false;
    }
    for (size_t p = from; p <= len - frame_min; p++) {
        if (tp_jrn_get_u32(buf + p) != (uint32_t)TP_JRN_SYNC_WORD) {
            continue;
        }
        const size_t len_off = p + (size_t)TP_JRN_SYNC_FIELD;
        const size_t payload_len_candidate = (size_t)tp_jrn_get_u32(buf + len_off);
        const size_t after_len = len_off + (size_t)TP_JRN_LEN_FIELD;
        if (payload_len_candidate > len - after_len ||
            (size_t)TP_JRN_CRC_FIELD > len - after_len - payload_len_candidate) {
            continue; /* incomplete candidate: no CRC work */
        }
        const size_t crc_work = (size_t)TP_JRN_SYNC_FIELD + (size_t)TP_JRN_LEN_FIELD +
                                payload_len_candidate;
        if (crc_work > len - crc_work_done) {
            /* Adversarial dense sync candidates exhausted the linear work budget. Preserve the
             * journal as corrupt instead of spending super-linear startup time or truncating it. */
            if (crc_work_out) {
                *crc_work_out = crc_work_done;
            }
            return true;
        }
        crc_work_done += crc_work;
        size_t payload_off = 0, payload_len = 0, frame_end = 0;
        if (frame_parse_at(buf, len, p, &payload_off, &payload_len, &frame_end) == FRAME_OK) {
            if (crc_work_out) {
                *crc_work_out = crc_work_done;
            }
            return true;
        }
    }
    if (crc_work_out) {
        *crc_work_out = crc_work_done;
    }
    return false;
}

/* Publish a fully validated and accepted record's retained IDs. The backing set
 * was reserved when the journal was created, so this phase is allocation-free and
 * cannot fail after the replay callback accepted the record. */
static void publish_payload_ids(tp_idset *ids, const uint8_t *pl, uint8_t type) {
    if (!ids) return;
    if (type == (uint8_t)TP_JRN_REC_TXN) {
        char idhex[TP_JRN_IDLEN + 1];
        memcpy(idhex, pl + 1, TP_JRN_IDLEN);
        idhex[TP_JRN_IDLEN] = '\0';
        tp_idset_put_reserved(ids, idhex);
        return;
    }
    if (type == (uint8_t)TP_JRN_REC_CKPT) {
        const uint32_t idc = tp_jrn_get_u32(pl + 9);
        tp_idset_reset(ids);
        for (uint32_t i = 0; i < idc; ++i) {
            char idhex[TP_JRN_IDLEN + 1];
            memcpy(idhex, pl + (size_t)TP_JRN_CKPT_FIXED + (size_t)i * (size_t)TP_JRN_IDLEN,
                   TP_JRN_IDLEN);
            idhex[TP_JRN_IDLEN] = '\0';
            tp_idset_put_reserved(ids, idhex);
        }
    }
}

bool tp_journal__test_has_valid_record_after(const uint8_t *buf, size_t len, size_t from,
                                             size_t *crc_work_out) {
    return has_valid_record_after(buf, len, from, crc_work_out);
}

bool tp_journal__test_recovery_ops_borrow_raw(const tp_journal_recovery *recovery) {
    if (!recovery) {
        return false;
    }
    if (recovery->op_count == 0U) {
        return true;
    }
    if (!recovery->ops || !recovery->_raw_record_buffer ||
        recovery->_raw_record_buffer_len == 0U) {
        return false;
    }
    const uintptr_t begin = (uintptr_t)recovery->_raw_record_buffer;
    if (recovery->_raw_record_buffer_len > UINTPTR_MAX - begin) {
        return false;
    }
    const uintptr_t end = begin + recovery->_raw_record_buffer_len;
    for (size_t i = 0U; i < recovery->op_count; ++i) {
        const uintptr_t payload = (uintptr_t)recovery->ops[i].payload;
        if (payload < begin || payload > end ||
            recovery->ops[i].payload_len > (size_t)(end - payload)) {
            return false;
        }
    }
    return true;
}

void tp_journal__test_recovery_copy_stats(
    const tp_journal_recovery *recovery,
    tp_journal_recovery_copy_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (!recovery) {
        return;
    }
    if (recovery->bytes_total > 0U) {
        out->raw_storage_copies = 1U;
        out->raw_storage_bytes = recovery->bytes_total;
    }
    const uintptr_t begin = (uintptr_t)recovery->_raw_record_buffer;
    const bool raw_range_valid = recovery->_raw_record_buffer &&
                                 recovery->_raw_record_buffer_len <= UINTPTR_MAX - begin;
    const uintptr_t end = raw_range_valid
                              ? begin + recovery->_raw_record_buffer_len
                              : begin;
    const uintptr_t snapshot = (uintptr_t)recovery->snapshot;
    const bool snapshot_borrowed = raw_range_valid && recovery->snapshot &&
                                   snapshot >= begin && snapshot <= end &&
                                   recovery->snapshot_len <= (size_t)(end - snapshot);
    if (recovery->snapshot && !snapshot_borrowed) {
        out->checkpoint_payload_copies = 1U;
        out->checkpoint_payload_bytes = recovery->snapshot_len;
    }
    for (size_t i = 0U; i < recovery->op_count; ++i) {
        const uintptr_t payload = (uintptr_t)recovery->ops[i].payload;
        const bool borrowed = raw_range_valid && recovery->ops[i].payload &&
                              payload >= begin && payload <= end &&
                              recovery->ops[i].payload_len <= (size_t)(end - payload);
        if (!borrowed) {
            out->operation_payload_copies++;
            out->operation_payload_bytes += recovery->ops[i].payload_len;
        }
    }
}

/* Per-record callback for the shared front-to-back walk. Invoked once per good CKPT/TXN
 * record (META is handled inside the walker, never dispatched here). It accumulates the type-specific
 * result (recover: base slice + ordered op-refs; peek: has_checkpoint). Returns TP_STATUS_OK, or
 * a hard resource status (refs OOM/count limit) which the walker propagates as an abort -- NOT a
 * corruption boundary. `buf` + the slice offsets locate the payload inside the raw buffer. */
typedef tp_status (*tp_jrn_on_record)(void *ctx, uint8_t rtype, const uint8_t *buf, size_t payload_off,
                                      size_t snap_off, size_t snap_len, int64_t rev);

/* Single front-to-back frame walk shared by recover and peek so their status
 * classification can never diverge. For each frame: validate geometry (frame_parse_at); decode and
 * validate ids without publishing them; capture META (last-wins) into *meta + *has_meta;
 * and for each good CKPT/TXN record invoke on_record. Counts good CKPT/TXN records into *records (META
 * excluded), tracks *last_rev (last good record's revision) + *have_any, and stops at the first
 * torn/corrupt boundary (sets *stop). Always sets *stop_off to where the walk stopped. Returns
 * a non-OK hard resource status (decode/parse/callback -> caller aborts); else TP_STATUS_OK
 * (corruption is reported via *stop, not the return). The caller does the post-walk
 * has_valid_record_after + classify_stop + (recover only) poison/mid_stream + materialization. */
static tp_status walk_records(const uint8_t *buf, size_t len, tp_idset *ids, tp_jrn_meta_ref *meta,
                              bool *has_meta, tp_jrn_on_record on_record, void *cb_ctx, size_t *stop_off,
                              tp_jrn_stop *stop, int *records, size_t *frame_count,
                              int64_t *last_rev, bool *have_any) {
    *records = 0;
    *frame_count = 0U;
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
        if (*frame_count >= journal_record_limit()) {
            *stop_off = off;
            return TP_STATUS_OUT_OF_BOUNDS;
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
            tp_status ms = parse_meta_ref(buf + payload_off, payload_len, meta);
            if (ms != TP_STATUS_OK) {
                *stop = TP_JRN_STOP_CORRUPT;
                break;
            }
            *has_meta = true;
            *frame_count += 1U;
            off = frame_end;
            continue;
        }
        /* A live model can advance only from a non-negative revision below INT64_MAX, and every durable
         * state record must move strictly forward. CRC-valid hostile bytes that violate this contract are
         * a corruption boundary, never a formally successful but permanently uneditable recovery. */
        if (rev < 0 || rev == INT64_MAX || (*have_any && rev <= *last_rev)) {
            *stop = TP_JRN_STOP_CORRUPT;
            break;
        }
        /* A good CKPT/TXN record: hand it to the type-specific accumulator. Allocation or
         * component-limit failure is a HARD fault, not a CORRUPT stop. */
        tp_status cs = on_record(cb_ctx, rtype, buf, payload_off, snap_off, snap_len, rev);
        if (cs != TP_STATUS_OK) {
            *stop_off = off;
            return cs;
        }
        publish_payload_ids(ids, buf + payload_off, rtype);
        *last_rev = rev;
        *have_any = true;
        *records += 1;
        *frame_count += 1U;
        off = frame_end;
    }
    *stop_off = off;
    return TP_STATUS_OK;
}

/* A post-checkpoint TXN op-payload located in the raw recovery buffer (offset+len+rev).
 * The walk records these lazily and materializes one borrowed-span descriptor array once
 * framing is fully classified -- a checkpoint mid-walk just resets the count (a fresh
 * baseline supersedes prior ops) with no payload allocations to unwind (format B). */
typedef struct {
    size_t off;
    size_t len;
    int64_t rev;
    uint8_t type;
} tp_recop_ref;

/* Recovery accumulator. A checkpoint sets a fresh replay base and drops any prior
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
    if (c->ref_count >= (size_t)TP_JOURNAL_MAX_REPLAY_RECORDS) {
        return TP_STATUS_OUT_OF_BOUNDS;
    }
    if (c->ref_count == c->ref_cap) {
        size_t new_cap = c->ref_cap ? c->ref_cap * 2U : 8U;
        if (new_cap > (size_t)TP_JOURNAL_MAX_REPLAY_RECORDS) {
            new_cap = (size_t)TP_JOURNAL_MAX_REPLAY_RECORDS;
        }
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
    c->refs[c->ref_count].type = rtype;
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
    tp_status read_status = journal_read_all_bounded(&j->io, &buf, &len, err);
    if (read_status != TP_STATUS_OK) {
        return read_status;
    }
    out->bytes_total = len;
    /* Recovery rebuilds both in-memory caches from the store so a reused journal reflects only what the
     * store actually holds: the retained-id index and the write-side metadata cache. Clearing
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

    /* Drive the shared front-to-back frame walk with recovery's
     * accumulator. It registers ids into j->ids, captures META into out->metadata (last-wins), and
     * feeds each good CKPT/TXN record to recover_on_record. `off`/`stop` mark where + why the walk
     * stopped; `last_rev`/`have_any` carry the FINAL recovered revision; a returned OOM is a hard fault. */
    recover_walk_ctx wc;
    memset(&wc, 0, sizeof wc);
    int records = 0;
    size_t frame_count = 0U;
    int64_t last_rev = 0;
    bool have_any = false;
    tp_jrn_stop stop = TP_JRN_STOP_EOF;
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    tp_jrn_meta_ref meta_ref = {0};
    tp_status hard = walk_records(buf, len, &j->ids, &meta_ref, &out->has_metadata, recover_on_record,
                                  &wc, &off, &stop, &records, &frame_count,
                                  &last_rev, &have_any);
    if (hard != TP_STATUS_OK) {
        free(wc.refs);
        free(buf);
        tp_journal_recovery_free(out); /* frees any captured metadata (nothing else set yet) */
        return tp_error_set(err, hard,
                            hard == TP_STATUS_OUT_OF_BOUNDS
                                ? "journal total record limit or replay-window limit exceeded"
                                : "journal recovery ran out of memory");
    }

    out->records_recovered = records;
    out->stop_offset = off;
    out->revision = have_any ? last_rev : 0;
    j->replay_count = wc.ref_count;
    j->record_count = frame_count;

    /* The truncate-vs-preserve decision reduces to one question: is there a
     * CRC-valid record STILL after the boundary? (Scan from off+1 so the corrupt record at
     * off never self-matches.) If so the break is a MID-STREAM corruption -- a bloated length
     * field, a lost sync-word, or a flipped byte -- and truncating would delete the trailing
     * acknowledged records, so preserve the file and poison the journal against appends behind
     * it. If not, the tail is genuinely torn/corrupt and is safe to truncate away. A bloated
     * length field is MID-STREAM corruption, not a clean torn tail, so it is preserved here. */
    bool more_after = (stop != TP_JRN_STOP_EOF) && has_valid_record_after(buf, len, off + 1, NULL);
    out->status = classify_stop(stop, out->records_recovered, more_after);
    if (more_after) {
        out->mid_stream_corrupt = true;
        j->poisoned = true;
    }

    /* Format B materialization: checkpoint and ordered op payloads are borrowed spans into the
     * one bounded raw record buffer. The length-aware project/transaction parsers need no NUL
     * sentinel, so recovery performs no payload malloc/copy. Any descriptor OOM still frees the
     * raw owner and returns a hard fault. */
    if (wc.have_base && wc.base_len > 0) {
        out->snapshot = (const char *)(buf + wc.base_off);
        out->snapshot_len = wc.base_len;
    }
    if (wc.ref_count > 0) {
        tp_journal_recovered_op *ops = (tp_journal_recovered_op *)calloc(wc.ref_count, sizeof *ops);
        if (!ops) {
            free(wc.refs);
            free(buf);
            tp_journal_recovery_free(out);
            return tp_error_set(err, TP_STATUS_OOM, "journal recovery op-list allocation failed (out of memory)");
        }
        for (size_t i = 0; i < wc.ref_count; i++) {
            ops[i].payload = (const char *)(buf + wc.refs[i].off);
            ops[i].payload_len = wc.refs[i].len;
            ops[i].revision = wc.refs[i].rev;
            ops[i].kind = wc.refs[i].type == (uint8_t)TP_JRN_REC_HISTORY
                              ? TP_JOURNAL_REPLAY_HISTORY
                              : TP_JOURNAL_REPLAY_TXN;
        }
        out->ops = ops;
        out->op_count = wc.ref_count;
    }

    if (out->has_metadata && materialize_meta(&meta_ref, &out->metadata) != TP_STATUS_OK) {
        free(wc.refs);
        tp_journal_recovery_free(out);
        free(buf);
        return tp_error_set(err, TP_STATUS_OOM,
                            "journal recovery metadata allocation failed (out of memory)");
    }
    if (out->snapshot || out->op_count > 0U) {
        out->_raw_record_buffer = buf;
        out->_raw_record_buffer_len = len;
        buf = NULL;
    }

    /* Seed the write-side metadata cache for later compaction. Copies are independent
     * from the recovery result. OOM is non-fatal; recovery still succeeds without a cache. */
    if (out->has_metadata) {
        char *cp = jrn_strdup(out->metadata.path ? out->metadata.path : "");
        char *cn = jrn_strdup(out->metadata.name ? out->metadata.name : "");
        if (cp && cn) {
            free(j->meta_path);
            free(j->meta_name);
            j->meta_path = cp;
            j->meta_name = cn;
            j->meta_time = out->metadata.timestamp;
            j->meta_file_fingerprint = out->metadata.file_fingerprint;
            j->meta_has_file_fingerprint = out->metadata.has_file_fingerprint;
            j->has_meta = true;
        } else { /* Recovery stays valid; later compaction cannot re-emit metadata. */
            free(cp);
            free(cn);
        }
    }

    free(wc.refs);
    free(buf);
    return TP_STATUS_OK;
}

/* ---- peek: header + metadata + status, no model reconstruction ----------- */

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

/* Peek accumulator: it only notes that a checkpoint (a recoverable base)
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
    if (!io.ctx || !io.length || !io.read_all) {
        if (io.destroy) {
            io.destroy(io.ctx);
        }
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "invalid journal io for peek");
    }
    uint8_t *buf = NULL;
    size_t len = 0;
    tp_status read_status = journal_read_all_bounded(&io, &buf, &len, err);
    if (io.destroy) {
        io.destroy(io.ctx); /* peek TAKES OWNERSHIP of io; the bytes are now in buf */
    }
    if (read_status != TP_STATUS_OK) {
        free(buf);
        return read_status;
    }

    /* Header: shared classifier (same BAD_MAGIC / VERSION_MISMATCH / EMPTY / torn-header split as
     * recover). peek has NO key to compare -- it REPORTS the header key instead. */
    tp_journal_recovery_status hstatus;
    uint32_t hversion = 0;
    const uint8_t *hkey = NULL;
    if (!header_ok(buf, len, &hstatus, &hversion, &hkey)) {
        out->status = hstatus;
        out->format_version = hversion;
        if (hkey) {
            memcpy(out->key, hkey, sizeof out->key);
        }
        free(buf);
        return TP_STATUS_OK;
    }
    out->format_version = hversion;
    memcpy(out->key, hkey, 16);

    /* Drive the same shared frame walk as recover with an independently-owned bounded
     * retained-id scratch set. Peek publishes accepted ids only into this scratch set,
     * so duplicate/cap/FIFO policy classifies identically without mutating a model. */
    tp_idset peek_ids = {0};
    if (tp_idset_reserve(&peek_ids) != TP_STATUS_OK) {
        free(buf);
        tp_journal_peek_free(out);
        return tp_error_set(err, TP_STATUS_OOM, "journal peek id-set allocation failed");
    }
    peek_walk_ctx pc;
    memset(&pc, 0, sizeof pc);
    int records = 0;
    size_t frame_count = 0U;
    int64_t last_rev = 0;
    bool have_any = false;
    tp_jrn_stop stop = TP_JRN_STOP_EOF;
    size_t off = (size_t)TP_JRN_HEADER_LEN;
    tp_jrn_meta_ref meta_ref = {0};
    tp_status hard = walk_records(buf, len, &peek_ids, &meta_ref, &out->has_meta, peek_on_record, &pc,
                                  &off, &stop, &records, &frame_count,
                                  &last_rev, &have_any);
    tp_idset_dispose(&peek_ids);
    if (hard != TP_STATUS_OK) {
        free(buf);
        tp_journal_peek_free(out);
        return tp_error_set(err, hard,
                            hard == TP_STATUS_OUT_OF_BOUNDS
                                ? "journal total record limit exceeded"
                                : "journal peek ran out of memory");
    }
    if (out->has_meta && materialize_meta(&meta_ref, &out->meta) != TP_STATUS_OK) {
        free(buf);
        out->has_meta = false;
        return tp_error_set(err, TP_STATUS_OOM,
                            "journal peek metadata allocation failed (out of memory)");
    }
    out->has_checkpoint = pc.has_checkpoint;
    out->record_count = records;
    bool more_after = (stop != TP_JRN_STOP_EOF) && has_valid_record_after(buf, len, off + 1, NULL);
    out->status = classify_stop(stop, out->record_count, more_after);
    free(buf);
    return TP_STATUS_OK;
}
