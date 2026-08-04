# Defines tp_stage_format_packages(target). Every build request prepares the
# complete executable-relative formats/ tree from formats/manifest.cmake, then
# replaces the previous tree. No glob or overlay can retain a deleted package.

function(tp_stage_format_packages target)
    if(NOT TARGET "${target}")
        message(FATAL_ERROR "tp_stage_format_packages: unknown target '${target}'")
    endif()
    include("${CMAKE_SOURCE_DIR}/formats/manifest.cmake")
    if(NOT TP_FORMAT_PACKAGE_MANIFEST_DEFINED)
        message(FATAL_ERROR
            "format package manifest did not define TP_FORMAT_PACKAGE_MANIFEST")
    endif()
    set(_tp_format_stage_dependencies
        "${CMAKE_SOURCE_DIR}/formats/manifest.cmake")
    foreach(_package IN LISTS TP_FORMAT_PACKAGE_MANIFEST)
        list(APPEND _tp_format_stage_dependencies
            "${CMAKE_SOURCE_DIR}/formats/${_package}/format.json"
            "${CMAKE_SOURCE_DIR}/formats/${_package}/export.lua")
    endforeach()
    set(_tp_format_stage_target "${target}_format_packages")
    add_custom_target("${_tp_format_stage_target}"
        COMMAND "${CMAKE_COMMAND}"
            "-DTP_FORMAT_STAGE_DEST=$<TARGET_FILE_DIR:${target}>/formats"
            "-DTP_FORMAT_SOURCE_ROOT=${CMAKE_SOURCE_DIR}/formats"
            "-DTP_FORMAT_MANIFEST=${CMAKE_SOURCE_DIR}/formats/manifest.cmake"
            -P "${CMAKE_SOURCE_DIR}/cmake/tp_stage_format_packages_run.cmake"
        DEPENDS ${_tp_format_stage_dependencies}
        VERBATIM
        COMMENT "Staging executable-relative format package root for ${target}")
    add_dependencies("${target}" "${_tp_format_stage_target}")
endfunction()
