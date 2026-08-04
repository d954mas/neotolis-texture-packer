function(_tp_lua_verify_source_inventory)
    set(vendor_root "${CMAKE_CURRENT_LIST_DIR}")
    set(manifest "${vendor_root}/SOURCE.sha256")

    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "Lua vendor manifest is missing: ${manifest}")
    endif()

    file(STRINGS "${manifest}" manifest_lines)
    if(NOT manifest_lines)
        message(FATAL_ERROR "Lua vendor manifest is empty: ${manifest}")
    endif()

    set(expected_files)
    foreach(line IN LISTS manifest_lines)
        if(NOT line MATCHES "^([0-9a-f]+)  (src/[A-Za-z0-9_.-]+)$")
            message(FATAL_ERROR "Malformed Lua vendor manifest line: ${line}")
        endif()

        set(expected_hash "${CMAKE_MATCH_1}")
        set(relative_path "${CMAKE_MATCH_2}")
        string(LENGTH "${expected_hash}" hash_length)
        if(NOT hash_length EQUAL 64)
            message(FATAL_ERROR
                "Malformed SHA-256 for Lua vendor file ${relative_path}")
        endif()

        list(FIND expected_files "${relative_path}" duplicate_index)
        if(NOT duplicate_index EQUAL -1)
            message(FATAL_ERROR
                "Duplicate Lua vendor manifest path: ${relative_path}")
        endif()

        set(absolute_path "${vendor_root}/${relative_path}")
        if(NOT EXISTS "${absolute_path}" OR IS_DIRECTORY "${absolute_path}")
            message(FATAL_ERROR "Lua vendor file is missing: ${relative_path}")
        endif()

        file(SHA256 "${absolute_path}" actual_hash)
        if(NOT actual_hash STREQUAL expected_hash)
            message(FATAL_ERROR
                "Lua vendor hash mismatch for ${relative_path}: "
                "expected ${expected_hash}, got ${actual_hash}")
        endif()

        list(APPEND expected_files "${relative_path}")
    endforeach()

    file(GLOB_RECURSE actual_files
        LIST_DIRECTORIES false
        RELATIVE "${vendor_root}"
        "${vendor_root}/src/*")
    list(SORT expected_files)
    list(SORT actual_files)
    if(NOT actual_files STREQUAL expected_files)
        message(FATAL_ERROR
            "Lua vendor src inventory differs from SOURCE.sha256. "
            "Expected '${expected_files}', got '${actual_files}'")
    endif()
endfunction()

_tp_lua_verify_source_inventory()
