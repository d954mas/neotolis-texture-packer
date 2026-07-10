#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_sb.h"

/* Full-fidelity json-neotolis writer. Deterministic (tp_project.c conventions:
 * "version" first then ascending keys, 2-space indent, LF, %.9g, trailing
 * newline). Capability-driven: emits only what `caps` can hold and raises a
 * metadata-loss notice for a genuine drop (never a hard error). Schema:
 * docs/formats/json-neotolis.md. */

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

/* Basename of a '/'- or '\\'-separated path (page files sit next to the json). */
static const char *path_basename(const char *p) {
    const char *base = p;
    for (const char *c = p; *c; c++) {
        if (*c == '/' || *c == '\\') {
            base = c + 1;
        }
    }
    return base;
}

/* Decodes a D4 mask into a readable token string (raw-mask semantics: diag =
 * main-diagonal transpose; a 90-degree rotation is diag composed with a flip). */
static void transform_str(uint8_t t, char *buf, size_t cap) {
    if (t == 0) {
        (void)snprintf(buf, cap, "identity");
        return;
    }
    buf[0] = '\0';
    size_t used = 0;
    const char *toks[3];
    int nt = 0;
    if (t & TP_TRANSFORM_FLIP_H) {
        toks[nt++] = "flipH";
    }
    if (t & TP_TRANSFORM_FLIP_V) {
        toks[nt++] = "flipV";
    }
    if (t & TP_TRANSFORM_DIAGONAL) {
        toks[nt++] = "diag";
    }
    for (int i = 0; i < nt; i++) {
        int n = snprintf(buf + used, cap - used, "%s%s", (i == 0) ? "" : "|", toks[i]);
        if (n < 0) {
            return;
        }
        used += (size_t)n;
    }
}

/* True when the sprite's hull is exactly the axis-aligned trim quad (a plain
 * RECT) -- then the frame rect already describes it and no polygon is emitted. */
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

static void emit_scaled(tp_sb *sb, long v, float scale) {
    if (scale == 1.0F) {
        tp_sb_int(sb, v);
    } else {
        tp_sb_num(sb, (double)v * (double)scale);
    }
}

/* ------------------------------------------------------------------ */
/* per-section emitters                                               */
/* ------------------------------------------------------------------ */

static void emit_frame(tp_sb *sb, int depth, int x, int y, int w, int h, float scale) {
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "h");
    emit_scaled(sb, h, scale);
    tp_obj_key(sb, depth + 1, &first, "w");
    emit_scaled(sb, w, scale);
    tp_obj_key(sb, depth + 1, &first, "x");
    emit_scaled(sb, x, scale);
    tp_obj_key(sb, depth + 1, &first, "y");
    emit_scaled(sb, y, scale);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void emit_polygon(tp_sb *sb, int depth, const tp_sprite *s, float scale) {
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "indices");
    tp_sb_char(sb, '[');
    for (int i = 0; i < s->index_count; i++) {
        tp_sb_str(sb, i == 0 ? "" : ", ");
        tp_sb_uint(sb, (unsigned long)s->indices[i]);
    }
    tp_sb_char(sb, ']');
    tp_obj_key(sb, depth + 1, &first, "verts");
    tp_sb_char(sb, '[');
    for (int i = 0; i < s->vert_count; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 2);
        tp_sb_char(sb, '[');
        emit_scaled(sb, s->verts[i].x, scale);
        tp_sb_str(sb, ", ");
        emit_scaled(sb, s->verts[i].y, scale);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_char(sb, ']');
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* Emits one sprite object. `caps` gates optional fields; a genuine drop of
 * non-default metadata raises a notice. */
static tp_status emit_sprite(tp_sb *sb, int depth, const tp_export_prepared *prep, const tp_export_sprite *es,
                             const tp_export_caps *caps, tp_export_notices *notices) {
    const tp_sprite *s = es->src;
    float scale = prep->scale;
    tp_sb_char(sb, '{');
    bool first = true;

    if (caps->aliases) {
        tp_obj_key(sb, depth + 1, &first, "alias_of");
        if (es->alias_of >= 0) {
            tp_sb_json_string(sb, prep->sprites[es->alias_of].final_name);
        } else {
            tp_sb_str(sb, "null");
        }
    } else if (es->alias_of >= 0 && notices) {
        (void)tp_export_notice_addf(notices, "alias link dropped for '%s' (target has no alias support)",
                                    es->final_name);
    }

    tp_obj_key(sb, depth + 1, &first, "frame");
    emit_frame(sb, depth + 1, s->frame.x, s->frame.y, s->frame.w, s->frame.h, scale);

    tp_obj_key(sb, depth + 1, &first, "name");
    tp_sb_json_string(sb, es->final_name);

    tp_obj_key(sb, depth + 1, &first, "page");
    tp_sb_int(sb, (long)s->page);

    bool has_pivot = (s->pivot.x != 0.5F || s->pivot.y != 0.5F);
    if (has_pivot) {
        if (caps->pivot) {
            tp_obj_key(sb, depth + 1, &first, "pivot");
            tp_sb_char(sb, '[');
            tp_sb_num(sb, (double)s->pivot.x);
            tp_sb_str(sb, ", ");
            tp_sb_num(sb, (double)s->pivot.y);
            tp_sb_char(sb, ']');
        } else if (notices) {
            (void)tp_export_notice_addf(notices, "pivot dropped for '%s' (target has no pivot support)",
                                        es->final_name);
        }
    }

    bool has_poly = (s->vert_count > 0 && !is_rect_quad(s));
    if (has_poly) {
        if (caps->polygons) {
            tp_obj_key(sb, depth + 1, &first, "polygon");
            emit_polygon(sb, depth + 1, s, scale);
        } else if (notices) {
            (void)tp_export_notice_addf(notices, "polygon flattened to rect for '%s' (target stores quads only)",
                                        es->final_name);
        }
    }

    bool has_slice9 = (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]);
    if (has_slice9) {
        if (caps->slice9) {
            tp_obj_key(sb, depth + 1, &first, "slice9");
            tp_sb_char(sb, '[');
            for (int k = 0; k < 4; k++) {
                tp_sb_str(sb, k == 0 ? "" : ", ");
                tp_sb_uint(sb, (unsigned long)s->slice9_lrtb[k]);
            }
            tp_sb_char(sb, ']');
        } else if (notices) {
            (void)tp_export_notice_addf(notices, "slice9 dropped for '%s' (target has no 9-slice support)",
                                        es->final_name);
        }
    }

    tp_obj_key(sb, depth + 1, &first, "sourceSize");
    tp_sb_char(sb, '{');
    {
        bool f2 = true;
        tp_obj_key(sb, depth + 2, &f2, "h");
        emit_scaled(sb, s->sourceSize.h, scale);
        tp_obj_key(sb, depth + 2, &f2, "w");
        emit_scaled(sb, s->sourceSize.w, scale);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_char(sb, '}');

    tp_obj_key(sb, depth + 1, &first, "spriteSourceSize");
    emit_frame(sb, depth + 1, s->spriteSourceSize.x, s->spriteSourceSize.y, s->spriteSourceSize.w,
               s->spriteSourceSize.h, scale);

    if (s->transform != 0) {
        if (caps->rotate90 || caps->flips) {
            char tbuf[32];
            transform_str(s->transform, tbuf, sizeof tbuf);
            tp_obj_key(sb, depth + 1, &first, "transform");
            tp_sb_int(sb, (long)s->transform);
            tp_obj_key(sb, depth + 1, &first, "transformStr");
            tp_sb_json_string(sb, tbuf);
        } else if (notices) {
            (void)tp_export_notice_addf(notices, "transform dropped for '%s' (target cannot rotate/flip)",
                                        es->final_name);
        }
    }

    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
    return TP_STATUS_OK;
}

static void emit_anim(tp_sb *sb, int depth, const tp_export_anim *a) {
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "flip_h");
    tp_sb_str(sb, a->flip_h ? "true" : "false");
    tp_obj_key(sb, depth + 1, &first, "flip_v");
    tp_sb_str(sb, a->flip_v ? "true" : "false");
    tp_obj_key(sb, depth + 1, &first, "fps");
    tp_sb_num(sb, (double)a->fps);
    tp_obj_key(sb, depth + 1, &first, "frames");
    tp_sb_char(sb, '[');
    for (int i = 0; i < a->frame_count; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 2);
        tp_sb_json_string(sb, a->frames[i]);
    }
    if (a->frame_count > 0) {
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
    }
    tp_sb_char(sb, ']');
    tp_obj_key(sb, depth + 1, &first, "id");
    tp_sb_json_string(sb, a->id);
    tp_obj_key(sb, depth + 1, &first, "playback");
    tp_sb_int(sb, (long)a->playback);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static tp_status emit_root(tp_sb *sb, const tp_export_prepared *prep, const tp_export_caps *caps,
                           const char *page_base, tp_export_notices *notices) {
    const tp_result *r = prep->result;
    tp_sb_char(sb, '{');
    bool first = true;

    tp_obj_key(sb, 1, &first, "version");
    tp_sb_int(sb, (long)TP_JSON_NEOTOLIS_SCHEMA_VERSION);

    if (prep->animation_count > 0) {
        tp_obj_key(sb, 1, &first, "animations");
        tp_sb_char(sb, '[');
        for (int i = 0; i < prep->animation_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            emit_anim(sb, 2, &prep->animations[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }

    tp_obj_key(sb, 1, &first, "atlas");
    tp_sb_json_string(sb, r->atlas_name ? r->atlas_name : "");

    tp_obj_key(sb, 1, &first, "pages");
    if (r->page_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int p = 0; p < r->page_count; p++) {
            tp_sb_str(sb, p == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '{');
            bool f2 = true;
            char file[1024];
            (void)snprintf(file, sizeof file, "%s-%d.png", page_base, p);
            tp_obj_key(sb, 3, &f2, "file");
            tp_sb_json_string(sb, file);
            tp_obj_key(sb, 3, &f2, "h");
            tp_sb_int(sb, (long)r->pages[p].h);
            tp_obj_key(sb, 3, &f2, "premultiplied");
            tp_sb_str(sb, r->pages[p].premultiplied ? "true" : "false");
            tp_obj_key(sb, 3, &f2, "w");
            tp_sb_int(sb, (long)r->pages[p].w);
            tp_sb_str(sb, "\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '}');
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }
    if (r->page_count > 1 && !caps->multipage && notices) {
        (void)tp_export_notice_addf(notices, "atlas '%s' has %d pages but the target is single-page",
                                    r->atlas_name ? r->atlas_name : "", r->page_count);
    }

    tp_obj_key(sb, 1, &first, "pixels_per_unit");
    tp_sb_num(sb, (double)r->pixels_per_unit);

    tp_obj_key(sb, 1, &first, "sprites");
    if (prep->sprite_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < prep->sprite_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_status st = emit_sprite(sb, 2, prep, &prep->sprites[i], caps, notices);
            if (st != TP_STATUS_OK) {
                return st;
            }
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }

    tp_sb_str(sb, "\n");
    tp_sb_char(sb, '}');
    tp_sb_char(sb, '\n'); /* trailing newline */
    return TP_STATUS_OK;
}

/* ------------------------------------------------------------------ */
/* entry                                                              */
/* ------------------------------------------------------------------ */

tp_status tp_export_json_neotolis_write(const tp_export_prepared *prep, const tp_export_caps *caps,
                                        const char *out_path_base, tp_export_notices *notices, tp_error *err) {
    if (!prep || !caps || !out_path_base) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "json-neotolis: NULL prep/caps/out_path_base");
    }

    /* Pages sit next to the json; straight-alpha default (ROADMAP). */
    tp_status st = tp_export_write_pages(prep->result, out_path_base, false, err);
    if (st != TP_STATUS_OK) {
        return st;
    }

    const char *page_base = path_basename(out_path_base);
    tp_sb sb = {0};
    st = emit_root(&sb, prep, caps, page_base, notices);
    if (st != TP_STATUS_OK) {
        free(sb.buf);
        return st;
    }
    if (sb.oom) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_OOM, "json-neotolis: OOM building JSON");
    }

    char path[1024];
    int nn = snprintf(path, sizeof path, "%s.json", out_path_base);
    if (nn < 0 || (size_t)nn >= sizeof path) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS, "json-neotolis: output path too long");
    }
    FILE *f = fopen(path, "wb"); /* binary: keep LF */
    if (!f) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "json-neotolis: cannot open '%s' for writing", path);
    }
    size_t wrote = fwrite(sb.buf, 1U, sb.len, f);
    (void)fclose(f);
    free(sb.buf);
    if (wrote != sb.len) {
        return tp_error_set(err, TP_STATUS_BAD_PROJECT, "json-neotolis: short write to '%s'", path);
    }
    return TP_STATUS_OK;
}
