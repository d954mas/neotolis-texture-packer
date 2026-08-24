local function indent(output, depth)
    local index = 1
    while index <= depth do
        output:write("  ")
        index = index + 1
    end
end

local function pb_string(output, value)
    output:write("\"")
    local index = 1
    while index <= string.len(value) do
        local byte = string.byte(value, index)
        if byte == 34 then
            output:write("\\\"")
        elseif byte == 92 then
            output:write("\\\\")
        elseif byte < 32 then
            output:write("\\")
            output:write(string.char(48 + math.floor(byte / 64)))
            output:write(string.char(48 + math.floor((byte % 64) / 8)))
            output:write(string.char(48 + (byte % 8)))
        else
            output:write(string.sub(value, index, index))
        end
        index = index + 1
    end
    output:write("\"")
end

local function key(output, depth, name)
    indent(output, depth)
    output:write(name)
    output:write(": ")
end

local function kv_i64(output, depth, name, value)
    key(output, depth, name)
    output:write_i64(value)
    output:write("\n")
end

local function kv_bool(output, depth, name, value)
    key(output, depth, name)
    output:write_bool(value)
    output:write("\n")
end

local function kv_string(output, depth, name, value)
    key(output, depth, name)
    pb_string(output, value)
    output:write("\n")
end

local function point_i(output, depth, name, x, y)
    indent(output, depth)
    output:write(name)
    output:write(" {\n")
    kv_i64(output, depth + 1, "x", x)
    kv_i64(output, depth + 1, "y", y)
    indent(output, depth)
    output:write("}\n")
end

local function point_f(output, depth, name, x, y)
    indent(output, depth)
    output:write(name)
    output:write(" {\n")
    key(output, depth + 1, "x")
    output:write_f32(x)
    output:write("\n")
    key(output, depth + 1, "y")
    output:write_f32(y)
    output:write("\n")
    indent(output, depth)
    output:write("}\n")
end

local function rect(output, depth, name, x, y, width, height)
    indent(output, depth)
    output:write(name)
    output:write(" {\n")
    kv_i64(output, depth + 1, "x", x)
    kv_i64(output, depth + 1, "y", y)
    kv_i64(output, depth + 1, "width", width)
    kv_i64(output, depth + 1, "height", height)
    indent(output, depth)
    output:write("}\n")
end

local function size(output, depth, name, width, height)
    indent(output, depth)
    output:write(name)
    output:write(" {\n")
    kv_i64(output, depth + 1, "width", width)
    kv_i64(output, depth + 1, "height", height)
    indent(output, depth)
    output:write("}\n")
end

local function is_rect_quad(sprite, width, height)
    if sprite:vertex_count() ~= 4 then
        return false
    end
    local seen_tl = false
    local seen_tr = false
    local seen_bl = false
    local seen_br = false
    local index = 1
    while index <= 4 do
        local x, y = sprite:vertex(index)
        if x == 0 and y == 0 and not seen_tl then
            seen_tl = true
        elseif x == width and y == 0 and not seen_tr then
            seen_tr = true
        elseif x == 0 and y == height and not seen_bl then
            seen_bl = true
        elseif x == width and y == height and not seen_br then
            seen_br = true
        else
            return false
        end
        index = index + 1
    end
    return true
end

local function emit_sprite(output, sprite)
    local frame_x, frame_y, frame_width, frame_height = sprite:frame()
    local trim_x, trim_y, trim_width, trim_height = sprite:trim_rect()
    local source_width, source_height = sprite:source_size()
    local footprint_width, footprint_height = sprite:footprint()
    local rotated = sprite:transform() == "rotate_90_cw"

    output:write("  sprites {\n")
    kv_string(output, 2, "name", sprite:name())
    kv_bool(output, 2, "trimmed", sprite:trimmed())
    kv_bool(output, 2, "rotated", rotated)
    kv_bool(output, 2, "is_solid", sprite:is_solid())
    point_i(output, 2, "corner_offset", trim_x, trim_y)
    rect(output, 2, "source_rect", trim_x, trim_y,
         trim_width, trim_height)

    local pivot_x, pivot_y = sprite:pivot()
    point_f(output, 2, "pivot", pivot_x * source_width,
            pivot_y * source_height)
    rect(output, 2, "frame_rect", frame_x, frame_y,
         footprint_width, footprint_height)
    size(output, 2, "untrimmed_size", source_width, source_height)

    local polygon = sprite:vertex_count() > 0 and
        not is_rect_quad(sprite, frame_width, frame_height)
    if polygon then
        indent(output, 2)
        output:write("indices: [")
        local index = 1
        while index <= sprite:index_count() do
            if index > 1 then
                output:write(", ")
            end
            output:write_u64(sprite:index(index))
            index = index + 1
        end
        output:write("]\n")
        index = 1
        while index <= sprite:vertex_count() do
            local x, y = sprite:vertex(index)
            point_i(output, 2, "vertices", x + trim_x, y + trim_y)
            index = index + 1
        end
    else
        indent(output, 2)
        output:write("indices: [1, 2, 3, 0, 1, 3]\n")
        point_i(output, 2, "vertices", trim_x + trim_width, trim_y)
        point_i(output, 2, "vertices", trim_x, trim_y)
        point_i(output, 2, "vertices", trim_x, trim_y + trim_height)
        point_i(output, 2, "vertices", trim_x + trim_width,
                trim_y + trim_height)
    end
    output:write("  }\n")
end

local playback_tokens = {
    "PLAYBACK_ONCE_FORWARD",
    "PLAYBACK_LOOP_FORWARD",
    "PLAYBACK_ONCE_BACKWARD",
    "PLAYBACK_LOOP_BACKWARD",
    "PLAYBACK_ONCE_PINGPONG",
    "PLAYBACK_LOOP_PINGPONG",
    "PLAYBACK_NONE",
}

return function(atlas, host)
    local tpinfo = host:document("tpinfo")
    tpinfo:write("# Exported by neotolis-texture-packer\n")
    tpinfo:write("# Format: Defold extension-texturepacker .tpinfo (protobuf text)\n\n")
    kv_string(tpinfo, 0, "version", "2.0")
    kv_string(tpinfo, 0, "description",
              "Exported using neotolis-texture-packer")

    local page_index = 1
    while page_index <= atlas:page_count() do
        local page = atlas:page(page_index)
        tpinfo:write("pages {\n")
        kv_string(tpinfo, 1, "name", page:image())
        size(tpinfo, 1, "size", page:width(), page:height())
        local sprite_index = 1
        while sprite_index <= atlas:sprite_count() do
            local sprite = atlas:sprite(sprite_index)
            if sprite:page() == page_index then
                emit_sprite(tpinfo, sprite)
            end
            sprite_index = sprite_index + 1
        end
        tpinfo:write("}\n")
        page_index = page_index + 1
    end
    tpinfo:finish()

    local tpatlas = host:document("tpatlas")
    kv_string(tpatlas, 0, "file", host:fact("tpinfo_resource"))
    kv_string(tpatlas, 0, "rename_patterns", "")
    local animation_index = 1
    while animation_index <= atlas:animation_count() do
        local animation = atlas:animation(animation_index)
        tpatlas:write("animations {\n")
        kv_string(tpatlas, 1, "id", animation:id())
        local frame_index = 1
        while frame_index <= animation:frame_count() do
            kv_string(tpatlas, 1, "images", animation:frame(frame_index))
            frame_index = frame_index + 1
        end
        key(tpatlas, 1, "playback")
        tpatlas:write(playback_tokens[animation:playback() + 1])
        tpatlas:write("\n")
        kv_i64(tpatlas, 1, "fps", math.floor(animation:fps() + 0.5))
        kv_i64(tpatlas, 1, "flip_horizontal",
               animation:flip_h() and 1 or 0)
        kv_i64(tpatlas, 1, "flip_vertical",
               animation:flip_v() and 1 or 0)
        tpatlas:write("}\n")
        animation_index = animation_index + 1
    end
    kv_bool(tpatlas, 0, "is_paged_atlas", false)
    tpatlas:finish()
end
