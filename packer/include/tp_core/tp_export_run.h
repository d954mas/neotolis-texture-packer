#ifndef TP_CORE_TP_EXPORT_RUN_H
#define TP_CORE_TP_EXPORT_RUN_H

/*
 * Per-target packing orchestration (SUMMARY.md §5h, ROADMAP Phase 2). Lives in
 * tp_build because it drives nt_builder (via tp_pack); tp_core's pure export
 * layer (tp_export.h) stays GUI-linkable. For each ENABLED target of an atlas:
 *   1. resolve its exporter by id (unknown id -> hard error listing known ids);
 *   2. effective settings = atlas settings INTERSECT exporter capabilities;
 *   3. targets whose effective settings coincide share ONE pack run;
 *   4. pack -> normalize -> the exporter writes its files at the target path.
 *
 * Input sprites are supplied by the caller (CLI/GUI/tests own input gathering
 * for now; folder scanning is a later phase), NOT scanned here.
 */

#include <stdbool.h>

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;
struct tp_pack_sprite_desc;
struct tp_arena;
struct tp_export_notices;

/* --- Structured export report (optional, produced by tp_export_run_ex) --------
 * The CLI build report (and, follow-up, GUI export stats) consume this instead of
 * re-deriving pages/occupancy/written-files by hand. Every pointer is arena-owned
 * (the arena passed to tp_export_run_ex); it lives exactly as long as that arena. */

/* One packed page of a shared pack run. `occupancy_pct` is the fraction of the
 * page covered by placed sprite content: sum over ORIGINAL placements (an alias
 * shares its original's pixels and is NOT counted again) of the placed frame area
 * (frame.w * frame.h), divided by the page area, times 100. Deterministic; a
 * non-empty page is always in (0,100] (placed frames never overlap). */
typedef struct tp_export_report_page {
    int index;
    int w, h;
    double occupancy_pct;
} tp_export_report_page;

/* One shared pack run: targets whose effective settings coincide collapse here. */
typedef struct tp_export_report_run {
    tp_export_report_page *pages;
    int page_count;
    int sprite_count; /* placed sprites in this run (original placements + aliases) */
} tp_export_report_run;

/* One ENABLED target's outcome. `written_files` are the absolute paths its writer
 * produced (NULL/empty when it failed, and ALWAYS empty on a dry run -- see
 * `would_write`). `would_write` is populated only on a DRY run: the paths the
 * target WOULD produce, assembled from the same list_outputs source the writer
 * uses, without touching the disk (empty on a wet run). `notice_begin`/`notice_end`
 * bound this target's slice of the caller's notices list ([begin,end)); on a dry
 * run those notices come from tp_export_predict_loss (the writers never run), on a
 * wet run from the writers themselves. `pack_run` indexes report.runs (-1 if the
 * target failed before packing). `error` holds the failure reason when !ok (arena
 * string), NULL when ok. */
typedef struct tp_export_report_target {
    const char *exporter_id;
    const char *out_path; /* resolved absolute output base (no extension) */
    const char *const *written_files;
    int written_file_count;
    const char *const *would_write; /* dry-run only: paths this target WOULD produce */
    int would_write_count;
    int pack_run;
    int notice_begin;
    int notice_end;
    const char *error;
    bool ok;
} tp_export_report_target;

/* Whole per-atlas run report. `pack_failed` is set when a pack/normalize/settings
 * error aborted the run before any target could write (nothing was produced -- the
 * caller maps this to a pack failure rather than a per-target export failure).
 * `dry_run` mirrors the request: true when NO target files were written (each ok
 * target instead carries a `would_write` list + predicted-loss notices). */
typedef struct tp_export_report {
    tp_export_report_run *runs;
    int run_count;
    tp_export_report_target *targets;
    int target_count;
    bool pack_failed;
    bool dry_run;
} tp_export_report;

/* Runs every enabled target of project->atlases[atlas_index] over `sprites`.
 *
 * `work_dir` holds the session .ntpack(s); `arena` owns all packed/normalized
 * data (destroy it to free everything). Metadata-loss notices are appended to
 * `notices` (init'd by the caller; never fatal). `out_pack_runs` (nullable)
 * receives the number of distinct pack runs performed (targets with identical
 * effective settings collapse to one). Target output paths are resolved against
 * the project dir; their parent directories must already exist. */
tp_status tp_export_run(const struct tp_project *project, int atlas_index, const struct tp_pack_sprite_desc *sprites,
                        int sprite_count, const char *work_dir, struct tp_arena *arena,
                        struct tp_export_notices *notices, int *out_pack_runs, tp_error *err);

/* Options for tp_export_run_ex. A NULL opts pointer (or an all-zero struct) makes
 * tp_export_run_ex behave EXACTLY like tp_export_run: no report, real writes. */
typedef struct tp_export_run_opts {
    /* Optional arena-owned structured report out-param (nullable). When set, a
     * target whose exporter id is unknown, whose out_path cannot be resolved, or
     * whose writer fails is recorded as failed (ok=false, with `error`) and the run
     * CONTINUES to the remaining targets, so partial success is observable. With a
     * NULL report the run bails on the first such failure (the GUI path, byte-
     * unchanged). A pack/normalize failure always aborts the atlas and sets
     * report->pack_failed. */
    tp_export_report *report;

    /* Dry run: still packs + normalizes (the report needs real pages/occupancy) and
     * still resolves each target's exporter + out_path + effective settings, but
     * writes NO target files. Each ok target reports written_files empty, a
     * `would_write` list (the paths it WOULD produce), and predicted-loss notices
     * from tp_export_predict_loss (the writers -- the wet-path notice source -- do
     * not run). Meaningful only with a non-NULL report (would_write/notices live
     * there); a dry run without a report just skips the writes. */
    bool dry_run;
} tp_export_run_opts;

/* tp_export_run plus optional behavior selected by `opts` (nullable == defaults;
 * see tp_export_run_opts). The returned status is the first per-target failure on
 * the report path (TP_STATUS_OK when every target succeeded/would succeed). */
tp_status tp_export_run_ex(const struct tp_project *project, int atlas_index, const struct tp_pack_sprite_desc *sprites,
                           int sprite_count, const char *work_dir, struct tp_arena *arena,
                           struct tp_export_notices *notices, int *out_pack_runs,
                           const tp_export_run_opts *opts, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_RUN_H */
