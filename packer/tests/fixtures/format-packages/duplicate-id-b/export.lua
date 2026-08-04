return function(atlas, host)
    local output = host:document("metadata")
    output:write_json_string(atlas:name())
    output:finish()
end
