#include "tp_core/tp_sprite_index.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_names.h"   /* tp_sprite_export_key */
#include "tp_core/tp_project.h" /* tp_project + tp_project_resolve_path */
#include "tp_core/tp_scan.h"    /* tp_scan_dir / _exists / _is_dir */
#include "tp_core/tp_srckey.h"  /* tp_srckey_normalize */
#include "tp_hex.h"             /* tp_hex_encode_lower (shared drift-guard encoder) */

/* ------------------------------------------------------------------ */
/* sprite-id text (sprite_<32hex>): no tp_id_kind entry exists for a   */
/* derived sprite, so the "sprite_" shape is owned here, not in tp_id. */
/* ------------------------------------------------------------------ */

void tp_sprite_id_format(tp_id128 sprite_id, char *out, size_t cap) {
    if (!out || cap == 0) {
        return;
    }
    /* 7 ("sprite_") + 32 hex + 1 NUL == 40 == TP_ID_TEXT_CAP. */
    if (cap < TP_ID_TEXT_CAP) {
        out[0] = '\0';
        return;
    }
    memcpy(out, TP_SPRITE_ID_PREFIX, sizeof(TP_SPRITE_ID_PREFIX) - 1U);
    tp_hex_encode_lower(sprite_id.bytes, sizeof sprite_id.bytes, out + (sizeof(TP_SPRITE_ID_PREFIX) - 1U));
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

tp_status tp_sprite_id_parse(const char *text, tp_id128 *out_id, tp_error *err) {
    if (!text) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_sprite_id_parse: NULL text");
    }
    const size_t plen = sizeof(TP_SPRITE_ID_PREFIX) - 1U;
    if (strncmp(text, TP_SPRITE_ID_PREFIX, plen) != 0) {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "sprite id must start with \"" TP_SPRITE_ID_PREFIX "\"");
    }
    const char *hex = text + plen;
    tp_id128 id = tp_id128_nil();
    for (int i = 0; i < 16; i++) {
        const int hi = hex_nibble(hex[2 * i]);
        const int lo = hex_nibble(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return tp_error_set(err, TP_STATUS_ID_MALFORMED, "sprite id has a non-hex digit");
        }
        id.bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    if (hex[32] != '\0') {
        return tp_error_set(err, TP_STATUS_ID_MALFORMED, "sprite id has trailing characters after 32 hex digits");
    }
    if (out_id) {
        *out_id = id;
    }
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* build                                                              */
/* ------------------------------------------------------------------ */

static char *dup_str(const char *s) {
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

/* Last path component of a '/'- or '\\'-separated path (file-source raw name). */
static const char *base_name(const char *p) {
    const char *b = p;
    for (const char *q = p; *q; q++) {
        if (*q == '/' || *q == '\\') {
            b = q + 1;
        }
    }
    return b;
}

/* Tolerant fallback: copy `src` into `out` (capacity `cap`) replacing '\\' with '/'.
 * Used only if tp_srckey_normalize rejects a scanned path, so the sprite still lands
 * in the index rather than vanishing (never aborts). */
static void slash_norm(const char *src, char *out, size_t cap) {
    size_t i = 0;
    for (; src[i] != '\0' && i + 1U < cap; i++) {
        out[i] = (src[i] == '\\') ? '/' : src[i];
    }
    out[i] = '\0';
}

static bool index_push(tp_sprite_index *idx, tp_id128 source_id, int source_index, const char *raw, const char *abs) {
    if (idx->count == idx->cap) {
        int nc = idx->cap ? idx->cap * 2 : 32;
        tp_sprite_ref *nr = (tp_sprite_ref *)realloc(idx->refs, (size_t)nc * sizeof *nr);
        if (!nr) {
            return false;
        }
        idx->refs = nr;
        idx->cap = nc;
    }
    char keybuf[TP_SRCKEY_MAX];
    if (tp_srckey_normalize(raw, keybuf, sizeof keybuf, NULL) != TP_STATUS_OK) {
        slash_norm(raw, keybuf, sizeof keybuf);
    }
    char ekey[TP_SRCKEY_MAX];
    tp_sprite_export_key(raw, ekey, sizeof ekey);

    tp_sprite_ref *r = &idx->refs[idx->count];
    memset(r, 0, sizeof *r);
    r->source_id = source_id;
    r->source_index = source_index;
    r->source_key = dup_str(keybuf);
    r->export_key = dup_str(ekey);
    r->raw_name = dup_str(raw);
    r->abs_path = dup_str(abs ? abs : "");
    if (!r->source_key || !r->export_key || !r->raw_name || !r->abs_path) {
        free(r->source_key);
        free(r->export_key);
        free(r->raw_name);
        free(r->abs_path);
        return false;
    }
    r->sprite_id = tp_sprite_id(source_id, r->source_key);
    idx->count++;
    return true;
}

tp_status tp_sprite_index_build(const struct tp_project *p, int atlas_index, tp_sprite_index *out, tp_error *err) {
    if (!out) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_sprite_index_build: out is NULL");
    }
    out->refs = NULL;
    out->count = 0;
    out->cap = 0;
    if (!p) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_sprite_index_build: project is NULL");
    }
    if (atlas_index < 0 || atlas_index >= p->atlas_count) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "tp_sprite_index_build: atlas index %d out of range",
                            atlas_index);
    }
    const tp_project_atlas *a = &p->atlases[atlas_index];

    bool oom = false;
    for (int si = 0; si < a->source_count && !oom; si++) {
        const tp_project_source *src = &a->sources[si];
        char abs[512];
        if (tp_project_resolve_path(p, src->path, abs, sizeof abs) != TP_STATUS_OK) {
            continue; /* unresolvable (relative source, unsaved project) -- skip */
        }
        if (!tp_scan_exists(abs)) {
            continue; /* missing source: a model state, contributes no sprites */
        }
        if (tp_scan_is_dir(abs)) {
            tp_scan_result sc;
            tp_scan_dir(abs, &sc);
            for (int ci = 0; ci < sc.count && !oom; ci++) {
                if (!index_push(out, src->id, si, sc.entries[ci].rel, sc.entries[ci].abs)) {
                    oom = true;
                }
            }
            tp_scan_free(&sc);
        } else if (!index_push(out, src->id, si, base_name(src->path), abs)) {
            oom = true;
        }
    }
    if (oom) {
        tp_sprite_index_free(out);
        return tp_error_set(err, TP_STATUS_OOM, "tp_sprite_index_build: out of memory building the sprite index");
    }
    return TP_STATUS_OK;
}

void tp_sprite_index_free(tp_sprite_index *idx) {
    if (!idx) {
        return;
    }
    for (int i = 0; i < idx->count; i++) {
        free(idx->refs[i].source_key);
        free(idx->refs[i].export_key);
        free(idx->refs[i].raw_name);
        free(idx->refs[i].abs_path);
    }
    free(idx->refs);
    idx->refs = NULL;
    idx->count = 0;
    idx->cap = 0;
}

/* ------------------------------------------------------------------ */
/* lookups                                                            */
/* ------------------------------------------------------------------ */

const tp_sprite_ref *tp_sprite_index_by_id(const tp_sprite_index *idx, tp_id128 sprite_id) {
    if (!idx || tp_id128_is_nil(sprite_id)) {
        return NULL;
    }
    for (int i = 0; i < idx->count; i++) {
        if (tp_id128_eq(idx->refs[i].sprite_id, sprite_id)) {
            return &idx->refs[i];
        }
    }
    return NULL;
}

const tp_sprite_ref *tp_sprite_index_by_export_key(const tp_sprite_index *idx, const char *export_key,
                                                   int *out_matches) {
    const tp_sprite_ref *first = NULL;
    int matches = 0;
    if (idx && export_key) {
        for (int i = 0; i < idx->count; i++) {
            if (strcmp(idx->refs[i].export_key, export_key) == 0) {
                if (!first) {
                    first = &idx->refs[i];
                }
                matches++;
            }
        }
    }
    if (out_matches) {
        *out_matches = matches;
    }
    return first;
}

const tp_sprite_ref *tp_sprite_index_by_source_key(const tp_sprite_index *idx, tp_id128 source_id,
                                                   const char *source_key) {
    if (!idx || !source_key || tp_id128_is_nil(source_id)) {
        return NULL;
    }
    for (int i = 0; i < idx->count; i++) {
        if (tp_id128_eq(idx->refs[i].source_id, source_id) && strcmp(idx->refs[i].source_key, source_key) == 0) {
            return &idx->refs[i];
        }
    }
    return NULL;
}
