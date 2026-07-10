#ifndef NTPACKER_GUI_PACK_H
#define NTPACKER_GUI_PACK_H

/* In-process packing for the GUI (ux.md §3.2/§3.3b). Owns the tp_pack input assembly from the
 * live project and one arena-owned tp_result PER ATLAS (the atlas-page canvas renders it).
 *
 * Raw-name convention (must match the exporter/CLI path -- see tp_normalize + test_export_run):
 *   - FILE source  -> sprite raw name = the file's BASENAME, extension KEPT (e.g. "a.png").
 *   - FOLDER source-> sprite raw name = the path RELATIVE to the folder root, extension KEPT and
 *     sub-folders preserved (e.g. "anim/test-0.png").
 * The raw name is what tp_name_map hashes; tp_normalize strips the extension (folders kept) at
 * export/preview naming, so the STRIPPED raw name == the sprite-tree override key ("anim/test-0"),
 * which is how selection sync maps a canvas region back to a list row.
 *
 * Packing is SYNCHRONOUS: a demo-scale pack (<100 sprites) measures well under the 150 ms bar
 * (see gui_pack_atlas out_ms), so no worker thread this round. Missing files are SKIPPED with a
 * notice (never fatal, ux.md §3.7). */

#include <stddef.h>
#include <stdbool.h>

#include "tp_core/tp_model.h" /* tp_result */

#ifdef __cplusplus
extern "C" {
#endif

/* Stores the session work dir (where tp_pack writes the transient .ntpack) and creates it. */
void gui_pack_init(const char *work_dir);

/* Packs atlas `atlas_index` of the live project: assembles sprites from its sources (files as-is,
 * folders expanded via gui_scan), maps per-sprite origin/slice9 overrides, runs tp_pack, and stores
 * the arena-owned tp_result in the atlas's slot (the previous arena is destroyed first).
 *
 * On success returns true, writes the wall-clock pack time to *out_ms (nullable), and appends any
 * skipped-missing-file count to `notice` (nullable, cap notice_cap). On failure returns false and
 * fills `err` (nullable). An atlas with zero usable sprites is a failure (nothing to show). */
bool gui_pack_atlas(int atlas_index, double *out_ms, char *err, size_t err_cap, char *notice, size_t notice_cap);

/* The stored result for `atlas_index`, or NULL if never packed / last pack failed. */
const tp_result *gui_pack_result(int atlas_index);

/* Sprite index within the stored result whose STRIPPED raw name (ext removed, folders kept) equals
 * `key` (the sprite-tree override key), or -1. Powers list-row -> canvas-region selection sync. */
int gui_pack_find_sprite(int atlas_index, const char *key);

/* Exports every ENABLED target of atlas `atlas_index` (tp_export_run packs per target with the
 * atlas settings INTERSECT each target's capabilities, then writes files). Assembles the same sprite
 * set as gui_pack_atlas; creates each target's output parent directory first. Returns true on success
 * and writes the enabled-target count to *out_targets and the metadata-loss notice count to
 * *out_notices (both nullable); a joined notice summary goes to `notice`. On failure returns false and
 * fills `err` (e.g. unsaved project with relative output paths). */
bool gui_pack_export(int atlas_index, int *out_targets, int *out_notices, char *err, size_t err_cap, char *notice,
                     size_t notice_cap);

/* Drops the stored result for one atlas (or all with index < 0). Frees its arena. Call on
 * project new/open and before a repack. */
void gui_pack_clear(int atlas_index);

void gui_pack_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* NTPACKER_GUI_PACK_H */
