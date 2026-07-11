#ifndef TP_CORE_TP_INPUT_H
#define TP_CORE_TP_INPUT_H

/* Project -> pack input: expand one atlas's sources into the sprite descs tp_pack
 * consumes. This is the single home for the bridge the GUI used to own
 * (arch review §3.1: assemble()/desc_add() in gui_pack.c): the raw-name policy
 * (folder child = rel WITH ext; file source = basename WITH ext), override lookup
 * by tp_sprite_export_key, per-sprite override encoding (+1 shape), and the
 * effective-shape/extrude rule. Frontends call this instead of assembling descs
 * by hand (AGENTS.md: features land in core first). Depends only on tp_core
 * (tp_scan/tp_names/tp_project + the tp_pack.h desc struct) -- no builder. */

#include "tp_core/tp_error.h"
#include "tp_core/tp_pack.h" /* tp_pack_sprite_desc */

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;

/* Assembled pack input for one atlas. `descs` and each `descs[i].name`/`.path`
 * are malloc-owned (free with tp_pack_input_free), independent of the project --
 * the input outlives a tp_project_destroy. `missing_sources` counts sources that
 * resolved but do not exist on disk (skipped, not fatal -- ux.md §3.7). */
typedef struct tp_pack_input {
    tp_pack_sprite_desc *descs;
    int count;
    int missing_sources;
} tp_pack_input;

/* Expands atlas[atlas_index]'s sources into *out (zeroed first): a file source is
 * keyed by its basename; a folder source is scanned recursively (tp_scan, entries
 * already sorted by rel path) and its children appended in scan order. Descs are
 * emitted per-source-then-sorted-within-source with NO global sort across sources
 * -- packing layout depends on input order (arch review R2). Each sprite's
 * per-sprite overrides are looked up by tp_sprite_export_key and encoded onto the
 * desc. Zero descs is not an error (the caller decides whether "empty" is fatal).
 * An unresolvable source (e.g. a relative path with no project_dir) is skipped and
 * NOT counted as missing. Returns TP_STATUS_OK, TP_STATUS_OUT_OF_BOUNDS (bad
 * atlas_index), TP_STATUS_INVALID_ARGUMENT (NULL project/out), or TP_STATUS_OOM.
 * On any error *out is left empty (except a NULL `out`, which is untouched). */
tp_status tp_pack_input_build(const struct tp_project *p, int atlas_index, tp_pack_input *out, tp_error *err);

/* Frees the descs (each name/path + the array) and zeroes *out. NULL-safe. */
void tp_pack_input_free(tp_pack_input *out);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_INPUT_H */
