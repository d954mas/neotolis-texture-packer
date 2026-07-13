/* tp_bench_journal -- F2-05b-ii-B / D2 dev benchmark (NOT a ctest).
 *
 * Measures the cost the LIVE recovery journal (F2-04) adds to every GUI commit: with a
 * journal attached, tp_model_apply serializes the WHOLE committed project (tp_project_save_
 * buffer) and appends it as one full-SNAPSHOT record (F2-04 v1 payload) before acknowledging.
 * This bench isolates that per-commit append cost -- serialize + frame + durable write -- on a
 * NORMAL and a HUGE tp_project, on both the in-memory io (pure CPU: serialize + frame + memcpy)
 * and the real file io (adds fwrite + fflush). It reports ms/commit, bytes/commit, and the
 * journal growth over an N-edit session. These are exactly the numbers the owner's DEFERRED
 * snapshot-vs-diff decision (D2, ADR 0013/0015) needs. It does NOT change the journal payload.
 *
 * Fixtures reuse tp_bench_clone's build_project/fill_atlas sizing so the numbers are directly
 * comparable to the P-01 arena clone bench.
 *
 * Usage: tp_bench_journal [iters_normal] [iters_huge]
 *
 * Plain exe (no nt_set_warning_flags), like tp_bench_clone -- a dev tool, not CI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tp_core/tp_journal.h"
#include "tp_core/tp_project.h"

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* One atlas with `nsrc` sources, `nspr` sprite overrides (name + rename strdups), and `nanim`
 * animations of `nframe` frames -- identical to tp_bench_clone's fixture. */
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
    return p;
}

/* Time `iters` full-snapshot appends onto a journal over `io` (created fresh here; TAKES
 * OWNERSHIP of io). Each iter re-serializes `p` (the GUI commit path serializes the committed
 * clone every time) and appends it as one TXN record with a unique id. Prints ms/commit +
 * bytes/commit (store-length delta) + total growth. `label` names the io backing. */
static void bench_appends(const char *label, const tp_project *p, tp_journal_io io, int iters) {
    if (!io.ctx) {
        (void)printf("   %-14s (io unavailable)\n", label);
        return;
    }
    tp_id128 key;
    memset(&key, 0, sizeof key);
    tp_journal *j = tp_journal_create(io, key); /* takes ownership of io */
    if (!j) {
        (void)printf("   %-14s (journal create failed)\n", label);
        return;
    }

    /* Prime: serialize once + the initial checkpoint (as tp_model_attach_journal does). */
    char *snap = NULL;
    size_t snap_len = 0;
    tp_error err = {{0}};
    if (tp_project_save_buffer(p, &snap, &snap_len, &err) != TP_STATUS_OK) {
        (void)printf("   %-14s (serialize failed)\n", label);
        tp_journal_destroy(j);
        return;
    }
    (void)tp_journal_init_checkpoint(j, (const uint8_t *)snap, snap_len, 0, &err);
    free(snap);

    int64_t len_before = io.length(io.ctx);
    double t = 0.0;
    size_t last_sl = 0; /* fix [6]: the snapshot size, taken from the loop's own serialize (no extra) */
    for (int i = 0; i < iters; i++) {
        char id[33];
        (void)snprintf(id, sizeof id, "%032llx", (unsigned long long)i); /* unique -> no idempotent skip */
        double a = now_ms();
        char *s = NULL;
        size_t sl = 0;
        if (tp_project_save_buffer(p, &s, &sl, &err) == TP_STATUS_OK) {
            (void)tp_journal_append_txn(j, id, i + 1, (const uint8_t *)s, sl, &err);
            last_sl = sl;
        }
        double b = now_ms();
        free(s);
        t += (b - a);
    }
    int64_t len_after = io.length(io.ctx);
    double ms_commit = t / (double)iters;
    double bytes_commit = (double)(len_after - len_before) / (double)iters;
    (void)printf("   %-14s snapshot=%.3f MB (%zu B)  %8.4f ms/commit   %9.0f bytes/commit   growth %.2f MB over %d edits\n",
                 label, (double)last_sl / 1048576.0, last_sl, ms_commit, bytes_commit,
                 (double)(len_after - len_before) / 1048576.0, iters);
    tp_journal_destroy(j); /* frees j + its owned io (the file io closes/keeps the temp file) */
}

static void bench(const char *label, const tp_project *p, int iters, const char *file_slot) {
    (void)printf("\n== %s ==\n", label);
    (void)printf("   atlases=%d  iters=%d\n", p->atlas_count, iters);
    bench_appends("memory-io", p, tp_journal_io_memory(), iters);
    if (file_slot) {
        (void)remove(file_slot);
        bench_appends("file-io", p, tp_journal_io_file(file_slot), iters);
        (void)remove(file_slot);
    }
}

int main(int argc, char **argv) {
    int iters_normal = (argc > 1) ? atoi(argv[1]) : 2000;
    int iters_huge = (argc > 2) ? atoi(argv[2]) : 50;
    if (iters_normal < 1) {
        iters_normal = 1;
    }
    if (iters_huge < 1) {
        iters_huge = 1;
    }

    /* Same fixtures as tp_bench_clone (comparable to the P-01 arena bench). */
    tp_project *normal = build_project(3, 4, 20, 3, 8);
    tp_project *huge = build_project(100, 2, 1000, 0, 0);
    if (!normal || !huge) {
        (void)fprintf(stderr, "tp_bench_journal: OOM building fixtures\n");
        tp_project_destroy(normal);
        tp_project_destroy(huge);
        return 1;
    }

    bench("NORMAL project", normal, iters_normal, "tp_bench_journal_normal.tmp");
    bench("HUGE project (100 atlas x 1000 sprites)", huge, iters_huge, "tp_bench_journal_huge.tmp");

    tp_project_destroy(normal);
    tp_project_destroy(huge);
    (void)printf("\ntp_bench_journal: OK\n");
    return 0;
}
