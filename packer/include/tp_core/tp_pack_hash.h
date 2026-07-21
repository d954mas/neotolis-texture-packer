#ifndef TP_CORE_TP_PACK_HASH_H
#define TP_CORE_TP_PACK_HASH_H

/* Canonical semantic pack-input hash (master spec §10.2, §59 items 20/28/29,
 * packet F3-03 T1).
 *
 * `pack_input_hash` is the content-addressed identity of everything that can
 * change a Pack RESULT: the effective packing settings, the ordered sprite
 * identities + per-sprite overrides + pivots, the selected preview target, the
 * packer algorithm version, and a per-sprite `semantic_image_hash` over the
 * canonical RGBA8 pixels (width + height + pixels). It is NOT a change-detection
 * token: raw file bytes, mtime, and container metadata never enter the hash
 * (§10.2 permits them ONLY to skip re-decoding). Two packs whose visible result
 * would be identical share a hash; any semantic change (reorder, edited pixels,
 * changed setting/override/target) yields a different hash.
 *
 * Determinism: the input is serialized into a fixed-width little-endian byte
 * stream (version byte FIRST, then reserved input-category tags so future B1
 * linked sources extend it additively -- decision 0020/D-N1) and folded through
 * the endian-stable tp_hasher (FNV-1a/128). No pointers, no timestamps, no
 * native-endian integers cross the hash boundary, so the same input yields the
 * same tp_id128 on every OS.
 *
 * The per-file image term is decoded through the shared bounded tp_image ingress
 * and cached by an (path, size, mtime) fingerprint. A changed fingerprint forces
 * a re-decode + re-hash; identical pixels re-saved with a new mtime therefore
 * yield the SAME semantic hash (the cache never contributes bytes to the hash --
 * it only decides whether to re-decode). The cache is a pure optimization: a
 * NULL cache is valid and just decodes every call. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tp_core/tp_error.h"
#include "tp_core/tp_id.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tp_pack_settings;

/* Version byte -- emitted FIRST in the canonical stream. Bumping it is a visible
 * change: every previously cached hash is invalidated. */
#define TP_PACK_INPUT_HASH_VERSION ((uint8_t)1)

/* Packer algorithm/version profile term (spec §10.2). A change in the packer
 * that would change layout for identical inputs must bump this so stale cached
 * results are not treated as current. */
#define TP_PACK_ALGO_VERSION ((uint32_t)1)

/* Reserved canonical input-category tags. Additive: new categories append; the
 * existing tags never shift so a serialized stream stays comparable across
 * releases that only ADD inputs (decision 0020/D-N1, B1 linked sources). */
typedef enum tp_pack_hash_category {
    TP_PACK_HASH_CAT_ALGO = 1,
    TP_PACK_HASH_CAT_SETTINGS = 2,
    TP_PACK_HASH_CAT_TARGET = 3,
    TP_PACK_HASH_CAT_SPRITE_LIST = 4,
    TP_PACK_HASH_CAT_SPRITE = 5,
    TP_PACK_HASH_CAT_IMAGE = 6,
    /* RESERVED (not yet emitted): B1 linked-source materialization inputs. */
    TP_PACK_HASH_CAT_LINKED_SOURCE = 7
} tp_pack_hash_category;

/* semantic_image_hash = hash(TP_PACK_HASH_CAT_IMAGE, width, height, canonical
 * RGBA8 pixels). `rgba` is width*height*4 bytes, y-down. Pure function -- no
 * filesystem, no cache; the same pixels always yield the same id on every OS. */
tp_id128 tp_pack_semantic_image_hash(int width, int height, const uint8_t *rgba);

/* Session-lifetime per-file image-hash cache keyed by (path, size, mtime).
 * In-memory only. Opaque. NULL is a valid "no cache" argument to the compute
 * call. */
typedef struct tp_pack_image_hash_cache tp_pack_image_hash_cache;

tp_pack_image_hash_cache *tp_pack_image_hash_cache_create(void);
void tp_pack_image_hash_cache_destroy(tp_pack_image_hash_cache *cache);

typedef struct tp_pack_image_hash_cache_stats {
    uint64_t decodes; /* files actually decoded (misses served by re-decode) */
    uint64_t hits;    /* fingerprint matched -- decode skipped */
    uint64_t misses;  /* absent or fingerprint changed -- decode performed */
    int entries;      /* distinct paths currently retained */
} tp_pack_image_hash_cache_stats;

void tp_pack_image_hash_cache_stats_get(const tp_pack_image_hash_cache *cache,
                                        tp_pack_image_hash_cache_stats *out);

/* Computes the canonical pack_input_hash for one atlas from its effective
 * settings (packing knobs + ordered sprite descs, settings->sprites/
 * sprite_count) and the selected preview target. `preview_exporter_id` may be
 * NULL (native settings). `cache` may be NULL. On success writes *out_hash and
 * returns TP_STATUS_OK. A path sprite whose file cannot be stat'd/decoded
 * propagates the tp_image/filesystem status and leaves *out_hash nil. NULL
 * settings/out_hash -> TP_STATUS_INVALID_ARGUMENT. */
tp_status tp_pack_input_hash_compute(const struct tp_pack_settings *settings,
                                     const char *preview_exporter_id,
                                     tp_pack_image_hash_cache *cache,
                                     tp_id128 *out_hash, tp_error *err);

/* Folds the canonical pack_input_hash from settings + preview target + an array of
 * PRE-COMPUTED per-sprite semantic image hashes (one per sprite, in
 * settings->sprites order, e.g. tp_pack_semantic_image_hash over each decoded
 * source). This is the seam for a caller that already decoded the pixels once (a
 * Pack worker collecting hashes from the pack's own decode pass) and must not
 * decode again: the folded stream is byte-identical to tp_pack_input_hash_compute
 * for the same pixels. `image_hash_count` must equal settings->sprite_count. No
 * filesystem access. NULL settings/out_hash, a NULL array, or a count mismatch ->
 * TP_STATUS_INVALID_ARGUMENT with *out_hash left nil. */
tp_status tp_pack_input_hash_from_images(const struct tp_pack_settings *settings,
                                         const char *preview_exporter_id,
                                         const tp_id128 *image_hashes,
                                         int image_hash_count,
                                         tp_id128 *out_hash, tp_error *err);

#ifdef __cplusplus
}
#endif

#endif /* TP_CORE_TP_PACK_HASH_H */
