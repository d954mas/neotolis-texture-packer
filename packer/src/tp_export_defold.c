#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_fs_internal.h"

#include "tp_sb.h"

/* Defold exporter: emits the extension-texturepacker `.tpinfo` (packed layout,
 * protobuf TEXT format) + a starter `.tpatlas` (animation wrapper) + straight-
 * alpha page PNGs. Ground truth: docs/research/defold.md (pinned against
 * extension-texturepacker 2.7.0 / Defold 1.12.4). Field-by-field contract:
 * docs/formats/defold-tpinfo.md.
 *
 * Everything is y-down pixel space (TexturePacker convention == our tp_model
 * canonical space), so no y-flip is needed. Deterministic: fixed header, pages
 * in page order, sprites in final-name order within a page, animations in id
 * order, %.9g floats, LF, no timestamps.
 *
 * Capability-driven (SUMMARY.md §5h): the format holds trim, 90-degree rotation,
 * polygons, pivots, multipage and aliases; it has NO 9-slice and NO region-level
 * flips (flips exist only per-animation). caps gates emission and raises a
 * metadata-loss notice for a genuine drop; never a hard error. The per-target
 * clamp (tp_export_effective_settings) additionally packs Defold IDENTITY-ONLY in
 * v1 (rotate90 && !flips), so a non-identity transform never reaches this writer
 * through the supported pipeline -- the rotation path below is exercised directly
 * by tests and is ready for the future rotation-only engine policy. */

#define TP_DEFOLD_PATH_MAX TP_IDENTITY_PATH_MAX
/* TP_DEFOLD_TPINFO_VERSION now lives in tp_core/tp_export.h (shared with the CLI
 * version manifest). */
#define TP_DEFOLD_DESCRIPTION "Exported using neotolis-texture-packer"

/* The single D4 orientation the .tpinfo `rotated` bool can represent: a 90-degree
 * clockwise rotation of the content (source top-left lands at frame top-right).
 * Verified against examples/rotate/rotate.tpinfo and the tp_transform_decode
 * corner mapping -- this is exactly our (DIAGONAL | FLIP_H) mask. Every other
 * non-identity D4 mask (pure flips, 180, transpose/anti-transpose reflections,
 * the opposite rotation) is NOT representable and cannot be baked for Defold. */
#define TP_DEFOLD_ROTATED_MASK ((uint8_t)(TP_TRANSFORM_DIAGONAL | TP_TRANSFORM_FLIP_H))

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Basename of a '/'- or '\\'-separated path (page/tpinfo files sit next to the
 * .tpatlas; bob resolves page `name` and the .tpatlas `file` relative to their
 * own directory -- verified against AtlasBuilder.java 2.7.0). */
static const char *path_basename(const char *p) {
    const char *base = p;
    for (const char *c = p; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    return base;
}

/* True if a regular, non-reparse file exists at `path`. */
static bool file_probe(const char *path) {
    tp_fs_info info;
    return tp_fs_stat(path, &info) && info.kind == TP_FS_KIND_REGULAR && !info.reparse;
}

/* How many parent directories we walk looking for the Defold project root. */
#define TP_DEFOLD_GAME_PROJECT_MAX_UP 10

/* Resolve the `.tpatlas` `file:` reference. extension-texturepacker declares that
 * field a Defold RESOURCE (tpatlas.proto `[(resource)=true]`) resolved from the
 * PROJECT ROOT, so it must be a project-absolute "/dir/base.tpinfo" -- a bare
 * basename resolves to "/base.tpinfo" (project root) and the build fails. We find
 * the project root by walking UP from the .tpinfo's own directory (bounded to
 * TP_DEFOLD_GAME_PROJECT_MAX_UP levels) looking for a `game.project` file. On
 * success `out` gets "/<relpath>/<base>.tpinfo" (forward slashes) and the fn
 * returns true; on failure `out` gets the bare "<base>.tpinfo" and returns false
 * (the caller raises a metadata notice). Zero configuration: matches the demo
 * layout (examples/defold-demo/game.project). */
static bool resolve_tpatlas_file_ref(const char *out_path_base, const char *tpinfo_basename, char *out, size_t out_sz) {
    /* Work on a forward-slash-normalized copy of the full .tpinfo path. */
    char full[TP_DEFOLD_PATH_MAX];
    int n = snprintf(full, sizeof full, "%s.tpinfo", out_path_base);
    if (n < 0 || (size_t)n >= sizeof full) {
        (void)snprintf(out, out_sz, "%s.tpinfo", tpinfo_basename);
        return false;
    }
    for (char *c = full; *c; c++) {
        if (*c == '\\') {
            *c = '/';
        }
    }
    const char *last_slash = strrchr(full, '/');
    if (!last_slash) {
        (void)snprintf(out, out_sz, "%s.tpinfo", tpinfo_basename); /* no directory to walk */
        return false;
    }
    size_t dir_len = (size_t)(last_slash - full); /* length of the current candidate dir */
    for (int up = 0; up <= TP_DEFOLD_GAME_PROJECT_MAX_UP; up++) {
        char probe[TP_DEFOLD_PATH_MAX];
        int pn = snprintf(probe, sizeof probe, "%.*s/game.project", (int)dir_len, full);
        if (pn > 0 && (size_t)pn < sizeof probe && file_probe(probe)) {
            /* project root = full[0..dir_len); resource = "/" + full[dir_len+1..]. */
            int rn = snprintf(out, out_sz, "/%s", full + dir_len + 1);
            return rn > 0 && (size_t)rn < out_sz;
        }
        /* Ascend one level: new dir = parent of the current candidate dir. */
        const char *prev = NULL;
        for (size_t i = 0; i < dir_len; i++) {
            if (full[i] == '/') {
                prev = full + i;
            }
        }
        if (!prev) {
            break; /* no parent component left (relative path exhausted) */
        }
        dir_len = (size_t)(prev - full);
        if (dir_len == 0) {
            break; /* reached filesystem root */
        }
    }
    (void)snprintf(out, out_sz, "%s.tpinfo", tpinfo_basename);
    return false;
}

/* True when the sprite's hull is exactly the axis-aligned trim quad (a plain
 * RECT) -- then the canonical source-rect quad is emitted, not the hull mesh.
 * Mirrors the json-neotolis writer's test (verts are trim-local, 0..frame.w/h). */
static bool is_rect_quad(const tp_sprite *s) {
    if (s->vert_count != 4 || !s->verts) {
        return false;
    }
    int w = s->frame.w;
    int h = s->frame.h;
    bool seen[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        int x = s->verts[i].x;
        int y = s->verts[i].y;
        int ci;
        if (x == 0 && y == 0) {
            ci = 0;
        } else if (x == w && y == 0) {
            ci = 1;
        } else if (x == 0 && y == h) {
            ci = 2;
        } else if (x == w && y == h) {
            ci = 3;
        } else {
            return false;
        }
        if (seen[ci]) {
            return false;
        }
        seen[ci] = true;
    }
    return true;
}

/* Our stable playback id -> Defold Playback enum token. The stable enum is pinned
 * to Defold's set (ux.md 3.7b): once_forward(0), loop_forward(1), once_backward(2),
 * loop_backward(3), once_pingpong(4), loop_pingpong(5), none(6). NULL for an
 * out-of-range id (caller substitutes ONCE_FORWARD + a notice). */
static const char *defold_playback(int id) {
    switch (id) {
        case 0: return "PLAYBACK_ONCE_FORWARD";
        case 1: return "PLAYBACK_LOOP_FORWARD";
        case 2: return "PLAYBACK_ONCE_BACKWARD";
        case 3: return "PLAYBACK_LOOP_BACKWARD";
        case 4: return "PLAYBACK_ONCE_PINGPONG";
        case 5: return "PLAYBACK_LOOP_PINGPONG";
        case 6: return "PLAYBACK_NONE";
        default: return NULL;
    }
}

/* Protobuf-text string literal: quote, escape '\\' and '"', octal-escape control
 * bytes; UTF-8 passes through (bob's TextFormat.merge reads UTF-8 verbatim). */
static void pb_string(tp_sb *sb, const char *s) {
    tp_sb_char(sb, '"');
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"') {
            tp_sb_str(sb, "\\\"");
        } else if (*c == '\\') {
            tp_sb_str(sb, "\\\\");
        } else if (*c < 0x20U) {
            char e[8];
            (void)snprintf(e, sizeof e, "\\%03o", (unsigned)*c);
            tp_sb_str(sb, e);
        } else {
            tp_sb_char(sb, (char)*c);
        }
    }
    tp_sb_char(sb, '"');
}

static void kv_int(tp_sb *sb, int depth, const char *key, long v) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, ": ");
    tp_sb_int(sb, v);
    tp_sb_char(sb, '\n');
}

static void kv_bool(tp_sb *sb, int depth, const char *key, bool v) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, ": ");
    tp_sb_str(sb, v ? "true" : "false");
    tp_sb_char(sb, '\n');
}

static void kv_str(tp_sb *sb, int depth, const char *key, const char *v) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, ": ");
    pb_string(sb, v);
    tp_sb_char(sb, '\n');
}

/* Point block with integer x,y (corner_offset). */
static void emit_point_i(tp_sb *sb, int depth, const char *key, long x, long y) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, " {\n");
    kv_int(sb, depth + 1, "x", x);
    kv_int(sb, depth + 1, "y", y);
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "}\n");
}

/* Point block with float x,y (pivot -- %.9g, so whole values print without a
 * fractional part, e.g. "64"; a centered odd dimension prints "63.5"). */
static void emit_point_f(tp_sb *sb, int depth, const char *key, double x, double y) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, " {\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_str(sb, "x: ");
    tp_sb_num(sb, x);
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth + 1);
    tp_sb_str(sb, "y: ");
    tp_sb_num(sb, y);
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "}\n");
}

static void emit_rect(tp_sb *sb, int depth, const char *key, long x, long y, long w, long h) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, " {\n");
    kv_int(sb, depth + 1, "x", x);
    kv_int(sb, depth + 1, "y", y);
    kv_int(sb, depth + 1, "width", w);
    kv_int(sb, depth + 1, "height", h);
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "}\n");
}

static void emit_size(tp_sb *sb, int depth, const char *key, long w, long h) {
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, key);
    tp_sb_str(sb, " {\n");
    kv_int(sb, depth + 1, "width", w);
    kv_int(sb, depth + 1, "height", h);
    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "}\n");
}

/* is_solid = no transparent texel in the sprite's placed footprint. Informational
 * (the bob loader ignores it), but we compute it honestly from the page pixels so
 * it matches what TexturePacker reports. Bounds-clamped; false for an absent page
 * or an empty/degenerate region. */
static bool region_is_solid(const tp_page *pg, long x, long y, long w, long h) {
    if (!pg || !pg->rgba || pg->w <= 0 || pg->h <= 0) {
        return false;
    }
    long x0 = x < 0 ? 0 : x;
    long y0 = y < 0 ? 0 : y;
    long x1 = x + w;
    long y1 = y + h;
    if (x1 > pg->w) {
        x1 = pg->w;
    }
    if (y1 > pg->h) {
        y1 = pg->h;
    }
    if (x0 >= x1 || y0 >= y1) {
        return false;
    }
    for (long yy = y0; yy < y1; yy++) {
        for (long xx = x0; xx < x1; xx++) {
            if (pg->rgba[((size_t)yy * (size_t)pg->w + (size_t)xx) * 4U + 3U] != 255U) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* .tpinfo                                                            */
/* ------------------------------------------------------------------ */

static void emit_sprite(tp_sb *sb, int depth, const tp_export_prepared *prep, const tp_export_sprite *es,
                        const tp_export_caps *caps, tp_export_notices *notices) {
    const tp_sprite *s = es->src;
    const tp_result *r = prep->result;

    /* Rotation: only the one representable 90-degree CW mask maps to rotated:true.
     * v1 never reaches here with a transform (identity clamp); the else-branch is a
     * guard for a caps-bypassing caller. */
    bool rotated = false;
    if (s->transform != 0) {
        if (caps->rotate90 && s->transform == TP_DEFOLD_ROTATED_MASK) {
            rotated = true;
        } else if (notices) {
            (void)tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_TRANSFORM, TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "transform %d dropped for '%s' (.tpinfo encodes only a 90-degree rotation)", (int)s->transform,
                es->final_name);
        }
    }

    /* corner_offset / source_rect ARE spriteSourceSize verbatim. tp_pack_read now
     * normalizes the hull so its vertex bounding box has min corner (0,0) and max
     * (trim_w, trim_h) == spriteSourceSize.{w,h}, so `verts + spriteSourceSize.xy`
     * lands with its leftmost/topmost vertex exactly on source_rect.xy -- i.e.
     * source_rect == the emitted vertices' bbox, TexturePacker's own invariant
     * (examples/basic/basic.tpinfo). The earlier per-sprite AABB re-derivation here
     * was a compensation for a decode bug (hull min could be negative); that bug is
     * now fixed at the source, so this reads spriteSourceSize directly. GUARD: the
     * invariant is proven by test_hull_normalized_to_origin (decode level) and the
     * basic.tpinfo bbox parity check (exporter level) -- a regression that
     * un-normalized the hull would fail both. */
    long sx = s->spriteSourceSize.x; /* trim offset inside the untrimmed image   */
    long sy = s->spriteSourceSize.y;
    long sw = s->spriteSourceSize.w; /* UNROTATED trim dims (== hull vertex span) */
    long sh = s->spriteSourceSize.h;

    long foot_w = rotated ? sh : sw; /* as-drawn footprint on the page */
    long foot_h = rotated ? sw : sh;

    bool solid = false;
    if (s->page >= 0 && s->page < r->page_count) {
        solid = region_is_solid(&r->pages[s->page], s->frame.x, s->frame.y, foot_w, foot_h);
    }

    bool poly = (s->vert_count > 0 && !is_rect_quad(s));
    if (poly && !caps->polygons) {
        if (notices) {
            (void)tp_export_notice_add_ex(notices, TP_NOTICE_FIELD_POLYGON, TP_NOTICE_REASON_CAPS_UNSUPPORTED,
                                          es->final_name, NULL,
                                          "polygon flattened to rect for '%s' (target stores quads only)",
                                          es->final_name);
        }
        poly = false;
    }

    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "sprites {\n");
    kv_str(sb, depth + 1, "name", es->final_name);
    kv_bool(sb, depth + 1, "trimmed", s->trimmed);
    kv_bool(sb, depth + 1, "rotated", rotated);
    kv_bool(sb, depth + 1, "is_solid", solid);
    emit_point_i(sb, depth + 1, "corner_offset", sx, sy);
    emit_rect(sb, depth + 1, "source_rect", sx, sy, sw, sh);

    if (caps->pivot) {
        /* px from the untrimmed top-left, y-down (pivot is normalized over the
         * untrimmed sourceSize). Centered default -> dim/2, matching upstream. */
        emit_point_f(sb, depth + 1, "pivot", (double)s->pivot.x * (double)s->sourceSize.w,
                     (double)s->pivot.y * (double)s->sourceSize.h);
    } else if ((s->pivot.x != 0.5F || s->pivot.y != 0.5F) && notices) {
        (void)tp_export_notice_add_ex(notices, TP_NOTICE_FIELD_PIVOT, TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name,
                                      NULL, "pivot dropped for '%s' (target has no pivot support)", es->final_name);
    }

    emit_rect(sb, depth + 1, "frame_rect", s->frame.x, s->frame.y, foot_w, foot_h);
    emit_size(sb, depth + 1, "untrimmed_size", s->sourceSize.w, s->sourceSize.h);

    /* .tpinfo has no 9-slice field. A non-default border set is genuine metadata
     * loss -> notice (caps.slice9 is always false for this format; the branch is
     * future-proof if a slice9-carrying variant is ever added). */
    if (!caps->slice9 && (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) &&
        notices) {
        (void)tp_export_notice_add_ex(notices, TP_NOTICE_FIELD_SLICE9, TP_NOTICE_REASON_CAPS_UNSUPPORTED,
                                      es->final_name, NULL, "slice9 dropped for '%s' (target has no 9-slice support)",
                                      es->final_name);
    }

    if (poly) {
        /* Hull, trim-local -> untrimmed source space (add the trim offset sx,sy;
         * the hull is normalized so its leftmost/topmost vertex lands exactly on
         * source_rect.xy), y-down, UNROTATED (the rotated flag + swapped frame_rect
         * carry the rotation; verts do not). */
        tp_sb_indent(sb, depth + 1);
        tp_sb_str(sb, "indices: [");
        for (int i = 0; i < s->index_count; i++) {
            tp_sb_str(sb, i == 0 ? "" : ", ");
            tp_sb_uint(sb, (unsigned long)s->indices[i]);
        }
        tp_sb_str(sb, "]\n");
        for (int i = 0; i < s->vert_count; i++) {
            emit_point_i(sb, depth + 1, "vertices", (long)s->verts[i].x + sx, (long)s->verts[i].y + sy);
        }
    } else {
        /* Canonical quad of the source_rect: TR, TL, BL, BR + [1,2,3,0,1,3]
         * (the reference exporter's convention, verified against basic.tpinfo). */
        tp_sb_indent(sb, depth + 1);
        tp_sb_str(sb, "indices: [1, 2, 3, 0, 1, 3]\n");
        emit_point_i(sb, depth + 1, "vertices", sx + sw, sy);      /* TR */
        emit_point_i(sb, depth + 1, "vertices", sx, sy);           /* TL */
        emit_point_i(sb, depth + 1, "vertices", sx, sy + sh);      /* BL */
        emit_point_i(sb, depth + 1, "vertices", sx + sw, sy + sh); /* BR */
    }

    tp_sb_indent(sb, depth);
    tp_sb_str(sb, "}\n");
}

static tp_status emit_tpinfo(tp_sb *sb, const tp_export_prepared *prep, const tp_export_caps *caps,
                             const char *page_base, tp_export_notices *notices, tp_error *err) {
    const tp_result *r = prep->result;

    tp_sb_str(sb, "# Exported by neotolis-texture-packer\n");
    tp_sb_str(sb, "# Format: Defold extension-texturepacker .tpinfo (protobuf text)\n\n");
    kv_str(sb, 0, "version", TP_DEFOLD_TPINFO_VERSION);
    kv_str(sb, 0, "description", TP_DEFOLD_DESCRIPTION);

    for (int p = 0; p < r->page_count; p++) {
        tp_sb_str(sb, "pages {\n");
        char name[TP_DEFOLD_PATH_MAX];
        tp_status st = tp_export_page_path(page_base, p, name, err);
        if (st != TP_STATUS_OK) {
            return st;
        }
        kv_str(sb, 1, "name", name);
        emit_size(sb, 1, "size", r->pages[p].w, r->pages[p].h);
        /* prep->sprites is final-name sorted; filtering by page keeps that order. */
        for (int i = 0; i < prep->sprite_count; i++) {
            if (prep->sprites[i].src->page != p) {
                continue;
            }
            emit_sprite(sb, 1, prep, &prep->sprites[i], caps, notices);
        }
        tp_sb_str(sb, "}\n");
    }

    if (r->page_count > 1 && !caps->multipage && notices) {
        (void)tp_export_notice_add_ex(notices, TP_NOTICE_FIELD_MULTIPAGE, TP_NOTICE_REASON_CAPS_UNSUPPORTED, NULL, NULL,
                                      "atlas '%s' has %d pages but the target is single-page",
                                      r->atlas_name ? r->atlas_name : "", r->page_count);
    }
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* .tpatlas                                                           */
/* ------------------------------------------------------------------ */

static void emit_tpatlas(tp_sb *sb, const tp_export_prepared *prep, const char *tpinfo_ref,
                         tp_export_notices *notices) {
    /* file: project-absolute Defold resource path ("/dir/base.tpinfo") when a
     * game.project was located, else the bare co-located basename (resolved by
     * resolve_tpatlas_file_ref in the writer). */
    kv_str(sb, 0, "file", tpinfo_ref);
    kv_str(sb, 0, "rename_patterns", "");

    for (int i = 0; i < prep->animation_count; i++) {
        const tp_export_anim *a = &prep->animations[i];
        tp_sb_str(sb, "animations {\n");
        kv_str(sb, 1, "id", a->id);
        for (int f = 0; f < a->frame_count; f++) {
            kv_str(sb, 1, "images", a->frames[f]);
        }
        const char *pb = defold_playback(a->playback);
        if (!pb) {
            pb = "PLAYBACK_ONCE_FORWARD";
            if (notices) {
                (void)tp_export_notice_addf(
                    notices, "animation '%s' has unknown playback id %d; exported as PLAYBACK_ONCE_FORWARD", a->id,
                    a->playback);
            }
        }
        tp_sb_indent(sb, 1);
        tp_sb_str(sb, "playback: ");
        tp_sb_str(sb, pb); /* bare enum token, not a quoted string */
        tp_sb_char(sb, '\n');
        kv_int(sb, 1, "fps", (long)(a->fps + 0.5F)); /* Defold fps is uint32 */
        kv_int(sb, 1, "flip_horizontal", a->flip_h ? 1 : 0);
        kv_int(sb, 1, "flip_vertical", a->flip_v ? 1 : 0);
        tp_sb_str(sb, "}\n");
    }

    /* Every .tpinfo sprite name is auto-promoted to a 1-frame animation by bob;
     * the .tpatlas only adds explicit flipbooks. >1 page always builds as a paged
     * (2D-array) texture regardless of this flag, so false is safe (matches the
     * upstream 2-page basic.tpatlas). */
    kv_bool(sb, 0, "is_paged_atlas", false);
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */

static void ignore_output_path(void *ud, const char *path);

static tp_status write_text(const char *base, const char *ext, const tp_sb *sb, tp_error *err) {
    if (sb->oom) {
        return tp_error_set(err, TP_STATUS_OOM, "defold: OOM building %s", ext);
    }
    char path[TP_DEFOLD_PATH_MAX];
    tp_status st = tp_export_output_path(base, ext, path, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    if (!tp_fs_write_file(path, sb->buf, sb->len)) { /* binary: keep LF */
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "defold: cannot write '%s'", path);
    }
    return TP_STATUS_OK;
}

tp_status tp_export_defold_write(const tp_export_prepared *prep, const tp_export_caps *caps, const char *out_path_base,
                                 tp_export_notices *notices, tp_error *err) {
    if (!prep || !caps || !out_path_base) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "defold: NULL prep/caps/out_path_base");
    }

    tp_status st = tp_export_defold_list_outputs(prep, out_path_base, ignore_output_path, NULL, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    /* Page PNGs: straight-alpha (Defold texture profiles premultiply at build). */
    st = tp_export_write_pages(prep->result, out_path_base, false, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    const char *base = path_basename(out_path_base);

    tp_sb info = {0};
    st = emit_tpinfo(&info, prep, caps, base, notices, err);
    if (st != TP_STATUS_OK) {
        free(info.buf);
        return st;
    }
    st = write_text(out_path_base, ".tpinfo", &info, err);
    free(info.buf);
    if (st != TP_STATUS_OK) {
        return st;
    }

    char tpinfo_ref[TP_DEFOLD_PATH_MAX];
    if (!resolve_tpatlas_file_ref(out_path_base, base, tpinfo_ref, sizeof tpinfo_ref) && notices) {
        (void)tp_export_notice_addf(
            notices,
            "could not locate game.project above '%s' -- .tpatlas 'file' reference '%s' may not resolve in Defold "
            "(expected a project-absolute \"/path/%s\")",
            out_path_base, tpinfo_ref, tpinfo_ref);
    }
    tp_sb atlas = {0};
    emit_tpatlas(&atlas, prep, tpinfo_ref, notices);
    st = write_text(out_path_base, ".tpatlas", &atlas, err);
    free(atlas.buf);
    return st;
}

static void ignore_output_path(void *ud, const char *path) {
    (void)ud;
    (void)path;
}

tp_status tp_export_defold_list_outputs(const tp_export_prepared *prep, const char *out_path_base,
                                        tp_export_path_sink sink, void *ud, tp_error *err) {
    if (!prep || !out_path_base || !sink) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "defold output listing requires prep, base, and sink");
    }
    char tpinfo[TP_DEFOLD_PATH_MAX];
    tp_status st = tp_export_output_path(out_path_base, ".tpinfo", tpinfo, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    char tpatlas[TP_DEFOLD_PATH_MAX];
    st = tp_export_output_path(out_path_base, ".tpatlas", tpatlas, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    st = tp_export_list_page_files(prep->result, out_path_base, ignore_output_path, NULL, err);
    if (st != TP_STATUS_OK) {
        return st;
    }
    sink(ud, tpinfo);
    sink(ud, tpatlas);
    return tp_export_list_page_files(prep->result, out_path_base, sink, ud, err);
}
