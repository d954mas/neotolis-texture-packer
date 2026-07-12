#include "tp_core/tp_project_migrate.h"

#include <stdlib.h>
#include <string.h>

/* ======================================================================== */
/* deterministic legacy assigner (promoted from C0-01 tp_c0_legacy)          */
/* ======================================================================== */

/* Upper bound on the salt sweep. The base hash is 128-bit, so a real collision
 * run this long is astronomically unlikely; the bound exists so a pathological
 * injected hash fails cleanly instead of looping forever. Enum (not a const
 * size_t) keeps it out of any array bound -- macos -Wgnu-folding-constant. */
enum { TP_LEGACY_MAX_SALT = 1048576 }; /* 1<<20 */

tp_id128 tp_legacy_hash_default(void *ctx, tp_id_kind kind, const char *tuple, uint32_t salt) {
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
    tp_hasher h = tp_hasher_init();
    tp_hasher_update(&h, tag, sizeof tag);
    tp_hasher_update(&h, &kind_byte, 1U);
    tp_hasher_update(&h, &sep, 1U);
    if (tuple) {
        tp_hasher_update(&h, tuple, strlen(tuple));
    }
    tp_hasher_update(&h, &sep, 1U);
    tp_hasher_update(&h, salt_le, sizeof salt_le);
    return tp_hasher_final(h);
}

/* Open-addressed (linear-probe) set of already-assigned IDs, keyed by
 * tp_id128_bucket(). An empty slot is a nil ID -- nil is reserved and never
 * inserted, so a zeroed table is an empty set. `mask` == capacity-1 (capacity is
 * a power of two >= 2n), so the load factor stays <= 0.5: a free slot always
 * exists and probing terminates. */
static bool legacy_set_contains(const tp_id128 *slots, size_t mask, tp_id128 id) {
    size_t idx = (size_t)tp_id128_bucket(id) & mask;
    for (;;) {
        if (tp_id128_is_nil(slots[idx])) {
            return false;
        }
        if (tp_id128_eq(slots[idx], id)) {
            return true;
        }
        idx = (idx + 1U) & mask;
    }
}

static void legacy_set_insert(tp_id128 *slots, size_t mask, tp_id128 id) {
    size_t idx = (size_t)tp_id128_bucket(id) & mask;
    while (!tp_id128_is_nil(slots[idx])) {
        idx = (idx + 1U) & mask;
    }
    slots[idx] = id;
}

/* Salt-sweep body shared by both paths, differing only in how a clash with an
 * already-assigned entry is detected. Returns TP_STATUS_OK or exhaustion. */
static tp_status legacy_assign_hashed(tp_legacy_entry *entries, size_t n, tp_legacy_hash_fn hash, void *ctx,
                                      tp_id128 *slots, size_t mask, tp_error *err) {
    for (size_t i = 0; i < n; i++) {
        bool assigned = false;
        for (uint32_t salt = 0; salt <= (uint32_t)TP_LEGACY_MAX_SALT; salt++) {
            tp_id128 cand = hash(ctx, entries[i].kind, entries[i].tuple, salt);
            if (tp_id128_is_nil(cand)) {
                continue; /* nil is reserved -- bump salt */
            }
            if (legacy_set_contains(slots, mask, cand)) {
                continue; /* clash with an already-assigned entry -- bump salt */
            }
            entries[i].id = cand;
            legacy_set_insert(slots, mask, cand);
            assigned = true;
            break;
        }
        if (!assigned) {
            return tp_error_set(err, TP_STATUS_ID_COLLISION_EXHAUSTED, "legacy salt sweep exhausted for entry %zu", i);
        }
    }
    return TP_STATUS_OK;
}

/* O(n^2) fallback used only if the set allocation fails, so a memory-pressure
 * load never aborts and never changes the result (byte-identical to the hashed
 * path: same array order, same salt-bump sequence, same collision bound). */
static tp_status legacy_assign_linear(tp_legacy_entry *entries, size_t n, tp_legacy_hash_fn hash, void *ctx,
                                      tp_error *err) {
    for (size_t i = 0; i < n; i++) {
        bool assigned = false;
        for (uint32_t salt = 0; salt <= (uint32_t)TP_LEGACY_MAX_SALT; salt++) {
            tp_id128 cand = hash(ctx, entries[i].kind, entries[i].tuple, salt);
            if (tp_id128_is_nil(cand)) {
                continue;
            }
            bool clash = false;
            for (size_t j = 0; j < i; j++) {
                if (tp_id128_eq(entries[j].id, cand)) {
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
            return tp_error_set(err, TP_STATUS_ID_COLLISION_EXHAUSTED, "legacy salt sweep exhausted for entry %zu", i);
        }
    }
    return TP_STATUS_OK;
}

tp_status tp_legacy_assign(tp_legacy_entry *entries, size_t n, tp_legacy_hash_fn hash, void *ctx, tp_error *err) {
    if (!entries && n > 0) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_legacy_assign: entries is NULL");
    }
    if (!hash) {
        hash = tp_legacy_hash_default;
    }
    for (size_t i = 0; i < n; i++) {
        entries[i].id = tp_id128_nil();
    }
    if (n == 0) {
        return TP_STATUS_OK;
    }
    size_t cap = 1;
    while (cap < n * 2U) {
        cap <<= 1; /* smallest power of two >= 2n */
    }
    tp_id128 *slots = calloc(cap, sizeof *slots); /* zeroed == all-nil == empty */
    if (!slots) {
        return legacy_assign_linear(entries, n, hash, ctx, err);
    }
    tp_status st = legacy_assign_hashed(entries, n, hash, ctx, slots, cap - 1U, err);
    free(slots);
    return st;
}
