#include "tp_core/tp_selector.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_project.h"
#include "tp_core/tp_sprite_index.h"

/* ------------------------------------------------------------------ */
/* candidate vector                                                   */
/* ------------------------------------------------------------------ */

void tp_selector_candidates_free(tp_selector_candidates *c) {
    if (!c) {
        return;
    }
    free(c->v);
    c->v = NULL;
    c->count = 0;
    c->cap = 0;
    c->oom = false;
}

const char *tp_selector_kind_token(tp_selector_kind kind) {
    switch (kind) {
        case TP_SEL_ATLAS: return "atlas";
        case TP_SEL_SOURCE: return "source";
        case TP_SEL_ANIM: return "anim";
        case TP_SEL_TARGET: return "target";
        case TP_SEL_SPRITE: return "sprite";
        case TP_SEL_NONE: return "";
    }
    return "";
}

/* A collector holds the growable candidate list AND a single "last" result so the
 * exactly-one case can be answered without re-scanning. Candidates are ONLY
 * materialized when a second match arrives (kept cheap for the common single hit). */
typedef struct {
    tp_selector_candidates *cands; /* may be NULL (caller does not want the list) */
    tp_selector_result last;       /* the most recent match */
    char last_idtext[TP_ID_TEXT_CAP];
    char last_label[256];
    int count;
    bool oom;
} collector;

static void fmt_id_text(tp_selector_kind kind, tp_id128 id, char *out, size_t cap) {
    if (kind == TP_SEL_SPRITE) {
        tp_sprite_id_format(id, out, cap);
        return;
    }
    tp_id_kind ik = TP_ID_KIND_INVALID;
    switch (kind) {
        case TP_SEL_ATLAS: ik = TP_ID_KIND_ATLAS; break;
        case TP_SEL_SOURCE: ik = TP_ID_KIND_SOURCE; break;
        case TP_SEL_ANIM: ik = TP_ID_KIND_ANIM; break;
        case TP_SEL_TARGET: ik = TP_ID_KIND_TARGET; break;
        default: break;
    }
    if (ik == TP_ID_KIND_INVALID || tp_id_format(ik, id, out, cap, NULL) != TP_STATUS_OK) {
        out[0] = '\0';
    }
}

static void cand_push(tp_selector_candidates *cands, tp_selector_kind kind, tp_id128 id, int atlas_index,
                      const char *idtext, const char *label) {
    if (cands->oom) {
        return;
    }
    if (cands->count == cands->cap) {
        int nc = cands->cap ? cands->cap * 2 : 8;
        tp_selector_candidate *nv = (tp_selector_candidate *)realloc(cands->v, (size_t)nc * sizeof *nv);
        if (!nv) {
            cands->oom = true;
            return;
        }
        cands->v = nv;
        cands->cap = nc;
    }
    tp_selector_candidate *c = &cands->v[cands->count++];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    c->id = id;
    c->atlas_index = atlas_index;
    (void)snprintf(c->idtext, sizeof c->idtext, "%s", idtext);
    (void)snprintf(c->label, sizeof c->label, "%s", label);
}

/* Record one match. The first match is stashed in `last`; the second flushes both
 * `last` and the new match into the candidate list, and every further match appends.
 * Order is the traversal order of the project (stable). */
static void match(collector *co, tp_selector_kind kind, tp_id128 id, int atlas_index, const char *label) {
    char idtext[TP_ID_TEXT_CAP];
    fmt_id_text(kind, id, idtext, sizeof idtext);
    co->count++;
    if (co->count == 1) {
        co->last.kind = kind;
        co->last.id = id;
        co->last.atlas_index = atlas_index;
        (void)snprintf(co->last_idtext, sizeof co->last_idtext, "%s", idtext);
        (void)snprintf(co->last_label, sizeof co->last_label, "%s", label);
        return;
    }
    if (!co->cands) {
        return; /* caller does not want the list; count still tracks ambiguity */
    }
    if (co->count == 2) {
        cand_push(co->cands, co->last.kind, co->last.id, co->last.atlas_index, co->last_idtext, co->last_label);
    }
    cand_push(co->cands, kind, id, atlas_index, idtext, label);
    if (co->cands->oom) {
        co->oom = true;
    }
}

/* ------------------------------------------------------------------ */
/* per-kind match collection                                          */
/* ------------------------------------------------------------------ */

static bool want(tp_selector_kind scope, tp_selector_kind kind) {
    return scope == TP_SEL_NONE || scope == kind;
}

static void collect_sprites(collector *co, const tp_project_atlas *a, const tp_sprite_index *sprites, int atlas_index,
                            const char *rest) {
    if (!sprites) {
        return;
    }
    for (int i = 0; i < sprites->count; i++) {
        const tp_sprite_ref *r = &sprites->refs[i];
        if (strcmp(r->export_key, rest) == 0 || strcmp(r->source_key, rest) == 0) {
            char label[256];
            const char *spath =
                (r->source_index >= 0 && r->source_index < a->source_count) ? a->sources[r->source_index].path : "";
            (void)snprintf(label, sizeof label, "sprite '%s' from source '%s'", r->export_key, spath ? spath : "");
            match(co, TP_SEL_SPRITE, r->sprite_id, atlas_index, label);
        }
    }
}

/* Collect matches for `rest` under `scope` (TP_SEL_NONE = every kind). Sprites are
 * searched only in `sprites` (its atlas == sprite_atlas_index). */
static void collect_matches(collector *co, const tp_project *p, const tp_sprite_index *sprites, int sprite_atlas_index,
                            tp_selector_kind scope, const char *rest) {
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (want(scope, TP_SEL_ATLAS) && a->name && strcmp(a->name, rest) == 0) {
            char label[256];
            (void)snprintf(label, sizeof label, "atlas '%s'", a->name);
            match(co, TP_SEL_ATLAS, a->id, ai, label);
        }
        if (want(scope, TP_SEL_SOURCE)) {
            for (int i = 0; i < a->source_count; i++) {
                if (a->sources[i].path && strcmp(a->sources[i].path, rest) == 0) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "source '%s' in atlas '%s'", a->sources[i].path,
                                   a->name ? a->name : "");
                    match(co, TP_SEL_SOURCE, a->sources[i].id, ai, label);
                }
            }
        }
        if (want(scope, TP_SEL_ANIM)) {
            for (int i = 0; i < a->animation_count; i++) {
                if (a->animations[i].name && strcmp(a->animations[i].name, rest) == 0) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "animation '%s' in atlas '%s'", a->animations[i].name,
                                   a->name ? a->name : "");
                    match(co, TP_SEL_ANIM, a->animations[i].id, ai, label);
                }
            }
        }
        if (want(scope, TP_SEL_TARGET)) {
            for (int i = 0; i < a->target_count; i++) {
                if (a->targets[i].out_path && strcmp(a->targets[i].out_path, rest) == 0) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "target '%s' (%s) in atlas '%s'", a->targets[i].out_path,
                                   a->targets[i].exporter_id ? a->targets[i].exporter_id : "", a->name ? a->name : "");
                    match(co, TP_SEL_TARGET, a->targets[i].id, ai, label);
                }
            }
        }
        if (want(scope, TP_SEL_SPRITE) && ai == sprite_atlas_index) {
            collect_sprites(co, a, sprites, ai, rest);
        }
    }
}

/* Sprite-in-source compound ("source_<hex>:<key>"): only sprites owned by `source_id`
 * whose export/source key equals `rest`. */
static void collect_sprite_in_source(collector *co, const tp_project *p, const tp_sprite_index *sprites,
                                     int sprite_atlas_index, tp_id128 source_id, const char *rest) {
    if (!sprites || sprite_atlas_index < 0 || sprite_atlas_index >= p->atlas_count) {
        return;
    }
    const tp_project_atlas *a = &p->atlases[sprite_atlas_index];
    for (int i = 0; i < sprites->count; i++) {
        const tp_sprite_ref *r = &sprites->refs[i];
        if (!tp_id128_eq(r->source_id, source_id)) {
            continue;
        }
        if (strcmp(r->export_key, rest) == 0 || strcmp(r->source_key, rest) == 0) {
            char label[256];
            const char *spath =
                (r->source_index >= 0 && r->source_index < a->source_count) ? a->sources[r->source_index].path : "";
            (void)snprintf(label, sizeof label, "sprite '%s' from source '%s'", r->export_key, spath ? spath : "");
            match(co, TP_SEL_SPRITE, r->sprite_id, sprite_atlas_index, label);
        }
    }
}

/* ------------------------------------------------------------------ */
/* canonical-id fast path                                             */
/* ------------------------------------------------------------------ */

/* Find the structural entity of `kind` carrying `id`; record it if present. */
static void find_structural_by_id(collector *co, const tp_project *p, tp_id_kind kind, tp_id128 id) {
    for (int ai = 0; ai < p->atlas_count; ai++) {
        const tp_project_atlas *a = &p->atlases[ai];
        if (kind == TP_ID_KIND_ATLAS && tp_id128_eq(a->id, id)) {
            char label[256];
            (void)snprintf(label, sizeof label, "atlas '%s'", a->name ? a->name : "");
            match(co, TP_SEL_ATLAS, id, ai, label);
        }
        if (kind == TP_ID_KIND_SOURCE) {
            for (int i = 0; i < a->source_count; i++) {
                if (tp_id128_eq(a->sources[i].id, id)) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "source '%s' in atlas '%s'", a->sources[i].path,
                                   a->name ? a->name : "");
                    match(co, TP_SEL_SOURCE, id, ai, label);
                }
            }
        }
        if (kind == TP_ID_KIND_ANIM) {
            for (int i = 0; i < a->animation_count; i++) {
                if (tp_id128_eq(a->animations[i].id, id)) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "animation '%s' in atlas '%s'", a->animations[i].name,
                                   a->name ? a->name : "");
                    match(co, TP_SEL_ANIM, id, ai, label);
                }
            }
        }
        if (kind == TP_ID_KIND_TARGET) {
            for (int i = 0; i < a->target_count; i++) {
                if (tp_id128_eq(a->targets[i].id, id)) {
                    char label[256];
                    (void)snprintf(label, sizeof label, "target '%s' in atlas '%s'", a->targets[i].out_path,
                                   a->name ? a->name : "");
                    match(co, TP_SEL_TARGET, id, ai, label);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* resolve                                                            */
/* ------------------------------------------------------------------ */

static tp_status finish(collector *co, tp_selector_result *out, const char *selector, tp_error *err) {
    if (co->oom || (co->cands && co->cands->oom)) {
        return tp_error_set(err, TP_STATUS_OOM, "tp_selector_resolve: out of memory collecting candidates");
    }
    if (co->count == 0) {
        return tp_error_set(err, TP_STATUS_NOT_FOUND, "selector '%s' matched no entity", selector);
    }
    if (co->count > 1) {
        return tp_error_set(err, TP_STATUS_AMBIGUOUS_SELECTOR, "selector '%s' is ambiguous (%d matches)", selector,
                            co->count);
    }
    *out = co->last;
    return TP_STATUS_OK;
}

tp_status tp_selector_resolve(const struct tp_project *p, const char *selector, const struct tp_sprite_index *sprites,
                              int sprite_atlas_index, tp_selector_result *out, tp_selector_candidates *cand,
                              tp_error *err) {
    if (out) {
        memset(out, 0, sizeof *out);
    }
    if (!p || !out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_selector_resolve: NULL project or out");
    }
    if (!selector || selector[0] == '\0') {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_selector_resolve: empty selector");
    }

    collector co = {0};
    co.cands = cand;

    const char *colon = strchr(selector, ':');
    if (colon) {
        char scope[TP_ID_TEXT_CAP];
        const size_t slen = (size_t)(colon - selector);
        const char *rest = colon + 1;
        /* A recognized SCOPE keyword restricts by kind. */
        if (slen < sizeof scope) {
            memcpy(scope, selector, slen);
            scope[slen] = '\0';
            tp_selector_kind sk = TP_SEL_NONE;
            if (strcmp(scope, "atlas") == 0) {
                sk = TP_SEL_ATLAS;
            } else if (strcmp(scope, "source") == 0) {
                sk = TP_SEL_SOURCE;
            } else if (strcmp(scope, "anim") == 0) {
                sk = TP_SEL_ANIM;
            } else if (strcmp(scope, "target") == 0) {
                sk = TP_SEL_TARGET;
            } else if (strcmp(scope, "sprite") == 0) {
                sk = TP_SEL_SPRITE;
            }
            if (sk != TP_SEL_NONE) {
                collect_matches(&co, p, sprites, sprite_atlas_index, sk, rest);
                return finish(&co, out, selector, err);
            }
            /* A source shape-id scope ("source_<hex>:<key>") -> sprite in that source. */
            tp_id_kind ik = TP_ID_KIND_INVALID;
            tp_id128 sid = tp_id128_nil();
            if (tp_id_parse(scope, &ik, &sid, NULL) == TP_STATUS_OK && ik == TP_ID_KIND_SOURCE) {
                collect_sprite_in_source(&co, p, sprites, sprite_atlas_index, sid, rest);
                return finish(&co, out, selector, err);
            }
        }
        /* Unknown scope: fall through to a bare-token search on the WHOLE selector
         * (so a path that legitimately contains ':' is not misparsed). */
    }

    /* Canonical id text (no recognized scope). "sprite_" first (tp_id_parse rejects it). */
    if (strncmp(selector, TP_SPRITE_ID_PREFIX, sizeof(TP_SPRITE_ID_PREFIX) - 1U) == 0) {
        tp_id128 spid = tp_id128_nil();
        if (tp_sprite_id_parse(selector, &spid, NULL) == TP_STATUS_OK) {
            if (sprites && sprite_atlas_index >= 0 && sprite_atlas_index < p->atlas_count) {
                const tp_sprite_ref *r = tp_sprite_index_by_id(sprites, spid);
                if (r) {
                    const tp_project_atlas *a = &p->atlases[sprite_atlas_index];
                    char label[256];
                    const char *spath = (r->source_index >= 0 && r->source_index < a->source_count)
                                            ? a->sources[r->source_index].path
                                            : "";
                    (void)snprintf(label, sizeof label, "sprite '%s' from source '%s'", r->export_key,
                                   spath ? spath : "");
                    match(&co, TP_SEL_SPRITE, r->sprite_id, sprite_atlas_index, label);
                }
            }
            return finish(&co, out, selector, err);
        }
    }
    tp_id_kind ik = TP_ID_KIND_INVALID;
    tp_id128 id = tp_id128_nil();
    if (tp_id_parse(selector, &ik, &id, NULL) == TP_STATUS_OK && ik != TP_ID_KIND_INVALID) {
        find_structural_by_id(&co, p, ik, id);
        return finish(&co, out, selector, err);
    }

    /* Bare token: search every kind. */
    collect_matches(&co, p, sprites, sprite_atlas_index, TP_SEL_NONE, selector);
    return finish(&co, out, selector, err);
}
