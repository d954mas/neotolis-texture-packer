if(NOT DEFINED TP_FORMAT_STAGE_DEST OR
   NOT DEFINED TP_FORMAT_SOURCE_ROOT OR
   NOT DEFINED TP_FORMAT_MANIFEST)
    message(FATAL_ERROR "format staging requires destination, source root, and manifest")
endif()

include("${TP_FORMAT_MANIFEST}")
if(NOT TP_FORMAT_PACKAGE_MANIFEST_DEFINED)
    message(FATAL_ERROR "format package manifest did not define TP_FORMAT_PACKAGE_MANIFEST")
endif()

file(REMOVE_RECURSE "${TP_FORMAT_STAGE_DEST}")
file(MAKE_DIRECTORY "${TP_FORMAT_STAGE_DEST}")

foreach(_package IN LISTS TP_FORMAT_PACKAGE_MANIFEST)
    string(LENGTH "${_package}" _package_length)
    if(_package_length GREATER 255 OR
       NOT _package MATCHES "^[a-z0-9][a-z0-9-]*$")
        message(FATAL_ERROR "invalid format package manifest entry '${_package}'")
    endif()
    set(_source "${TP_FORMAT_SOURCE_ROOT}/${_package}")
    foreach(_file IN ITEMS format.json export.lua)
        if(NOT EXISTS "${_source}/${_file}" OR
           IS_DIRECTORY "${_source}/${_file}")
            message(FATAL_ERROR
                "format package '${_package}' is missing regular file '${_file}'")
        endif()
    endforeach()
    file(MAKE_DIRECTORY "${TP_FORMAT_STAGE_DEST}/${_package}")
    file(COPY_FILE "${_source}/format.json"
         "${TP_FORMAT_STAGE_DEST}/${_package}/format.json" ONLY_IF_DIFFERENT)
    file(COPY_FILE "${_source}/export.lua"
         "${TP_FORMAT_STAGE_DEST}/${_package}/export.lua" ONLY_IF_DIFFERENT)
endforeach()
