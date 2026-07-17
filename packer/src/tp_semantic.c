/*
 * Semantic-state identity, computed SEPARATELY from the revision
 * counter (master spec §8, decision 0011 §4).
 * `dirty = current identity != saved-baseline
 * identity` -- NOT derived from the revision number, so applying the inverse of an
 * edit returns to clean even at a higher revision, and "mark saved" re-baselines
 * without changing revision.
 *
 * The identity is a deterministic, endian-stable 128-bit hash (the tp_id FNV-1a/128
 * seam -- byte-at-a-time, big-endian packing, NO libm, NO __int128) over the
 * PARTICIPATING persistent partition only. Numbers are hashed via their canonical
 * decimal / "%.9g" text (the same forms the serializer emits) so the identity is
 * byte-identical on every OS regardless of float/int representation.
 *
 * PARTICIPATES: atlas name + the 10 packing knobs +
 * id; source id + normalized key (path) + kind; sprite identity (display name +
 * source_ref + src_key) + origin + slice9 + rename + the five ov_* overrides;
 * animation id + name + fps + playback + flips + FRAMES; target exporter_id +
 * out_path + enabled + id; every structural id.
 * EXCLUDED runtime state (never dirty): the revision counter, dirty flag, Undo/Redo,
 * the saved baseline, session/authority, pack results/hashes, source watchers/mtime/
 * pixels, thumbnails, GUI view state + s_model_ver, the project file PATH (identity
 * key, not content).
 *
 * ORDER RULE (decision 0011 §4): ID-keyed collections
 * (atlases/sources/sprites/animations/
 * targets) are ORDER-NORMALIZED -- their per-element hashes are combined with a
 * COMMUTATIVE 128-bit sum, so a reorder does not change identity. The sole exception
 * is an animation's `frames`, whose order IS semantic (playback order): frames fold
 * in array order into the animation hash.
 *
 * A note on source `kind`: the field table enumerates {source_id, key}. This
 * implementation additionally folds `kind` (folder/file) -- a deliberate, minor,
 * conservative SUPERSET: kind is persistent serialized content whose change alters
 * packing and (for a missing source) sprite-id derivation, so over-detecting a change
 * is the safe error direction. Documented in docs/decisions/0011.
 */

#include "tp_core/tp_transaction.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_project.h"

/* ---- feed helpers: canonical, endian-stable field mixing ----------------- */

static void feed_bytes(tp_hasher *h, const void *p, size_t n) { tp_hasher_update(h, p, n); }

/* A field separator so "ab"+"c" and "a"+"bc" cannot alias across field bounds. */
static void feed_sep(tp_hasher *h) {
    static const unsigned char nul = 0;
    tp_hasher_update(h, &nul, 1);
}

static void feed_str(tp_hasher *h, const char *s) {
    if (s) {
        tp_hasher_update(h, s, strlen(s));
    }
    feed_sep(h); /* also distinguishes NULL from "" via presence of no bytes */
}

static void feed_i64(tp_hasher *h, int64_t v) {
    char b[32];
    (void)snprintf(b, sizeof b, "%" PRId64, v);
    feed_str(h, b);
}

static void feed_f(tp_hasher *h, double v) {
    char b[64];
    (void)snprintf(b, sizeof b, "%.9g", v); /* same byte form the serializer writes */
    feed_str(h, b);
}

static void feed_bool(tp_hasher *h, bool v) { feed_i64(h, v ? 1 : 0); }

/* id128 bytes are already canonical big-endian text order -- feeding them raw is
 * endian-stable. */
static void feed_id(tp_hasher *h, tp_id128 id) {
    feed_bytes(h, id.bytes, sizeof id.bytes);
    feed_sep(h);
}

/* ---- commutative 128-bit accumulate (order-normalized collections) -------- */

/* acc += val, big-endian (bytes[0] most-significant), carry LSB->MSB. */
static void id128_add(tp_id128 *acc, tp_id128 val) {
    unsigned carry = 0;
    for (int i = 15; i >= 0; i--) {
        unsigned s = (unsigned)acc->bytes[i] + (unsigned)val.bytes[i] + carry;
        acc->bytes[i] = (uint8_t)(s & 0xFFU);
        carry = s >> 8;
    }
}

/* ---- per-entity hashes --------------------------------------------------- */

static tp_id128 source_identity(const tp_project *project,
                                const tp_project_source *s) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "src");
    feed_id(&h, s->id);
    char canonical_path[TP_IDENTITY_PATH_MAX];
    const char *path = s->path;
    if ((project->source_base_dir &&
         tp_project_resolve_source_path(project, s->path, canonical_path,
                                        sizeof canonical_path) == TP_STATUS_OK) ||
        (!project->source_base_dir &&
         tp_identity_path_absolute_lexical(s->path, canonical_path,
                                           sizeof canonical_path,
                                           NULL) == TP_STATUS_OK)) {
        path = canonical_path;
    }
    feed_str(&h, path);
    feed_i64(&h, (int64_t)s->kind);
    return tp_hasher_final(h);
}

static tp_id128 sprite_identity(const tp_project_sprite *s) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "spr");
    feed_id(&h, s->source_ref);
    feed_str(&h, s->src_key);
    feed_f(&h, (double)s->origin_x);
    feed_f(&h, (double)s->origin_y);
    for (int k = 0; k < 4; k++) {
        feed_i64(&h, (int64_t)s->slice9_lrtb[k]);
    }
    feed_str(&h, s->rename);
    feed_i64(&h, (int64_t)s->ov_shape);
    feed_i64(&h, (int64_t)s->ov_allow_rotate);
    feed_i64(&h, (int64_t)s->ov_max_vertices);
    feed_i64(&h, (int64_t)s->ov_margin);
    feed_i64(&h, (int64_t)s->ov_extrude);
    return tp_hasher_final(h);
}

static tp_id128 anim_identity(const tp_project_anim *a) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "anm");
    feed_id(&h, a->id);
    feed_str(&h, a->name);
    feed_f(&h, (double)a->fps);
    feed_i64(&h, (int64_t)a->playback);
    feed_bool(&h, a->flip_h);
    feed_bool(&h, a->flip_v);
    /* frames are ORDER-SEMANTIC: fold in array (playback) order. */
    feed_i64(&h, (int64_t)a->frame_count);
    for (int i = 0; i < a->frame_count; i++) {
        feed_id(&h, a->frames[i].source_ref);
        feed_str(&h, a->frames[i].src_key);
    }
    return tp_hasher_final(h);
}

static tp_id128 target_identity(const tp_project_target *t) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "tgt");
    feed_id(&h, t->id);
    feed_str(&h, t->exporter_id);
    feed_str(&h, t->out_path);
    feed_bool(&h, t->enabled);
    return tp_hasher_final(h);
}

/* Fold an ID-keyed collection order-independently: sum the per-element hashes
 * (commutative) plus the element count, then mix into the parent hasher. */
#define FOLD_UNORDERED(H, TAG, COUNT, ELEM_ID_EXPR)          \
    do {                                                     \
        tp_id128 acc = tp_id128_nil();                       \
        for (int _i = 0; _i < (COUNT); _i++) {               \
            id128_add(&acc, (ELEM_ID_EXPR));                 \
        }                                                    \
        feed_str((H), (TAG));                                \
        feed_i64((H), (int64_t)(COUNT));                     \
        feed_id((H), acc);                                   \
    } while (0)

tp_id128 tp_semantic_atlas_identity(const tp_project *project,
                                    const tp_project_atlas *a) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "atl");
    feed_id(&h, a->id);
    feed_str(&h, a->name);
    feed_i64(&h, (int64_t)a->max_size);
    feed_i64(&h, (int64_t)a->padding);
    feed_i64(&h, (int64_t)a->margin);
    feed_i64(&h, (int64_t)a->extrude);
    feed_i64(&h, (int64_t)a->alpha_threshold);
    feed_i64(&h, (int64_t)a->max_vertices);
    feed_i64(&h, (int64_t)a->shape);
    feed_bool(&h, a->allow_transform);
    feed_bool(&h, a->power_of_two);
    feed_f(&h, (double)a->pixels_per_unit);
    FOLD_UNORDERED(&h, "sources", a->source_count,
                   source_identity(project, &a->sources[_i]));
    FOLD_UNORDERED(&h, "sprites", a->sprite_count, sprite_identity(&a->sprites[_i]));
    FOLD_UNORDERED(&h, "animations", a->animation_count, anim_identity(&a->animations[_i]));
    FOLD_UNORDERED(&h, "targets", a->target_count, target_identity(&a->targets[_i]));
    return tp_hasher_final(h);
}

tp_id128 tp_semantic_identity(const tp_project *p) {
    tp_hasher h = tp_hasher_init();
    feed_str(&h, "tp-project-identity/2"); /* canonical-ref algorithm tag */
    if (p) {
        FOLD_UNORDERED(&h, "atlases", p->atlas_count,
                       tp_semantic_atlas_identity(p, &p->atlases[_i]));
    } else {
        feed_str(&h, "atlases");
        feed_i64(&h, 0);
        feed_id(&h, tp_id128_nil());
    }
    /* project_dir is identity metadata, not semantic content. */
    return tp_hasher_final(h);
}
