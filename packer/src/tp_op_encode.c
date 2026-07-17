/*
 * Determinism: canonical BYTE-STABLE encoders for an operation and an apply
 * result. Same conventions as the tp_project writer (src/tp_sb.h): 2-space indent,
 * LF, %.9g floats, keys ASCENDING with the "op" discriminator first, a trailing
 * newline. Endian-independent (no reinterpret), so goldens are byte-identical on
 * every OS. Sparse: a SET op emits only the fields its presence mask selects.
 */

#include "tp_core/tp_operation.h"

#include <stdlib.h>
#include <string.h>

#include "tp_core/tp_id.h"
#include "tp_encode_internal.h"
#include "tp_sb.h"

typedef enum { FT_ID, FT_STR, FT_INT, FT_NUM, FT_BOOL, FT_ARR, FT_REF, FT_REFS } ftype;

typedef struct {
    const char *key;
    ftype t;
    tp_id_kind idk;
    tp_id128 id;
    const char *s;
    long i;
    double n;
    bool b;
    char *const *arr;
    int arrn;
    const tp_op_sprite_ref *refs;
} efield;

static void emit_str_array(tp_sb *sb, int keydepth, char *const *arr, int n) {
    if (n <= 0) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < n && !sb->oom && !sb->limit_exceeded; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, keydepth + 1);
        tp_sb_json_string(sb, arr[i] ? arr[i] : "");
    }
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, keydepth);
    tp_sb_char(sb, ']');
}

static void emit_sprite_ref(tp_sb *sb, const tp_op_sprite_ref *ref,
                            int depth) {
    char source[TP_ID_TEXT_CAP];
    if (tp_id_format(TP_ID_KIND_SOURCE, ref->source_id, source,
                     sizeof source, NULL) != TP_STATUS_OK) {
        source[0] = '\0';
    }
    tp_sb_str(sb, "{\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_str(sb, "\"source_id\": ");
    tp_sb_json_string(sb, source);
    tp_sb_str(sb, ",\n");
    tp_sb_indent(sb, depth + 1);
    tp_sb_str(sb, "\"src_key\": ");
    tp_sb_json_string(sb, ref->src_key ? ref->src_key : "");
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void emit_sprite_refs(tp_sb *sb, const tp_op_sprite_ref *refs, int n,
                             int depth) {
    if (n <= 0) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < n && !sb->oom && !sb->limit_exceeded; i++) {
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 1);
        emit_sprite_ref(sb, &refs[i], depth + 1);
    }
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, ']');
}

static void emit_value(tp_sb *sb, const efield *f, int depth) {
    switch (f->t) {
        case FT_ID: {
            char buf[TP_ID_TEXT_CAP];
            if (tp_id_format(f->idk, f->id, buf, sizeof buf, NULL) != TP_STATUS_OK) {
                buf[0] = '\0';
            }
            tp_sb_json_string(sb, buf);
            break;
        }
        case FT_STR: tp_sb_json_string(sb, f->s ? f->s : ""); break;
        case FT_INT: tp_sb_int(sb, f->i); break;
        case FT_NUM: tp_sb_num(sb, f->n); break;
        case FT_BOOL: tp_sb_str(sb, f->b ? "true" : "false"); break;
        case FT_ARR: emit_str_array(sb, depth, f->arr, f->arrn); break;
        case FT_REF: emit_sprite_ref(sb, f->refs, depth); break;
        case FT_REFS: emit_sprite_refs(sb, f->refs, f->arrn, depth); break;
    }
}

/* Emit the object at its final indentation: `force_first` (or NULL) is emitted
 * first, the rest ASCENDING. The same writer serves byte output and the exact
 * count-only admission pass. */
static bool emit_object(tp_sb *sb, const char *force_first, efield *f, int n,
                        int depth, bool trailing_newline) {
    for (int i = 1; i < n; i++) { /* insertion sort by key (ascending ASCII) */
        efield tmp = f[i];
        int j = i - 1;
        while (j >= 0 && strcmp(f[j].key, tmp.key) > 0) {
            f[j + 1] = f[j];
            j--;
        }
        f[j + 1] = tmp;
    }
    tp_sb_char(sb, '{');
    bool first = true;
    int forced = -1;
    if (force_first) {
        for (int i = 0; i < n; i++) {
            if (strcmp(f[i].key, force_first) == 0) {
                forced = i;
                tp_obj_key(sb, depth + 1, &first, f[i].key);
                emit_value(sb, &f[i], depth + 1);
                break;
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (i == forced) {
            continue;
        }
        tp_obj_key(sb, depth + 1, &first, f[i].key);
        emit_value(sb, &f[i], depth + 1);
    }
    tp_sb_char(sb, '\n');
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
    if (trailing_newline) {
        tp_sb_char(sb, '\n');
    }
    return !sb->oom && !sb->limit_exceeded;
}

/* push helpers over a bounded efield array. */
#define PUSH_ID(K, KIND, IDV) (f[c].key = (K), f[c].t = FT_ID, f[c].idk = (KIND), f[c].id = (IDV), c++)
#define PUSH_STR(K, SV) (f[c].key = (K), f[c].t = FT_STR, f[c].s = (SV), c++)
#define PUSH_INT(K, IV) (f[c].key = (K), f[c].t = FT_INT, f[c].i = (long)(IV), c++)
#define PUSH_NUM(K, NV) (f[c].key = (K), f[c].t = FT_NUM, f[c].n = (double)(NV), c++)
#define PUSH_BOOL(K, BV) (f[c].key = (K), f[c].t = FT_BOOL, f[c].b = (BV), c++)
#define PUSH_ARR(K, AV, AN) (f[c].key = (K), f[c].t = FT_ARR, f[c].arr = (AV), f[c].arrn = (AN), c++)
#define PUSH_REF(K, RV) (f[c].key = (K), f[c].t = FT_REF, f[c].refs = (RV), c++)
#define PUSH_REFS(K, RV, RN) (f[c].key = (K), f[c].t = FT_REFS, f[c].refs = (RV), f[c].arrn = (RN), c++)

bool tp_operation_emit_canonical(tp_sb *sb, const tp_operation *op, int depth,
                                 bool trailing_newline) {
    if (!sb || !op || depth < 0) {
        return false;
    }
    const tp_op_info *info = tp_op_info_by_kind(op->kind);
    if (!info) {
        return NULL;
    }
    efield f[24];
    int c = 0;
    const char *clr_toks[8];
    int clr_n = 0;

    PUSH_STR("op", info->wire);
    PUSH_ID("atlas_id", TP_ID_KIND_ATLAS, op->atlas_id);

    switch (op->kind) {
        case TP_OP_ATLAS_CREATE: PUSH_STR("name", op->u.atlas_create.name); break;
        case TP_OP_ATLAS_REMOVE: break;
        case TP_OP_ATLAS_RENAME: PUSH_STR("name", op->u.atlas_rename.name); break;
        case TP_OP_ATLAS_SETTINGS_SET: {
            const tp_op_atlas_settings *s = &op->u.atlas_settings;
            if (s->mask & TP_AF_MAX_SIZE) PUSH_INT("max_size", s->max_size);
            if (s->mask & TP_AF_PADDING) PUSH_INT("padding", s->padding);
            if (s->mask & TP_AF_MARGIN) PUSH_INT("margin", s->margin);
            if (s->mask & TP_AF_EXTRUDE) PUSH_INT("extrude", s->extrude);
            if (s->mask & TP_AF_ALPHA_THRESHOLD) PUSH_INT("alpha_threshold", s->alpha_threshold);
            if (s->mask & TP_AF_MAX_VERTICES) PUSH_INT("max_vertices", s->max_vertices);
            if (s->mask & TP_AF_SHAPE) PUSH_INT("shape", s->shape);
            if (s->mask & TP_AF_ALLOW_TRANSFORM) PUSH_BOOL("allow_transform", s->allow_transform);
            if (s->mask & TP_AF_POWER_OF_TWO) PUSH_BOOL("power_of_two", s->power_of_two);
            if (s->mask & TP_AF_PIXELS_PER_UNIT) PUSH_NUM("pixels_per_unit", s->pixels_per_unit);
            break;
        }
        case TP_OP_SOURCE_ADD:
            PUSH_ID("source_id", TP_ID_KIND_SOURCE, op->u.source_add.source_id);
            PUSH_STR("key", op->u.source_add.key);
            PUSH_STR("kind", op->u.source_add.kind == TP_SOURCE_KIND_FILE ? "file" : "folder");
            break;
        case TP_OP_SOURCE_REMOVE: PUSH_ID("source_id", TP_ID_KIND_SOURCE, op->u.source_ref.source_id); break;
        case TP_OP_SOURCE_REPLACE:
            PUSH_ID("source_id", TP_ID_KIND_SOURCE, op->u.source_ref.source_id);
            PUSH_STR("key", op->u.source_ref.key);
            break;
        case TP_OP_SPRITE_OVERRIDE_SET: {
            const tp_op_sprite_set *s = &op->u.sprite_set;
            PUSH_ID("source_id", TP_ID_KIND_SOURCE, s->source_id);
            PUSH_STR("src_key", s->src_key);
            if (s->mask & TP_SPF_ORIGIN) {
                PUSH_NUM("origin_x", s->origin_x);
                PUSH_NUM("origin_y", s->origin_y);
            }
            if (s->mask & TP_SPF_SLICE9) {
                PUSH_INT("slice9_l", s->slice9[0]);
                PUSH_INT("slice9_r", s->slice9[1]);
                PUSH_INT("slice9_t", s->slice9[2]);
                PUSH_INT("slice9_b", s->slice9[3]);
            }
            if (s->mask & TP_SPF_SHAPE) PUSH_INT("ov_shape", s->ov_shape);
            if (s->mask & TP_SPF_ALLOW_ROTATE) PUSH_INT("ov_allow_rotate", s->ov_allow_rotate);
            if (s->mask & TP_SPF_MAX_VERTICES) PUSH_INT("ov_max_vertices", s->ov_max_vertices);
            if (s->mask & TP_SPF_MARGIN) PUSH_INT("ov_margin", s->ov_margin);
            if (s->mask & TP_SPF_EXTRUDE) PUSH_INT("ov_extrude", s->ov_extrude);
            break;
        }
        case TP_OP_SPRITE_OVERRIDE_CLEAR: {
            uint32_t m = op->u.sprite_clear.mask;
            PUSH_ID("source_id", TP_ID_KIND_SOURCE, op->u.sprite_clear.source_id);
            PUSH_STR("src_key", op->u.sprite_clear.src_key);
            if (m & TP_SPF_ORIGIN) clr_toks[clr_n++] = "origin";
            if (m & TP_SPF_SLICE9) clr_toks[clr_n++] = "slice9";
            if (m & TP_SPF_SHAPE) clr_toks[clr_n++] = "shape";
            if (m & TP_SPF_ALLOW_ROTATE) clr_toks[clr_n++] = "allow_rotate";
            if (m & TP_SPF_MAX_VERTICES) clr_toks[clr_n++] = "max_vertices";
            if (m & TP_SPF_MARGIN) clr_toks[clr_n++] = "margin";
            if (m & TP_SPF_EXTRUDE) clr_toks[clr_n++] = "extrude";
            PUSH_ARR("fields", (char *const *)clr_toks, clr_n);
            break;
        }
        case TP_OP_SPRITE_NAME_SET:
            PUSH_ID("source_id", TP_ID_KIND_SOURCE, op->u.sprite_name.source_id);
            PUSH_STR("src_key", op->u.sprite_name.src_key);
            PUSH_STR("name", op->u.sprite_name.name ? op->u.sprite_name.name : "");
            break;
        case TP_OP_ANIMATION_CREATE: {
            const tp_op_anim_create *a = &op->u.anim_create;
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, a->anim_id);
            PUSH_STR("name", a->name);
            PUSH_NUM("fps", a->fps);
            PUSH_INT("playback", a->playback);
            PUSH_BOOL("flip_h", a->flip_h);
            PUSH_BOOL("flip_v", a->flip_v);
            PUSH_REFS("frames", a->frames, a->frame_count);
            break;
        }
        case TP_OP_ANIMATION_REMOVE: PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_ref.anim_id); break;
        case TP_OP_ANIMATION_RENAME:
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_rename.anim_id);
            PUSH_STR("name", op->u.anim_rename.name);
            break;
        case TP_OP_ANIMATION_SETTINGS_SET: {
            const tp_op_anim_settings *s = &op->u.anim_settings;
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, s->anim_id);
            if (s->mask & TP_ANF_FPS) PUSH_NUM("fps", s->fps);
            if (s->mask & TP_ANF_PLAYBACK) PUSH_INT("playback", s->playback);
            if (s->mask & TP_ANF_FLIP_H) PUSH_BOOL("flip_h", s->flip_h);
            if (s->mask & TP_ANF_FLIP_V) PUSH_BOOL("flip_v", s->flip_v);
            break;
        }
        case TP_OP_ANIMATION_FRAMES_SET:
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_frames_set.anim_id);
            PUSH_REFS("frames", op->u.anim_frames_set.frames, op->u.anim_frames_set.frame_count);
            break;
        case TP_OP_ANIMATION_FRAME_ADD:
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_add.anim_id);
            PUSH_REF("frame", &op->u.anim_frame_add.frame);
            PUSH_INT("index", op->u.anim_frame_add.index);
            break;
        case TP_OP_ANIMATION_FRAME_REMOVE:
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_rm.anim_id);
            PUSH_INT("index", op->u.anim_frame_rm.index);
            break;
        case TP_OP_ANIMATION_FRAME_MOVE:
            PUSH_ID("anim_id", TP_ID_KIND_ANIM, op->u.anim_frame_move.anim_id);
            PUSH_INT("from_index", op->u.anim_frame_move.from_index);
            PUSH_INT("to_index", op->u.anim_frame_move.to_index);
            break;
        case TP_OP_TARGET_CREATE:
            PUSH_ID("target_id", TP_ID_KIND_TARGET, op->u.target_create.target_id);
            PUSH_STR("exporter_id", op->u.target_create.exporter_id);
            PUSH_STR("out_path", op->u.target_create.out_path);
            PUSH_BOOL("enabled", op->u.target_create.enabled);
            break;
        case TP_OP_TARGET_REMOVE: PUSH_ID("target_id", TP_ID_KIND_TARGET, op->u.target_ref.target_id); break;
        case TP_OP_TARGET_SET: {
            const tp_op_target_set *s = &op->u.target_set;
            PUSH_ID("target_id", TP_ID_KIND_TARGET, s->target_id);
            if (s->mask & TP_TF_EXPORTER) PUSH_STR("exporter_id", s->exporter_id);
            if (s->mask & TP_TF_OUT_PATH) PUSH_STR("out_path", s->out_path);
            if (s->mask & TP_TF_ENABLED) PUSH_BOOL("enabled", s->enabled);
            break;
        }
        case TP_OP_INVALID:
        case TP_OP_KIND_COUNT: break;
    }
    return emit_object(sb, "op", f, c, depth, trailing_newline);
}

char *tp_operation_encode_bounded(const tp_operation *op, size_t max_bytes, bool *too_large) {
    if (too_large) *too_large = false;
    tp_sb sb = {0};
    sb.limit = max_bytes;
    if (!tp_operation_emit_canonical(&sb, op, 0, true)) {
        if (too_large) *too_large = sb.limit_exceeded;
        free(sb.buf);
        return NULL;
    }
    return sb.buf;
}

char *tp_operation_encode(const tp_operation *op) {
    return tp_operation_encode_bounded(op, 0U, NULL);
}

char *tp_op_result_encode(const tp_operation *op, const tp_op_reject *rej) {
    const tp_op_info *info = op ? tp_op_info_by_kind(op->kind) : NULL;
    efield f[6];
    int c = 0;
    PUSH_STR("op", info ? info->wire : "");
    if (rej && rej->status != TP_STATUS_OK) {
        PUSH_STR("code", tp_status_id(rej->status));
        if (rej->field[0]) {
            PUSH_STR("field", rej->field);
        }
        PUSH_STR("message", rej->message);
        PUSH_STR("status", "rejected");
    } else {
        PUSH_STR("status", "committed");
    }
    tp_sb sb = {0};
    if (!emit_object(&sb, NULL, f, c, 0, true)) {
        free(sb.buf);
        return NULL;
    }
    return sb.buf; /* result keys are pure ascending (no discriminator) */
}
