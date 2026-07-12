#include "tp_core/tp_project_migrate.h"

#include <stdio.h>
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

/* Salt-sweep body shared by the hashed path and its O(n^2) fallback, so the sweep
 * policy (nil-skip, salt bound, exhaustion token, array order) lives in ONE place
 * and the two paths can never silently diverge. They differ ONLY in how a clash
 * with an already-assigned id is detected (`clashes`) and how a freshly assigned id
 * is recorded (`note`). Returns TP_STATUS_OK or exhaustion. */
typedef bool (*legacy_clash_fn)(void *ctx, size_t assigned, tp_id128 cand);
typedef void (*legacy_note_fn)(void *ctx, tp_id128 cand);

static tp_status legacy_salt_sweep(tp_legacy_entry *entries, size_t n, tp_legacy_hash_fn hash, void *hctx,
                                   legacy_clash_fn clashes, legacy_note_fn note, void *cctx, tp_error *err) {
    for (size_t i = 0; i < n; i++) {
        bool assigned = false;
        for (uint32_t salt = 0; salt <= (uint32_t)TP_LEGACY_MAX_SALT; salt++) {
            tp_id128 cand = hash(hctx, entries[i].kind, entries[i].tuple, salt);
            if (tp_id128_is_nil(cand)) {
                continue; /* nil is reserved -- bump salt */
            }
            if (clashes(cctx, i, cand)) {
                continue; /* clash with an already-assigned entry -- bump salt */
            }
            entries[i].id = cand;
            note(cctx, cand);
            assigned = true;
            break;
        }
        if (!assigned) {
            return tp_error_set(err, TP_STATUS_ID_COLLISION_EXHAUSTED, "legacy salt sweep exhausted for entry %zu", i);
        }
    }
    return TP_STATUS_OK;
}

/* Hashed path (O(n) total): probe + update the open-addressed set. */
typedef struct {
    tp_id128 *slots;
    size_t mask;
} legacy_hashed_ctx;
static bool legacy_clash_hashed(void *ctx, size_t assigned, tp_id128 cand) {
    (void)assigned;
    const legacy_hashed_ctx *h = (const legacy_hashed_ctx *)ctx;
    return legacy_set_contains(h->slots, h->mask, cand);
}
static void legacy_note_hashed(void *ctx, tp_id128 cand) {
    legacy_hashed_ctx *h = (legacy_hashed_ctx *)ctx;
    legacy_set_insert(h->slots, h->mask, cand);
}

/* O(n^2) fallback used only if the set allocation fails, so a memory-pressure load
 * never aborts and never changes the result (byte-identical to the hashed path:
 * same array order, same salt-bump sequence, same collision bound). Scans the
 * already-assigned prefix entries[0..assigned) and records nothing -- the entries
 * array the sweep just wrote is its own source of truth. */
static bool legacy_clash_linear(void *ctx, size_t assigned, tp_id128 cand) {
    const tp_legacy_entry *entries = (const tp_legacy_entry *)ctx;
    for (size_t j = 0; j < assigned; j++) {
        if (tp_id128_eq(entries[j].id, cand)) {
            return true;
        }
    }
    return false;
}
static void legacy_note_linear(void *ctx, tp_id128 cand) {
    (void)ctx;
    (void)cand;
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
        return legacy_salt_sweep(entries, n, hash, ctx, legacy_clash_linear, legacy_note_linear, entries, err);
    }
    legacy_hashed_ctx hc = {slots, cap - 1U};
    tp_status st = legacy_salt_sweep(entries, n, hash, ctx, legacy_clash_hashed, legacy_note_hashed, &hc, err);
    free(slots);
    return st;
}

/* ======================================================================== */
/* project-level identity migration / promotion                             */
/* ======================================================================== */

/* Count the structural IDs (atlas + animation + target) whose value is nil.
 * The nested iteration order (atlas, then its anims, then its targets) is the
 * SINGLE canonical order every pass below reuses. */
static size_t count_nil_ids(const tp_project *p) {
    size_t n = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (tp_id128_is_nil(a->id)) {
            n++;
        }
        for (int i = 0; i < a->animation_count; i++) {
            if (tp_id128_is_nil(a->animations[i].id)) {
                n++;
            }
        }
        for (int i = 0; i < a->target_count; i++) {
            if (tp_id128_is_nil(a->targets[i].id)) {
                n++;
            }
        }
    }
    return n;
}

/* malloc a canonical legacy tuple; NULL on OOM. C99 snprintf(NULL,0,..) sizes it. */
static char *tuple_fmt(const char *fmt, ...) TP_PRINTF_ATTR(1, 2);
static char *tuple_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        return NULL;
    }
    char *s = (char *)malloc((size_t)need + 1U);
    if (!s) {
        return NULL;
    }
    va_start(ap, fmt);
    (void)vsnprintf(s, (size_t)need + 1U, fmt, ap);
    va_end(ap);
    return s;
}

tp_status tp_project_assign_legacy_ids(tp_project *p, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_assign_legacy_ids: NULL project");
    }
    size_t n = count_nil_ids(p);
    if (n == 0) {
        return TP_STATUS_OK; /* every entity already carries an ID */
    }
    tp_legacy_entry *entries = (tp_legacy_entry *)calloc(n, sizeof *entries);
    tp_id128 **slots = (tp_id128 **)calloc(n, sizeof *slots); /* where each synthetic ID lands */
    if (!entries || !slots) {
        free(entries);
        free(slots);
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_assign_legacy_ids: out of memory");
    }

    /* Build the legacy discriminator tuples in canonical order. Tuples must be
     * stable across loads of the same file (position + stable fields). */
    size_t k = 0;
    bool oom = false;
    for (int ai = 0; ai < p->atlas_count && !oom; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (tp_id128_is_nil(a->id)) {
            entries[k].kind = TP_ID_KIND_ATLAS;
            entries[k].tuple = tuple_fmt("%d", ai);
            slots[k] = &a->id;
            oom = (entries[k].tuple == NULL);
            k++;
        }
        for (int i = 0; i < a->animation_count && !oom; i++) {
            if (tp_id128_is_nil(a->animations[i].id)) {
                entries[k].kind = TP_ID_KIND_ANIM;
                entries[k].tuple = tuple_fmt("%d|%s", ai, a->animations[i].name ? a->animations[i].name : "");
                slots[k] = &a->animations[i].id;
                oom = (entries[k].tuple == NULL);
                k++;
            }
        }
        for (int i = 0; i < a->target_count && !oom; i++) {
            if (tp_id128_is_nil(a->targets[i].id)) {
                entries[k].kind = TP_ID_KIND_TARGET;
                entries[k].tuple = tuple_fmt("%d|%s|%s", ai, a->targets[i].exporter_id ? a->targets[i].exporter_id : "",
                                             a->targets[i].out_path ? a->targets[i].out_path : "");
                slots[k] = &a->targets[i].id;
                oom = (entries[k].tuple == NULL);
                k++;
            }
        }
    }

    tp_status st = oom ? tp_error_set(err, TP_STATUS_OOM, "tp_project_assign_legacy_ids: out of memory")
                       : tp_legacy_assign(entries, n, NULL, NULL, err);
    if (st == TP_STATUS_OK) {
        for (size_t i = 0; i < n; i++) {
            *slots[i] = entries[i].id; /* commit synthetic IDs into the model */
        }
    }
    for (size_t i = 0; i < n; i++) {
        free((void *)entries[i].tuple); /* tuples were malloc'd by tuple_fmt */
    }
    free(entries);
    free(slots);
    return st;
}

tp_status tp_project_promote_ids(tp_project *p, const tp_rng *rng, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_promote_ids: NULL project");
    }
    size_t n = count_nil_ids(p);
    if (n == 0) {
        return TP_STATUS_OK; /* idempotent: nothing to promote -> never re-change an ID */
    }
    /* Stage all random IDs FIRST so an RNG fault leaves the model untouched
     * (atomicity: a save failure / partial promotion never remaps some IDs). */
    tp_id128 *staged = (tp_id128 *)calloc(n, sizeof *staged);
    if (!staged) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_promote_ids: out of memory");
    }
    for (size_t i = 0; i < n; i++) {
        tp_status st = tp_id128_generate(rng, &staged[i], err);
        if (st != TP_STATUS_OK) {
            free(staged);
            return st; /* every model ID unchanged */
        }
    }
    /* Commit in the same canonical order count_nil_ids walked. */
    size_t k = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (tp_id128_is_nil(a->id)) {
            a->id = staged[k++];
        }
        for (int i = 0; i < a->animation_count; i++) {
            if (tp_id128_is_nil(a->animations[i].id)) {
                a->animations[i].id = staged[k++];
            }
        }
        for (int i = 0; i < a->target_count; i++) {
            if (tp_id128_is_nil(a->targets[i].id)) {
                a->targets[i].id = staged[k++];
            }
        }
    }
    free(staged);
    return TP_STATUS_OK;
}

/* Duplicate detection over the FLAT list of structural IDs. Project sizes are
 * small; an O(n^2) sweep is fine and the flat array keeps the logic legible. */
tp_status tp_project_validate_ids(const tp_project *p, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_validate_ids: NULL project");
    }

    /* Flatten every structural ID + a human label, checking nil during the walk. */
    typedef struct {
        tp_id128 id;
        const char *label;
    } id_ref;

    size_t total = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        total += 1U + (size_t)p->atlases[ai].animation_count + (size_t)p->atlases[ai].target_count;
    }
    if (total == 0) {
        return TP_STATUS_OK;
    }
    id_ref *refs = (id_ref *)calloc(total, sizeof *refs);
    if (!refs) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_validate_ids: out of memory");
    }

    size_t n = 0;
    tp_status st = TP_STATUS_OK;
    for (int ai = 0; ai < p->atlas_count && st == TP_STATUS_OK; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (tp_id128_is_nil(a->id)) {
            st = tp_error_set(err, TP_STATUS_ID_MALFORMED, "atlas '%s' has a nil structural id",
                              a->name ? a->name : "");
            break;
        }
        refs[n].id = a->id;
        refs[n].label = a->name ? a->name : "atlas";
        n++;
        for (int i = 0; i < a->animation_count; i++) {
            if (tp_id128_is_nil(a->animations[i].id)) {
                st = tp_error_set(err, TP_STATUS_ID_MALFORMED, "animation '%s' has a nil structural id",
                                  a->animations[i].name ? a->animations[i].name : "");
                break;
            }
            refs[n].id = a->animations[i].id;
            refs[n].label = a->animations[i].name ? a->animations[i].name : "animation";
            n++;
        }
        if (st != TP_STATUS_OK) {
            break;
        }
        for (int i = 0; i < a->target_count; i++) {
            if (tp_id128_is_nil(a->targets[i].id)) {
                st = tp_error_set(err, TP_STATUS_ID_MALFORMED, "target '%s' has a nil structural id",
                                  a->targets[i].exporter_id ? a->targets[i].exporter_id : "");
                break;
            }
            refs[n].id = a->targets[i].id;
            refs[n].label = a->targets[i].exporter_id ? a->targets[i].exporter_id : "target";
            n++;
        }
    }

    for (size_t i = 0; i < n && st == TP_STATUS_OK; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (tp_id128_eq(refs[i].id, refs[j].id)) {
                st = tp_error_set(err, TP_STATUS_DUPLICATE_ID, "'%s' and '%s' share a structural id", refs[i].label,
                                  refs[j].label);
                break;
            }
        }
    }
    free(refs);
    return st;
}
