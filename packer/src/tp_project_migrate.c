#include "tp_core/tp_project_migrate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_sprite_index.h" /* lazy v3->v4 sprite re-keying uses the resolved index */

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

/* Count nil structural IDs. The nested iteration order (atlas, then its SOURCES,
 * then its anims, then its targets) is the SINGLE canonical order every pass below
 * reuses. `sources_only` restricts the count (and, in the assigner, the synthesis)
 * to source ids -- the v2->v3 migration path, where atlas/anim/target already carry
 * ids and a nil among them must stay an anomaly (decision 0008). */
static size_t count_nil_ids(const tp_project *p, bool sources_only) {
    size_t n = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (!sources_only && tp_id128_is_nil(a->id)) {
            n++;
        }
        for (int i = 0; i < a->source_count; i++) {
            if (tp_id128_is_nil(a->sources[i].id)) {
                n++;
            }
        }
        if (sources_only) {
            continue;
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

/* Shared implementation for the v1 (all-kinds) and v2->v3 (sources-only) legacy
 * synthesis. Builds discriminator tuples in the canonical order (atlas, sources,
 * anims, targets), assigns deterministic unique ids, and commits them. Tuples must
 * be stable across loads of the same file (position + stable fields). */
static tp_status assign_legacy_scoped(tp_project *p, bool sources_only, const char *who, tp_error *err) {
    size_t n = count_nil_ids(p, sources_only);
    if (n == 0) {
        return TP_STATUS_OK; /* every entity in scope already carries an ID */
    }
    tp_legacy_entry *entries = (tp_legacy_entry *)calloc(n, sizeof *entries);
    tp_id128 **slots = (tp_id128 **)calloc(n, sizeof *slots); /* where each synthetic ID lands */
    if (!entries || !slots) {
        free(entries);
        free(slots);
        return tp_error_set(err, TP_STATUS_OOM, "%s: out of memory", who);
    }

    size_t k = 0;
    bool oom = false;
    for (int ai = 0; ai < p->atlas_count && !oom; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (!sources_only && tp_id128_is_nil(a->id)) {
            entries[k].kind = TP_ID_KIND_ATLAS;
            entries[k].tuple = tuple_fmt("%d", ai);
            slots[k] = &a->id;
            oom = (entries[k].tuple == NULL);
            k++;
        }
        for (int i = 0; i < a->source_count && !oom; i++) {
            if (tp_id128_is_nil(a->sources[i].id)) {
                entries[k].kind = TP_ID_KIND_SOURCE;
                entries[k].tuple = tuple_fmt("%d|%s", ai, a->sources[i].path ? a->sources[i].path : "");
                slots[k] = &a->sources[i].id;
                oom = (entries[k].tuple == NULL);
                k++;
            }
        }
        if (sources_only) {
            continue;
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

    tp_status st = oom ? tp_error_set(err, TP_STATUS_OOM, "%s: out of memory", who)
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

tp_status tp_project_assign_legacy_ids(tp_project *p, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_assign_legacy_ids: NULL project");
    }
    return assign_legacy_scoped(p, false, "tp_project_assign_legacy_ids", err);
}

tp_status tp_project_assign_legacy_source_ids(tp_project *p, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_assign_legacy_source_ids: NULL project");
    }
    return assign_legacy_scoped(p, true, "tp_project_assign_legacy_source_ids", err);
}

tp_status tp_project_promote_ids(tp_project *p, const tp_rng *rng, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_promote_ids: NULL project");
    }
    size_t n = count_nil_ids(p, false);
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
    /* Commit in the same canonical order count_nil_ids walked (atlas, sources,
     * anims, targets). */
    size_t k = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (tp_id128_is_nil(a->id)) {
            a->id = staged[k++];
        }
        for (int i = 0; i < a->source_count; i++) {
            if (tp_id128_is_nil(a->sources[i].id)) {
                a->sources[i].id = staged[k++];
            }
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
/* Local strdup (no cross-CRT / POSIX strdup dependency). NULL on OOM or NULL input. */
static char *mig_strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1U;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

/* F2 ENTRY POINT (groundwork; decision 0009 "Область"). This lazy v3->v4 re-key is
 * currently exercised ONLY by tests, `inspect`, and `validate` -- it has NO production
 * caller yet, so real GUI/CLI projects keep their sprite/frame records in PENDING {name}
 * form and pack/export apply them by the name bridge exactly as before F1-03. The
 * production trigger (a writable session that scans, re-keys, then saves) and the switch
 * to id-based override APPLICATION both land in F2's op-layer (plan F2-01). Until then
 * this is dormant groundwork: correct, atomic, but not wired into any save path. */
tp_status tp_project_resolve_atlas_sprites(tp_project *p, int atlas_index, const struct tp_sprite_index *idx,
                                           tp_error *err) {
    if (!p || !idx) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_resolve_atlas_sprites: NULL project or index");
    }
    if (atlas_index < 0 || atlas_index >= p->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_project_resolve_atlas_sprites: atlas index %d out of range",
                            atlas_index);
    }
    tp_project_atlas *a = &p->atlases[atlas_index];

    /* ATOMICITY (fix [4]): stage every new {source_ref, src_key} into scratch -- doing
     * all strdups up front -- and mutate the model only after EVERY allocation has
     * succeeded. So an OOM on record k does not leave records 0..k-1 already re-keyed;
     * the model stays byte-unchanged (the header's all-or-nothing contract). Upper bound
     * = every override + every frame (a record re-keys at most once). key_slot/ref_slot
     * point INTO the records (the arrays are not resized during the scan, so stable). */
    size_t total = (size_t)a->sprite_count;
    for (int ai = 0; ai < a->animation_count; ai++) {
        total += (size_t)a->animations[ai].frame_count;
    }
    if (total == 0) {
        return TP_STATUS_OK;
    }
    typedef struct {
        char **key_slot;    /* &record.src_key */
        tp_id128 *ref_slot; /* &record.source_ref */
        char *new_key;      /* freshly strdup'd; owned by scratch until commit */
        tp_id128 new_ref;
    } rekey_stage;
    rekey_stage *stage = (rekey_stage *)calloc(total, sizeof *stage);
    if (!stage) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_resolve_atlas_sprites: out of memory");
    }
    size_t staged = 0;
    bool oom = false;

    for (int i = 0; i < a->sprite_count && !oom; i++) {
        tp_project_sprite *s = &a->sprites[i];
        const bool pending = tp_id128_is_nil(s->source_ref) || s->src_key == NULL;
        if (!pending || !s->name) {
            continue; /* already migrated (or a stored orphan) -- leave its identity intact */
        }
        int matches = 0;
        const tp_sprite_ref *r = tp_sprite_index_by_export_key(idx, s->name, &matches);
        if (matches != 1 || !r) {
            continue; /* 0 = soft orphan (keeps applying by name); >1 = ambiguous, never guessed */
        }
        char *k = mig_strdup(r->source_key);
        if (!k) {
            oom = true;
            break;
        }
        stage[staged] = (rekey_stage){&s->src_key, &s->source_ref, k, r->source_id};
        staged++;
        /* `name` stays the export-key bridge -- it already equals r->export_key (that
         * is how the record matched), so the name-based apply path is unchanged. */
    }
    /* Animation frame references re-key identically (a frame IS a sprite reference). */
    for (int ai = 0; ai < a->animation_count && !oom; ai++) {
        tp_project_anim *an = &a->animations[ai];
        for (int f = 0; f < an->frame_count && !oom; f++) {
            tp_project_frame *fr = &an->frames[f];
            const bool fpending = tp_id128_is_nil(fr->source_ref) || fr->src_key == NULL;
            if (!fpending || !fr->name) {
                continue;
            }
            int fmatches = 0;
            const tp_sprite_ref *r = tp_sprite_index_by_export_key(idx, fr->name, &fmatches);
            if (fmatches != 1 || !r) {
                continue;
            }
            char *k = mig_strdup(r->source_key);
            if (!k) {
                oom = true;
                break;
            }
            stage[staged] = (rekey_stage){&fr->src_key, &fr->source_ref, k, r->source_id};
            staged++;
        }
    }

    if (oom) {
        for (size_t j = 0; j < staged; j++) {
            free(stage[j].new_key); /* nothing committed to the model yet -- model unchanged */
        }
        free(stage);
        return tp_error_set(err, TP_STATUS_OOM, "tp_project_resolve_atlas_sprites: out of memory");
    }
    /* Commit: every strdup succeeded, so re-key the staged records in one sweep. */
    for (size_t j = 0; j < staged; j++) {
        free(*stage[j].key_slot); /* old src_key (NULL for a pending record) */
        *stage[j].key_slot = stage[j].new_key;
        *stage[j].ref_slot = stage[j].new_ref;
    }
    free(stage);
    return TP_STATUS_OK;
}

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
        total += 1U + (size_t)p->atlases[ai].source_count + (size_t)p->atlases[ai].animation_count +
                 (size_t)p->atlases[ai].target_count;
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
        for (int i = 0; i < a->source_count; i++) {
            if (tp_id128_is_nil(a->sources[i].id)) {
                st = tp_error_set(err, TP_STATUS_ID_MALFORMED, "source '%s' has a nil structural id",
                                  a->sources[i].path ? a->sources[i].path : "");
                break;
            }
            refs[n].id = a->sources[i].id;
            refs[n].label = a->sources[i].path ? a->sources[i].path : "source";
            n++;
        }
        if (st != TP_STATUS_OK) {
            break;
        }
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
