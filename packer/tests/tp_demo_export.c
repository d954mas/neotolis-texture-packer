/* tp_demo_export -- dev driver + smoke tool (NOT a ctest).
 *
 * Loads a `.ntpacker_project` and runs EVERY enabled target of EVERY atlas.
 * Input assembly is routed through the shared op layer (tp_pack_input_build):
 * each atlas's declared `sources` are expanded exactly as the GUI and CLI expand
 * them -- the image-ext whitelist, folder recursion + sort, file-source naming,
 * and per-sprite override encoding all live in tp_core now, not here. Outputs are
 * written to each target's project-relative out_path, resolved against the project
 * dir -- exactly the path tp_export_run drives for the CLI/GUI. This makes the
 * driver a faithful parity oracle for the demo exports (a byte-for-byte match with
 * `ntpacker pack` on the same project) and a handy end-to-end smoke of
 * load -> assemble -> pack -> export.
 *
 * Usage: tp_demo_export <path/to.ntpacker_project> [work_dir]
 *   work_dir (default ".") only holds the transient session .ntpack files
 *   (gitignored); target outputs land at the project out_paths.
 */

#define _CRT_SECURE_NO_WARNINGS

#include <stdio.h>
#include <string.h>

#include "tp_core/tp_arena.h"
#include "tp_core/tp_export.h"
#include "tp_core/tp_export_run.h"
#include "tp_core/tp_input.h"
#include "tp_core/tp_project.h"
#include "tp_core/tp_scan.h" /* tp_mkdirs / tp_mkdirs_parent (tp_core owns FS helpers) */

static int run_atlas(tp_project *proj, int idx, const char *work_dir) {
    const tp_project_atlas *a = &proj->atlases[idx];
    const char *name = a->name ? a->name : "?";

    /* Assemble sprites through the shared bridge (image-ext policy, recursion,
     * naming, override encoding all owned by tp_core). */
    tp_pack_input input;
    tp_error e = {{0}};
    tp_status bst = tp_pack_input_build(proj, idx, &input, &e);
    if (bst != TP_STATUS_OK) {
        (void)fprintf(stderr, "atlas '%s': input build failed: %s (%s)\n", name, tp_status_str(bst), e.msg);
        return 1;
    }
    if (input.count == 0) {
        (void)fprintf(stderr, "atlas '%s': no source sprites found\n", name);
        tp_pack_input_free(&input);
        return 1;
    }

    tp_arena *ar = tp_arena_create(0);
    tp_export_notices notices;
    tp_export_notices_init(&notices);
    int pack_runs = 0;
    tp_error ee = {{0}};
    tp_status st = tp_export_run(proj, idx, input.descs, input.count, work_dir, ar, &notices, &pack_runs, &ee);
    int rc = 0;
    if (st != TP_STATUS_OK) {
        (void)fprintf(stderr, "atlas '%s': export failed: %s (%s)\n", name, tp_status_str(st), ee.msg);
        rc = 1;
    } else {
        (void)printf("atlas '%s': %d sprite(s), %d target(s), %d pack run(s)%s\n", name, input.count, a->target_count,
                     pack_runs, input.missing_sources > 0 ? " (missing sources skipped)" : "");
        for (int n = 0; n < notices.count; n++) {
            (void)printf("    notice: %s\n", notices.items[n].msg);
        }
    }
    tp_export_notices_free(&notices);
    tp_arena_destroy(ar);
    tp_pack_input_free(&input);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        (void)fprintf(stderr, "usage: %s <project.ntpacker_project> [work_dir]\n", argv[0]);
        return 2;
    }
    const char *proj_path = argv[1];

    tp_project *proj = NULL;
    tp_error e = {{0}};
    if (tp_project_load(proj_path, &proj, &e) != TP_STATUS_OK) {
        (void)fprintf(stderr, "cannot load project '%s': %s\n", proj_path, e.msg);
        return 1;
    }
    /* work_dir holds only transient session .ntpack files (gitignored); default to
     * the current directory so we never resurrect an output tree. */
    const char *work_dir = (argc > 2) ? argv[2] : ".";
    tp_mkdirs(work_dir);

    /* ensure every target's parent directory exists (tp_export_run requires it). */
    for (int i = 0; i < proj->atlas_count; i++) {
        const tp_project_atlas *a = &proj->atlases[i];
        for (int t = 0; t < a->target_count; t++) {
            char abs[1024];
            if (tp_project_resolve_path(proj, a->targets[t].out_path, abs, sizeof abs) == TP_STATUS_OK) {
                tp_mkdirs_parent(abs);
            }
        }
    }

    int rc = 0;
    for (int i = 0; i < proj->atlas_count; i++) {
        rc |= run_atlas(proj, i, work_dir);
    }

    tp_project_destroy(proj);
    if (rc == 0) {
        (void)printf("tp_demo_export: OK\n");
    }
    return rc;
}
