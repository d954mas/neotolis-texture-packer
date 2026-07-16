#include "tp_core/tp_project_migrate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_sprite_index.h" /* lazy v3->v4 sprite re-keying uses the resolved index */
#include "tp_project_internal.h"      /* component-local complexity probes */
#include "tp_strutil.h"              /* shared tp_strdup (one core definition, fix [8]) */

static _Thread_local bool s_test_measure_id_validation;
static _Thread_local size_t s_test_id_validation_probes;
static _Thread_local bool s_test_measure_legacy_ids;
static _Thread_local size_t s_test_legacy_id_probes;
static _Thread_local bool s_test_fail_legacy_id_index_alloc;

void tp_project__test_id_validation_work_reset(void) {
    s_test_id_validation_probes = 0U;
    s_test_measure_id_validation = true;
}

size_t tp_project__test_id_validation_work_take(void) {
    s_test_measure_id_validation = false;
    return s_test_id_validation_probes;
}

void tp_project__test_legacy_id_work_reset(void) {
    s_test_legacy_id_probes = 0U;
    s_test_measure_legacy_ids = true;
}

size_t tp_project__test_legacy_id_work_take(void) {
    s_test_measure_legacy_ids = false;
    return s_test_legacy_id_probes;
}

void tp_project__test_fail_next_legacy_id_index_alloc(void) {
    s_test_fail_legacy_id_index_alloc = true;
}

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

#define TP_LEGACY_ID_MAX_PROBES 64U

/* Assign in canonical entry order. The table is at most half full and each
 * candidate performs bounded work; memory pressure fails instead of falling
 * back to a quadratic scan. */
static tp_status legacy_salt_sweep(tp_legacy_entry *entries, size_t n,
                                   tp_legacy_hash_fn hash, void *hctx,
                                   tp_id128 *slots, size_t mask,
                                   tp_error *err) {
    for (size_t i = 0; i < n; i++) {
        bool assigned = false;
        for (uint32_t salt = 0; salt <= (uint32_t)TP_LEGACY_MAX_SALT; salt++) {
            tp_id128 cand = hash(hctx, entries[i].kind, entries[i].tuple, salt);
            if (tp_id128_is_nil(cand)) {
                continue; /* nil is reserved -- bump salt */
            }
            size_t slot = (size_t)tp_id128_bucket(cand) & mask;
            bool resolved = false;
            for (size_t probe = 0U;
                 probe < (size_t)TP_LEGACY_ID_MAX_PROBES; probe++) {
                if (s_test_measure_legacy_ids) {
                    s_test_legacy_id_probes++;
                }
                if (tp_id128_is_nil(slots[slot])) {
                    slots[slot] = cand;
                    entries[i].id = cand;
                    assigned = true;
                    resolved = true;
                    break;
                }
                if (tp_id128_eq(slots[slot], cand)) {
                    resolved = true; /* collision: retry this entry at next salt */
                    break;
                }
                slot = (slot + 1U) & mask;
            }
            if (!resolved) {
                return tp_error_set(
                    err, TP_STATUS_OUT_OF_BOUNDS,
                    "legacy structural id lookup exceeds the work limit");
            }
            if (assigned) {
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
    if (n > SIZE_MAX / 2U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "too many legacy structural ids");
    }
    size_t cap = 16U;
    const size_t needed = n * 2U;
    while (cap < needed) {
        if (cap > SIZE_MAX / 2U) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "too many legacy structural ids");
        }
        cap *= 2U;
    }
    const bool fail_alloc = s_test_fail_legacy_id_index_alloc;
    s_test_fail_legacy_id_index_alloc = false;
    tp_id128 *slots = fail_alloc ? NULL : (tp_id128 *)calloc(cap, sizeof *slots);
    if (!slots) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_legacy_assign: out of memory");
    }
    tp_status st = legacy_salt_sweep(entries, n, hash, ctx, slots, cap - 1U,
                                     err);
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
    bool **synth = (bool **)calloc(n, sizeof *synth);         /* the id_synthetic flag beside each slot */
    if (!entries || !slots || !synth) {
        free(entries);
        free(slots);
        free(synth);
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
            synth[k] = &a->id_synthetic;
            oom = (entries[k].tuple == NULL);
            k++;
        }
        for (int i = 0; i < a->source_count && !oom; i++) {
            if (tp_id128_is_nil(a->sources[i].id)) {
                entries[k].kind = TP_ID_KIND_SOURCE;
                entries[k].tuple = tuple_fmt("%d|%s", ai, a->sources[i].path ? a->sources[i].path : "");
                slots[k] = &a->sources[i].id;
                synth[k] = &a->sources[i].id_synthetic;
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
                synth[k] = &a->animations[i].id_synthetic;
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
                synth[k] = &a->targets[i].id_synthetic;
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
            *synth[i] = true;          /* mark synthesized: the first writable promote re-randomizes it (§5.5) */
        }
    }
    for (size_t i = 0; i < n; i++) {
        free((void *)entries[i].tuple); /* tuples were malloc'd by tuple_fmt */
    }
    free(entries);
    free(slots);
    free(synth);
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

/* A structural entity gets a fresh random id at writable promote when its id is NIL
 * (a freshly-created entity) OR was SYNTHESIZED by the loader for a legacy gap. §5.5:
 * a migrated project's first writable save persists fresh RANDOM ids, not the stable
 * synthetic ones. A REAL loaded id (v3/v4, or a v2 file's atlas/anim/target id) has
 * id_synthetic == false and is left UNTOUCHED -- per-entity granularity is why a v2
 * file re-randomizes only its synthesized SOURCE ids while keeping its real ids. */
static bool id_needs_promote(tp_id128 id, bool synthetic) { return tp_id128_is_nil(id) || synthetic; }

/* Count the entities a writable promote must (re)assign, in the SAME canonical order
 * count_nil_ids walks (atlas, its sources, its anims, its targets). */
static size_t count_promote_ids(const tp_project *p) {
    size_t n = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (id_needs_promote(a->id, a->id_synthetic)) {
            n++;
        }
        for (int i = 0; i < a->source_count; i++) {
            if (id_needs_promote(a->sources[i].id, a->sources[i].id_synthetic)) {
                n++;
            }
        }
        for (int i = 0; i < a->animation_count; i++) {
            if (id_needs_promote(a->animations[i].id, a->animations[i].id_synthetic)) {
                n++;
            }
        }
        for (int i = 0; i < a->target_count; i++) {
            if (id_needs_promote(a->targets[i].id, a->targets[i].id_synthetic)) {
                n++;
            }
        }
    }
    return n;
}

tp_status tp_project_promote_ids(tp_project *p, const tp_rng *rng, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_promote_ids: NULL project");
    }
    size_t n = count_promote_ids(p);
    if (n == 0) {
        return TP_STATUS_OK; /* idempotent: nothing nil or synthetic remains -> never re-change an ID */
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
    /* Commit in the same canonical order count_promote_ids walked (atlas, sources,
     * anims, targets). Clearing id_synthetic makes a second promote a no-op. */
    size_t k = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        tp_project_atlas *a = &p->atlases[ai];
        if (id_needs_promote(a->id, a->id_synthetic)) {
            a->id = staged[k++];
            a->id_synthetic = false;
        }
        for (int i = 0; i < a->source_count; i++) {
            if (id_needs_promote(a->sources[i].id, a->sources[i].id_synthetic)) {
                a->sources[i].id = staged[k++];
                a->sources[i].id_synthetic = false;
            }
        }
        for (int i = 0; i < a->animation_count; i++) {
            if (id_needs_promote(a->animations[i].id, a->animations[i].id_synthetic)) {
                a->animations[i].id = staged[k++];
                a->animations[i].id_synthetic = false;
            }
        }
        for (int i = 0; i < a->target_count; i++) {
            if (id_needs_promote(a->targets[i].id, a->targets[i].id_synthetic)) {
                a->targets[i].id = staged[k++];
                a->targets[i].id_synthetic = false;
            }
        }
    }
    free(staged);
    return TP_STATUS_OK;
}

/* Legacy v3/v4 re-key primitive (decision 0009). Session open/snapshot load/save
 * call it on a project-level clone, then swap only after every atlas succeeds.
 * A uniquely resolved record becomes canonical; a missing record remains an
 * inert orphan; ambiguity is a structured failure and commits nothing. */
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
    tp_status resolve_status = TP_STATUS_OK;

    for (int i = 0; i < a->sprite_count && !oom; i++) {
        tp_project_sprite *s = &a->sprites[i];
        const bool pending = tp_id128_is_nil(s->source_ref) || s->src_key == NULL;
        if (!pending || !s->name) {
            continue; /* already migrated (or a stored orphan) -- leave its identity intact */
        }
        int matches = 0;
        const tp_sprite_ref *r = tp_sprite_index_by_export_key(idx, s->name, &matches);
        if (matches == 0 || !r) {
            continue; /* inert legacy orphan; no name fallback applies it */
        }
        if (matches > 1) {
            resolve_status = tp_error_set(
                err, TP_STATUS_INVALID_ARGUMENT,
                "legacy sprite reference '%s' is ambiguous (%d candidates)",
                s->name, matches);
            break;
        }
        char *k = tp_strdup(r->source_key);
        if (!k) {
            oom = true;
            break;
        }
        stage[staged] = (rekey_stage){&s->src_key, &s->source_ref, k, r->source_id};
        staged++;
        /* `name` remains display/migration metadata. Normal apply is keyed only by
         * the canonical pair staged above. */
    }
    /* Animation frame references re-key identically (a frame IS a sprite reference). */
    for (int ai = 0; ai < a->animation_count && !oom && resolve_status == TP_STATUS_OK; ai++) {
        tp_project_anim *an = &a->animations[ai];
        for (int f = 0; f < an->frame_count && !oom; f++) {
            tp_project_frame *fr = &an->frames[f];
            const bool fpending = tp_id128_is_nil(fr->source_ref) || fr->src_key == NULL;
            if (!fpending || !fr->name) {
                continue;
            }
            int fmatches = 0;
            const tp_sprite_ref *r = tp_sprite_index_by_export_key(idx, fr->name, &fmatches);
            if (fmatches == 0 || !r) {
                continue; /* inert orphan until one sprite reappears */
            }
            if (fmatches > 1) {
                resolve_status = tp_error_set(
                    err, TP_STATUS_INVALID_ARGUMENT,
                    "legacy animation frame '%s' is ambiguous (%d candidates)",
                    fr->name, fmatches);
                break;
            }
            char *k = tp_strdup(r->source_key);
            if (!k) {
                oom = true;
                break;
            }
            stage[staged] = (rekey_stage){&fr->src_key, &fr->source_ref, k, r->source_id};
            staged++;
        }
    }

    if (oom || resolve_status != TP_STATUS_OK) {
        for (size_t j = 0; j < staged; j++) {
            free(stage[j].new_key); /* nothing committed to the model yet -- model unchanged */
        }
        free(stage);
        return oom ? tp_error_set(err, TP_STATUS_OOM,
                                  "tp_project_resolve_atlas_sprites: out of memory")
                   : resolve_status;
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

bool tp_project_has_pending_sprite_refs(const tp_project *project) {
    for (int ai = 0; project && ai < project->atlas_count; ai++) {
        const tp_project_atlas *atlas = &project->atlases[ai];
        for (int i = 0; i < atlas->sprite_count; i++) {
            if (tp_id128_is_nil(atlas->sprites[i].source_ref) ||
                !atlas->sprites[i].src_key) {
                return true;
            }
        }
        for (int i = 0; i < atlas->animation_count; i++) {
            const tp_project_anim *animation = &atlas->animations[i];
            for (int f = 0; f < animation->frame_count; f++) {
                if (tp_id128_is_nil(animation->frames[f].source_ref) ||
                    !animation->frames[f].src_key) {
                    return true;
                }
            }
        }
    }
    return false;
}

tp_status tp_project_migrate_sprite_refs(tp_project *project, tp_error *err) {
    if (!project) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_migrate_sprite_refs: NULL project");
    }
    if (!tp_project_has_pending_sprite_refs(project)) {
        return TP_STATUS_OK;
    }
    tp_project *candidate = tp_project_clone(project);
    if (!candidate) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "legacy reference migration clone failed");
    }
    for (int ai = 0; ai < candidate->atlas_count; ai++) {
        tp_sprite_index index;
        tp_status status = tp_sprite_index_build(candidate, ai, &index, err);
        if (status != TP_STATUS_OK) {
            tp_project_destroy(candidate);
            return status;
        }
        status = tp_project_resolve_atlas_sprites(candidate, ai, &index, err);
        tp_sprite_index_free(&index);
        if (status != TP_STATUS_OK) {
            tp_project_destroy(candidate);
            return status;
        }
    }
    const tp_project old = *project;
    *project = *candidate;
    *candidate = old;
    tp_project_destroy(candidate);
    return TP_STATUS_OK;
}

#define TP_ID_VALIDATION_MAX_PROBES 64U

typedef struct tp_project_id_ref {
    tp_id128 id;
    const char *label;
} tp_project_id_ref;

static tp_status validate_unique_ids(const tp_project_id_ref *refs, size_t count,
                                     tp_error *err) {
    if (count == 0U) {
        return TP_STATUS_OK;
    }
    if (count > (size_t)UINT32_MAX || count > SIZE_MAX / 2U) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "too many structural ids to validate");
    }
    size_t capacity = 16U;
    const size_t needed = count * 2U;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2U) {
            return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                "too many structural ids to validate");
        }
        capacity *= 2U;
    }
    uint32_t *slots = (uint32_t *)calloc(capacity, sizeof *slots);
    if (!slots) {
        return tp_error_set(err, TP_STATUS_OOM,
                            "tp_project_validate_ids: out of memory");
    }

    size_t duplicate_first = SIZE_MAX;
    size_t duplicate_second = SIZE_MAX;
    tp_status status = TP_STATUS_OK;
    for (size_t i = 0U; i < count; i++) {
        size_t slot_index =
            (size_t)tp_id128_bucket(refs[i].id) & (capacity - 1U);
        bool resolved = false;
        for (size_t probe = 0U;
             probe < capacity && probe < (size_t)TP_ID_VALIDATION_MAX_PROBES;
             probe++) {
            if (s_test_measure_id_validation) {
                s_test_id_validation_probes++;
            }
            const uint32_t existing = slots[slot_index];
            if (existing == 0U) {
                slots[slot_index] = (uint32_t)(i + 1U);
                resolved = true;
                break;
            }
            const size_t first = (size_t)existing - 1U;
            if (tp_id128_eq(refs[first].id, refs[i].id)) {
                if (first < duplicate_first) {
                    duplicate_first = first;
                    duplicate_second = i;
                }
                resolved = true;
                break;
            }
            slot_index = (slot_index + 1U) & (capacity - 1U);
        }
        if (!resolved) {
            status = tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                                  "structural id lookup exceeds the work limit");
            break;
        }
    }
    if (status == TP_STATUS_OK && duplicate_first != SIZE_MAX) {
        status = tp_error_set(
            err, TP_STATUS_DUPLICATE_ID,
            "'%s' and '%s' share a structural id", refs[duplicate_first].label,
            refs[duplicate_second].label);
    }
    free(slots);
    return status;
}

tp_status tp_project_validate_ids(const tp_project *p, tp_error *err) {
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_validate_ids: NULL project");
    }

    /* Flatten every structural ID + a human label, checking nil during the walk. */
    size_t total = 0;
    for (int ai = 0; ai < p->atlas_count; ai++) {
        total += 1U + (size_t)p->atlases[ai].source_count + (size_t)p->atlases[ai].animation_count +
                 (size_t)p->atlases[ai].target_count;
    }
    if (total == 0) {
        return TP_STATUS_OK;
    }
    tp_project_id_ref *refs =
        (tp_project_id_ref *)calloc(total, sizeof *refs);
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

    if (st == TP_STATUS_OK) {
        st = validate_unique_ids(refs, n, err);
    }
    free(refs);
    return st;
}
