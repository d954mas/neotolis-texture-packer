/* ntpacker-gui dev seam: the `--bench-perf` headless perf-probe mode (compiled into every build; see
 * gui_bench.h). It opens the owner-scale bench fixture, times the EXISTING GUI interactions on the
 * main thread, asserts the two non-blocking hard-gates D relies on, measures per-frame render time
 * when a real GL context is present, prints machine-readable `bench_perf ...` lines (echoed to stdout
 * and, if requested, to a file), then quits. Mirrors the --shot flag/lifecycle shape (gui_shot.c).
 *
 * The tiny percentile/sample helper below is copied inline from packer/tests/tp_bench_support.h ON
 * PURPOSE -- the GUI must not take a packer/tests build dependency, so it stays apps/gui-local. */

#include "gui_bench.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "time/nt_time.h" /* nt_time_now (monotonic seconds) */

#include "app/nt_app.h" /* nt_app_quit */
#include "log/nt_log.h" /* nt_log_info / nt_log_error (human status markers only) */

#include "nt_utf8_fs.h" /* nt_utf8_fopen (UTF-8 output path on Windows) */

#include "tp_core/tp_identity.h" /* TP_IDENTITY_PATH_MAX */
#include "tp_core/tp_session.h"  /* snapshot atlas/revision accessors */
#include "tp_core/tp_utf8.h"     /* tp_utf8_is_valid_c_string (validate --bench-perf=path) */

#include "gui_actions.h" /* do_pack_blocking */
#include "gui_pack.h"    /* gui_pack_result / gui_pack_async_start / gui_pack_worker_active + GUI_PACK_ASYNC_* */
#include "gui_project.h" /* gui_project_* (snapshot / edit / undo / redo / refresh / dirty) */
#include "gui_rows.h"    /* build_rows / select_row_for_region */
#include "gui_state.h"   /* s_sel_atlas + GUI_PRINTF (printf-format attribute) */

/* Default owner-scale fixture (36 atlases / 5480 sprites). Relative to the CWD, like a CLI project
 * arg; the verify commands run from the repo root, where this resolves. */
#define GUI_BENCH_DEFAULT_PROJECT "examples/projects/bench-owner-scale.ntpacker_project"

/* Tuning. N repeats per action probe (<= GUI_BENCH_MAX_SAMPLES); frame timing collects
 * GUI_BENCH_FRAME_TARGET samples after a short GL warmup. */
#define GUI_BENCH_REPEATS 20
#define GUI_BENCH_WARMUP_FRAMES 12
#define GUI_BENCH_FRAME_WARMUP 5
#define GUI_BENCH_FRAME_TARGET ((size_t)120U)

/* --- inline copy of the tp_bench_support.h sample helper (see file banner) --------------------- */
#define GUI_BENCH_MAX_SAMPLES 128U

typedef struct gui_bench_samples {
    double values[GUI_BENCH_MAX_SAMPLES];
    size_t count;
    size_t failed;
} gui_bench_samples;

static void bench_samples_init(gui_bench_samples *samples) { memset(samples, 0, sizeof *samples); }

static void bench_samples_accept(gui_bench_samples *samples, double elapsed_ms) {
    if (elapsed_ms < 0.0 || samples->count >= GUI_BENCH_MAX_SAMPLES) {
        samples->failed++;
        return;
    }
    samples->values[samples->count++] = elapsed_ms;
}

static void bench_samples_record(gui_bench_samples *samples, bool operation_ok, double elapsed_ms) {
    if (!operation_ok) {
        samples->failed++;
        return;
    }
    bench_samples_accept(samples, elapsed_ms);
}

/* Exact nearest-rank percentile; sorts in place (idempotent across repeat calls). */
static double bench_samples_percentile(gui_bench_samples *samples, unsigned percentile) {
    if (samples->count == 0U || percentile == 0U || percentile > 100U) {
        return 0.0;
    }
    for (size_t i = 1U; i < samples->count; i++) {
        const double value = samples->values[i];
        size_t j = i;
        while (j > 0U && samples->values[j - 1U] > value) {
            samples->values[j] = samples->values[j - 1U];
            j--;
        }
        samples->values[j] = value;
    }
    const size_t rank = ((size_t)percentile * samples->count + 99U) / 100U;
    return samples->values[rank - 1U];
}

static double bench_now_ms(void) { return nt_time_now() * 1000.0; }

/* --- state ------------------------------------------------------------------------------------- */
typedef enum {
    BP_WARMUP = 0, /* let the project load + resources bind (a few frames, like --shot) */
    BP_PROBES,     /* run every action-latency + invariant probe (one heavy, untimed frame) */
    BP_DRAIN,      /* wait for the async-pack invariant's worker to be joined before timing frames */
    BP_FRAMES,     /* collect per-frame render-time samples (real GL only) */
    BP_FINALIZE,   /* write the output file, then quit on the next frame boundary */
    BP_DONE
} bp_phase;

static bool s_bench_active;
static bool s_bench_fail; /* an invariant assert or a hard load failure fired -> non-zero exit */
static char s_bench_out_path[TP_IDENTITY_PATH_MAX];
static bp_phase s_phase;
static int s_warmup_frames;
static double s_frame_start_ms;
static gui_bench_samples s_frame_samples;
static int s_frame_seen;
static bool s_written;

/* Accumulated output lines (LF-terminated), mirrored to the optional file. Sized for the fixed line
 * set; a full run emits well under this. */
static char s_bench_log[8192];
static size_t s_bench_log_len;

/* --- output ------------------------------------------------------------------------------------ */
/* Emit one machine-readable line: to stdout (the canonical channel) and into the file buffer (LF). */
static void bench_emit(const char *fmt, ...) GUI_PRINTF(1, 2);
static void bench_emit(const char *fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    (void)fputs(line, stdout);
    (void)fputc('\n', stdout);
    const size_t len = strlen(line);
    if (s_bench_log_len + len + 1U < sizeof s_bench_log) {
        memcpy(s_bench_log + s_bench_log_len, line, len);
        s_bench_log_len += len;
        s_bench_log[s_bench_log_len++] = '\n';
    }
}

static void bench_emit_action(const char *name, gui_bench_samples *s) {
    if (s->count == 0U) {
        bench_emit("bench_perf action=%s ms no_samples=1 failed=%zu", name, s->failed);
        return;
    }
    const double p50 = bench_samples_percentile(s, 50U);
    const double p95 = bench_samples_percentile(s, 95U);
    const double mx = bench_samples_percentile(s, 100U);
    bench_emit("bench_perf action=%s ms p50=%.3f p95=%.3f max=%.3f n=%zu failed=%zu", name, p50, p95,
               mx, s->count, s->failed);
}

/* Best-effort write of the accumulated lines to the requested file (UTF-8, LF -- "wb" keeps the LF
 * bytes as-is on Windows). Advisory: a write failure does not fail the run. */
static void bench_write_file(void) {
    if (s_bench_out_path[0] == '\0') {
        return;
    }
    FILE *f = nt_utf8_fopen(s_bench_out_path, "wb");
    if (!f) {
        nt_log_error("BENCH: cannot open %s for writing", s_bench_out_path);
        return;
    }
    const bool ok = fwrite(s_bench_log, 1, s_bench_log_len, f) == s_bench_log_len;
    (void)fclose(f);
    if (ok) {
        nt_log_info("BENCH: wrote %s (%zu bytes)", s_bench_out_path, s_bench_log_len);
    } else {
        nt_log_error("BENCH: short write to %s", s_bench_out_path);
    }
}

/* --- probes ------------------------------------------------------------------------------------ */
static bool bench_headless(void) { return getenv("NTPACKER_GUI_HEADLESS") != NULL; }

/* All action-latency + invariant probes, on the main thread, in one (untimed) frame. */
static void bench_run_probes(void) {
    /* Hard load gate: a successful fixture open leaves a saved path + many atlases; a failed open
     * falls back to the untitled default (no path, one atlas) -> a hard, non-zero-exit failure. */
    const tp_session_snapshot *snap = gui_project_snapshot();
    const int atlas_count = snap ? tp_session_snapshot_atlas_count(snap) : 0;
    if (!gui_project_has_path() || atlas_count <= 0) {
        s_bench_fail = true;
        bench_emit("bench_perf error=fixture_not_loaded has_path=%d atlases=%d",
                   gui_project_has_path() ? 1 : 0, atlas_count);
        return;
    }
    int total_sources = 0;
    for (int i = 0; i < atlas_count; i++) {
        const tp_snapshot_atlas *a = tp_session_snapshot_atlas_at(snap, i);
        if (a) {
            total_sources += a->source_count;
        }
    }
    bench_emit("bench_perf fixture project=%s atlases=%d sources=%d repeats=%d",
               gui_project_display_name(), atlas_count, total_sources, GUI_BENCH_REPEATS);

    /* 1. build_rows: flip the selected atlas each repeat so the {atlas id, model gen, source gen,
     *    snapshot lifetime} row cache always misses and a full tree/row-model rebuild is measured.
     *    (This is the harness U-02's tree filter will be measured against -- there is no filter yet.) */
    gui_bench_samples build;
    bench_samples_init(&build);
    for (int i = 0; i < GUI_BENCH_REPEATS; i++) {
        s_sel_atlas = i % atlas_count; /* distinct id vs the cached one -> forced rebuild */
        const double t0 = bench_now_ms();
        build_rows();
        const double t1 = bench_now_ms();
        bench_samples_accept(&build, t1 - t0);
    }
    bench_emit_action("build_rows", &build);

    /* Prepare atlas 0 for the selection probe: a blocking pack gives region -> row mapping real data. */
    s_sel_atlas = 0;
    build_rows();
    do_pack_blocking();
    const tp_result *packed = gui_pack_result(0);
    build_rows(); /* row cache still keyed to atlas 0; ensure rows are present for the scan */
    const int regions = packed ? packed->sprite_count : 0;

    /* 2. selection change through the canvas-region -> tree-row sync (a full row scan + ref match). */
    gui_bench_samples selection;
    bench_samples_init(&selection);
    for (int i = 0; i < GUI_BENCH_REPEATS; i++) {
        const int region = regions > 0 ? (i % regions) : 0;
        const double t0 = bench_now_ms();
        select_row_for_region(region);
        const double t1 = bench_now_ms();
        bench_samples_accept(&selection, t1 - t0);
    }
    if (regions <= 0) {
        bench_emit("bench_perf note=selection_pack_result_missing");
    }
    bench_emit_action("selection", &selection);

    /* 3. a reversible model edit (an atlas padding knob) + undo + redo on atlas 0, timed separately. */
    gui_bench_samples edit;
    gui_bench_samples undo;
    gui_bench_samples redo;
    bench_samples_init(&edit);
    bench_samples_init(&undo);
    bench_samples_init(&redo);
    for (int i = 0; i < GUI_BENCH_REPEATS; i++) {
        snap = gui_project_snapshot();
        const tp_snapshot_atlas *a0 = snap ? tp_session_snapshot_atlas_at(snap, 0) : NULL;
        if (!a0) {
            break;
        }
        const int64_t rev = tp_session_snapshot_revision(snap);
        const int newpad = (a0->padding == 4) ? 2 : 4; /* always differs; stays in a valid band */
        const double e0 = bench_now_ms();
        const bool edit_ok =
            gui_project_set_atlas_setting(a0->id, rev, GUI_ATLAS_PADDING, newpad, 0.0F);
        const double e1 = bench_now_ms();
        bench_samples_record(&edit, edit_ok, e1 - e0);
        if (!edit_ok) {
            continue;
        }
        const double u0 = bench_now_ms();
        const bool undo_ok = gui_project_undo();
        const double u1 = bench_now_ms();
        bench_samples_record(&undo, undo_ok, u1 - u0);
        const double r0 = bench_now_ms();
        const bool redo_ok = gui_project_redo();
        const double r1 = bench_now_ms();
        bench_samples_record(&redo, redo_ok, r1 - r0);
    }
    bench_emit_action("model_edit", &edit);
    bench_emit_action("undo", &undo);
    bench_emit_action("redo", &redo);

    /* 4. refresh / rescan + the non-blocking invariant: an external source refresh must NOT mutate the
     *    project revision or dirty state (AGENTS hard invariant). Compare revision + dirty before/after. */
    snap = gui_project_snapshot();
    const int64_t rev_before = snap ? tp_session_snapshot_revision(snap) : 0;
    const bool dirty_before = gui_project_is_dirty();
    gui_bench_samples refresh;
    bench_samples_init(&refresh);
    for (int i = 0; i < GUI_BENCH_REPEATS; i++) {
        const double t0 = bench_now_ms();
        gui_project_invalidate_sources(); /* the refresh action's core: drop scan cache + republish */
        const double t1 = bench_now_ms();
        bench_samples_accept(&refresh, t1 - t0);
    }
    bench_emit_action("refresh", &refresh);
    snap = gui_project_snapshot();
    const int64_t rev_after = snap ? tp_session_snapshot_revision(snap) : 0;
    const bool dirty_after = gui_project_is_dirty();
    const bool refresh_ok = (rev_after == rev_before) && (dirty_after == dirty_before);
    bench_emit("bench_perf invariant=refresh_nonblocking ok=%d rev_before=%lld rev_after=%lld "
               "dirty_before=%d dirty_after=%d",
               refresh_ok ? 1 : 0, (long long)rev_before, (long long)rev_after,
               dirty_before ? 1 : 0, dirty_after ? 1 : 0);
    if (!refresh_ok) {
        s_bench_fail = true;
    }

    /* 5. a Pack REQUEST is non-blocking: it must hand off to the async worker, not produce the result
     *    synchronously on the calling thread. Assert the request returns AND a worker is in flight. */
    s_sel_atlas = 0;
    char err[256] = {0};
    const double p0 = bench_now_ms();
    const bool started = gui_pack_async_start(0, err, sizeof err);
    const double p1 = bench_now_ms();
    const bool worker = gui_pack_worker_active() || gui_pack_async_busy();
    const bool pack_ok = started && worker;
    if (pack_ok) {
        bench_emit("bench_perf invariant=pack_async ok=1 started=1 worker_active=1 request_ms=%.3f",
                   p1 - p0);
    } else {
        bench_emit("bench_perf invariant=pack_async ok=0 started=%d worker_active=%d request_ms=%.3f "
                   "err=%s",
                   started ? 1 : 0, worker ? 1 : 0, p1 - p0, err);
    }
    if (!pack_ok) {
        s_bench_fail = true;
    }
    if (started) {
        gui_pack_async_cancel(); /* discard the result; main()'s frame poll + drain join the worker */
    }
}

/* --- arg parsing + lifecycle ------------------------------------------------------------------- */
bool gui_bench_parse_arg(const char *arg) {
    if (strcmp(arg, "--bench-perf") == 0) {
        s_bench_out_path[0] = '\0';
        s_bench_active = true;
        setvbuf(stdout, NULL, _IONBF, 0); /* CI reads stdout; never lose a line to buffering */
        return true;
    }
    if (strncmp(arg, "--bench-perf=", 13) == 0) {
        const char *path = arg + 13;
        const size_t length = strlen(path);
        if (path[0] == '\0' || length >= sizeof s_bench_out_path ||
            !tp_utf8_is_valid_c_string(path)) {
            (void)fprintf(stderr,
                          "ntpacker-gui: --bench-perf path is empty, invalid UTF-8, or too long\n");
            s_bench_out_path[0] = '\0';
            s_bench_active = true; /* still run the probes; just skip the file mirror */
            setvbuf(stdout, NULL, _IONBF, 0);
            return true;
        }
        memcpy(s_bench_out_path, path, length + 1U);
        s_bench_active = true;
        setvbuf(stdout, NULL, _IONBF, 0);
        return true;
    }
    return false;
}

bool gui_bench_active(void) { return s_bench_active; }

const char *gui_bench_default_project(void) { return GUI_BENCH_DEFAULT_PROJECT; }

int gui_bench_exit_code(void) { return s_bench_fail ? 1 : 0; }

void gui_bench_tick(void) {
    if (!s_bench_active) {
        return;
    }
    s_frame_start_ms = bench_now_ms(); /* read here (pre-render) for the BP_FRAMES delta in post_draw */
    switch (s_phase) {
    case BP_WARMUP:
        if (++s_warmup_frames >= GUI_BENCH_WARMUP_FRAMES) {
            s_phase = BP_PROBES;
        }
        break;
    case BP_PROBES:
        nt_log_info("BENCH: begin (--bench-perf)");
        bench_run_probes();
        if (bench_headless()) {
            bench_emit("bench_perf frame_ms skipped=headless");
            s_phase = BP_FINALIZE;
        } else {
            s_phase = BP_DRAIN; /* let the pack-async invariant's worker be joined before timing */
        }
        break;
    case BP_DRAIN:
        if (!gui_pack_worker_active()) {
            bench_samples_init(&s_frame_samples);
            s_frame_seen = 0;
            s_phase = BP_FRAMES;
        }
        break;
    case BP_FRAMES:
    case BP_FINALIZE:
    case BP_DONE:
        break; /* driven from post_draw */
    }
}

void gui_bench_post_draw(void) {
    if (!s_bench_active) {
        return;
    }
    if (s_phase == BP_FRAMES) {
        s_frame_seen++;
        if (s_frame_seen > GUI_BENCH_FRAME_WARMUP) {
            bench_samples_accept(&s_frame_samples, bench_now_ms() - s_frame_start_ms);
        }
        if (s_frame_samples.count >= GUI_BENCH_FRAME_TARGET) {
            const double p50 = bench_samples_percentile(&s_frame_samples, 50U);
            const double p95 = bench_samples_percentile(&s_frame_samples, 95U);
            const double mx = bench_samples_percentile(&s_frame_samples, 100U);
            bench_emit("bench_perf frame_ms p50=%.3f p95=%.3f max=%.3f n=%zu", p50, p95, mx,
                       s_frame_samples.count);
            s_phase = BP_FINALIZE;
        }
        return;
    }
    if (s_phase == BP_FINALIZE) {
        if (!s_written) {
            bench_write_file();
            s_written = true; /* quit on the next clean frame boundary (mirror gui_shot_post_draw) */
            return;
        }
        s_phase = BP_DONE;
        s_bench_active = false;
        nt_app_quit();
    }
}
