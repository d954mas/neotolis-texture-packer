#ifndef TP_PROJECT_OWNED_INTERNAL_H
#define TP_PROJECT_OWNED_INTERNAL_H

#include <stddef.h>

#include "tp_core/tp_project.h"

typedef void *(*tp_project_owned_alloc_fn)(void *context, size_t size);

typedef struct tp_project_owned_allocator {
    tp_project_owned_alloc_fn allocate;
    void *context;
} tp_project_owned_allocator;

tp_status tp_project_owned_copy_project(tp_project *dst, const tp_project *src,
                                        tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_atlas(tp_project_atlas *dst,
                                      const tp_project_atlas *src,
                                      tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_source(tp_project_source *dst,
                                       const tp_project_source *src,
                                       tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_sprite(tp_project_sprite *dst,
                                       const tp_project_sprite *src,
                                       tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_anim(tp_project_anim *dst,
                                     const tp_project_anim *src,
                                     tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_frame(tp_project_frame *dst,
                                      const tp_project_frame *src,
                                      tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_frames(tp_project_frame **out,
                                       const tp_project_frame *src, int count,
                                       tp_project_owned_allocator allocator);
tp_status tp_project_owned_copy_target(tp_project_target *dst,
                                       const tp_project_target *src,
                                       tp_project_owned_allocator allocator);

void tp_project_owned_free_atlas(tp_project_atlas *atlas);
void tp_project_owned_free_source(tp_project_source *source);
void tp_project_owned_free_sprite(tp_project_sprite *sprite);
void tp_project_owned_free_anim(tp_project_anim *animation);
void tp_project_owned_free_frame(tp_project_frame *frame);
void tp_project_owned_free_frames(tp_project_frame *frames, int count);
void tp_project_owned_free_target(tp_project_target *target);

#endif
