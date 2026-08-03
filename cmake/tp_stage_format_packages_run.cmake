cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED TP_FORMAT_STAGE_DEST OR
   NOT DEFINED TP_FORMAT_SOURCE_ROOT OR
   NOT DEFINED TP_FORMAT_MANIFEST)
    message(FATAL_ERROR "format staging requires destination, source root, and manifest")
endif()

if(NOT IS_ABSOLUTE "${TP_FORMAT_STAGE_DEST}")
    message(FATAL_ERROR "format staging destination must be absolute")
endif()
get_filename_component(_stage_destination_name "${TP_FORMAT_STAGE_DEST}" NAME)
if(NOT _stage_destination_name STREQUAL "formats")
    message(FATAL_ERROR "format staging destination must name a formats directory")
endif()

set(_stage_tree "${TP_FORMAT_STAGE_DEST}.tp-stage")
set(_backup_tree "${TP_FORMAT_STAGE_DEST}.tp-backup")

# Repair an interrupted prior replacement before validating the next requested
# input. A bad new manifest must not leave the last-good tree hidden in backup.
if((EXISTS "${_backup_tree}" OR IS_SYMLINK "${_backup_tree}") AND
   NOT (EXISTS "${TP_FORMAT_STAGE_DEST}" OR
        IS_SYMLINK "${TP_FORMAT_STAGE_DEST}"))
    file(RENAME "${_backup_tree}" "${TP_FORMAT_STAGE_DEST}"
         RESULT _recovery_result)
    if(NOT _recovery_result STREQUAL "0")
        message(FATAL_ERROR
            "could not restore the previous staged format root: ${_recovery_result}")
    endif()
endif()
if((EXISTS "${_backup_tree}" OR IS_SYMLINK "${_backup_tree}") AND
   (EXISTS "${TP_FORMAT_STAGE_DEST}" OR
    IS_SYMLINK "${TP_FORMAT_STAGE_DEST}"))
    # Destination exists, so the prior install committed and its backup is
    # stale. Finish that transaction before considering the next input.
    file(REMOVE_RECURSE "${_backup_tree}")
endif()
# A private stage tree is never authoritative across process boundaries.
file(REMOVE_RECURSE "${_stage_tree}")

include("${TP_FORMAT_MANIFEST}")
if(NOT TP_FORMAT_PACKAGE_MANIFEST_DEFINED)
    message(FATAL_ERROR "format package manifest did not define TP_FORMAT_PACKAGE_MANIFEST")
endif()
if(NOT IS_DIRECTORY "${TP_FORMAT_SOURCE_ROOT}")
    message(FATAL_ERROR "format package source root is not a directory")
endif()
file(REAL_PATH "${TP_FORMAT_SOURCE_ROOT}" _source_root_real)

function(_tp_format_require_regular_file path package file_name)
    if(NOT EXISTS "${path}" OR
       IS_DIRECTORY "${path}" OR
       IS_SYMLINK "${path}")
        message(FATAL_ERROR
            "format package '${package}' is missing regular file '${file_name}'")
    endif()
    if(UNIX)
        # CMake 3.25 has no regular-file predicate. POSIX `test -f` rejects
        # FIFOs, sockets, and device nodes without opening (and potentially
        # blocking on) them. The path is passed as $1, never interpolated into
        # shell source.
        execute_process(
            COMMAND /bin/sh -c [[test -f "$1"]] sh "${path}"
            RESULT_VARIABLE _regular_result)
        if(NOT _regular_result EQUAL 0)
            message(FATAL_ERROR
                "format package '${package}' is missing regular file '${file_name}'")
        endif()
    endif()
endfunction()

# Validate the whole manifest before touching the last-good destination. Package
# spelling has no second semantic policy here: the runtime scanner owns the API-v1
# UTF-8/length/control rules. Staging rejects only components that could address a
# parent, child, drive, or alternate stream instead of one direct child directory.
foreach(_package IN LISTS TP_FORMAT_PACKAGE_MANIFEST)
    string(FIND "${_package}" "/" _package_slash)
    string(FIND "${_package}" "\\" _package_backslash)
    string(FIND "${_package}" ":" _package_colon)
    if(_package STREQUAL "" OR
       _package STREQUAL "." OR
       _package STREQUAL ".." OR
       NOT _package_slash EQUAL -1 OR
       NOT _package_backslash EQUAL -1 OR
       NOT _package_colon EQUAL -1)
        message(FATAL_ERROR
            "path-unsafe format package manifest entry '${_package}'")
    endif()

    string(SHA256 _package_key "${_package}")
    set(_package_seen_var "_tp_format_package_seen_${_package_key}")
    if(DEFINED ${_package_seen_var})
        message(FATAL_ERROR
            "duplicate format package manifest entry '${_package}'")
    endif()
    set(${_package_seen_var} TRUE)

    set(_source "${TP_FORMAT_SOURCE_ROOT}/${_package}")
    if(NOT IS_DIRECTORY "${_source}" OR IS_SYMLINK "${_source}")
        message(FATAL_ERROR
            "format package '${_package}' is not a real source directory")
    endif()
    file(REAL_PATH "${_source}" _source_real)
    get_filename_component(_source_parent_real "${_source_real}" DIRECTORY)
    if(NOT "${_source_parent_real}" STREQUAL "${_source_root_real}")
        message(FATAL_ERROR
            "format package '${_package}' escapes the source root")
    endif()
    foreach(_file IN ITEMS format.json export.lua)
        _tp_format_require_regular_file(
            "${_source}/${_file}" "${_package}" "${_file}")
    endforeach()
endforeach()

file(MAKE_DIRECTORY "${_stage_tree}")

foreach(_package IN LISTS TP_FORMAT_PACKAGE_MANIFEST)
    set(_source "${TP_FORMAT_SOURCE_ROOT}/${_package}")
    set(_destination "${_stage_tree}/${_package}")
    file(MAKE_DIRECTORY "${_destination}")
    foreach(_file IN ITEMS format.json export.lua)
        file(COPY_FILE "${_source}/${_file}" "${_destination}/${_file}"
             RESULT _copy_result ONLY_IF_DIFFERENT)
        if(NOT _copy_result STREQUAL "0")
            file(REMOVE_RECURSE "${_stage_tree}")
            message(FATAL_ERROR
                "could not stage '${_package}/${_file}': ${_copy_result}")
        endif()
    endforeach()
endforeach()

if(EXISTS "${_backup_tree}" OR IS_SYMLINK "${_backup_tree}")
    file(REMOVE_RECURSE "${_backup_tree}")
endif()

set(_had_previous_tree FALSE)
if(EXISTS "${TP_FORMAT_STAGE_DEST}" OR IS_SYMLINK "${TP_FORMAT_STAGE_DEST}")
    file(RENAME "${TP_FORMAT_STAGE_DEST}" "${_backup_tree}"
         RESULT _backup_result)
    if(NOT _backup_result STREQUAL "0")
        file(REMOVE_RECURSE "${_stage_tree}")
        message(FATAL_ERROR
            "could not preserve the previous staged format root: ${_backup_result}")
    endif()
    set(_had_previous_tree TRUE)
endif()

file(RENAME "${_stage_tree}" "${TP_FORMAT_STAGE_DEST}"
     RESULT _install_result)
if(NOT _install_result STREQUAL "0")
    set(_restore_result "not attempted")
    if(_had_previous_tree)
        file(RENAME "${_backup_tree}" "${TP_FORMAT_STAGE_DEST}"
             RESULT _restore_result)
    endif()
    file(REMOVE_RECURSE "${_stage_tree}")
    if(_had_previous_tree AND NOT _restore_result STREQUAL "0")
        message(FATAL_ERROR
            "could not install the staged format root (${_install_result}) or restore the previous root (${_restore_result}); previous files remain at '${_backup_tree}'")
    endif()
    message(FATAL_ERROR
        "could not install the staged format root: ${_install_result}")
endif()

if(_had_previous_tree)
    file(REMOVE_RECURSE "${_backup_tree}")
endif()
