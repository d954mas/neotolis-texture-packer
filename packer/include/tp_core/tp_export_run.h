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

#include "tp_core/tp_error.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;
struct tp_pack_sprite_desc;
struct tp_arena;
struct tp_export_notices;

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

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_EXPORT_RUN_H */
