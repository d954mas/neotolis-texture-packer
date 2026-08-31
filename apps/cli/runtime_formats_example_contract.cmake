if(NOT DEFINED EXE OR NOT DEFINED EXAMPLE OR NOT DEFINED WORK)
    message(FATAL_ERROR "EXE, EXAMPLE, and WORK are required")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY
    "${EXAMPLE}/runtime-formats.ntpacker_project"
    "${EXAMPLE}/game.project"
    DESTINATION "${WORK}")
file(MAKE_DIRECTORY "${WORK}/sprites")
file(COPY
    "${EXAMPLE}/sprites/coin.png"
    "${EXAMPLE}/sprites/hero.png"
    DESTINATION "${WORK}/sprites")
execute_process(
    COMMAND "${EXE}" pack
        "${WORK}/runtime-formats.ntpacker_project"
        --json
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR
        "runtime format example export failed\n${_stdout}\n${_stderr}")
endif()

set(_outputs
    json/atlas.json
    json/atlas-0.png
    defold/atlas.tpinfo
    defold/atlas.tpatlas
    defold/atlas-0.png
    phaser/atlas.json
    phaser/atlas-0.png)
foreach(_relative IN LISTS _outputs)
    set(_golden "${EXAMPLE}/golden/${_relative}")
    set(_actual "${WORK}/golden/${_relative}")
    if(NOT EXISTS "${_actual}" OR NOT EXISTS "${_golden}")
        message(FATAL_ERROR "runtime example output missing: ${_relative}")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${_golden}" "${_actual}"
        RESULT_VARIABLE _compare)
    if(NOT _compare EQUAL 0)
        message(FATAL_ERROR "runtime example golden changed: ${_relative}")
    endif()
endforeach()

file(SHA256 "${WORK}/golden/json/atlas-0.png" _json_png)
file(SHA256 "${WORK}/golden/defold/atlas-0.png" _defold_png)
file(SHA256 "${WORK}/golden/phaser/atlas-0.png" _phaser_png)
if(NOT _json_png STREQUAL _defold_png OR
   NOT _json_png STREQUAL _phaser_png)
    message(FATAL_ERROR
        "capability-equivalent runtime example targets changed core PNG bytes")
endif()

string(REGEX MATCHALL "\"pack_runs\"[ \t\r\n]*:[ \t\r\n]*1" _one_pack "${_stdout}")
list(LENGTH _one_pack _one_pack_count)
if(NOT _one_pack_count EQUAL 1)
    message(FATAL_ERROR "runtime example no longer shares one pack run\n${_stdout}")
endif()
