/* tp_bench_clone -- P-01 dev benchmark (NOT a ctest).
 *
 * Measures the real cost of the transaction clone: the current malloc/strdup
 * tp_project_clone vs the arena-backed tp_project_clone_into_arena, on a NORMAL and
 * a HUGE tp_project. Reports clone-only and clone+free (the full per-transaction
 * allocator cost: the txn path clones the live model, then frees the old one).
 *
 * Usage: tp_bench_clone [iters_normal] [iters_huge]
 *
 * Plain exe (no nt_set_warning_flags), like tp_demo_export -- a dev tool, not CI.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_project.h"
#include "tp_project_clone_arena.h" /* src/ internal: the arena clone under test */

static double now_ms(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}

/* Build one atlas with `nsrc` sources, `nspr` sprite overrides (2 strdups each:
 * name + rename), and `nanim` animations of `nframe` frames. */
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
            sp->origin_x = 0.25F; /* non-default -> serialized, realistic */
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

/* natlas atlases, each filled as above. atlases[0] already exists (create). */
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

static size_t serialized_len(const tp_project *p) {
    char *buf = NULL;
    size_t len = 0;
    tp_error err = {{0}};
    if (tp_project_save_buffer(p, &buf, &len, &err) != TP_STATUS_OK) {
        return 0;
    }
    free(buf);
    return len;
}

static void bench(const char *label, tp_project *p, int iters) {
    size_t footprint = tp_project_clone_arena_footprint(p);
    size_t ser = serialized_len(p);
    (void)printf("\n== %s ==\n", label);
    (void)printf("   atlases=%d  serialized=%.2f MB  arena footprint=%.2f MB  iters=%d\n", p->atlas_count,
                 (double)ser / 1048576.0, (double)footprint / 1048576.0, iters);

    /* --- malloc clone: clone-only, then free-only (separately timed) --- */
    double t_mc = 0.0;
    double t_mf = 0.0;
    for (int i = 0; i < iters; i++) {
        double a = now_ms();
        tp_project *c = tp_project_clone(p);
        double b = now_ms();
        tp_project_destroy(c);
        double d = now_ms();
        t_mc += (b - a);
        t_mf += (d - b);
    }
    t_mc /= iters;
    t_mf /= iters;

    /* --- arena clone (sized to footprint: one contiguous block) --- */
    double t_ac = 0.0;
    double t_af = 0.0;
    for (int i = 0; i < iters; i++) {
        double a = now_ms();
        tp_arena *ar = tp_arena_create(footprint);
        tp_project *c = tp_project_clone_into_arena(p, ar);
        double b = now_ms();
        (void)c;
        tp_arena_destroy(ar);
        double d = now_ms();
        t_ac += (b - a);
        t_af += (d - b);
    }
    t_ac /= iters;
    t_af /= iters;

    /* --- arena clone (default block, let it grow) --- */
    double t_gc = 0.0;
    for (int i = 0; i < iters; i++) {
        double a = now_ms();
        tp_arena *ar = tp_arena_create(0);
        tp_project *c = tp_project_clone_into_arena(p, ar);
        double b = now_ms();
        (void)c;
        tp_arena_destroy(ar);
        t_gc += (b - a);
    }
    t_gc /= iters;

    (void)printf("   %-26s %8.3f ms clone  +%8.3f ms free  = %8.3f ms\n", "malloc clone", t_mc, t_mf, t_mc + t_mf);
    (void)printf("   %-26s %8.3f ms clone  +%8.3f ms free  = %8.3f ms\n", "arena clone (sized 1 block)", t_ac, t_af,
                 t_ac + t_af);
    (void)printf("   %-26s %8.3f ms clone  (create+grow, incl. free above)\n", "arena clone (default grow)", t_gc);
    if (t_ac + t_af > 0.0) {
        (void)printf("   -> clone+free speedup: %.1fx\n", (t_mc + t_mf) / (t_ac + t_af));
    }
}

int main(int argc, char **argv) {
    int iters_normal = (argc > 1) ? atoi(argv[1]) : 2000;
    int iters_huge = (argc > 2) ? atoi(argv[2]) : 20;
    if (iters_normal < 1) {
        iters_normal = 1;
    }
    if (iters_huge < 1) {
        iters_huge = 1;
    }

    /* NORMAL: a realistic hand-authored project. */
    tp_project *normal = build_project(3, 4, 20, 3, 8);
    /* HUGE: the lead's shape -- 100 atlases x 1000 override sprites (~10 MB). */
    tp_project *huge = build_project(100, 2, 1000, 0, 0);
    if (!normal || !huge) {
        (void)fprintf(stderr, "tp_bench_clone: OOM building fixtures\n");
        return 1;
    }

    bench("NORMAL project", normal, iters_normal);
    bench("HUGE project (100 atlas x 1000 sprites)", huge, iters_huge);

    tp_project_destroy(normal);
    tp_project_destroy(huge);
    (void)printf("\ntp_bench_clone: OK\n");
    return 0;
}
