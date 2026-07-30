#include "tp_project_write_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/nt_assert.h"

#include "tp_core/tp_id.h"
#include "tp_core/tp_identity.h"
#include "tp_core/tp_pack.h"
#include "tp_core/tp_sb.h"
#include "tp_core/tp_utf8.h"
#include "tp_project_identity_internal.h"
#include "tp_project_parse_internal.h"
#include "tp_project_path_internal.h"

/* ======================================================================== */
/* deterministic writer (ux.md principle 7)                                 */
/* ======================================================================== */

/* Checkpoint mode rewrites each source path to its absolute lexical form; the
 * project supplies the base directory. Carried beside the shared builder rather
 * than inside it so tp_sb stays one general writer. */
typedef struct tp_write_ctx {
    bool absolute_sources;
    const tp_project *project;
} tp_write_ctx;

/* Serialization work probes. The counters and every increment are compiled out of
 * a shipping build, so the writer carries no accounting a shipped consumer could
 * read. */
#ifdef TP_ENABLE_TEST_SEAMS
static _Thread_local size_t s_test_save_buffer_calls;
static _Thread_local size_t s_test_serializer_allocations;
static _Thread_local size_t s_test_serializer_peak_capacity;
static _Thread_local size_t s_test_load_buffer_calls;
static _Thread_local size_t s_test_size_query_calls;

void tp_project__test_serialization_stats_reset(void) {
    s_test_save_buffer_calls = 0U;
    s_test_serializer_allocations = 0U;
    s_test_serializer_peak_capacity = 0U;
    s_test_load_buffer_calls = 0U;
    s_test_size_query_calls = 0U;
}

size_t tp_project__test_save_buffer_calls(void) {
    return s_test_save_buffer_calls;
}

size_t tp_project__test_serializer_allocations(void) {
    return s_test_serializer_allocations;
}

size_t tp_project__test_serializer_peak_capacity(void) {
    return s_test_serializer_peak_capacity;
}

size_t tp_project__test_load_buffer_calls(void) {
    return s_test_load_buffer_calls;
}

size_t tp_project__test_size_query_calls(void) {
    return s_test_size_query_calls;
}
#endif

/* The shared builder reads limit==0 as "unlimited"; the persisted-project cap is
 * a HARD byte budget, so a zero budget must reject before the first byte. */
static void tp_write_begin(tp_sb *sb, size_t limit) {
    sb->limit = limit;
    sb->limit_exceeded = (limit == 0U);
#ifdef TP_ENABLE_TEST_SEAMS
    sb->allocation_count = &s_test_serializer_allocations;
#else
    sb->allocation_count = NULL;
#endif
}

/* Capacity only grows within one emit, so the final capacity IS the peak. */
static void tp_write_end(const tp_sb *sb) {
#ifdef TP_ENABLE_TEST_SEAMS
    if (sb->cap > s_test_serializer_peak_capacity) {
        s_test_serializer_peak_capacity = sb->cap;
    }
#else
    (void)sb;
#endif
}

/* Every string this writer emits is UTF-8-validated by
 * tp_project_validate_canonical before the first byte is written, so an invalid
 * byte here is an internal invariant violation, not user data to launder: assert
 * instead of substituting U+FFFD into the round-tripped project file. */
static void tp_emit_json_string(tp_sb *sb, const char *s) {
    NT_ASSERT(tp_utf8_is_valid_c_string(s));
    tp_sb_json_string(sb, s);
}

static void tp_emit_id(tp_sb *sb, tp_id_kind kind, tp_id128 id); /* defined below */

/* Canonical animation frames are objects {key, source}. */
static void tp_emit_frames(tp_sb *sb, int depth, const tp_project_frame *frames, int count) {
    if (count == 0) {
        tp_sb_str(sb, "[]");
        return;
    }
    tp_sb_char(sb, '[');
    for (int i = 0; i < count; i++) {
        const tp_project_frame *fr = &frames[i];
        tp_sb_str(sb, i == 0 ? "\n" : ",\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, '{');
        bool first = true;
        tp_obj_key(sb, depth + 2, &first, "key");
        tp_emit_json_string(sb, fr->src_key);
        tp_obj_key(sb, depth + 2, &first, "source");
        tp_emit_id(sb, TP_ID_KIND_SOURCE, fr->source_ref);
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, '}');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, ']');
}

/* Emit a structural shape-ID ("<kind>_<32hex>") as a JSON string value. The
 * canonical save gate has already rejected nil or duplicate IDs. */
static void tp_emit_id(tp_sb *sb, tp_id_kind kind, tp_id128 id) {
    char text[TP_ID_TEXT_CAP];
    if (tp_id_format(kind, id, text, sizeof text, NULL) != TP_STATUS_OK) {
        text[0] = '\0'; /* unreachable: kind is valid and the buffer is TP_ID_TEXT_CAP */
    }
    tp_sb_json_string(sb, text);
}

static void tp_emit_sprite(tp_sb *sb, int depth, const tp_project_sprite *s) {
    tp_sb_char(sb, '{');
    bool first = true;
    /* Keys ASCII-ascending. Overrides are sparse (INHERIT skipped); canonical
     * identity fields `key` and `source` are always emitted. */
    if (s->ov_allow_rotate != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "allow_rotate");
        tp_sb_int(sb, (long)s->ov_allow_rotate);
    }
    if (s->ov_extrude != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "extrude");
        tp_sb_int(sb, (long)s->ov_extrude);
    }
    tp_obj_key(sb, depth + 1, &first, "key"); /* source-local key (ext kept) */
    tp_emit_json_string(sb, s->src_key);
    if (s->ov_margin != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "margin");
        tp_sb_int(sb, (long)s->ov_margin);
    }
    if (s->ov_max_vertices != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "max_vertices");
        tp_sb_int(sb, (long)s->ov_max_vertices);
    }
    if (s->origin_x != TP_PROJECT_ORIGIN_DEFAULT || s->origin_y != TP_PROJECT_ORIGIN_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "origin");
        tp_sb_char(sb, '[');
        tp_sb_num(sb, (double)s->origin_x);
        tp_sb_str(sb, ", ");
        tp_sb_num(sb, (double)s->origin_y);
        tp_sb_char(sb, ']');
    }
    if (s->rename) {
        tp_obj_key(sb, depth + 1, &first, "rename");
        tp_emit_json_string(sb, s->rename);
    }
    if (s->ov_shape != TP_PROJECT_OV_INHERIT) {
        tp_obj_key(sb, depth + 1, &first, "shape");
        tp_sb_int(sb, (long)s->ov_shape);
    }
    if (s->slice9_lrtb[0] || s->slice9_lrtb[1] || s->slice9_lrtb[2] || s->slice9_lrtb[3]) {
        tp_obj_key(sb, depth + 1, &first, "slice9");
        tp_sb_char(sb, '[');
        for (int k = 0; k < 4; k++) {
            if (k) {
                tp_sb_str(sb, ", ");
            }
            tp_sb_uint(sb, (unsigned long)s->slice9_lrtb[k]);
        }
        tp_sb_char(sb, ']');
    }
    tp_obj_key(sb, depth + 1, &first, "source"); /* owning source's structural shape-ID */
    tp_emit_id(sb, TP_ID_KIND_SOURCE, s->source_ref);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_anim(tp_sb *sb, int depth, const tp_project_anim *an) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (an->flip_h) {
        tp_obj_key(sb, depth + 1, &first, "flip_h");
        tp_sb_str(sb, "true");
    }
    if (an->flip_v) {
        tp_obj_key(sb, depth + 1, &first, "flip_v");
        tp_sb_str(sb, "true");
    }
    if (an->fps != TP_PROJECT_ANIM_FPS_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "fps");
        tp_sb_num(sb, (double)an->fps);
    }
    tp_obj_key(sb, depth + 1, &first, "frames");
    tp_emit_frames(sb, depth + 1, an->frames, an->frame_count);
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent) */
    tp_emit_id(sb, TP_ID_KIND_ANIM, an->id);
    tp_obj_key(sb, depth + 1, &first, "name"); /* logical/display name */
    tp_emit_json_string(sb, an->name);
    if (an->playback != TP_PROJECT_ANIM_PLAYBACK_DEFAULT) {
        tp_obj_key(sb, depth + 1, &first, "playback");
        tp_sb_int(sb, (long)an->playback);
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_target(tp_sb *sb, int depth, const tp_project_target *t) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (!t->enabled) {
        tp_obj_key(sb, depth + 1, &first, "enabled");
        tp_sb_str(sb, "false");
    }
    tp_obj_key(sb, depth + 1, &first, "exporter_id");
    tp_emit_json_string(sb, t->exporter_id);
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent) */
    tp_emit_id(sb, TP_ID_KIND_TARGET, t->id);
    tp_obj_key(sb, depth + 1, &first, "out_path");
    tp_emit_json_string(sb, t->out_path);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

/* Stable machine token per source kind. "folder" is the omitted sparse
 * default; only "file" is written. APPEND-ONLY -- a new kind adds a token, never
 * renames one (the token is an on-disk contract, pinned by the round-trip tests). */
static const char *tp_source_kind_token(tp_source_kind kind) {
    switch (kind) {
        case TP_SOURCE_KIND_FOLDER: return "folder";
        case TP_SOURCE_KIND_FILE: return "file";
    }
    return "folder"; /* defensive: an unknown value serializes as the default */
}

/* One tagged source object: keys ascending ASCII (id, [kind], path). `id` is always
 * written (same discipline as atlas/anim/target); `kind` is sparse (folder omitted). */
static void tp_emit_source(tp_sb *sb, const tp_write_ctx *ctx, int depth,
                           const tp_project_source *s) {
    tp_sb_char(sb, '{');
    bool first = true;
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent, always written) */
    tp_emit_id(sb, TP_ID_KIND_SOURCE, s->id);
    if (s->kind != TP_SOURCE_KIND_FOLDER) { /* sparse: folder is the default */
        tp_obj_key(sb, depth + 1, &first, "kind");
        tp_emit_json_string(sb, tp_source_kind_token(s->kind));
    }
    tp_obj_key(sb, depth + 1, &first, "path");
    const char *path = s->path;
    char absolute_path[TP_PATH_MAX];
    if (ctx->absolute_sources) {
        tp_error path_error = {0};
        tp_status path_status = tp_project_source_path_absolute_lexical(
            ctx->project, path, absolute_path, sizeof absolute_path,
            &path_error);
        if (path_status == TP_STATUS_PATH_NOT_ABSOLUTE) {
            path_status = tp_identity_path_absolute_lexical(
                path, absolute_path, sizeof absolute_path, &path_error);
        }
        /* The base directory is an OS-supplied path with no UTF-8 admission, so
         * a malformed join is refused here rather than reaching the assert. */
        if (path_status != TP_STATUS_OK ||
            !tp_utf8_is_valid_c_string(absolute_path)) {
            sb->limit_exceeded = true;
            return;
        }
        path = absolute_path;
    }
    tp_emit_json_string(sb, path);
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_atlas(tp_sb *sb, const tp_write_ctx *ctx, int depth,
                          const tp_project_atlas *a, const tp_pack_settings *d) {
    tp_sb_char(sb, '{');
    bool first = true;
    if (a->allow_transform != d->allow_transform) {
        tp_obj_key(sb, depth + 1, &first, "allow_transform");
        tp_sb_str(sb, a->allow_transform ? "true" : "false");
    }
    if (a->alpha_threshold != d->alpha_threshold) {
        tp_obj_key(sb, depth + 1, &first, "alpha_threshold");
        tp_sb_int(sb, (long)a->alpha_threshold);
    }
    if (a->animation_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "animations");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->animation_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_anim(sb, depth + 2, &a->animations[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (a->extrude != d->extrude) {
        tp_obj_key(sb, depth + 1, &first, "extrude");
        tp_sb_int(sb, (long)a->extrude);
    }
    tp_obj_key(sb, depth + 1, &first, "id"); /* structural shape-ID (persistent, always written) */
    tp_emit_id(sb, TP_ID_KIND_ATLAS, a->id);
    if (a->margin != d->margin) {
        tp_obj_key(sb, depth + 1, &first, "margin");
        tp_sb_int(sb, (long)a->margin);
    }
    if (a->max_size != d->max_size) {
        tp_obj_key(sb, depth + 1, &first, "max_size");
        tp_sb_int(sb, (long)a->max_size);
    }
    if (a->max_vertices != d->max_vertices) {
        tp_obj_key(sb, depth + 1, &first, "max_vertices");
        tp_sb_int(sb, (long)a->max_vertices);
    }
    tp_obj_key(sb, depth + 1, &first, "name");
    tp_emit_json_string(sb, a->name);
    if (a->padding != d->padding) {
        tp_obj_key(sb, depth + 1, &first, "padding");
        tp_sb_int(sb, (long)a->padding);
    }
    if (a->pixels_per_unit != d->pixels_per_unit) {
        tp_obj_key(sb, depth + 1, &first, "pixels_per_unit");
        tp_sb_num(sb, (double)a->pixels_per_unit);
    }
    if (a->power_of_two != d->power_of_two) {
        tp_obj_key(sb, depth + 1, &first, "power_of_two");
        tp_sb_str(sb, a->power_of_two ? "true" : "false");
    }
    if (a->shape != d->shape) {
        tp_obj_key(sb, depth + 1, &first, "shape");
        tp_sb_int(sb, (long)a->shape);
    }
    if (a->source_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "sources");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->source_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_source(sb, ctx, depth + 2, &a->sources[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (a->sprite_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "sprites");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->sprite_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_sprite(sb, depth + 2, &a->sprites[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    if (a->target_count > 0) {
        tp_obj_key(sb, depth + 1, &first, "targets");
        tp_sb_char(sb, '[');
        for (int i = 0; i < a->target_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, depth + 2);
            tp_emit_target(sb, depth + 2, &a->targets[i]);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, depth + 1);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, depth);
    tp_sb_char(sb, '}');
}

static void tp_emit_root(tp_sb *sb, const tp_write_ctx *ctx,
                         const tp_project *p, const tp_pack_settings *d) {
    tp_sb_char(sb, '{');
    tp_sb_str(sb, "\n");
    tp_sb_indent(sb, 1);
    tp_sb_str(sb, "\"version\": ");
    tp_sb_int(sb, (long)TP_PROJECT_SCHEMA_VERSION); /* always canonical */
    tp_sb_str(sb, ",\n");
    tp_sb_indent(sb, 1);
    tp_sb_str(sb, "\"atlases\": ");
    if (p->atlas_count == 0) {
        tp_sb_str(sb, "[]");
    } else {
        tp_sb_char(sb, '[');
        for (int i = 0; i < p->atlas_count; i++) {
            tp_sb_str(sb, i == 0 ? "\n" : ",\n");
            tp_sb_indent(sb, 2);
            tp_emit_atlas(sb, ctx, 2, &p->atlases[i], d);
        }
        tp_sb_str(sb, "\n");
        tp_sb_indent(sb, 1);
        tp_sb_char(sb, ']');
    }
    tp_sb_str(sb, "\n");
    tp_sb_char(sb, '}');
    tp_sb_char(sb, '\n'); /* trailing newline */
}

/* ======================================================================== */
/* save                                                                     */
/* ======================================================================== */

static tp_status project_save_buffer_mode(const tp_project *p, bool checkpoint,
                                          const tp_project_json_limits *limits,
                                          char **out, size_t *out_len,
                                          tp_error *err) {
    if (!p || !limits || !out || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT, "tp_project_save_buffer: NULL argument");
    }
    *out = NULL;
    *out_len = 0;
    const tp_status canonical = tp_project_validate_canonical(p, err);
    if (canonical != TP_STATUS_OK) {
        return canonical;
    }
#ifdef TP_ENABLE_TEST_SEAMS
    s_test_save_buffer_calls++;
#endif
    tp_pack_settings defaults;
    tp_pack_settings_defaults(&defaults);
    const tp_write_ctx ctx = {checkpoint, p};
    tp_sb sb = {0};
    tp_write_begin(&sb, limits->bytes);
    tp_emit_root(&sb, &ctx, p, &defaults);
    tp_write_end(&sb);
    if (sb.oom || sb.limit_exceeded) {
        free(sb.buf);
        return tp_error_set(err, sb.oom ? TP_STATUS_OOM : TP_STATUS_OUT_OF_BOUNDS,
                            sb.oom ? "tp_project_save_buffer: out of memory building JSON"
                                   : "tp_project_save_buffer: serialized size overflow");
    }
    const tp_status admission = tp_project_json_admit(
        sb.buf, sb.len, limits, err);
    if (admission != TP_STATUS_OK) {
        free(sb.buf);
        return admission;
    }
    *out = sb.buf;
    *out_len = sb.len;
    return TP_STATUS_OK;
}

tp_status tp_project_save_buffer(const tp_project *p, char **out,
                                 size_t *out_len, tp_error *err) {
    return project_save_buffer_mode(p, false, &TP_PROJECT_JSON_LIMITS, out,
                                    out_len, err);
}

tp_status tp_project_checkpoint_save_buffer(const tp_project *p, char **out,
                                            size_t *out_len, tp_error *err) {
    return project_save_buffer_mode(p, true, &TP_PROJECT_JSON_LIMITS, out,
                                    out_len, err);
}

#ifdef TP_ENABLE_TEST_SEAMS
tp_status tp_project__test_save_buffer_with_json_limits(
    const tp_project *project, bool checkpoint,
    const tp_project_json_limits *limits, char **out, size_t *out_len,
    tp_error *err) {
    return project_save_buffer_mode(project, checkpoint, limits, out, out_len,
                                    err);
}
#endif

static tp_status project_serialized_size_bounded_mode(
    const tp_project *p, bool checkpoint, size_t limit, size_t *out_len,
    tp_error *err) {
#ifdef TP_ENABLE_TEST_SEAMS
    s_test_size_query_calls++;
#endif
    if (!p || !out_len) {
        return tp_error_set(err, TP_STATUS_INVALID_ARGUMENT,
                            "tp_project_serialized_size_bounded: NULL argument");
    }
    *out_len = 0U;
    const tp_status canonical = tp_project_validate_canonical(p, err);
    if (canonical != TP_STATUS_OK) {
        return canonical;
    }
    tp_pack_settings defaults;
    tp_pack_settings_defaults(&defaults);
    const tp_write_ctx ctx = {checkpoint, p};
    tp_sb sb = {0};
    tp_write_begin(&sb, limit);
    sb.count_only = true;
    tp_emit_root(&sb, &ctx, p, &defaults);
    if (sb.limit_exceeded) {
        return tp_error_set(err, TP_STATUS_OUT_OF_BOUNDS,
                            "serialized project exceeds the %zu-byte checkpoint limit",
                            limit);
    }
    *out_len = sb.len;
    return TP_STATUS_OK;
}

tp_status tp_project_serialized_size_bounded(const tp_project *p, size_t limit,
                                             size_t *out_len, tp_error *err) {
    return project_serialized_size_bounded_mode(p, false, limit, out_len, err);
}

tp_status tp_project_checkpoint_serialized_size_bounded(
    const tp_project *p, size_t limit, size_t *out_len, tp_error *err) {
    return project_serialized_size_bounded_mode(p, true, limit, out_len, err);
}

void tp_project_write_note_load_buffer_call(void) {
#ifdef TP_ENABLE_TEST_SEAMS
    s_test_load_buffer_calls++;
#endif
}
