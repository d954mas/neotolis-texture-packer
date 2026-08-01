#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_export_geom.h"
#include "tp_fs_internal.h"
#include "tp_strutil.h" /* shared tp_path_basename (one core definition) */

#include "tp_core/tp_sb.h"

/* Defold exporter: emits the extension-texturepacker `.tpinfo` (packed layout,
 * protobuf TEXT format) + a starter `.tpatlas` (animation wrapper) + straight-
 * alpha page PNGs. Field-by-field contract:
 * docs/formats/defold-tpinfo.md (pinned against the executable demo/tests).
 *
 * Everything is y-down pixel space (TexturePacker convention == our tp_model
 * canonical space), so no y-flip is needed. Deterministic: fixed header, pages
 * in page order, sprites in final-name order within a page, animations in id
 * order, %.9g floats, LF, no timestamps.
 *
 * Capability-driven: the format holds trim, 90-degree rotation,
 * polygons, pivots, multipage and aliases; it has NO 9-slice and NO region-level
 * flips (flips exist only per-animation). caps gates emission and raises a
 * metadata-loss notice for a genuine drop; never a hard error. The per-target
 * clamp passes the exact identity + clockwise-90 mask to the engine, so every
 * transform reaching this serializer is representable by `.tpinfo`. */

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

/* Our stable playback id -> Defold Playback enum token. The stable enum is pinned
 * by docs/formats/defold-tpinfo.md: once_forward(0), loop_forward(1), once_backward(2),
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

/* ------------------------------------------------------------------ */
/* .tpinfo                                                            */
/* ------------------------------------------------------------------ */

static tp_status emit_sprite(tp_sb *sb, int depth, const tp_export_ir *prep,
                             const tp_export_sprite *es,
                             const tp_export_caps *caps,
                             tp_export_notices *notices) {
    const tp_sprite *s = &es->data;

    /* Rotation: only the one representable 90-degree CW mask maps to rotated:true.
     * v1 never reaches here with a transform (identity clamp); the else-branch is a
     * guard for a caps-bypassing caller. */
    bool rotated = false;
    if (s->transform != 0) {
        if ((caps->transform_mask & TP_EXPORT_TRANSFORM_BIT(s->transform)) != 0U &&
            s->transform == TP_DEFOLD_ROTATED_MASK) {
            rotated = true;
        } else if (notices) {
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_TRANSFORM, TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "transform %d dropped for '%s' (.tpinfo encodes only a 90-degree rotation)", (int)s->transform,
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
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

    bool solid = es->is_solid;

    bool poly = (s->vert_count > 0 && !tp_export_is_rect_quad(s));
    if (poly && !caps->polygons) {
        if (notices) {
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_POLYGON,
                TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "polygon flattened to rect for '%s' (target stores quads only)",
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
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
        tp_status status = tp_export_notice_add_ex(
            notices, TP_NOTICE_FIELD_PIVOT,
            TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
            "pivot dropped for '%s' (target has no pivot support)",
            es->final_name);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }

    emit_rect(sb, depth + 1, "frame_rect", s->frame.x, s->frame.y, foot_w, foot_h);
    emit_size(sb, depth + 1, "untrimmed_size", s->sourceSize.w, s->sourceSize.h);

    /* .tpinfo has no 9-slice field. A non-default border set is genuine metadata
     * loss -> notice (caps.slice9 is always false for this format; the branch is
     * future-proof if a slice9-carrying variant is ever added). */
    if (!caps->slice9 && (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) &&
        notices) {
        tp_status status = tp_export_notice_add_ex(
            notices, TP_NOTICE_FIELD_SLICE9,
            TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
            "slice9 dropped for '%s' (target has no 9-slice support)",
            es->final_name);
        if (status != TP_STATUS_OK) {
            return status;
        }
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
    return TP_STATUS_OK;
}

static tp_status emit_tpinfo(tp_sb *sb, const tp_export_ir *prep, const tp_export_caps *caps,
                             const tp_export_artifact_plan *plan,
                             tp_export_notices *notices, tp_error *err) {
    tp_sb_str(sb, "# Exported by neotolis-texture-packer\n");
    tp_sb_str(sb, "# Format: Defold extension-texturepacker .tpinfo (protobuf text)\n\n");
    kv_str(sb, 0, "version", TP_DEFOLD_TPINFO_VERSION);
    kv_str(sb, 0, "description", TP_DEFOLD_DESCRIPTION);

    for (int p = 0; p < prep->page_count; p++) {
        tp_sb_str(sb, "pages {\n");
        const tp_export_artifact *page =
            &plan->artifacts[plan->document_count + p];
        kv_str(sb, 1, "name", tp_path_basename(page->path));
        emit_size(sb, 1, "size", prep->pages[p].w, prep->pages[p].h);
        /* prep->sprites is final-name sorted; filtering by page keeps that order. */
        for (int i = 0; i < prep->sprite_count; i++) {
            if (prep->sprites[i].data.page != p) {
                continue;
            }
            tp_status status = emit_sprite(
                sb, 1, prep, &prep->sprites[i], caps, notices);
            if (status != TP_STATUS_OK) {
                return status;
            }
        }
        tp_sb_str(sb, "}\n");
    }

    if (prep->page_count > 1 && !caps->multipage && notices) {
        tp_status status = tp_export_notice_add_ex(
            notices, TP_NOTICE_FIELD_MULTIPAGE,
            TP_NOTICE_REASON_CAPS_UNSUPPORTED, NULL, NULL,
            "atlas '%s' has %d pages but the target is single-page",
            prep->atlas_name ? prep->atlas_name : "", prep->page_count);
        if (status != TP_STATUS_OK) {
            return status;
        }
    }
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* .tpatlas                                                           */
/* ------------------------------------------------------------------ */

static tp_status emit_tpatlas(tp_sb *sb, const tp_export_ir *prep,
                              const char *tpinfo_ref,
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
                tp_status status = tp_export_notice_addf(
                    notices, "animation '%s' has unknown playback id %d; exported as PLAYBACK_ONCE_FORWARD", a->id,
                    a->playback);
                if (status != TP_STATUS_OK) {
                    return status;
                }
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
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* files                                                              */
/* ------------------------------------------------------------------ */

tp_status tp_export_defold_serialize(const tp_export_serialize_ctx *ctx,
                                     tp_export_document *documents,
                                     int document_count,
                                     tp_error *err) {
    if (!ctx || !ctx->ir || !ctx->format || !ctx->plan || !documents ||
        document_count != 2 || ctx->plan->document_count != 2) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "defold: incomplete serialize context");
    }
    const tp_export_ir *prep = ctx->ir;
    tp_export_notices *notices = ctx->notices;

    /* Page/tpinfo files sit next to the .tpatlas; bob resolves page `name` and the
     * .tpatlas `file` relative to their own directory (AtlasBuilder.java 2.7.0).
     * Both references describe where the atlas will BE, so they are derived from
     * the published base -- staging shares its basename, and the game.project
     * walk must climb the real output tree, not the staging dir. */
    const char *base = tp_path_basename(ctx->plan->out_path_base);

    tp_sb info = {0};
    tp_status st = emit_tpinfo(&info, prep, &ctx->format->caps, ctx->plan,
                               notices, err);
    if (st != TP_STATUS_OK) {
        free(info.buf);
        return tp_error_set(err, st,
                            "defold: could not record export notice");
    }
    if (info.oom) {
        free(info.buf);
        return tp_error_set(err, TP_STATUS_OOM, "defold: OOM building .tpinfo");
    }

    char tpinfo_ref[TP_DEFOLD_PATH_MAX];
    if (!resolve_tpatlas_file_ref(ctx->plan->out_path_base, base, tpinfo_ref,
                                 sizeof tpinfo_ref) && notices) {
        st = tp_export_notice_addf(
            notices,
            "could not locate game.project above '%s' -- .tpatlas 'file' reference '%s' may not resolve in Defold "
            "(expected a project-absolute \"/path/%s\")",
            ctx->plan->out_path_base, tpinfo_ref, tpinfo_ref);
        if (st != TP_STATUS_OK) {
            free(info.buf);
            return tp_error_set(err, st,
                                "defold: could not record export notice");
        }
    }
    tp_sb atlas = {0};
    st = emit_tpatlas(&atlas, prep, tpinfo_ref, notices);
    if (st != TP_STATUS_OK) {
        free(info.buf);
        free(atlas.buf);
        return tp_error_set(err, st,
                            "defold: could not record export notice");
    }
    if (atlas.oom) {
        free(info.buf);
        free(atlas.buf);
        return tp_error_set(err, TP_STATUS_OOM, "defold: OOM building .tpatlas");
    }
    documents[0] = (tp_export_document){.data = info.buf, .size = info.len};
    documents[1] = (tp_export_document){.data = atlas.buf, .size = atlas.len};
    return TP_STATUS_OK;
}
