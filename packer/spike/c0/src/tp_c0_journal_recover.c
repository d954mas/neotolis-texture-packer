/* C0-03 task 1: recovery scan + idempotency retention seam. Walks a journal byte
 * buffer from the start, recovering every clean record until EOF or the first
 * torn/corrupt record, and rebuilds the committed-transaction id set so a retried
 * transaction is not applied twice after restart (master spec §7.2, §22.3). */

#include "tp_c0/tp_c0_journal.h"

#include <string.h>

bool tp_c0_journal_recovery_has_txn(const tp_c0_journal_recovery *rec, tp_c0_id128 txn_id) {
    if (!rec) {
        return false;
    }
    for (int i = 0; i < rec->txn_count; i++) {
        if (tp_c0_id128_eq(rec->txns[i], txn_id)) {
            return true;
        }
    }
    return false;
}

tp_c0_detail tp_c0_journal_recover(const uint8_t *buf, size_t len, tp_c0_journal_recovery *out, tp_error *err) {
    if (!out) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal recover: out is NULL");
    }
    memset(out, 0, sizeof *out);
    out->last_revision = -1;
    out->checkpoint_revision = -1;
    out->stop_reason = TP_C0_OK;
    if (len == 0) {
        return TP_C0_OK; /* empty journal is clean, not an error */
    }
    if (!buf) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "journal recover: buf is NULL but len > 0");
    }

    size_t off = 0;
    while (off < len) {
        tp_c0_journal_record rec;
        tp_c0_detail d = tp_c0_journal_decode(buf + off, len - off, &rec, NULL);
        if (d != TP_C0_OK) {
            /* Torn/short FINAL record is the EXPECTED crash case: recover the clean
             * prefix. Bad magic/version/kind/checksum is unexpected corruption; the
             * prefix is still recovered but the data loss is flagged. */
            out->stop_reason = d;
            if (d == TP_C0_ERR_JOURNAL_SHORT) {
                out->truncated_tail = true;
            } else {
                out->corrupt = true;
            }
            return TP_C0_OK;
        }
        if (rec.kind == TP_C0_JREC_TXN) {
            if (out->txn_count >= TP_C0_JOURNAL_MAX_TXNS) {
                /* Retention cap reached -- stop cleanly (a production journal uses a
                 * bounded window, §60 item 1). Records so far stay recovered. */
                out->stop_reason = TP_C0_ERR_BUFFER_TOO_SMALL;
                out->corrupt = true;
                return TP_C0_OK;
            }
            out->txns[out->txn_count] = rec.txn_id;
            out->txn_revisions[out->txn_count] = rec.revision;
            out->txn_count++;
        } else if (rec.kind == TP_C0_JREC_CHECKPOINT) {
            out->checkpoint_revision = rec.revision;
        }
        if (rec.revision > out->last_revision) {
            out->last_revision = rec.revision;
        }
        off += rec.record_len;
        out->valid_bytes = off;
    }
    return TP_C0_OK; /* clean EOF */
}
