#include "tp_lua_host_private.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lauxlib.h"
#include "lualib.h"

#include "tp_utf8_internal.h"

#define TP_LUA_MT_ATLAS "ntpacker.lua.atlas.v1"
#define TP_LUA_MT_PAGE "ntpacker.lua.page.v1"
#define TP_LUA_MT_SPRITE "ntpacker.lua.sprite.v1"
#define TP_LUA_MT_ANIMATION "ntpacker.lua.animation.v1"
#define TP_LUA_MT_HOST "ntpacker.lua.host.v1"
#define TP_LUA_MT_WRITER "ntpacker.lua.writer.v1"

static const char *const g_transform_names[8] = {
    "identity",       "flip_h",       "flip_v",       "rotate_180",
    "transpose",      "rotate_90_cw", "rotate_90_ccw", "anti_transpose",
};

static const char *metatable_for_kind(tp_lua_view_kind kind) {
    switch (kind) {
        case TP_LUA_VIEW_ATLAS: return TP_LUA_MT_ATLAS;
        case TP_LUA_VIEW_PAGE: return TP_LUA_MT_PAGE;
        case TP_LUA_VIEW_SPRITE: return TP_LUA_MT_SPRITE;
        case TP_LUA_VIEW_ANIMATION: return TP_LUA_MT_ANIMATION;
        case TP_LUA_VIEW_HOST: return TP_LUA_MT_HOST;
        case TP_LUA_VIEW_WRITER: return TP_LUA_MT_WRITER;
    }
    return NULL;
}

static int contract(lua_State *state, const char *message) {
    return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_CONTRACT,
                        TP_FORMAT_PHASE_RUNTIME, message);
}

static void require_gate(lua_State *state, bool writer_call) {
    if (!tp_lua_gate(state, writer_call)) {
        (void)lua_error(state); /* private sentinel was left on top */
    }
}

static bool exact_args(lua_State *state, int count) {
    return lua_gettop(state) == count;
}

static tp_lua_view *view(lua_State *state, int argument,
                         tp_lua_view_kind kind) {
    const char *metatable = metatable_for_kind(kind);
    tp_lua_view *value = metatable
                             ? (tp_lua_view *)luaL_testudata(
                                   state, argument, metatable)
                             : NULL;
    tp_lua_runtime_context *context = tp_lua_context(state);
    return value && value->context == context && value->kind == kind ? value
                                                                     : NULL;
}

static void push_view(lua_State *state, tp_lua_runtime_context *context,
                      tp_lua_view_kind kind, size_t index) {
    tp_lua_view *value =
        (tp_lua_view *)lua_newuserdatauv(state, sizeof *value, 0);
    value->context = context;
    value->index = index;
    value->kind = kind;
    luaL_getmetatable(state, metatable_for_kind(kind));
    lua_setmetatable(state, -2);
}

static bool collection_index(lua_State *state, int argument, size_t count,
                             size_t *out) {
    if (!lua_isinteger(state, argument)) return false;
    const lua_Integer index = lua_tointeger(state, argument);
    if (index < 1 || (uint64_t)index > (uint64_t)count) return false;
    *out = (size_t)index - 1U;
    return true;
}

static int atlas_name(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    if (!exact_args(state, 1) || !atlas)
        return contract(state, "atlas:name expects an atlas");
    lua_pushstring(
        state, atlas->context->input->projected_ir->value->atlas_name);
    return 1;
}

static int atlas_pixels_per_unit(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    if (!exact_args(state, 1) || !atlas)
        return contract(state, "atlas:pixels_per_unit expects an atlas");
    lua_pushnumber(
        state,
        (lua_Number)atlas->context->input->projected_ir->value->pixels_per_unit);
    return 1;
}

static int atlas_page_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    if (!exact_args(state, 1) || !atlas)
        return contract(state, "atlas:page_count expects an atlas");
    lua_pushinteger(
        state,
        (lua_Integer)atlas->context->input->projected_ir->value->page_count);
    return 1;
}

static int atlas_page(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    size_t index = 0U;
    const int count =
        atlas ? atlas->context->input->projected_ir->value->page_count : 0;
    if (!exact_args(state, 2) || !atlas ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "atlas:page index is out of range");
    }
    push_view(state, atlas->context, TP_LUA_VIEW_PAGE, index);
    return 1;
}

static int atlas_sprite_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    if (!exact_args(state, 1) || !atlas)
        return contract(state, "atlas:sprite_count expects an atlas");
    lua_pushinteger(
        state,
        (lua_Integer)atlas->context->input->projected_ir->value->sprite_count);
    return 1;
}

static int atlas_sprite(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    size_t index = 0U;
    const int count =
        atlas ? atlas->context->input->projected_ir->value->sprite_count : 0;
    if (!exact_args(state, 2) || !atlas ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "atlas:sprite index is out of range");
    }
    push_view(state, atlas->context, TP_LUA_VIEW_SPRITE, index);
    return 1;
}

static int atlas_animation_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    if (!exact_args(state, 1) || !atlas)
        return contract(state, "atlas:animation_count expects an atlas");
    lua_pushinteger(
        state,
        (lua_Integer)atlas->context->input->projected_ir->value->animation_count);
    return 1;
}

static int atlas_animation(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *atlas = view(state, 1, TP_LUA_VIEW_ATLAS);
    size_t index = 0U;
    const int count =
        atlas ? atlas->context->input->projected_ir->value->animation_count : 0;
    if (!exact_args(state, 2) || !atlas ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "atlas:animation index is out of range");
    }
    push_view(state, atlas->context, TP_LUA_VIEW_ANIMATION, index);
    return 1;
}

static const tp_export_page *page_value(tp_lua_view *page) {
    return &page->context->input->projected_ir->value->pages[page->index];
}

static int page_index(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *page = view(state, 1, TP_LUA_VIEW_PAGE);
    if (!exact_args(state, 1) || !page)
        return contract(state, "page:index expects a page");
    lua_pushinteger(state, (lua_Integer)(page->index + 1U));
    return 1;
}

static int page_width(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *page = view(state, 1, TP_LUA_VIEW_PAGE);
    if (!exact_args(state, 1) || !page)
        return contract(state, "page:width expects a page");
    lua_pushinteger(state, (lua_Integer)page_value(page)->w);
    return 1;
}

static int page_height(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *page = view(state, 1, TP_LUA_VIEW_PAGE);
    if (!exact_args(state, 1) || !page)
        return contract(state, "page:height expects a page");
    lua_pushinteger(state, (lua_Integer)page_value(page)->h);
    return 1;
}

static int page_image(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *page = view(state, 1, TP_LUA_VIEW_PAGE);
    if (!exact_args(state, 1) || !page)
        return contract(state, "page:image expects a page");
    lua_pushstring(
        state,
        page->context->input->projected_ir->page_images[page->index]);
    return 1;
}

static const tp_export_sprite *sprite_value(tp_lua_view *sprite) {
    return &sprite->context->input->projected_ir->value->sprites[sprite->index];
}

static int sprite_name(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:name expects a sprite");
    lua_pushstring(state, sprite_value(sprite)->final_name);
    return 1;
}

static int sprite_page(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:page expects a sprite");
    lua_pushinteger(state, (lua_Integer)sprite_value(sprite)->data.page + 1);
    return 1;
}

static int sprite_frame(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:frame expects a sprite");
    const tp_sprite *value = &sprite_value(sprite)->data;
    lua_pushinteger(state, value->frame.x);
    lua_pushinteger(state, value->frame.y);
    lua_pushinteger(state, value->frame.w);
    lua_pushinteger(state, value->frame.h);
    return 4;
}

static int sprite_footprint(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:footprint expects a sprite");
    const tp_sprite *value = &sprite_value(sprite)->data;
    const bool diagonal = (value->transform & 4U) != 0U;
    lua_pushinteger(state, diagonal ? value->frame.h : value->frame.w);
    lua_pushinteger(state, diagonal ? value->frame.w : value->frame.h);
    return 2;
}

static int sprite_trim_rect(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:trim_rect expects a sprite");
    const tp_sprite *value = &sprite_value(sprite)->data;
    lua_pushinteger(state, value->spriteSourceSize.x);
    lua_pushinteger(state, value->spriteSourceSize.y);
    lua_pushinteger(state, value->spriteSourceSize.w);
    lua_pushinteger(state, value->spriteSourceSize.h);
    return 4;
}

static int sprite_source_size(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:source_size expects a sprite");
    const tp_sprite *value = &sprite_value(sprite)->data;
    lua_pushinteger(state, value->sourceSize.w);
    lua_pushinteger(state, value->sourceSize.h);
    return 2;
}

static int sprite_transform(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:transform expects a sprite");
    lua_pushstring(
        state, g_transform_names[sprite_value(sprite)->data.transform & 7U]);
    return 1;
}

static int sprite_trimmed(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:trimmed expects a sprite");
    lua_pushboolean(state, sprite_value(sprite)->data.trimmed);
    return 1;
}

static int sprite_is_solid(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:is_solid expects a sprite");
    lua_pushboolean(state, sprite_value(sprite)->is_solid);
    return 1;
}

static int sprite_pivot(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:pivot expects a sprite");
    if (!sprite->context->input->projected_ir->pivot_visible) {
        lua_pushnil(state);
        return 1;
    }
    const tp_sprite *value = &sprite_value(sprite)->data;
    lua_pushnumber(state, (lua_Number)value->pivot.x);
    lua_pushnumber(state, (lua_Number)value->pivot.y);
    return 2;
}

static int sprite_slice9(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:slice9 expects a sprite");
    const tp_sprite *value = &sprite_value(sprite)->data;
    if (!sprite->context->input->projected_ir->slice9_visible ||
        (value->slice9_lrtb[0] | value->slice9_lrtb[1] |
         value->slice9_lrtb[2] | value->slice9_lrtb[3]) == 0U) {
        lua_pushnil(state);
        return 1;
    }
    for (int i = 0; i < 4; ++i)
        lua_pushinteger(state, value->slice9_lrtb[i]);
    return 4;
}

static int sprite_alias_of(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:alias_of expects a sprite");
    const int alias = sprite_value(sprite)->data.alias_of;
    if (!sprite->context->input->projected_ir->aliases_visible || alias < 0) {
        lua_pushnil(state);
    } else {
        lua_pushinteger(state, (lua_Integer)alias + 1);
    }
    return 1;
}

static int sprite_vertex_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:vertex_count expects a sprite");
    const int count = sprite->context->input->projected_ir->polygons_visible
                          ? sprite_value(sprite)->data.vert_count
                          : 0;
    lua_pushinteger(state, count);
    return 1;
}

static int sprite_vertex(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    size_t index = 0U;
    const int count =
        sprite && sprite->context->input->projected_ir->polygons_visible
            ? sprite_value(sprite)->data.vert_count
            : 0;
    if (!exact_args(state, 2) || !sprite ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "sprite:vertex index is out of range");
    }
    const tp_point value = sprite_value(sprite)->data.verts[index];
    lua_pushinteger(state, value.x);
    lua_pushinteger(state, value.y);
    return 2;
}

static int sprite_index_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    if (!exact_args(state, 1) || !sprite)
        return contract(state, "sprite:index_count expects a sprite");
    const int count = sprite->context->input->projected_ir->polygons_visible
                          ? sprite_value(sprite)->data.index_count
                          : 0;
    lua_pushinteger(state, count);
    return 1;
}

static int sprite_index(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *sprite = view(state, 1, TP_LUA_VIEW_SPRITE);
    size_t index = 0U;
    const int count =
        sprite && sprite->context->input->projected_ir->polygons_visible
            ? sprite_value(sprite)->data.index_count
            : 0;
    if (!exact_args(state, 2) || !sprite ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "sprite:index index is out of range");
    }
    lua_pushinteger(state, sprite_value(sprite)->data.indices[index]);
    return 1;
}

static const tp_export_anim *animation_value(tp_lua_view *animation) {
    return &animation->context->input->projected_ir->value
                ->animations[animation->index];
}

static int animation_id(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:id expects an animation");
    lua_pushstring(state, animation_value(animation)->id);
    return 1;
}

static int animation_frame_count(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:frame_count expects an animation");
    lua_pushinteger(state, animation_value(animation)->frame_count);
    return 1;
}

static int animation_frame(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    size_t index = 0U;
    const int count = animation ? animation_value(animation)->frame_count : 0;
    if (!exact_args(state, 2) || !animation ||
        !collection_index(state, 2, (size_t)count, &index)) {
        return contract(state, "animation:frame index is out of range");
    }
    lua_pushstring(state, animation_value(animation)->frames[index]);
    return 1;
}

static int animation_fps(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:fps expects an animation");
    lua_pushnumber(state, (lua_Number)animation_value(animation)->fps);
    return 1;
}

static int animation_playback(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:playback expects an animation");
    lua_pushinteger(state, animation_value(animation)->playback);
    return 1;
}

static int animation_flip_h(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:flip_h expects an animation");
    lua_pushboolean(state, animation_value(animation)->flip_h);
    return 1;
}

static int animation_flip_v(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *animation = view(state, 1, TP_LUA_VIEW_ANIMATION);
    if (!exact_args(state, 1) || !animation)
        return contract(state, "animation:flip_v expects an animation");
    lua_pushboolean(state, animation_value(animation)->flip_v);
    return 1;
}

static bool text_argument(lua_State *state, int argument, const char **out,
                          size_t *out_length) {
    if (lua_type(state, argument) != LUA_TSTRING) return false;
    *out = lua_tolstring(state, argument, out_length);
    return *out && tp_lua_text_valid(*out, *out_length);
}

static int host_document(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *host = view(state, 1, TP_LUA_VIEW_HOST);
    const char *id = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !host ||
        !text_argument(state, 2, &id, &length)) {
        return contract(state, "host:document expects a text document id");
    }
    for (size_t i = 0U; i < host->context->document_count; ++i) {
        tp_lua_document_state *document = &host->context->documents[i];
        if (strlen(document->id) != length ||
            memcmp(document->id, id, length) != 0)
            continue;
        if (document->opened) {
            return tp_lua_raise(
                state, TP_FORMAT_DIAGNOSTIC_DOCUMENT_DUPLICATE,
                TP_FORMAT_PHASE_OUTPUT,
                "declared document was opened more than once");
        }
        document->opened = true;
        push_view(state, host->context, TP_LUA_VIEW_WRITER, i);
        return 1;
    }
    return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_DOCUMENT_UNKNOWN,
                        TP_FORMAT_PHASE_OUTPUT,
                        "handler requested an undeclared document");
}

static int host_fact(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *host = view(state, 1, TP_LUA_VIEW_HOST);
    const char *id = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !host ||
        !text_argument(state, 2, &id, &length)) {
        return contract(state, "host:fact expects a text fact id");
    }
    for (size_t i = 0U; i < host->context->input->fact_count; ++i) {
        const tp_lua_fact_value *fact = &host->context->input->facts[i];
        if (strlen(fact->id) == length &&
            memcmp(fact->id, id, length) == 0) {
            lua_pushstring(state, fact->value);
            return 1;
        }
    }
    return contract(state, "host:fact requested an undeclared fact");
}

static int host_notice(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *host = view(state, 1, TP_LUA_VIEW_HOST);
    const char *message = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !host ||
        !text_argument(state, 2, &message, &length)) {
        return contract(
            state, "host:notice expects valid UTF-8 text without NUL");
    }
    tp_lua_runtime_context *context = host->context;
    if (length > context->limits.notice_message_bytes ||
        context->notice_count >= context->limits.notices ||
        length > context->limits.notice_total_bytes -
                     context->notice_total_bytes) {
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_NOTICE_LIMIT,
                            TP_FORMAT_PHASE_LIMIT,
                            "Lua notice output exceeded a fixed limit");
    }
    if (context->notice_count == context->notice_capacity) {
        size_t capacity =
            context->notice_capacity ? context->notice_capacity * 2U : 8U;
        if (capacity > context->limits.notices)
            capacity = context->limits.notices;
        tp_lua_notice_state *items = (tp_lua_notice_state *)realloc(
            context->notices, capacity * sizeof *items);
        if (!items) {
            context->allocator.host_oom = true;
            return tp_lua_raise(
                state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                TP_FORMAT_PHASE_RUNTIME,
                "host allocation failed while recording a notice");
        }
        context->notices = items;
        context->notice_capacity = capacity;
    }
    char *copy = (char *)malloc(length + 1U);
    if (!copy) {
        context->allocator.host_oom = true;
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                            TP_FORMAT_PHASE_RUNTIME,
                            "host allocation failed while recording a notice");
    }
    memcpy(copy, message, length);
    copy[length] = '\0';
    context->notices[context->notice_count++] =
        (tp_lua_notice_state){copy, length};
    context->notice_total_bytes += length;
    return 0;
}

static int host_fail(lua_State *state) {
    require_gate(state, false);
    tp_lua_view *host = view(state, 1, TP_LUA_VIEW_HOST);
    const char *message = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !host ||
        !text_argument(state, 2, &message, &length)) {
        return contract(state,
                        "host:fail expects valid UTF-8 text without NUL");
    }
    char bounded[TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES + 1U];
    const size_t count = length <= TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES
                             ? length
                             : TP_FORMAT_DIAGNOSTIC_MESSAGE_MAX_BYTES;
    memcpy(bounded, message, count);
    bounded[count] = '\0';
    tp_error_trim_partial_utf8(bounded);
    return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
                        TP_FORMAT_PHASE_RUNTIME, bounded);
}

static tp_lua_document_state *writer_document(lua_State *state,
                                              tp_lua_view **out_writer) {
    tp_lua_view *writer = view(state, 1, TP_LUA_VIEW_WRITER);
    if (!writer || writer->index >= writer->context->document_count) return NULL;
    *out_writer = writer;
    return &writer->context->documents[writer->index];
}

static int append_document(lua_State *state, tp_lua_runtime_context *context,
                           tp_lua_document_state *document,
                           const char *bytes, size_t length) {
    if (document->finished) {
        return tp_lua_raise(
            state, TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH,
            TP_FORMAT_PHASE_OUTPUT, "document writer was used after finish");
    }
    if (length > context->limits.writer_argument_bytes ||
        length > context->limits.document_bytes - document->bytes.len ||
        length > context->limits.document_total_bytes -
                     context->document_total_bytes) {
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT,
                            TP_FORMAT_PHASE_LIMIT,
                            "Lua document output exceeded a fixed limit");
    }
    const size_t before = document->bytes.len;
    tp_sb_write(&document->bytes, bytes, length);
    if (document->bytes.oom) {
        context->allocator.host_oom = true;
        return tp_lua_raise(
            state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
            TP_FORMAT_PHASE_RUNTIME,
            "host allocation failed while writing a document");
    }
    if (document->bytes.limit_exceeded) {
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT,
                            TP_FORMAT_PHASE_LIMIT,
                            "Lua document output exceeded a fixed limit");
    }
    context->document_total_bytes += document->bytes.len - before;
    return 0;
}

static int writer_write(lua_State *state) {
    require_gate(state, true);
    tp_lua_view *writer = NULL;
    tp_lua_document_state *document = writer_document(state, &writer);
    const char *text = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !document ||
        !text_argument(state, 2, &text, &length)) {
        return contract(
            state, "writer:write expects valid UTF-8 text without NUL");
    }
    return append_document(state, writer->context, document, text, length);
}

static size_t json_escaped_length(const unsigned char *text, size_t length) {
    size_t total = 2U;
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = text[i];
        size_t add = 1U;
        if (value == '"' || value == '\\' || value == '\b' || value == '\f' ||
            value == '\n' || value == '\r' || value == '\t') {
            add = 2U;
        } else if (value < 0x20U) {
            add = 6U;
        }
        if (total > SIZE_MAX - add) return SIZE_MAX;
        total += add;
    }
    return total;
}

static int writer_write_json_string(lua_State *state) {
    require_gate(state, true);
    tp_lua_view *writer = NULL;
    tp_lua_document_state *document = writer_document(state, &writer);
    const char *text = NULL;
    size_t length = 0U;
    if (!exact_args(state, 2) || !document ||
        !text_argument(state, 2, &text, &length)) {
        return contract(state,
                        "writer:write_json_string expects valid UTF-8 text");
    }
    const size_t escaped =
        json_escaped_length((const unsigned char *)text, length);
    if (length > writer->context->limits.writer_argument_bytes ||
        escaped == SIZE_MAX ||
        escaped > writer->context->limits.document_bytes -
                      document->bytes.len ||
        escaped > writer->context->limits.document_total_bytes -
                      writer->context->document_total_bytes) {
        return tp_lua_raise(state, TP_FORMAT_DIAGNOSTIC_OUTPUT_LIMIT,
                            TP_FORMAT_PHASE_LIMIT,
                            "Lua document output exceeded a fixed limit");
    }
    tp_sb scratch = {.limit = escaped};
    tp_sb_char(&scratch, '"');
    for (size_t i = 0U; i < length; ++i) {
        const unsigned char value = (unsigned char)text[i];
        switch (value) {
            case '"': tp_sb_str(&scratch, "\\\""); break;
            case '\\': tp_sb_str(&scratch, "\\\\"); break;
            case '\b': tp_sb_str(&scratch, "\\b"); break;
            case '\f': tp_sb_str(&scratch, "\\f"); break;
            case '\n': tp_sb_str(&scratch, "\\n"); break;
            case '\r': tp_sb_str(&scratch, "\\r"); break;
            case '\t': tp_sb_str(&scratch, "\\t"); break;
            default:
                if (value < 0x20U) {
                    char escape[7];
                    (void)snprintf(escape, sizeof escape, "\\u%04x", value);
                    tp_sb_str(&scratch, escape);
                } else {
                    tp_sb_char(&scratch, (char)value);
                }
                break;
        }
    }
    tp_sb_char(&scratch, '"');
    if (scratch.oom) {
        tp_sb_free(&scratch);
        writer->context->allocator.host_oom = true;
        return tp_lua_raise(
            state, TP_FORMAT_DIAGNOSTIC_HANDLER_FAILED,
            TP_FORMAT_PHASE_RUNTIME,
            "host allocation failed while escaping JSON text");
    }
    const int result = append_document(state, writer->context, document,
                                       scratch.buf, scratch.len);
    tp_sb_free(&scratch);
    return result;
}

static int writer_number_text(lua_State *state, const char *text) {
    tp_lua_view *writer = NULL;
    tp_lua_document_state *document = writer_document(state, &writer);
    if (!document)
        return contract(state, "writer method expects a writer");
    return append_document(state, writer->context, document, text, strlen(text));
}

static int writer_write_i64(lua_State *state) {
    require_gate(state, true);
    if (!exact_args(state, 2) || !lua_isinteger(state, 2))
        return contract(state, "writer:write_i64 expects an integer");
    char text[32];
    (void)snprintf(text, sizeof text, "%" PRId64,
                   (int64_t)lua_tointeger(state, 2));
    return writer_number_text(state, text);
}

static int writer_write_u64(lua_State *state) {
    require_gate(state, true);
    if (!exact_args(state, 2) || !lua_isinteger(state, 2) ||
        lua_tointeger(state, 2) < 0) {
        return contract(
            state, "writer:write_u64 expects an integer in 0..INT64_MAX");
    }
    char text[32];
    (void)snprintf(text, sizeof text, "%" PRIu64,
                   (uint64_t)lua_tointeger(state, 2));
    return writer_number_text(state, text);
}

static int writer_write_f32(lua_State *state) {
    require_gate(state, true);
    if (!exact_args(state, 2) || lua_type(state, 2) != LUA_TNUMBER) {
        return contract(state, "writer:write_f32 expects a finite number");
    }
    const lua_Number value = lua_tonumber(state, 2);
    const float rounded = (float)value;
    if (!isfinite((double)value) || !isfinite((double)rounded)) {
        return contract(state,
                        "writer:write_f32 expects a finite binary32 value");
    }
    char text[64];
    (void)snprintf(text, sizeof text, "%.9g", (double)rounded);
    return writer_number_text(state, text);
}

static int writer_write_bool(lua_State *state) {
    require_gate(state, true);
    if (!exact_args(state, 2) || lua_type(state, 2) != LUA_TBOOLEAN) {
        return contract(state, "writer:write_bool expects a boolean");
    }
    return writer_number_text(state,
                              lua_toboolean(state, 2) ? "true" : "false");
}

static int writer_write_null(lua_State *state) {
    require_gate(state, true);
    if (!exact_args(state, 1) ||
        !view(state, 1, TP_LUA_VIEW_WRITER)) {
        return contract(state, "writer:write_null expects a writer");
    }
    return writer_number_text(state, "null");
}

static int writer_finish(lua_State *state) {
    require_gate(state, true);
    tp_lua_view *writer = NULL;
    tp_lua_document_state *document = writer_document(state, &writer);
    (void)writer;
    if (!exact_args(state, 1) || !document)
        return contract(state, "writer:finish expects a writer");
    if (document->finished) {
        return tp_lua_raise(
            state, TP_FORMAT_DIAGNOSTIC_DOCUMENT_WRITE_AFTER_FINISH,
            TP_FORMAT_PHASE_OUTPUT,
            "document writer was finished more than once");
    }
    document->finished = true;
    return 0;
}

static const luaL_Reg g_atlas_methods[] = {
    {"name", atlas_name},
    {"pixels_per_unit", atlas_pixels_per_unit},
    {"page_count", atlas_page_count},
    {"page", atlas_page},
    {"sprite_count", atlas_sprite_count},
    {"sprite", atlas_sprite},
    {"animation_count", atlas_animation_count},
    {"animation", atlas_animation},
    {NULL, NULL},
};

static const luaL_Reg g_page_methods[] = {
    {"index", page_index}, {"width", page_width}, {"height", page_height},
    {"image", page_image}, {NULL, NULL},
};

static const luaL_Reg g_sprite_methods[] = {
    {"name", sprite_name},
    {"page", sprite_page},
    {"frame", sprite_frame},
    {"footprint", sprite_footprint},
    {"trim_rect", sprite_trim_rect},
    {"source_size", sprite_source_size},
    {"transform", sprite_transform},
    {"trimmed", sprite_trimmed},
    {"is_solid", sprite_is_solid},
    {"pivot", sprite_pivot},
    {"slice9", sprite_slice9},
    {"alias_of", sprite_alias_of},
    {"vertex_count", sprite_vertex_count},
    {"vertex", sprite_vertex},
    {"index_count", sprite_index_count},
    {"index", sprite_index},
    {NULL, NULL},
};

static const luaL_Reg g_animation_methods[] = {
    {"id", animation_id},
    {"frame_count", animation_frame_count},
    {"frame", animation_frame},
    {"fps", animation_fps},
    {"playback", animation_playback},
    {"flip_h", animation_flip_h},
    {"flip_v", animation_flip_v},
    {NULL, NULL},
};

static const luaL_Reg g_host_methods[] = {
    {"document", host_document},
    {"fact", host_fact},
    {"notice", host_notice},
    {"fail", host_fail},
    {NULL, NULL},
};

static const luaL_Reg g_writer_methods[] = {
    {"write", writer_write},
    {"write_json_string", writer_write_json_string},
    {"write_i64", writer_write_i64},
    {"write_u64", writer_write_u64},
    {"write_f32", writer_write_f32},
    {"write_bool", writer_write_bool},
    {"write_null", writer_write_null},
    {"finish", writer_finish},
    {NULL, NULL},
};

static void make_metatable(lua_State *state, const char *name,
                           const luaL_Reg *methods) {
    (void)luaL_newmetatable(state, name);
    lua_newtable(state);
    luaL_setfuncs(state, methods, 0);
    lua_setfield(state, -2, "__index");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_pop(state, 1);
}

static void copy_field(lua_State *state, int source, int destination,
                       const char *name) {
    source = lua_absindex(state, source);
    destination = lua_absindex(state, destination);
    lua_getfield(state, source, name);
    lua_setfield(state, destination, name);
}

static void copy_fields(lua_State *state, int source, int destination,
                        const char *const *names) {
    for (size_t i = 0U; names[i]; ++i)
        copy_field(state, source, destination, names[i]);
}

static void install_sandbox(lua_State *state, int environment) {
    static const char *const base_names[] = {
        "assert", "error", "select", "type", NULL};
    static const char *const math_names[] = {
        "abs",        "ceil",       "floor",      "max",
        "min",        "tointeger",  "type",       "ult",
        "maxinteger", "mininteger", NULL};
    static const char *const string_names[] = {
        "byte", "char", "len", "sub", NULL};
    static const char *const utf8_names[] = {
        "char", "codepoint", "codes", "len", "offset", NULL};
    environment = lua_absindex(state, environment);

    (void)luaopen_base(state);
    copy_fields(state, -1, environment, base_names);
    lua_pop(state, 1);

    (void)luaopen_math(state);
    lua_newtable(state);
    copy_fields(state, -2, -1, math_names);
    lua_setfield(state, environment, "math");
    lua_pop(state, 1);

    (void)luaopen_string(state);
    const int full_string = lua_absindex(state, -1);
    lua_newtable(state);
    const int safe_string = lua_absindex(state, -1);
    copy_fields(state, full_string, safe_string, string_names);
    lua_pushliteral(state, "");
    (void)lua_getmetatable(state, -1);
    lua_pushvalue(state, safe_string);
    lua_setfield(state, -2, "__index");
    lua_pop(state, 2);
    lua_pushvalue(state, safe_string);
    lua_setfield(state, environment, "string");
    lua_pop(state, 2);

    (void)luaopen_utf8(state);
    lua_newtable(state);
    copy_fields(state, -2, -1, utf8_names);
    lua_setfield(state, environment, "utf8");
    lua_pop(state, 1);

    lua_pushvalue(state, environment);
    lua_rawseti(state, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
}

tp_status tp_lua_bindings_install(lua_State *state,
                                  tp_lua_runtime_context *context,
                                  int *out_environment_index,
                                  tp_error *error) {
    if (!state || !context || !out_environment_index) {
        return tp_error_set(error, TP_STATUS_INVALID_ARGUMENT,
                            "Lua sandbox requires state, context, and output");
    }
    make_metatable(state, TP_LUA_MT_ATLAS, g_atlas_methods);
    make_metatable(state, TP_LUA_MT_PAGE, g_page_methods);
    make_metatable(state, TP_LUA_MT_SPRITE, g_sprite_methods);
    make_metatable(state, TP_LUA_MT_ANIMATION, g_animation_methods);
    make_metatable(state, TP_LUA_MT_HOST, g_host_methods);
    make_metatable(state, TP_LUA_MT_WRITER, g_writer_methods);
    lua_newtable(state);
    const int environment = lua_absindex(state, -1);
    install_sandbox(state, environment);
    *out_environment_index = environment;
    return TP_STATUS_OK;
}

void tp_lua_push_atlas(lua_State *state, tp_lua_runtime_context *context) {
    push_view(state, context, TP_LUA_VIEW_ATLAS, 0U);
}

void tp_lua_push_host(lua_State *state, tp_lua_runtime_context *context) {
    push_view(state, context, TP_LUA_VIEW_HOST, 0U);
}
