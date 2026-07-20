#include "tp_core/tp_pack_hash.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_image.h"
#include "tp_core/tp_pack.h" /* tp_pack_settings, tp_pack_sprite_desc */
#include "tp_fs_internal.h"  /* tp_fs_stat: (size, mtime) change-detection */

/* ---- fixed-width little-endian serialization into the streaming hasher ------
 * Every integer is emitted as explicit LE bytes so the folded stream is
 * byte-identical on every OS regardless of native endianness. tp_hasher itself
 * mixes byte-at-a-time and packs its output big-endian, so the id is stable. */

static void hash_u8(tp_hasher *h, uint8_t v) { tp_hasher_update(h, &v, 1U); }

static void hash_u16(tp_hasher *h, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU)};
    tp_hasher_update(h, b, sizeof b);
}

static void hash_u32(tp_hasher *h, uint32_t v) {
    uint8_t b[4] = {(uint8_t)(v & 0xFFU), (uint8_t)((v >> 8) & 0xFFU),
                    (uint8_t)((v >> 16) & 0xFFU), (uint8_t)((v >> 24) & 0xFFU)};
    tp_hasher_update(h, b, sizeof b);
}

static void hash_u64(tp_hasher *h, uint64_t v) {
    uint8_t b[8];
    for (int i = 0; i < 8; i++) {
        b[i] = (uint8_t)((v >> (8 * i)) & 0xFFU);
    }
    tp_hasher_update(h, b, sizeof b);
}

static void hash_f32(tp_hasher *h, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    hash_u32(h, bits);
}

/* Length-prefixed byte field: the u64 length makes concatenated fields
 * unambiguous (no separator can be forged by field contents). */
static void hash_bytes_field(tp_hasher *h, const void *p, size_t n) {
    hash_u64(h, (uint64_t)n);
    if (n > 0U) {
        tp_hasher_update(h, p, n);
    }
}

static void hash_str_field(tp_hasher *h, const char *s) {
    hash_bytes_field(h, s ? s : "", s ? strlen(s) : 0U);
}

tp_id128 tp_pack_semantic_image_hash(int width, int height,
                                     const uint8_t *rgba) {
    tp_hasher h = tp_hasher_init();
    hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_IMAGE);
    hash_u32(&h, (uint32_t)width);
    hash_u32(&h, (uint32_t)height);
    if (rgba && width > 0 && height > 0) {
        const size_t bytes = (size_t)width * (size_t)height * 4U;
        tp_hasher_update(&h, rgba, bytes);
    }
    return tp_hasher_final(h);
}

/* ---- per-file image-hash cache (path,size,mtime) -> semantic_image_hash ----- */

typedef struct img_cache_entry {
    char *path; /* owned; NULL = empty slot */
    uint64_t size;
    int64_t mtime;
    tp_id128 hash;
} img_cache_entry;

struct tp_pack_image_hash_cache {
    img_cache_entry *slots;
    size_t cap; /* power of two, 0 until first insert */
    size_t count;
    uint64_t decodes;
    uint64_t hits;
    uint64_t misses;
};

tp_pack_image_hash_cache *tp_pack_image_hash_cache_create(void) {
    return calloc(1U, sizeof(tp_pack_image_hash_cache));
}

void tp_pack_image_hash_cache_destroy(tp_pack_image_hash_cache *cache) {
    if (!cache) {
        return;
    }
    if (cache->slots) {
        for (size_t i = 0; i < cache->cap; i++) {
            free(cache->slots[i].path);
        }
        free(cache->slots);
    }
    free(cache);
}

void tp_pack_image_hash_cache_stats_get(const tp_pack_image_hash_cache *cache,
                                        tp_pack_image_hash_cache_stats *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof *out);
    if (!cache) {
        return;
    }
    out->decodes = cache->decodes;
    out->hits = cache->hits;
    out->misses = cache->misses;
    out->entries = (int)cache->count;
}

static char *dup_cstr(const char *s) {
    const size_t n = strlen(s) + 1U;
    char *copy = malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

static uint64_t path_bucket(const char *path) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Returns the slot index for `path` (existing or the first empty slot on the
 * probe chain). Assumes cap > 0 and at least one empty slot exists. */
static size_t img_cache_slot(const tp_pack_image_hash_cache *cache,
                             const char *path) {
    const size_t mask = cache->cap - 1U;
    size_t at = (size_t)path_bucket(path) & mask;
    while (cache->slots[at].path &&
           strcmp(cache->slots[at].path, path) != 0) {
        at = (at + 1U) & mask;
    }
    return at;
}

static bool img_cache_grow(tp_pack_image_hash_cache *cache) {
    const size_t new_cap = cache->cap ? cache->cap * 2U : 16U;
    img_cache_entry *slots = calloc(new_cap, sizeof *slots);
    if (!slots) {
        return false;
    }
    img_cache_entry *old = cache->slots;
    const size_t old_cap = cache->cap;
    cache->slots = slots;
    cache->cap = new_cap;
    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].path) {
            const size_t at = img_cache_slot(cache, old[i].path);
            cache->slots[at] = old[i];
        }
    }
    free(old);
    return true;
}

/* Resolves the semantic image hash for one sprite, decoding through the bounded
 * tp_image ingress and serving repeat calls from the fingerprint cache. */
static tp_status image_hash_for_sprite(tp_pack_image_hash_cache *cache,
                                       const tp_pack_sprite_desc *sprite,
                                       tp_id128 *out, tp_error *err) {
    if (!sprite->path) {
        /* Raw pixels: no file, no fingerprint -- hash straight from bytes. */
        *out = tp_pack_semantic_image_hash(sprite->w, sprite->h, sprite->rgba);
        return TP_STATUS_OK;
    }

    tp_fs_info info;
    if (!tp_fs_stat(sprite->path, &info) || info.kind != TP_FS_KIND_REGULAR) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack_hash: cannot stat sprite source '%s'",
                            sprite->path);
    }

    if (cache) {
        if (cache->count + 1U > (cache->cap ? cache->cap / 2U : 0U)) {
            if (!img_cache_grow(cache)) {
                return tp_error_set(err, TP_STATUS_OOM,
                                    "tp_pack_hash: image cache grow failed");
            }
        }
        const size_t at = img_cache_slot(cache, sprite->path);
        img_cache_entry *slot = &cache->slots[at];
        if (slot->path && slot->size == info.size && slot->mtime == info.mtime) {
            cache->hits++;
            *out = slot->hash;
            return TP_STATUS_OK;
        }
        cache->misses++;
        tp_image_rgba8 image = {0};
        tp_status st = tp_image_load_file(sprite->path, &image, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
        cache->decodes++;
        const tp_id128 hash =
            tp_pack_semantic_image_hash(image.width, image.height, image.pixels);
        tp_image_free(&image);
        if (!slot->path) {
            slot->path = dup_cstr(sprite->path);
            if (!slot->path) {
                return tp_error_set(err, TP_STATUS_OOM,
                                    "tp_pack_hash: image cache key alloc failed");
            }
            cache->count++;
        }
        slot->size = info.size;
        slot->mtime = info.mtime;
        slot->hash = hash;
        *out = hash;
        return TP_STATUS_OK;
    }

    /* No cache: decode and hash every call. */
    tp_image_rgba8 image = {0};
    tp_status st = tp_image_load_file(sprite->path, &image, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    *out = tp_pack_semantic_image_hash(image.width, image.height, image.pixels);
    tp_image_free(&image);
    return TP_STATUS_OK;
}

tp_status tp_pack_input_hash_compute(const tp_pack_settings *settings,
                                     const char *preview_exporter_id,
                                     tp_pack_image_hash_cache *cache,
                                     tp_id128 *out_hash, tp_error *err) {
    if (!settings || !out_hash) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_pack_hash: settings and out_hash are required");
    }
    *out_hash = tp_id128_nil();

    tp_hasher h = tp_hasher_init();

    /* Version byte FIRST, then reserved input categories in a fixed order. */
    hash_u8(&h, TP_PACK_INPUT_HASH_VERSION);

    hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_ALGO);
    hash_u32(&h, TP_PACK_ALGO_VERSION);

    hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_SETTINGS);
    /* atlas_name is a semantic result field (tp_result.atlas_name, export path);
     * work_dir is a runtime staging path and is intentionally NOT hashed. */
    hash_str_field(&h, settings->atlas_name);
    hash_u32(&h, (uint32_t)settings->max_size);
    hash_u32(&h, (uint32_t)settings->padding);
    hash_u32(&h, (uint32_t)settings->margin);
    hash_u32(&h, (uint32_t)settings->extrude);
    hash_u32(&h, (uint32_t)settings->alpha_threshold);
    hash_u32(&h, (uint32_t)settings->max_vertices);
    hash_u32(&h, (uint32_t)settings->shape);
    hash_u8(&h, settings->allow_transform ? 1U : 0U);
    hash_u8(&h, settings->power_of_two ? 1U : 0U);
    hash_f32(&h, settings->pixels_per_unit);

    hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_TARGET);
    hash_str_field(&h, preview_exporter_id);

    hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_SPRITE_LIST);
    hash_u32(&h, (uint32_t)settings->sprite_count);
    for (int i = 0; i < settings->sprite_count; i++) {
        const tp_pack_sprite_desc *sp = &settings->sprites[i];
        hash_u8(&h, (uint8_t)TP_PACK_HASH_CAT_SPRITE);
        /* Ordered identity: packing key, logical name, and structural source. */
        hash_str_field(&h, sp->name);
        hash_str_field(&h, sp->logical_name);
        tp_hasher_update(&h, sp->source_id.bytes, sizeof sp->source_id.bytes);
        hash_str_field(&h, sp->source_key);
        /* Effective per-sprite overrides + pivot + 9-slice (all result-visible). */
        hash_u8(&h, sp->ov_mask);
        hash_u8(&h, sp->ov_shape);
        hash_u8(&h, sp->ov_allow_rotate);
        hash_u8(&h, sp->ov_max_vertices);
        hash_u8(&h, sp->ov_margin);
        hash_u8(&h, sp->ov_extrude);
        hash_f32(&h, sp->origin_x);
        hash_f32(&h, sp->origin_y);
        for (int k = 0; k < 4; k++) {
            hash_u16(&h, sp->slice9_lrtb[k]);
        }
        /* semantic_image_hash: the pixel term. */
        tp_id128 image_hash;
        tp_status st = image_hash_for_sprite(cache, sp, &image_hash, err);
        if (st != TP_STATUS_OK) {
            return st; /* *out_hash stays nil */
        }
        tp_hasher_update(&h, image_hash.bytes, sizeof image_hash.bytes);
    }

    *out_hash = tp_hasher_final(h);
    return TP_STATUS_OK;
}
