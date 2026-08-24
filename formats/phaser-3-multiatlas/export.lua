local function indent(output, depth)
    local index = 1
    while index <= depth do
        output:write("  ")
        index = index + 1
    end
end

local function json_key(output, name)
    output:write_json_string(name)
    output:write(": ")
end

local function json_string(output, name, value)
    json_key(output, name)
    output:write_json_string(value)
end

local function json_i64(output, name, value)
    json_key(output, name)
    output:write_i64(value)
end

local function emit_size(output, name, width, height)
    json_key(output, name)
    output:write("{")
    json_i64(output, "w", width)
    output:write(", ")
    json_i64(output, "h", height)
    output:write("}")
end

local function emit_rect(output, name, x, y, width, height)
    json_key(output, name)
    output:write("{")
    json_i64(output, "x", x)
    output:write(", ")
    json_i64(output, "y", y)
    output:write(", ")
    json_i64(output, "w", width)
    output:write(", ")
    json_i64(output, "h", height)
    output:write("}")
end

local function emit_frame(output, sprite, is_last)
    local frame_x, frame_y, frame_width, frame_height = sprite:frame()
    local trim_x, trim_y, trim_width, trim_height = sprite:trim_rect()
    local source_width, source_height = sprite:source_size()
    local pivot_x, pivot_y = sprite:pivot()

    indent(output, 4)
    output:write("{\n")
    indent(output, 5)
    json_string(output, "filename", sprite:name())
    output:write(",\n")
    indent(output, 5)
    emit_rect(output, "frame", frame_x, frame_y, frame_width, frame_height)
    output:write(",\n")
    indent(output, 5)
    json_key(output, "rotated")
    output:write_bool(sprite:transform() == "rotate_90_cw")
    output:write(",\n")
    indent(output, 5)
    json_key(output, "trimmed")
    output:write_bool(sprite:trimmed())
    output:write(",\n")
    indent(output, 5)
    emit_rect(output, "spriteSourceSize", trim_x, trim_y,
              trim_width, trim_height)
    output:write(",\n")
    indent(output, 5)
    emit_size(output, "sourceSize", source_width, source_height)
    output:write(",\n")
    indent(output, 5)
    json_key(output, "pivot")
    output:write("{")
    json_key(output, "x")
    output:write_f32(pivot_x)
    output:write(", ")
    json_key(output, "y")
    output:write_f32(pivot_y)
    output:write("}")

    local left, right, top, bottom = sprite:slice9()
    if left ~= nil then
        output:write(",\n")
        indent(output, 5)
        emit_rect(output, "scale9Borders", left, top,
                  source_width - left - right,
                  source_height - top - bottom)
    end
    output:write("\n")
    indent(output, 4)
    output:write("}")
    if not is_last then
        output:write(",")
    end
    output:write("\n")
end

return function(atlas, host)
    local sprite_index = 1
    while sprite_index <= atlas:sprite_count() do
        local name = atlas:sprite(sprite_index):name()
        if name == "__BASE" or name == "__proto__" or
           name == "hasOwnProperty" then
            host:fail("reserved Phaser frame name: " .. name)
        end
        sprite_index = sprite_index + 1
    end

    local output = host:document("multiatlas")
    output:write("{\n")
    indent(output, 1)
    json_key(output, "textures")
    output:write("[\n")
    local page_index = 1
    while page_index <= atlas:page_count() do
        local page = atlas:page(page_index)
        indent(output, 2)
        output:write("{\n")
        indent(output, 3)
        json_string(output, "image", page:image())
        output:write(",\n")
        indent(output, 3)
        json_string(output, "format", "RGBA8888")
        output:write(",\n")
        indent(output, 3)
        emit_size(output, "size", page:width(), page:height())
        output:write(",\n")
        indent(output, 3)
        json_i64(output, "scale", 1)
        output:write(",\n")
        indent(output, 3)
        json_key(output, "frames")
        output:write("[\n")

        local page_sprite_count = 0
        sprite_index = 1
        while sprite_index <= atlas:sprite_count() do
            if atlas:sprite(sprite_index):page() == page_index then
                page_sprite_count = page_sprite_count + 1
            end
            sprite_index = sprite_index + 1
        end
        local emitted = 0
        sprite_index = 1
        while sprite_index <= atlas:sprite_count() do
            local sprite = atlas:sprite(sprite_index)
            if sprite:page() == page_index then
                emitted = emitted + 1
                emit_frame(output, sprite, emitted == page_sprite_count)
            end
            sprite_index = sprite_index + 1
        end
        indent(output, 3)
        output:write("]\n")
        indent(output, 2)
        output:write("}")
        if page_index < atlas:page_count() then
            output:write(",")
        end
        output:write("\n")
        page_index = page_index + 1
    end
    indent(output, 1)
    output:write("]\n")
    output:write("}\n")
    output:finish()
end
