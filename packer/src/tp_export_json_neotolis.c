#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_export_geom.h"
#include "tp_export_internal.h"
#include "tp_strutil.h" /* shared tp_path_basename (one core definition) */

#include "tp_core/tp_sb.h"

/* Full-fidelity json-neotolis writer. Deterministic (tp_project.c conventions:
 * "version" first then ascending keys, 2-space indent, LF, %.9g, trailing
 * newline). Capability-driven: unsupported optional metadata is omitted after
 * the common capability owner records any required notices. Schema:
 * docs/formats/json-neotolis.md. */

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* per-section emitters                                               */
/* ------------------------------------------------------------------ */

static void emit_frame(tp_sb *sb, int depth, int x, int y, int w, int h) {
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "h");
    tp_sb_int(sb, h);
    tp_obj_key(sb, depth + 1, &first, "w");
    tp_sb_int(sb, w);
    tp_obj_key(sb, depth + 1, &first, "x");
    tp_sb_int(sb, x);
    tp_obj_key(sb, depth + 1, &first, "y");
    tp_sb_int(sb, y);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void emit_polygon(tp_sb *sb, int depth, const tp_sprite *s) {
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
        tp_sb_int(sb, s->verts[i].x);
        tp_sb_str(sb, ", ");
        tp_sb_int(sb, s->verts[i].y);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_char(sb, ']');
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* `caps` gates optional fields; common orchestration owns loss notices. */
static void emit_sprite(tp_sb *sb, int depth, const tp_export_ir *ir,
                        const tp_export_sprite *es,
                        const tp_export_caps *caps) {
    const tp_sprite *s = &es->data;
    tp_sb_char(sb, '{');
    bool first = true;

    if (caps->aliases) {
        tp_obj_key(sb, depth + 1, &first, "alias_of");
        if (s->alias_of >= 0) {
            tp_sb_json_string(sb, ir->sprites[s->alias_of].final_name);
        } else {
            tp_sb_str(sb, "null");
        }
    }

    tp_obj_key(sb, depth + 1, &first, "frame");
    emit_frame(sb, depth + 1, s->frame.x, s->frame.y, s->frame.w,
               s->frame.h);

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
        }
    }

    bool has_poly = (s->vert_count > 0 && !tp_export_is_rect_quad(s));
    if (has_poly) {
        if (caps->polygons) {
            tp_obj_key(sb, depth + 1, &first, "polygon");
            emit_polygon(sb, depth + 1, s);
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
        }
    }

    tp_obj_key(sb, depth + 1, &first, "sourceSize");
    tp_sb_char(sb, '{');
    {
        bool f2 = true;
        tp_obj_key(sb, depth + 2, &f2, "h");
        tp_sb_int(sb, s->sourceSize.h);
        tp_obj_key(sb, depth + 2, &f2, "w");
        tp_sb_int(sb, s->sourceSize.w);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_char(sb, '}');

    tp_obj_key(sb, depth + 1, &first, "spriteSourceSize");
    emit_frame(sb, depth + 1, s->spriteSourceSize.x,
               s->spriteSourceSize.y, s->spriteSourceSize.w,
               s->spriteSourceSize.h);

    if (s->transform != 0) {
        if ((caps->transform_mask & TP_EXPORT_TRANSFORM_BIT(s->transform)) != 0U) {
            char tbuf[32];
            transform_str(s->transform, tbuf, sizeof tbuf);
            tp_obj_key(sb, depth + 1, &first, "transform");
            tp_sb_int(sb, (long)s->transform);
            tp_obj_key(sb, depth + 1, &first, "transformStr");
            tp_sb_json_string(sb, tbuf);
        }
    }

    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
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

static void emit_root(tp_sb *sb, const tp_export_ir *ir,
                      const tp_export_caps *caps,
                      const tp_export_artifact_plan *plan) {
    tp_sb_char(sb, '{');
    bool first = true;

    tp_obj_key(sb, 1, &first, "version");
    tp_sb_int(sb, (long)TP_JSON_NEOTOLIS_SCHEMA_VERSION);

    if (ir->animation_count > 0) {
        tp_obj_key(sb, 1, &first, "animations");
        tp_sb_char(sb, '[');
        for (int i = 0; i < ir->animation_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            emit_anim(sb, 2, &ir->animations[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }

    tp_obj_key(sb, 1, &first, "atlas");
    tp_sb_json_string(sb, ir->atlas_name ? ir->atlas_name : "");

    tp_obj_key(sb, 1, &first, "pages");
    if (ir->page_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int p = 0; p < ir->page_count; p++) {
            tp_sb_str(sb, p == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '{');
            bool f2 = true;
            const tp_export_artifact *page =
                &plan->artifacts[plan->document_count + p];
            tp_obj_key(sb, 3, &f2, "file");
            tp_sb_json_string(sb, tp_path_basename(page->path));
            tp_obj_key(sb, 3, &f2, "h");
            tp_sb_int(sb, (long)ir->pages[p].h);
            tp_obj_key(sb, 3, &f2, "premultiplied");
            tp_sb_str(sb, ir->pages[p].premultiplied ? "true" : "false");
            tp_obj_key(sb, 3, &f2, "w");
            tp_sb_int(sb, (long)ir->pages[p].w);
            tp_sb_str(sb, "\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '}');
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }
    tp_obj_key(sb, 1, &first, "pixels_per_unit");
    tp_sb_num(sb, (double)ir->pixels_per_unit);

    tp_obj_key(sb, 1, &first, "sprites");
    if (ir->sprite_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < ir->sprite_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            emit_sprite(sb, 2, ir, &ir->sprites[i], caps);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }

    tp_sb_str(sb, "\n");
    tp_sb_char(sb, '}');
    tp_sb_char(sb, '\n'); /* trailing newline */
}

/* ------------------------------------------------------------------ */
/* entry                                                              */
/* ------------------------------------------------------------------ */

tp_status tp_export_json_neotolis_serialize(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err) {
    if (!ctx || !ctx->ir || !ctx->format || !ctx->plan || !documents ||
        document_count != 1 || ctx->plan->document_count != 1) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "json-neotolis: incomplete serialize context");
    }
    const tp_export_ir *ir = ctx->ir;
    tp_sb sb = {0};
    emit_root(&sb, ir, &ctx->format->caps, ctx->plan);
    if (sb.oom) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_OOM, "json-neotolis: OOM building JSON");
    }

    documents[0].data = sb.buf;
    documents[0].size = sb.len;
    return TP_STATUS_OK;
}
