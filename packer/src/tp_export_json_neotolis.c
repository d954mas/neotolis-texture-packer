#include "tp_core/tp_export.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tp_export_geom.h"
#include "tp_strutil.h" /* shared tp_path_basename (one core definition) */

#include "tp_core/tp_sb.h"

/* Full-fidelity json-neotolis writer. Deterministic (tp_project.c conventions:
 * "version" first then ascending keys, 2-space indent, LF, %.9g, trailing
 * newline). Capability-driven: emits only what `caps` can hold and raises a
 * metadata-loss notice for a genuine drop (never a hard error). Schema:
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
static tp_status emit_sprite(tp_sb *sb, int depth, const tp_export_ir *prep, const tp_export_sprite *es,
                             const tp_export_caps *caps, tp_export_notices *notices) {
    const tp_sprite *s = &es->data;
    float scale = prep->scale;
    tp_sb_char(sb, '{');
    bool first = true;

    if (caps->aliases) {
        tp_obj_key(sb, depth + 1, &first, "alias_of");
        if (s->alias_of >= 0) {
            tp_sb_json_string(sb, prep->sprites[s->alias_of].final_name);
        } else {
            tp_sb_str(sb, "null");
        }
    } else if (s->alias_of >= 0 && notices) {
        /* target unknown to the writer (id lives on the descriptor); the run/predict layer fills it. */
        tp_status status = tp_export_notice_add_ex(
            notices, TP_NOTICE_FIELD_ALIAS, TP_NOTICE_REASON_CAPS_UNSUPPORTED,
            es->final_name, NULL,
            "alias link dropped for '%s' (target has no alias support)",
            es->final_name);
        if (status != TP_STATUS_OK) {
            return status;
        }
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
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_PIVOT,
                TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "pivot dropped for '%s' (target has no pivot support)",
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
        }
    }

    bool has_poly = (s->vert_count > 0 && !tp_export_is_rect_quad(s));
    if (has_poly) {
        if (caps->polygons) {
            tp_obj_key(sb, depth + 1, &first, "polygon");
            emit_polygon(sb, depth + 1, s, scale);
        } else if (notices) {
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_POLYGON,
                TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "polygon flattened to rect for '%s' (target stores quads only)",
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
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
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_SLICE9,
                TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "slice9 dropped for '%s' (target has no 9-slice support)",
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
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
        if ((caps->transform_mask & TP_EXPORT_TRANSFORM_BIT(s->transform)) != 0U) {
            char tbuf[32];
            transform_str(s->transform, tbuf, sizeof tbuf);
            tp_obj_key(sb, depth + 1, &first, "transform");
            tp_sb_int(sb, (long)s->transform);
            tp_obj_key(sb, depth + 1, &first, "transformStr");
            tp_sb_json_string(sb, tbuf);
        } else if (notices) {
            tp_status status = tp_export_notice_add_ex(
                notices, TP_NOTICE_FIELD_TRANSFORM,
                TP_NOTICE_REASON_CAPS_UNSUPPORTED, es->final_name, NULL,
                "transform dropped for '%s' (target cannot rotate/flip)",
                es->final_name);
            if (status != TP_STATUS_OK) {
                return status;
            }
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

static tp_status emit_root(tp_sb *sb, const tp_export_ir *prep, const tp_export_caps *caps,
                           const tp_export_artifact_plan *plan,
                           tp_export_notices *notices, tp_error *err) {
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
    tp_sb_json_string(sb, prep->atlas_name ? prep->atlas_name : "");

    tp_obj_key(sb, 1, &first, "pages");
    if (prep->page_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int p = 0; p < prep->page_count; p++) {
            tp_sb_str(sb, p == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '{');
            bool f2 = true;
            const tp_export_artifact *page =
                &plan->artifacts[plan->document_count + p];
            tp_obj_key(sb, 3, &f2, "file");
            tp_sb_json_string(sb, tp_path_basename(page->path));
            tp_obj_key(sb, 3, &f2, "h");
            tp_sb_int(sb, (long)prep->pages[p].h);
            tp_obj_key(sb, 3, &f2, "premultiplied");
            tp_sb_str(sb, prep->pages[p].premultiplied ? "true" : "false");
            tp_obj_key(sb, 3, &f2, "w");
            tp_sb_int(sb, (long)prep->pages[p].w);
            tp_sb_str(sb, "\n");
            tp_sb_indent(sb, 2);
            tp_sb_char(sb, '}');
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
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

    tp_obj_key(sb, 1, &first, "pixels_per_unit");
    tp_sb_num(sb, (double)prep->pixels_per_unit);

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

tp_status tp_export_json_neotolis_serialize(const tp_export_serialize_ctx *ctx,
                                            tp_export_document *documents,
                                            int document_count,
                                            tp_error *err) {
    if (!ctx || !ctx->ir || !ctx->format || !ctx->plan || !documents ||
        document_count != 1 || ctx->plan->document_count != 1) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "json-neotolis: incomplete serialize context");
    }
    const tp_export_ir *prep = ctx->ir;
    tp_sb sb = {0};
    tp_status st = emit_root(&sb, prep, &ctx->format->caps, ctx->plan,
                             ctx->notices, err);
    if (st != TP_STATUS_OK) {
        free(sb.buf);
        return tp_error_set(err, st,
                            "json-neotolis: could not record export notice");
    }
    if (sb.oom) {
        free(sb.buf);
        return tp_error_set(err, TP_STATUS_OOM, "json-neotolis: OOM building JSON");
    }

    documents[0].data = sb.buf;
    documents[0].size = sb.len;
    return TP_STATUS_OK;
}
