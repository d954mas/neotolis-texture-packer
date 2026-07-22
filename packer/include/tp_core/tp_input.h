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

#include "tp_core/tp_cancel.h"
#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"
#include "tp_core/tp_pack.h" /* tp_pack_sprite_desc */
#include "tp_core/tp_srckey.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_project;
struct tp_session_snapshot;

/* Collision-free internal packed name. This is the single owner of the
 * source-id/key encoding consumed by pack-result canonical lookups. */
#define TP_PACK_INTERNAL_NAME_CAP (TP_ID_TEXT_CAP + TP_SRCKEY_MAX)
tp_status tp_pack_input_format_sprite_name(tp_id128 source_id,
                                           const char *source_key,
                                           char *out, size_t capacity,
                                           tp_error *err);

/* Assembled pack input for one atlas. `descs` and each descriptor's `name`,
 * `path`, `source_key`, and `logical_name` are malloc-owned (free with
 * tp_pack_input_free), independent of the project -- the input outlives a
 * tp_project_destroy. `missing_sources` counts sources that resolved but do not
 * exist on disk (skipped, not fatal -- ux.md §3.7). */
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
 * A source that resolves but is absent is counted as missing and skipped. Source
 * resolution, directory open/read, UTF-8/key normalization, path-bound, and OOM
 * failures propagate their precise tp_status/tp_error. On any error *out is left
 * empty (except a NULL `out`, which is untouched). */
tp_status tp_pack_input_build(const struct tp_project *p, int atlas_index, tp_pack_input *out, tp_error *err);

/* Cancellable form of tp_pack_input_build: `cancel` is polled once per source and
 * threaded into the folder walk (per directory entry), so the async pack worker can
 * interrupt a slow / network folder source promptly. A NULL `cancel` means "never
 * cancel" -- tp_pack_input_build() is exactly this. On cancellation the partial input
 * is freed, *out is left empty, and TP_STATUS_CANCELLED is returned (a clean stop, not
 * a failure). All other semantics match tp_pack_input_build. */
tp_status tp_pack_input_build_cancellable(const struct tp_project *p, int atlas_index,
                                          tp_pack_input *out,
                                          const tp_cancel_token *cancel,
                                          tp_error *err);

/* Frontend-safe pack admission: resolves a stable atlas ID inside an immutable
 * session snapshot and returns only the typed, caller-owned pack input. */
tp_status tp_pack_input_build_snapshot(const struct tp_session_snapshot *snapshot,
                                       tp_id128 atlas_id, tp_pack_input *out,
                                       tp_error *err);

/* Cancellable form of tp_pack_input_build_snapshot (NULL `cancel` => never cancel,
 * exactly the non-cancellable form). Cancellation returns TP_STATUS_CANCELLED with
 * *out left empty. */
tp_status tp_pack_input_build_snapshot_cancellable(
    const struct tp_session_snapshot *snapshot, tp_id128 atlas_id,
    tp_pack_input *out, const tp_cancel_token *cancel, tp_error *err);
tp_status tp_pack_settings_build_snapshot(const struct tp_session_snapshot *snapshot,
                                          tp_id128 atlas_id,
                                          tp_pack_settings *out,
                                          tp_error *err);

/* Frees every owned descriptor string plus the array and zeroes *out. NULL-safe. */
void tp_pack_input_free(tp_pack_input *out);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_INPUT_H */
