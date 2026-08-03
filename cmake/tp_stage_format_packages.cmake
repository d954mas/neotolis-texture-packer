# Defines tp_stage_format_packages(target). The generated command always removes
# and recreates the executable-relative formats/ directory, then copies only the
# two fixed files of packages named by formats/manifest.cmake. No glob or overlay
# can retain a deleted/stale package.

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
    add_custom_command(TARGET "${target}" POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DTP_FORMAT_STAGE_DEST=$<TARGET_FILE_DIR:${target}>/formats"
            "-DTP_FORMAT_SOURCE_ROOT=${CMAKE_SOURCE_DIR}/formats"
            "-DTP_FORMAT_MANIFEST=${CMAKE_SOURCE_DIR}/formats/manifest.cmake"
            -P "${CMAKE_SOURCE_DIR}/cmake/tp_stage_format_packages_run.cmake"
        VERBATIM
        COMMENT "Recreating executable-relative format package root for ${target}")
    set_property(TARGET "${target}" APPEND PROPERTY LINK_DEPENDS
        ${_tp_format_stage_dependencies})
endfunction()
