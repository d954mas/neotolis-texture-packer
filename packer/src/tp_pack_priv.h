#ifndef TP_PACK_PRIV_H
#define TP_PACK_PRIV_H

/* In-process pack seams shared between tp_pack.c and its co-process callers
 * (tp_job.c). NOT part of the public tp_core/tp_pack.h contract. Named *_priv.h
 * (not *_internal.h) as a deliberately unregistered private header, like
 * tp_txn_parse_priv.h -- kept out of the check_boundaries R18 registry scan. */

#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_pack.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_arena;
struct tp_result;

/* Per-image observer for the pack's single decode pass. Fired once per sprite, in
 * settings->sprites order, right after that sprite's canonical RGBA8 pixels are
 * resolved (decoded for a path sprite, borrowed for a raw sprite) and BEFORE the
 * page preflight. `rgba` is width*height*4 bytes, valid only for the duration of
 * the call. It lets an in-process caller derive per-sprite terms (the semantic
 * image hash for pack_input_hash) from the SAME decode the pack consumes, so a
 * pack pays exactly ONE decode per source. */
typedef void (*tp_pack_image_observer)(void *ctx, int sprite_index, int width,
                                       int height, const uint8_t *rgba);

/* tp_pack_cancellable plus the per-image observer seam. `observer` may be NULL
 * (then this is exactly tp_pack_cancellable). The pack RESULT is byte-identical
 * whether or not an observer is supplied -- the observer only inspects decoded
 * pixels; it never influences the pack. */
tp_status tp_pack_cancellable_observed(const tp_pack_settings *settings,
                                       struct tp_arena *arena,
                                       struct tp_result **out_result,
                                       tp_pack_cancel_poll cancel_poll,
                                       void *cancel_ctx,
                                       tp_pack_image_observer observer,
                                       void *observer_ctx, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_PACK_PRIV_H */
