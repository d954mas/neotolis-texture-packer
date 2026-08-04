return function(atlas, host)
    local output = host:document("metadata")
    output:write("atlas=")
    output:write_json_string(atlas:name())
    output:write("\n")
    output:finish()
end
