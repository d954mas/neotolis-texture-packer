if(NOT DEFINED TP_STAGE_SCRIPT OR NOT DEFINED TP_SOURCE_ROOT OR
   NOT DEFINED TP_MANIFEST OR NOT DEFINED TP_TEST_ROOT)
    message(FATAL_ERROR
        "TP_STAGE_SCRIPT, TP_SOURCE_ROOT, TP_MANIFEST, and TP_TEST_ROOT are required")
endif()

file(REMOVE_RECURSE "${TP_TEST_ROOT}")
file(MAKE_DIRECTORY "${TP_TEST_ROOT}")
set(_destination "${TP_TEST_ROOT}/formats")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DTP_FORMAT_STAGE_DEST=${_destination}"
        "-DTP_FORMAT_SOURCE_ROOT=${TP_SOURCE_ROOT}"
        "-DTP_FORMAT_MANIFEST=${TP_MANIFEST}"
        -P "${TP_STAGE_SCRIPT}"
    RESULT_VARIABLE _status
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _status EQUAL 0)
    message(FATAL_ERROR
        "bundled format staging failed\n${_stdout}\n${_stderr}")
endif()

file(GLOB_RECURSE _staged RELATIVE "${_destination}" "${_destination}/*")
list(SORT _staged)
set(_expected
    defold-tpinfo-2/export.lua
    defold-tpinfo-2/format.json
    phaser-3-multiatlas/export.lua
    phaser-3-multiatlas/format.json)
if(NOT _staged STREQUAL _expected)
    message(FATAL_ERROR
        "portable format tree differs: '${_staged}' != '${_expected}'")
endif()

foreach(_relative IN LISTS _expected)
    file(SHA256 "${TP_SOURCE_ROOT}/${_relative}" _source_hash)
    file(SHA256 "${_destination}/${_relative}" _staged_hash)
    if(NOT _source_hash STREQUAL _staged_hash)
        message(FATAL_ERROR "staged bytes differ for ${_relative}")
    endif()
endforeach()
