#ifndef TP_PACK_HASH_PRIV_H
#define TP_PACK_HASH_PRIV_H

/* In-process cache seeding seam for Pack jobs. `content_hash` is derived from
 * the exact encoded bytes whose decoded pixels produced `semantic_hash`;
 * before/after metadata checks ensure the observed pathname stayed stable. */

#include <stdint.h>

#include "tp_core/tp_pack_hash.h"

tp_status tp_pack_image_hash_cache_seed(
    tp_pack_image_hash_cache *cache, const char *path, uint64_t size,
    int64_t mtime, tp_id128 content_hash, tp_id128 semantic_hash,
    tp_error *err);

/* Narrow test observation for proving that an existing-key update does not
 * allocate or grow the cache. */
size_t tp_pack_image_hash_cache_capacity_for_test(
    const tp_pack_image_hash_cache *cache);

#endif /* TP_PACK_HASH_PRIV_H */
