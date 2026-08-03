return function(atlas, host)
    local metadata = host:document("metadata")
    local index = host:document("index")

    metadata:write("name=")
    metadata:write_json_string(atlas:name())
    metadata:write("\npages=")
    metadata:write_i64(atlas:page_count())
    metadata:write("\nresource=")
    metadata:write_json_string(host:fact("metadata_resource"))
    metadata:write("\n")

    local sprite_index = 1
    while sprite_index <= atlas:sprite_count() do
        local sprite = atlas:sprite(sprite_index)
        metadata:write_json_string(sprite:name())
        metadata:write("\n")
        sprite_index = sprite_index + 1
    end

    metadata:finish()
    index:write_bool(true)
    index:write("\n")
    index:finish()
end
