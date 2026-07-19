#include "tp_core/tp_transaction.h"

#include <stdlib.h>
#include <string.h>

#include "tp_diff_internal.h"
#include "tp_history_codec_internal.h"
#include "tp_idset_internal.h"
#include "tp_journal_internal.h"
#include "tp_op_internal.h"
#include "tp_project_identity_internal.h"
#include "tp_project_internal.h"
#include "tp_txn_internal.h"
/* R2b/R3: serialize a candidate project into a fresh malloc'd snapshot
 * (*snap / *snap_len, caller frees) and PROVE it round-trips through tp_project_load_buffer
 * before it is handed to a checkpoint. The checkpoint is the load-bearing recovery BASE --
 * format B replays post-checkpoint ops ONTO it (see tp_model_recover) -- so a base that
 * serializes but does NOT reload (e.g. nil structural ids) would silently discard the whole
 * recovered session. Enforcing "the checkpoint must round-trip" HERE in core, below any
 * client's own promotion (e.g. the GUI's ensure_ids), means no caller can persist an
 * unrecoverable base. Shared by tp_model_attach_journal (initial checkpoint) and
 * tp_model_compact_journal (Save-window compaction), and explicit history checkpoints
 * so all checkpoint paths inherit the guarantee. On any
 * failure *snap is NULL/0 and a wrapped error is returned; the caller leaves the journal
 * untouched. */
static tp_status project_checkpoint_snapshot(const tp_project *project, size_t expected_len,
                                             char **snap, size_t *snap_len, tp_error *err) {
    *snap = NULL;
    *snap_len = 0;
    tp_status ss = tp_project_checkpoint_save_buffer(project, snap, snap_len,
                                                     err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    if (*snap_len != expected_len) {
        free(*snap);
        *snap = NULL;
        *snap_len = 0;
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "project changed while its checkpoint was serialized");
    }
    tp_project *probe = NULL;
    tp_status vs = tp_project_load_buffer(*snap, *snap_len, &probe, err);
    if (vs != TP_STATUS_OK) {
        free(*snap);
        *snap = NULL;
        *snap_len = 0;
        return tp_error_set(err, vs, "recovery checkpoint does not round-trip "
                                     "(promote structural ids before checkpointing)");
    }
    tp_project_destroy(probe);
    return TP_STATUS_OK;
}

tp_status tp_model_attach_journal(tp_model *m, tp_journal *j, tp_error *err) {
    if (!m || !j) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model or journal");
    }
    if (m->journal) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "a journal is already attached");
    }
    /* Migrate ids already committed in live memory into the fresh journal before
     * its initial checkpoint, so a later recovery can seed the rebuilt model's
     * authoritative in-memory idstore. */
    const tp_idset *pre = tp_txn_idstore_mem_view(m->idstore);
    if (pre) {
        int pre_count = tp_idset_count(pre);
        for (int i = 0; i < pre_count; i++) {
            char id_hex[TP_IDSET_IDLEN + 1];
            if (!tp_idset_format_at(pre, i, id_hex)) {
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "in-memory retained-id ordering is invalid");
            }
            tp_status ms = tp_journal_seed_retained_id(j, id_hex);
            if (ms != TP_STATUS_OK) {
                return tp_error_set(err, ms, "could not migrate retained ids into the journal (out of memory)");
            }
        }
    }
    /* Initial CHECKPOINT of the current committed state so the journal is self-sufficient for
     * recovery (spec §22.3 checkpoint + journal), via the shared round-trip-proving helper. On
     * failure the model is NOT attached and the caller still owns j. */
    size_t measured_len = 0;
    tp_status ss = tp_project_checkpoint_serialized_size_bounded(
        m->project, SIZE_MAX, &measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    ss = tp_journal__check_checkpoint_append_bytes(j, measured_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    char *snap = NULL;
    size_t snap_len = 0;
    ss = project_checkpoint_snapshot(m->project, measured_len,
                                     &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    tp_status cs = tp_journal_init_checkpoint(j, (const uint8_t *)snap, snap_len, m->revision, err);
    free(snap);
    if (cs != TP_STATUS_OK) {
        return cs;
    }
    m->journal = j; /* ownership transferred */
    tp_model__restore_recovery(m);
    return TP_STATUS_OK;
}

tp_status tp_model__append_history_checkpoint(tp_model *m, const tp_project *candidate, int64_t revision,
                                              size_t snapshot_bytes, tp_error *err) {
    if (!m || !candidate) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model or history candidate");
    }
    if (!m->journal) {
        return TP_STATUS_OK;
    }
    tp_status admission = tp_journal__check_checkpoint_append_bytes(
        m->journal, snapshot_bytes, err);
    if (admission != TP_STATUS_OK) {
        return admission;
    }

    /* Compact HISTORY is the normal Undo/Redo record. This full checkpoint is
     * the fallback only for an unsupported future diff shape or an oversized
     * compact transition. It is APPENDED, never compacted: the existing clean
     * checkpoint remains intact until this candidate is durable, and
     * record_count advances past the startup scan's unsaved-work threshold. */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_status ss = project_checkpoint_snapshot(candidate, snapshot_bytes,
                                               &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        return ss;
    }
    tp_status cs = tp_journal_init_checkpoint(m->journal, (const uint8_t *)snap, snap_len, revision, err);
    free(snap);
    return cs;
}

static tp_status compact_journal(tp_model *m, bool preserve_evidence,
                                 tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    if (!m->journal) {
        return TP_STATUS_OK; /* no journal attached (recovery off / journal-less): nothing to compact */
    }
    tp_idset prior_ids = {0};
    bool live_ids_staged = false;
    if (preserve_evidence) {
        const tp_idset *live_ids = tp_txn_idstore_mem_view(m->idstore);
        tp_idset replacement_ids = {0};
        if (!live_ids || tp_idset_reserve(&replacement_ids) != TP_STATUS_OK) {
            tp_idset_dispose(&replacement_ids);
            return tp_error_set(
                err, live_ids ? TP_STATUS_OOM : TP_STATUS_INVALID_ARGUMENT,
                "could not stage the live retained-id window for recovery healing");
        }
        const int live_count = tp_idset_count(live_ids);
        for (int i = 0; i < live_count; ++i) {
            char id_hex[TP_IDSET_IDLEN + 1];
            if (!tp_idset_format_at(live_ids, i, id_hex)) {
                tp_idset_dispose(&replacement_ids);
                return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                    "live retained-id ordering is invalid");
            }
            tp_idset_put_reserved(&replacement_ids, id_hex);
        }
        prior_ids = m->journal->ids;
        m->journal->ids = replacement_ids;
        live_ids_staged = true;
    }
    /* R3 (plan S18 R): compact the recovery journal to one fresh checkpoint == the current committed
     * state -- the Save-window reset. Uses the SAME round-trip-proving snapshot helper as attach, so a
     * compacted checkpoint is guaranteed loadable (it becomes the recovery base). On any snapshot/probe
     * failure the journal is left UNCHANGED (keeps its larger-but-correct checkpoint+ops and still
     * recovers); the compaction primitive itself fails closed on a truncate failure (poison preserved). */
    size_t measured_len = 0;
    tp_status ss = tp_project_checkpoint_serialized_size_bounded(
        m->project, SIZE_MAX, &measured_len, err);
    if (ss != TP_STATUS_OK) {
        if (live_ids_staged) {
            tp_idset_dispose(&m->journal->ids);
            m->journal->ids = prior_ids;
        }
        return ss;
    }
    ss = tp_journal__check_checkpoint_compact_bytes(
        m->journal, measured_len, err);
    if (ss != TP_STATUS_OK) {
        if (live_ids_staged) {
            tp_idset_dispose(&m->journal->ids);
            m->journal->ids = prior_ids;
        }
        return ss;
    }
    char *snap = NULL;
    size_t snap_len = 0;
    ss = project_checkpoint_snapshot(m->project, measured_len,
                                     &snap, &snap_len, err);
    if (ss != TP_STATUS_OK) {
        if (live_ids_staged) {
            tp_idset_dispose(&m->journal->ids);
            m->journal->ids = prior_ids;
        }
        return ss; /* snapshot/probe failed BEFORE any truncate: journal untouched, keeps recovering */
    }

    /* Save healing must never trade an older recoverable prefix for an empty
     * or partial replacement. Preserve the exact byte store and journal state
     * around the destructive compact primitive; a one-shot replacement fault
     * can then be rolled back without weakening the low-level fail-closed API. */
    uint8_t *evidence = NULL;
    size_t evidence_len = 0U;
    if (preserve_evidence) {
        if (!m->journal->io.read_all ||
            m->journal->io.read_all(m->journal->io.ctx,
                                    (size_t)TP_JOURNAL_MAX_FILE_BYTES,
                                    &evidence, &evidence_len) != 0) {
            free(snap);
            tp_idset_dispose(&m->journal->ids);
            m->journal->ids = prior_ids;
            return tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                                "could not preserve recovery evidence before compaction");
        }
    }
    const bool was_poisoned = m->journal->poisoned;
    const size_t old_record_count = m->journal->record_count;
    const size_t old_replay_count = m->journal->replay_count;
    const size_t old_replay_operations = m->journal->replay_operations;
    tp_status cs = tp_journal_compact(m->journal, (const uint8_t *)snap, snap_len, m->revision, err);
    free(snap);
    if (preserve_evidence && cs != TP_STATUS_OK &&
        (m->journal->record_count != old_record_count ||
         m->journal->poisoned != was_poisoned)) {
        bool restored = m->journal->io.truncate && m->journal->io.write &&
                        m->journal->io.sync &&
                        m->journal->io.truncate(m->journal->io.ctx, 0U) == 0;
        if (restored && evidence_len > 0U) {
            restored = m->journal->io.write(m->journal->io.ctx, evidence,
                                             evidence_len) ==
                       (int64_t)evidence_len;
        }
        if (restored) {
            restored = m->journal->io.sync(m->journal->io.ctx) == 0;
        }
        if (restored) {
            m->journal->poisoned = was_poisoned;
            m->journal->record_count = old_record_count;
            m->journal->replay_count = old_replay_count;
            m->journal->replay_operations = old_replay_operations;
        } else {
            m->journal->poisoned = true;
            cs = tp_error_set(err, TP_STATUS_JOURNAL_FAILED,
                              "recovery compaction failed and old evidence could not be restored");
        }
    }
    free(evidence);
    if (live_ids_staged) {
        if (cs == TP_STATUS_OK) {
            tp_idset_dispose(&prior_ids);
        } else {
            tp_idset_dispose(&m->journal->ids);
            m->journal->ids = prior_ids;
        }
    }
    /* Never auto-detach on a poisoned replacement failure. The older recovery
     * prefix remains available for diagnosis; live idempotency and edits do not
     * depend on the poisoned journal. */
    if (cs == TP_STATUS_OK) {
        tp_model__restore_recovery(m);
    }
    return cs;
}

tp_status tp_model_compact_journal(tp_model *m, tp_error *err) {
    return compact_journal(m, false, err);
}

tp_status tp_model__heal_journal(tp_model *m, tp_error *err) {
    return compact_journal(m, true, err);
}

tp_status tp_model_set_recovery_metadata(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                         tp_error *err) {
    return tp_model_set_recovery_metadata_ex(m, timestamp, path, name, NULL, err);
}

tp_status tp_model_set_recovery_metadata_ex(tp_model *m, int64_t timestamp, const char *path, const char *name,
                                            const tp_id128 *file_fingerprint, tp_error *err) {
    if (!m) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "null model");
    }
    if (!m->journal) {
        return TP_STATUS_OK; /* no journal attached (recovery off / journal-less): nothing to record */
    }
    /* Forward the canonical project identity used by recovery Save Original. Same null-journal-tolerant
     * contract as tp_model_compact_journal; NULL path/name are normalized to "". Every durable write
     * failure is returned so the host can detach/remove stale recovery authority. */
    return tp_journal_set_metadata_ex(m->journal, timestamp, path ? path : "", name ? name : "",
                                      file_fingerprint, err);
}

bool tp_model_has_journal(const tp_model *m) {
    /* m->journal is set ONLY after tp_journal_init_checkpoint durably wrote (see
     * tp_model_attach_journal), so a non-NULL journal proves a durable recovery
     * baseline exists. It does not promise the latest live revision is recorded
     * after recovery degradation. */
    return m && m->journal != NULL;
}

void tp_model_detach_journal(tp_model *m) {
    if (!m || !m->journal) {
        return;
    }
    tp_journal_destroy(m->journal);
    m->journal = NULL;
    tp_model__restore_recovery(m);
}

tp_status tp_model_recover(tp_journal_io io, tp_id128 key, tp_model **out, tp_journal_recovery *info, tp_error *err) {
    if (out) {
        *out = NULL;
    }
    if (info) {
        memset(info, 0, sizeof *info); /* always in a freeable state, even on an early hard fault */
    }
    tp_journal *j = tp_journal_create(io, key);
    if (!j) {
        return tp_error_set(err, TP_STATUS_OOM, "could not create journal for recovery");
    }
    tp_journal_recovery rec;
    tp_status rc = tp_journal_recover(j, &rec, err);
    if (rc != TP_STATUS_OK) {
        tp_journal_destroy(j);
        return rc;
    }
    /* Clean a torn/incomplete TAIL so continued appends stay recoverable (never guess
     * past the last good record). C2: a MID-STREAM corruption (a bad record with valid
     * records STILL after it) is NOT truncated -- that would physically delete those
     * trailing durable records. Recover up to the last good record and PRESERVE the
     * file; tp_journal_recover has already poisoned the journal against appends behind
     * the corruption. A torn tail, or a single trailing corrupt record, is safe to drop.
     * C3: if the tail-clean truncate itself fails, poison the journal -- a still-present
     * bad record must never hide a later durable append. */
    bool clean_tail = (rec.status == TP_JOURNAL_RECOVERY_TRUNCATED) ||
                      (rec.status == TP_JOURNAL_RECOVERY_CORRUPT && !rec.mid_stream_corrupt);
    if (clean_tail) {
        if (io.truncate(io.ctx, rec.stop_offset) != 0) {
            tp_journal__poison(j);
        }
    }
    /* Preflight EVERY physical replay record before materializing the project.
     * This is deliberately separate from apply: a malformed compact history
     * transition cannot publish the prefix that happened to precede it. */
    size_t *operation_counts = rec.op_count > 0U
                                   ? (size_t *)calloc(rec.op_count,
                                                     sizeof *operation_counts)
                                : NULL;
    if (rec.op_count > 0U && !operation_counts) {
        if (info) {
            *info = rec;
        } else {
            tp_journal_recovery_free(&rec);
        }
        tp_journal_destroy(j);
        return tp_error_set(err, TP_STATUS_OOM,
                            "journal replay count index allocation failed");
    }
    size_t replay_operations = 0U;
    for (size_t k = 0U; k < rec.op_count; ++k) {
        size_t operation_count = 0U;
        tp_status count_st = TP_STATUS_INVALID_ARGUMENT;
        if (rec.ops[k].kind == TP_JOURNAL_REPLAY_TXN) {
            int txn_operation_count = 0;
            count_st = tp_txn__count_operations_json_n(
                rec.ops[k].payload, rec.ops[k].payload_len,
                &txn_operation_count, err);
            if (count_st == TP_STATUS_OK && txn_operation_count >= 0) {
                operation_count = (size_t)txn_operation_count;
            } else if (count_st == TP_STATUS_OK) {
                count_st = TP_STATUS_INVALID_ARGUMENT;
            }
        } else if (rec.ops[k].kind == TP_JOURNAL_REPLAY_HISTORY) {
            uint32_t history_operation_count = 0U;
            count_st = tp_history_transition_validate(
                (const uint8_t *)rec.ops[k].payload, rec.ops[k].payload_len,
                &history_operation_count, err);
            operation_count = (size_t)history_operation_count;
        }
        if (count_st != TP_STATUS_OK ||
            operation_count >
                (size_t)TP_JOURNAL_MAX_REPLAY_OPERATIONS - replay_operations) {
            const tp_status reject_st = count_st != TP_STATUS_OK
                                            ? count_st
                                            : TP_STATUS_OUT_OF_BOUNDS;
            if (count_st == TP_STATUS_OK) {
                (void)tp_error_set(err, reject_st,
                                   "journal replay operation limit exceeded before materialization");
            }
            if (info) {
                *info = rec;
            } else {
                tp_journal_recovery_free(&rec);
            }
            free(operation_counts);
            tp_journal_destroy(j);
            return reject_st;
        }
        replay_operations += (size_t)operation_count;
        operation_counts[k] = operation_count;
    }
    tp_status seed_st = tp_journal__set_replay_operations(j, replay_operations, err);
    if (seed_st != TP_STATUS_OK) {
        if (info) {
            *info = rec;
        } else {
            tp_journal_recovery_free(&rec);
        }
        free(operation_counts);
        tp_journal_destroy(j);
        return seed_st;
    }
    bool keep_info = (info != NULL);
    if (keep_info) {
        *info = rec; /* transfer the snapshot ownership to the caller */
    }

    tp_status ret = TP_STATUS_OK;
    size_t applied_operations = 0U;
    bool j_consumed = false; /* set once a model takes ownership of j */
    if (rec.records_recovered > 0 && rec.snapshot && rec.snapshot_len > 0) {
        /* Format B: load the last CHECKPOINT snapshot as the base, then REPLAY the post-checkpoint
         * TXN op-payloads onto it in commit order -- the SAME tp_operation_apply, in the SAME order,
         * as the commit path (tp_txn__commit_validated), so the replayed project reaches exactly the
         * committed state. A replay-time decode/apply failure is a HARD fault (the op was already
         * durably recorded after commit): surface it as a non-OK recover, never silently skip an op. */
        tp_project *p = NULL;
        tp_status ls = tp_project_load_buffer(rec.snapshot, rec.snapshot_len, &p, err); /* base checkpoint */
        if (ls != TP_STATUS_OK) {
            ret = ls; /* durable checkpoint snapshot did not load (real corruption despite a valid crc) */
        } else {
            for (size_t k = 0; ls == TP_STATUS_OK && k < rec.op_count; k++) {
                if (rec.ops[k].kind == TP_JOURNAL_REPLAY_HISTORY) {
                    uint32_t history_operation_count = 0U;
                    /* The full replay stream was structurally preflighted
                     * before `p` was built. `p` is caller-owned and disposable:
                     * a stale semantic reference may mutate its prefix, but the
                     * failure path destroys the entire candidate before any
                     * model is published. Avoid a full clone per HISTORY frame. */
                    ls = tp_history_transition_apply_disposable(
                        p, (const uint8_t *)rec.ops[k].payload,
                        rec.ops[k].payload_len, &history_operation_count, err);
                    if (ls == TP_STATUS_OK &&
                        (size_t)history_operation_count != operation_counts[k]) {
                        ls = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                          "history replay count changed after preflight");
                    }
                    if (ls == TP_STATUS_OK) {
                        applied_operations += (size_t)history_operation_count;
                    }
                    continue;
                }
                if (rec.ops[k].kind != TP_JOURNAL_REPLAY_TXN) {
                    ls = tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                                      "unknown journal replay record kind");
                    break;
                }
                tp_txn_request *req = NULL;
                tp_status ds = tp_txn__decode_prechecked_json_n(
                    rec.ops[k].payload, rec.ops[k].payload_len,
                    (int)operation_counts[k], &req, err);
                if (ds != TP_STATUS_OK) {
                    ls = ds; /* a durable op-payload did not decode */
                    break;
                }
                for (int i = 0; i < req->op_count; i++) {
                    tp_op_reject rej;
                    memset(&rej, 0, sizeof rej);
                    tp_status as = tp_operation_apply(p, &req->ops[i], &rej); /* identical to the commit apply */
                    if (as != TP_STATUS_OK) {
                        ls = tp_error_set(err, as, "recovery replay of transaction %zu op %d rejected: %s", k, i,
                                          rej.message);
                        break;
                    }
                    applied_operations++;
                }
                tp_txn_request_free(req);
            }
            /* Individual replay operations establish only their local
             * preconditions. Validate the complete candidate exactly once
             * before publication so a CRC-valid TXN/HISTORY stream cannot
             * leave a noncanonical graph (for example an unnormalized key or
             * a dangling reference) in a recovered live model. */
            if (ls == TP_STATUS_OK) {
                ls = tp_project_validate_canonical(p, err);
            }
            if (ls != TP_STATUS_OK) {
                tp_project_destroy(p);
                ret = ls;
            } else {
                tp_model *rm = tp_model_wrap(p);
                if (!rm) {
                    tp_project_destroy(p);
                    ret = tp_error_set(err, TP_STATUS_OOM, "could not wrap the recovered project");
                } else {
                    rm->revision = rec.revision; /* the FINAL recovered revision (last record's) */
                    rm->saved_identity = tp_semantic_identity(p);
                    rm->recovered_unsaved = true; /* C5: recovered state is ahead of the project file -> DIRTY */
                    tp_status retained_status = TP_STATUS_OK;
                    const int retained_count = tp_idset_count(&j->ids);
                    for (int i = 0; i < retained_count; ++i) {
                        char id_hex[TP_IDSET_IDLEN + 1];
                        if (!tp_idset_format_at(&j->ids, i, id_hex)) {
                            retained_status = TP_STATUS_INVALID_ARGUMENT;
                            break;
                        }
                        retained_status = rm->idstore->record(
                            rm->idstore->ctx, id_hex, err);
                        if (retained_status != TP_STATUS_OK) {
                            break;
                        }
                    }
                    if (retained_status != TP_STATUS_OK) {
                        tp_model_destroy(rm);
                        ret = tp_error_set(
                            err, retained_status,
                            "could not seed live retained transaction ids");
                    } else {
                        rm->journal = j; /* owns j; both indexes now agree */
                        j_consumed = true;
                        if (out) {
                            *out = rm;
                        } else {
                            tp_model_destroy(rm); /* also destroys j */
                        }
                    }
                }
            }
        }
    }
    /* Nothing rebuilt (empty / bad-header / stale-key / torn-first / load-fail): destroy
     * j here so the caller falls back to loading the project file. */
    if (!j_consumed) {
        tp_journal_destroy(j);
    }
    if (!keep_info) {
        tp_journal_recovery_free(&rec);
    }
    free(operation_counts);
    tp_op__test_apply_count_publish(applied_operations);
    return ret;
}
