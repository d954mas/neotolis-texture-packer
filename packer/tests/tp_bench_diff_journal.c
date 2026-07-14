/* tp_bench_diff_journal -- SPIKE bench for the diff-based recovery journal (F2-04 v2, plan
 * S18 "R"). THROWAWAY dev tool, NOT a ctest. Answers the owner's numbers for the
 * "snapshot-per-commit -> diff-per-commit + checkpoints" redesign:
 *
 *   (1) full-SNAPSHOT bytes + serialize(checkpoint) cost  -- the v1 per-commit cost + the v2 checkpoint cost.
 *   (2) diff RECORD bytes per commit, two candidate formats:
 *         A. semantic-diff forward payload (state-capture; tp_diff_record) -- NEEDS a new serializer;
 *            here we SIZE the record's forward fields with a throwaway estimator.
 *         B. serialized OPERATION (tp_txn_request_encode -- EXISTING byte-stable encoder), the real bytes.
 *   (3) per-diff durable APPEND cost through the REAL journal framing/CRC/write (memory-io + file-io),
 *       feeding a ~diff-sized payload -- the v2 analogue of tp_bench_journal's v1 snapshot append.
 *   (4) RECOVERY-REPLAY time vs N (100/1000/10000 diffs since the last checkpoint), IN-PLACE, single
 *       mutable project, no per-step clone -- measured TWO ways:
 *         A. tp_diff_record_apply(reverse=false)  -- the F2-03 forward diff-apply (the Design-A inner loop).
 *         B. tp_operation_apply(project,...)       -- forward op re-apply (the Design-B inner loop).
 *       This is the "2 hours of work -> how long to recover?" answer.
 *
 * Fixtures mirror tp_bench_clone / tp_bench_journal sizing so the numbers are directly comparable.
 * Representative replay ops are idempotent SET shapes (re-appliable N times without divergence):
 *   - atlas.settings.set on the LAST atlas  (worst-case atlas linear scan; alloc-free apply)
 *   - atlas.rename       on the LAST atlas  (atlas scan + one string dup/free per apply)
 * Both exercise the recovery inner loop faithfully; the lookup cost is project-size dependent, which
 * is exactly why every measurement runs on NORMAL and HUGE.
 *
 * Usage: tp_bench_diff_journal [append_iters]
 * Plain exe (no nt_set_warning_flags), like tp_bench_clone -- a dev tool, not CI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tp_core/tp_diff.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_journal.h"
#include "tp_core/tp_operation.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_project_migrate.h"
#include "tp_core/tp_transaction.h"
#include "tp_diff_internal.h" /* src/ internal: tp_diff_record/op structs + tp_diff_record_apply + tp_history_undo_record */

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Deterministic RNG fill (like test_diff's det_fill) so promote_ids is reproducible. */
static int det_fill(void *ctx, uint8_t *out, size_t len) {
    uint8_t *ctr = (uint8_t *)ctx;
    for (size_t j = 0; j < len; j++) {
        out[j] = (uint8_t)(*ctr + (uint8_t)j + 1U);
    }
    (*ctr)++;
    return (int)len;
}

/* ---- fixtures (identical sizing to tp_bench_clone / tp_bench_journal) ------ */
static void fill_atlas(tp_project_atlas *a, int nsrc, int nspr, int nanim, int nframe) {
    char buf[64];
    char buf2[64];
    for (int s = 0; s < nsrc; s++) {
        (void)snprintf(buf, sizeof buf, "art/folder_%03d", s);
        (void)tp_project_atlas_add_source(a, buf);
    }
    for (int i = 0; i < nspr; i++) {
        (void)snprintf(buf, sizeof buf, "sprites/hero_walk_%05d", i);
        tp_project_sprite *sp = NULL;
        if (tp_project_atlas_add_sprite(a, buf, &sp) == TP_STATUS_OK && sp) {
            sp->origin_x = 0.25F;
            sp->slice9_lrtb[0] = 4;
            (void)snprintf(buf2, sizeof buf2, "player_walk_%05d", i);
            (void)tp_project_atlas_set_sprite_rename(a, buf, buf2);
        }
    }
    for (int n = 0; n < nanim; n++) {
        (void)snprintf(buf, sizeof buf, "anim_%03d", n);
        tp_project_anim *an = NULL;
        if (tp_project_atlas_add_animation(a, buf, &an) == TP_STATUS_OK && an) {
            an->fps = 24.0F;
            for (int f = 0; f < nframe; f++) {
                (void)snprintf(buf2, sizeof buf2, "sprites/hero_walk_%05d", f);
                (void)tp_project_anim_add_frame(an, buf2);
            }
        }
    }
    (void)tp_project_atlas_add_target(a, "json-neotolis", "out/atlas", NULL);
}

static tp_project *build_project(int natlas, int nsrc, int nspr, int nanim, int nframe) {
    tp_project *p = tp_project_create();
    if (!p) {
        return NULL;
    }
    fill_atlas(&p->atlases[0], nsrc, nspr, nanim, nframe);
    for (int a = 1; a < natlas; a++) {
        int idx = -1;
        char nm[32];
        (void)snprintf(nm, sizeof nm, "atlas_%03d", a);
        if (tp_project_add_atlas(p, nm, &idx) != TP_STATUS_OK) {
            break;
        }
        fill_atlas(&p->atlases[idx], nsrc, nspr, nanim, nframe);
    }
    /* Promote structural ids so ops can address the atlas by a real (non-nil) id. */
    uint8_t ctr = 1;
    tp_rng rng = {det_fill, &ctr};
    tp_error err = {{0}};
    (void)tp_project_promote_ids(p, &rng, &err);
    return p;
}

/* ---- throwaway forward-diff byte estimator (Design A record size) ---------- *
 * Sizes the FORWARD (recovery) projection of one op-diff: the after-state fields only (recovery
 * never needs the before/undo half). Mirrors a plausible compact binary payload:
 *   shape u8 | atlas_id[16] | (anim_id[16]|entity_id[16] as the shape needs) | after-fields.
 * Strings are u32 len + bytes. This is an ESTIMATE for the format that does not yet exist. */
static size_t fwd_diff_payload_bytes(const tp_diff_op *e) {
    size_t n = 1 /*shape*/ + 16 /*atlas_id*/;
    switch (e->shape) {
        case TP_DIFF_SHAPE_ATLAS_KNOBS:
            n += 7 * 4 /*ints*/ + 2 /*bools*/ + 4 /*float*/; /* full 10-knob after-state */
            break;
        case TP_DIFF_SHAPE_ATLAS_NAME:
            n += 4 + (e->name_after ? strlen(e->name_after) : 0);
            break;
        default:
            n += 64; /* other shapes: coarse placeholder (not used by this bench's replay ops) */
            break;
    }
    return n;
}
/* Full framed record = payload + F2-04 framing (len u32 | type u8 | tx_id[16] | rev i64 | crc u32). */
static size_t framed_record_bytes(size_t payload) { return payload + 4 + 1 + 16 + 8 + 4; }

/* ---- build a one-op request + capture its real semantic diff --------------- *
 * Wraps a fresh clone of `base` in a history-enabled model and applies `op` (rev 0), so the
 * committed transaction captures a real tp_diff_record. Returns the model (KEEP ALIVE: the returned
 * *rec_out points into its history) and, via out-params, the captured record + the request JSON size
 * (Design-B record bytes) + a copy of the op for Design-B replay. */
static tp_model *capture_diff(const tp_project *base, const tp_operation *op, tp_diff_record **rec_out,
                              size_t *json_bytes_out) {
    tp_model *m = tp_model_wrap(tp_project_clone(base));
    if (!m) {
        return NULL;
    }
    (void)tp_model_enable_history(m);

    tp_txn_request req = {0};
    req.schema = TP_TXN_SCHEMA;
    (void)snprintf(req.id_hex, sizeof req.id_hex, "%032x", 1u);
    req.expected_revision = 0;
    req.ops = (tp_operation *)op; /* borrowed: not freed by us */
    req.op_count = 1;

    char *json = tp_txn_request_encode(&req);
    *json_bytes_out = json ? strlen(json) : 0;
    free(json);

    tp_error err = {{0}};
    tp_txn_result res;
    if (tp_model_apply(m, &req, &res, &err) != TP_STATUS_OK || !res.committed) {
        (void)fprintf(stderr, "  capture_diff: apply failed\n");
        tp_txn_result_free(&res);
        tp_model_destroy(m);
        return NULL;
    }
    tp_txn_result_free(&res);
    *rec_out = tp_history_undo_record(m->history);
    return m;
}

/* Time N in-place forward replays via the SEMANTIC DIFF (Design A) onto ONE mutable checkpoint clone;
 * sample cumulative elapsed at 100/1000/10000. Returns nothing; prints a row. */
static void replay_diff(const char *op_label, const tp_project *base, const tp_diff_record *rec) {
    const int Ns[3] = {100, 1000, 10000};
    tp_project *chk = tp_project_clone(base); /* the "last checkpoint" we replay onto */
    if (!chk) {
        return;
    }
    tp_error err = {{0}};
    double t100 = 0, t1k = 0, t10k = 0;
    double t0 = now_ms();
    for (int i = 1; i <= 10000; i++) {
        (void)tp_diff_record_apply(chk, rec, /*reverse=*/false, &err);
        if (i == Ns[0]) {
            t100 = now_ms() - t0;
        } else if (i == Ns[1]) {
            t1k = now_ms() - t0;
        } else if (i == Ns[2]) {
            t10k = now_ms() - t0;
        }
    }
    (void)printf("   %-22s A diff-apply    %8.3f ms /100   %8.3f ms /1000   %8.3f ms /10000   (%.3f us/apply)\n",
                 op_label, t100, t1k, t10k, t10k * 1000.0 / 10000.0);
    tp_project_destroy(chk);
}

/* Time N in-place forward replays via tp_operation_apply (Design B) onto ONE mutable checkpoint clone. */
static void replay_op(const char *op_label, const tp_project *base, const tp_operation *op) {
    const int Ns[3] = {100, 1000, 10000};
    tp_project *chk = tp_project_clone(base);
    if (!chk) {
        return;
    }
    tp_op_reject rej;
    double t100 = 0, t1k = 0, t10k = 0;
    double t0 = now_ms();
    for (int i = 1; i <= 10000; i++) {
        (void)tp_operation_apply(chk, op, &rej);
        if (i == Ns[0]) {
            t100 = now_ms() - t0;
        } else if (i == Ns[1]) {
            t1k = now_ms() - t0;
        } else if (i == Ns[2]) {
            t10k = now_ms() - t0;
        }
    }
    (void)printf("   %-22s B op-apply      %8.3f ms /100   %8.3f ms /1000   %8.3f ms /10000   (%.3f us/apply)\n",
                 op_label, t100, t1k, t10k, t10k * 1000.0 / 10000.0);
    tp_project_destroy(chk);
}

/* Real per-diff durable append through the journal framing/CRC/write, feeding a `payload_len`-byte
 * payload (the v2 diff record), after an initial full-snapshot checkpoint. Prints ms/append + bytes. */
static void bench_diff_append(const char *label, const tp_project *p, tp_journal_io io, size_t payload_len,
                              int iters) {
    if (!io.ctx) {
        (void)printf("   %-14s (io unavailable)\n", label);
        return;
    }
    tp_id128 key;
    memset(&key, 0, sizeof key);
    tp_journal *j = tp_journal_create(io, key);
    if (!j) {
        (void)printf("   %-14s (journal create failed)\n", label);
        return;
    }
    char *snap = NULL;
    size_t snap_len = 0;
    tp_error err = {{0}};
    if (tp_project_save_buffer(p, &snap, &snap_len, &err) != TP_STATUS_OK) {
        tp_journal_destroy(j);
        return;
    }
    (void)tp_journal_init_checkpoint(j, (const uint8_t *)snap, snap_len, 0, &err);
    free(snap);

    uint8_t *payload = (uint8_t *)malloc(payload_len ? payload_len : 1);
    memset(payload, 0xAB, payload_len);
    int64_t len_before = io.length(io.ctx);
    double t = 0.0;
    for (int i = 0; i < iters; i++) {
        char id[33];
        (void)snprintf(id, sizeof id, "%032llx", (unsigned long long)i);
        double a = now_ms();
        (void)tp_journal_append_txn(j, id, i + 1, payload, payload_len, &err);
        t += now_ms() - a;
    }
    int64_t len_after = io.length(io.ctx);
    (void)printf("   %-14s payload=%zu B   %9.6f ms/append   %6.0f bytes/append   growth %.3f MB over %d\n", label,
                 payload_len, t / (double)iters, (double)(len_after - len_before) / (double)iters,
                 (double)(len_after - len_before) / 1048576.0, iters);
    free(payload);
    tp_journal_destroy(j);
}

/* ---- one fixture: snapshot cost, both diff formats, append, replay-vs-N ---- */
static void run_fixture(const char *title, const tp_project *p, int append_iters, const char *file_slot) {
    (void)printf("\n================ %s (atlases=%d) ================\n", title, p->atlas_count);

    /* (1) full snapshot: bytes + serialize(checkpoint) cost. */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_error err = {{0}};
    double ser_t = 0.0;
    int ser_iters = (p->atlas_count > 10) ? 20 : 500;
    for (int i = 0; i < ser_iters; i++) {
        double a = now_ms();
        char *s = NULL;
        size_t sl = 0;
        if (tp_project_save_buffer(p, &s, &sl, &err) == TP_STATUS_OK) {
            snap_len = sl;
            double b = now_ms();
            ser_t += (b - a);
            free(s);
        }
    }
    (void)printf("  full snapshot (v1 record + v2 checkpoint):  %.3f MB (%zu B)   serialize %.3f ms\n",
                 (double)snap_len / 1048576.0, snap_len, ser_t / (double)ser_iters);
    (void)snap;

    /* the LAST atlas -- worst-case linear scan for find_atlas_by_id on HUGE. */
    tp_id128 last_atlas = p->atlases[p->atlas_count - 1].id;

    /* ---- op 1: atlas.settings.set (alloc-free SET) ---- */
    {
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_SETTINGS_SET;
        op.atlas_id = last_atlas;
        op.u.atlas_settings.mask = TP_AF_MAX_SIZE | TP_AF_PADDING | TP_AF_SHAPE | TP_AF_POWER_OF_TWO;
        op.u.atlas_settings.max_size = 2048;
        op.u.atlas_settings.padding = 7;
        op.u.atlas_settings.shape = 2;
        op.u.atlas_settings.power_of_two = true;

        tp_diff_record *rec = NULL;
        size_t json_bytes = 0;
        tp_model *m = capture_diff(p, &op, &rec, &json_bytes);
        if (m && rec && rec->op_count == 1) {
            size_t fwd = fwd_diff_payload_bytes(&rec->ops[0]);
            (void)printf("  atlas.settings.set : diffA(bin,est)=%zu B framed=%zu B | opB(json)=%zu B  "
                         "(vs snapshot %zu B)\n",
                         fwd, framed_record_bytes(fwd), json_bytes, snap_len);
            replay_diff("atlas.settings.set", p, rec);
            replay_op("atlas.settings.set", p, &op);
        }
        tp_model_destroy(m);
    }

    /* ---- op 2: atlas.rename (SET with one string dup/free per apply) ---- */
    {
        tp_operation op;
        memset(&op, 0, sizeof op);
        op.kind = TP_OP_ATLAS_RENAME;
        op.atlas_id = last_atlas;
        op.u.atlas_rename.name = (char *)"renamed_bench_atlas";

        tp_diff_record *rec = NULL;
        size_t json_bytes = 0;
        tp_model *m = capture_diff(p, &op, &rec, &json_bytes);
        if (m && rec && rec->op_count == 1) {
            size_t fwd = fwd_diff_payload_bytes(&rec->ops[0]);
            (void)printf("  atlas.rename       : diffA(bin,est)=%zu B framed=%zu B | opB(json)=%zu B  "
                         "(vs snapshot %zu B)\n",
                         fwd, framed_record_bytes(fwd), json_bytes, snap_len);
            replay_diff("atlas.rename", p, rec);
            replay_op("atlas.rename", p, &op);
        }
        tp_model_destroy(m);
    }

    /* (3) per-diff durable append (real framing/CRC/write) with a ~120 B diff payload. */
    (void)printf("  per-diff durable append (120 B payload, after full-snapshot checkpoint):\n");
    bench_diff_append("memory-io", p, tp_journal_io_memory(), 120, append_iters);
    if (file_slot) {
        (void)remove(file_slot);
        bench_diff_append("file-io", p, tp_journal_io_file(file_slot), 120, append_iters);
        (void)remove(file_slot);
    }
}

int main(int argc, char **argv) {
    int append_iters = (argc > 1) ? atoi(argv[1]) : 5000;
    if (append_iters < 1) {
        append_iters = 1;
    }

    tp_project *normal = build_project(3, 4, 20, 3, 8);
    tp_project *huge = build_project(100, 2, 1000, 0, 0);
    if (!normal || !huge) {
        (void)fprintf(stderr, "tp_bench_diff_journal: OOM building fixtures\n");
        tp_project_destroy(normal);
        tp_project_destroy(huge);
        return 1;
    }

    (void)printf("tp_bench_diff_journal -- diff-journal (F2-04 v2) design spike\n");
    (void)printf("  A = replay via tp_diff_record_apply(reverse=false)  [semantic diff, needs new serializer]\n");
    (void)printf("  B = replay via tp_operation_apply(project,...)       [serialized op, reuses tp_txn encoder]\n");

    run_fixture("NORMAL", normal, append_iters, "tp_bench_diffj_normal.tmp");
    run_fixture("HUGE (100 atlas x 1000 sprites)", huge, append_iters, "tp_bench_diffj_huge.tmp");

    tp_project_destroy(normal);
    tp_project_destroy(huge);
    (void)printf("\ntp_bench_diff_journal: OK\n");
    return 0;
}
