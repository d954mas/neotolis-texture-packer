#include "tp_c0/tp_c0_legacy.h"

#include <string.h>

/* Upper bound on the salt sweep. The default hash is 128-bit, so a real
 * collision run this long is astronomically unlikely; the bound exists so a
 * pathological injected hash fails cleanly instead of looping forever. */
#define TP_C0_LEGACY_MAX_SALT 1048576U /* 1<<20 */

tp_c0_id128 tp_c0_legacy_hash_default(void *ctx, tp_c0_id_kind kind, const char *tuple, uint32_t salt) {
    (void)ctx;
    static const char tag[4] = {'l', 'i', 'd', '1'}; /* versioned algorithm tag */
    uint8_t kind_byte = (uint8_t)kind;
    uint8_t sep = 0x00U;
    uint8_t salt_le[4] = {
        (uint8_t)(salt & 0xFFU),
        (uint8_t)((salt >> 8) & 0xFFU),
        (uint8_t)((salt >> 16) & 0xFFU),
        (uint8_t)((salt >> 24) & 0xFFU),
    };
    tp_c0_hasher h = tp_c0_hasher_init();
    tp_c0_hasher_update(&h, tag, sizeof tag);
    tp_c0_hasher_update(&h, &kind_byte, 1U);
    tp_c0_hasher_update(&h, &sep, 1U);
    if (tuple) {
        tp_c0_hasher_update(&h, tuple, strlen(tuple));
    }
    tp_c0_hasher_update(&h, &sep, 1U);
    tp_c0_hasher_update(&h, salt_le, sizeof salt_le);
    return tp_c0_hasher_final(h);
}

tp_c0_detail tp_c0_legacy_assign(tp_c0_legacy_entry *entries, size_t n, tp_c0_legacy_hash_fn hash, void *ctx,
                                 tp_error *err) {
    if (!entries && n > 0) {
        return tp_c0_fail(err, TP_C0_ERR_NULL_ARG, "entries is NULL");
    }
    if (!hash) {
        hash = tp_c0_legacy_hash_default;
    }
    for (size_t i = 0; i < n; i++) {
        entries[i].id = tp_c0_id128_nil();
    }
    for (size_t i = 0; i < n; i++) {
        bool assigned = false;
        for (uint32_t salt = 0; salt <= TP_C0_LEGACY_MAX_SALT; salt++) {
            tp_c0_id128 cand = hash(ctx, entries[i].kind, entries[i].tuple, salt);
            if (tp_c0_id128_is_nil(cand)) {
                continue; /* nil is reserved -- bump salt */
            }
            bool clash = false;
            for (size_t j = 0; j < i; j++) {
                if (tp_c0_id128_eq(entries[j].id, cand)) {
                    clash = true;
                    break;
                }
            }
            if (!clash) {
                entries[i].id = cand;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            return tp_c0_fail(err, TP_C0_ERR_COLLISION_EXHAUSTED,
                              "salt sweep exhausted for entry %zu", i);
        }
    }
    return TP_C0_OK;
}
