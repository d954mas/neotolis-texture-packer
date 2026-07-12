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
 * produced (NULL/empty when it failed). `notice_begin`/`notice_end` bound this
 * target's slice of the caller's notices list ([begin,end)). `pack_run` indexes
 * report.runs (-1 if the target failed before packing). `error` holds the failure
 * reason when !ok (arena string), NULL when ok. */
typedef struct tp_export_report_target {
    const char *exporter_id;
    const char *out_path; /* resolved absolute output base (no extension) */
    const char *const *written_files;
    int written_file_count;
    int pack_run;
    int notice_begin;
    int notice_end;
    const char *error;
    bool ok;
} tp_export_report_target;

/* Whole per-atlas run report. `pack_failed` is set when a pack/normalize/settings
 * error aborted the run before any target could write (nothing was produced -- the
 * caller maps this to a pack failure rather than a per-target export failure). */
typedef struct tp_export_report {
    tp_export_report_run *runs;
    int run_count;
    tp_export_report_target *targets;
    int target_count;
    bool pack_failed;
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

/* tp_export_run plus an optional arena-owned structured `report` (nullable).
 *
 * With `report == NULL` this behaves EXACTLY like tp_export_run (bail on the first
 * unknown-exporter / resolve / write failure -- the GUI path is byte-unchanged).
 *
 * With a non-NULL `report`, a target whose exporter id is unknown, whose out_path
 * cannot be resolved, or whose writer fails is recorded as failed (ok=false, with
 * `error`) and the run CONTINUES to the remaining targets, so partial success is
 * observable. The returned status is the first such failure (TP_STATUS_OK when
 * every target wrote). A pack/normalize failure still aborts the atlas and sets
 * report->pack_failed (no target could have produced output). */
tp_status tp_export_run_ex(const struct tp_project *project, int atlas_index, const struct tp_pack_sprite_desc *sprites,
                           int sprite_count, const char *work_dir, struct tp_arena *arena,
                           struct tp_export_notices *notices, int *out_pack_runs, tp_export_report *report,
                           tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_RUN_H */
