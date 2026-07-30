#include "tp_project_owned_internal.h"

#include <stdlib.h>
#include <string.h>

static void *owned_array(tp_project_owned_allocator a, int count, size_t size) {
    return count > 0 ? a.allocate(a.context, (size_t)count * size) : NULL;
}

static tp_status owned_dup(tp_project_owned_allocator a, const char *src,
                           char **out) {
    *out = NULL;
    if (!src) {
        return TP_STATUS_OK;
    }
    const size_t size = strlen(src) + 1U;
    char *copy = a.allocate(a.context, size);
    if (!copy) {
        return TP_STATUS_OOM;
    }
    memcpy(copy, src, size);
    *out = copy;
    return TP_STATUS_OK;
}

void tp_project_owned_free_source(tp_project_source *source) {
    if (source) {
        free(source->path);
        source->path = NULL;
    }
}

void tp_project_owned_free_sprite(tp_project_sprite *sprite) {
    if (!sprite) {
        return;
    }
    free(sprite->name);
    free(sprite->src_key);
    free(sprite->rename);
    sprite->name = sprite->src_key = sprite->rename = NULL;
}

void tp_project_owned_free_frame(tp_project_frame *frame) {
    if (!frame) {
        return;
    }
    free(frame->name);
    free(frame->src_key);
    frame->name = frame->src_key = NULL;
}

void tp_project_owned_free_frames(tp_project_frame *frames, int count) {
    for (int i = 0; frames && i < count; ++i) {
        tp_project_owned_free_frame(&frames[i]);
    }
    free(frames);
}

void tp_project_owned_free_anim(tp_project_anim *animation) {
    if (!animation) {
        return;
    }
    free(animation->name);
    tp_project_owned_free_frames(animation->frames, animation->frame_count);
    animation->name = NULL;
    animation->frames = NULL;
    animation->frame_count = animation->frame_cap = 0;
}

void tp_project_owned_free_target(tp_project_target *target) {
    if (!target) {
        return;
    }
    free(target->exporter_id);
    free(target->out_path);
    target->exporter_id = target->out_path = NULL;
}

void tp_project_owned_free_atlas(tp_project_atlas *atlas) {
    if (!atlas) {
        return;
    }
    free(atlas->name);
    for (int i = 0; i < atlas->source_count; ++i) {
        tp_project_owned_free_source(&atlas->sources[i]);
    }
    free(atlas->sources);
    for (int i = 0; i < atlas->sprite_count; ++i) {
        tp_project_owned_free_sprite(&atlas->sprites[i]);
    }
    free(atlas->sprites);
    for (int i = 0; i < atlas->animation_count; ++i) {
        tp_project_owned_free_anim(&atlas->animations[i]);
    }
    free(atlas->animations);
    for (int i = 0; i < atlas->target_count; ++i) {
        tp_project_owned_free_target(&atlas->targets[i]);
    }
    free(atlas->targets);
    memset(atlas, 0, sizeof *atlas);
}

tp_status tp_project_owned_copy_source(tp_project_source *dst,
                                       const tp_project_source *src,
                                       tp_project_owned_allocator a) {
    memset(dst, 0, sizeof *dst);
    dst->id = src->id;
    dst->kind = src->kind;
    return owned_dup(a, src->path, &dst->path);
}

tp_status tp_project_owned_copy_sprite(tp_project_sprite *dst,
                                       const tp_project_sprite *src,
                                       tp_project_owned_allocator a) {
    *dst = *src;
    dst->name = dst->src_key = dst->rename = NULL;
    tp_status status = owned_dup(a, src->name, &dst->name);
    if (status == TP_STATUS_OK) {
        status = owned_dup(a, src->src_key, &dst->src_key);
    }
    if (status == TP_STATUS_OK) {
        status = owned_dup(a, src->rename, &dst->rename);
    }
    if (status != TP_STATUS_OK) {
        tp_project_owned_free_sprite(dst);
    }
    return status;
}

tp_status tp_project_owned_copy_frame(tp_project_frame *dst,
                                      const tp_project_frame *src,
                                      tp_project_owned_allocator a) {
    memset(dst, 0, sizeof *dst);
    dst->source_ref = src->source_ref;
    tp_status status = owned_dup(a, src->name, &dst->name);
    if (status == TP_STATUS_OK) {
        status = owned_dup(a, src->src_key, &dst->src_key);
    }
    if (status != TP_STATUS_OK) {
        tp_project_owned_free_frame(dst);
    }
    return status;
}

tp_status tp_project_owned_copy_frames(tp_project_frame **out,
                                       const tp_project_frame *src, int count,
                                       tp_project_owned_allocator a) {
    *out = NULL;
    if (count <= 0) {
        return TP_STATUS_OK;
    }
    tp_project_frame *frames = owned_array(a, count, sizeof *frames);
    if (!frames) {
        return TP_STATUS_OOM;
    }
    for (int i = 0; i < count; ++i) {
        const tp_status status =
            tp_project_owned_copy_frame(&frames[i], &src[i], a);
        if (status != TP_STATUS_OK) {
            tp_project_owned_free_frames(frames, i + 1);
            return status;
        }
    }
    *out = frames;
    return TP_STATUS_OK;
}

tp_status tp_project_owned_copy_anim(tp_project_anim *dst,
                                     const tp_project_anim *src,
                                     tp_project_owned_allocator a) {
    *dst = *src;
    dst->name = NULL;
    dst->frames = NULL;
    dst->frame_count = dst->frame_cap = 0;
    tp_status status = owned_dup(a, src->name, &dst->name);
    if (status == TP_STATUS_OK) {
        status = tp_project_owned_copy_frames(
            &dst->frames, src->frames, src->frame_count, a);
    }
    if (status != TP_STATUS_OK) {
        tp_project_owned_free_anim(dst);
        return status;
    }
    dst->frame_count = dst->frame_cap = src->frame_count;
    return TP_STATUS_OK;
}

tp_status tp_project_owned_copy_target(tp_project_target *dst,
                                       const tp_project_target *src,
                                       tp_project_owned_allocator a) {
    memset(dst, 0, sizeof *dst);
    dst->id = src->id;
    dst->enabled = src->enabled;
    tp_status status = owned_dup(a, src->exporter_id, &dst->exporter_id);
    if (status == TP_STATUS_OK) {
        status = owned_dup(a, src->out_path, &dst->out_path);
    }
    if (status != TP_STATUS_OK) {
        tp_project_owned_free_target(dst);
    }
    return status;
}

tp_status tp_project_owned_copy_atlas(tp_project_atlas *dst,
                                      const tp_project_atlas *src,
                                      tp_project_owned_allocator a) {
    *dst = *src;
    dst->name = NULL;
    dst->sources = NULL;
    dst->source_count = dst->source_cap = 0;
    dst->sprites = NULL;
    dst->sprite_count = dst->sprite_cap = 0;
    dst->animations = NULL;
    dst->animation_count = dst->animation_cap = 0;
    dst->targets = NULL;
    dst->target_count = dst->target_cap = 0;
    tp_status status = owned_dup(a, src->name, &dst->name);
    if (status != TP_STATUS_OK) {
        return status;
    }
#define COPY_COLLECTION(field, count, cap, type, copy_fn)                    \
    do {                                                                     \
        if (src->count > 0) {                                                \
            dst->field = owned_array(a, src->count, sizeof(type));           \
            if (!dst->field) { status = TP_STATUS_OOM; break; }              \
            dst->cap = src->count;                                           \
            for (int i = 0; i < src->count; ++i) {                           \
                status = copy_fn(&dst->field[i], &src->field[i], a);          \
                dst->count = i + 1;                                          \
                if (status != TP_STATUS_OK) break;                            \
            }                                                                \
        }                                                                    \
    } while (0)
    COPY_COLLECTION(sources, source_count, source_cap, tp_project_source,
                    tp_project_owned_copy_source);
    if (status == TP_STATUS_OK) {
        COPY_COLLECTION(sprites, sprite_count, sprite_cap, tp_project_sprite,
                        tp_project_owned_copy_sprite);
    }
    if (status == TP_STATUS_OK) {
        COPY_COLLECTION(animations, animation_count, animation_cap,
                        tp_project_anim, tp_project_owned_copy_anim);
    }
    if (status == TP_STATUS_OK) {
        COPY_COLLECTION(targets, target_count, target_cap, tp_project_target,
                        tp_project_owned_copy_target);
    }
#undef COPY_COLLECTION
    if (status != TP_STATUS_OK) {
        tp_project_owned_free_atlas(dst);
    }
    return status;
}

tp_status tp_project_owned_copy_project(tp_project *dst, const tp_project *src,
                                        tp_project_owned_allocator a) {
    memset(dst, 0, sizeof *dst);
    tp_status status = owned_dup(a, src->project_dir, &dst->project_dir);
    if (status == TP_STATUS_OK) {
        status = owned_dup(a, src->source_base_dir, &dst->source_base_dir);
    }
    if (status == TP_STATUS_OK && src->atlas_count > 0) {
        dst->atlases = owned_array(a, src->atlas_count, sizeof *dst->atlases);
        if (!dst->atlases) {
            status = TP_STATUS_OOM;
        } else {
            dst->atlas_cap = src->atlas_count;
            for (int i = 0; i < src->atlas_count; ++i) {
                status = tp_project_owned_copy_atlas(
                    &dst->atlases[i], &src->atlases[i], a);
                dst->atlas_count = i + 1;
                if (status != TP_STATUS_OK) {
                    break;
                }
            }
        }
    }
    return status;
}
