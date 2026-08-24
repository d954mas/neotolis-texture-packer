if(NOT DEFINED EXE OR NOT DEFINED CHECKER OR NOT DEFINED PROJECT OR
   NOT DEFINED PACKAGE OR NOT DEFINED WORK OR NOT DEFINED DRY_RUN)
    message(FATAL_ERROR
        "EXE, CHECKER, PROJECT, PACKAGE, WORK, and DRY_RUN are required")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/bin/formats/fixture-minimal")
file(COPY "${EXE}" DESTINATION "${WORK}/bin"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                     GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(COPY "${PACKAGE}/format.json" "${PACKAGE}/export.lua"
    DESTINATION "${WORK}/bin/formats/fixture-minimal")
get_filename_component(_exe_name "${EXE}" NAME)
set(_staged_exe "${WORK}/bin/${_exe_name}")
set(_out_dir "${WORK}/output")
set(_args pack "${PROJECT}" --json --out-dir "${_out_dir}")
if(DRY_RUN)
    list(APPEND _args --dry-run)
endif()

execute_process(
    COMMAND "${_staged_exe}" ${_args}
    RESULT_VARIABLE _code
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _code EQUAL 0)
    message(FATAL_ERROR
        "Lua Export exited ${_code}\n--stdout--\n${_stdout}\n--stderr--\n${_stderr}")
endif()

set(_report "${WORK}/report.json")
file(WRITE "${_report}" "${_stdout}")
execute_process(
    COMMAND "${CHECKER}" "${_report}" pack
            "dry_run=${DRY_RUN}" targets_ok=1 targets_failed=0
    RESULT_VARIABLE _check_code
    OUTPUT_VARIABLE _check_stdout
    ERROR_VARIABLE _check_stderr)
if(NOT _check_code EQUAL 0)
    message(FATAL_ERROR
        "Lua Export report check failed\n${_check_stdout}${_check_stderr}\n${_stdout}")
endif()

set(_metadata "${_out_dir}/out/lua-runtime.txt")
set(_page "${_out_dir}/out/lua-runtime-0.png")
if(DRY_RUN)
    if(EXISTS "${_out_dir}")
        message(FATAL_ERROR "Lua dry-run created output directory: ${_out_dir}")
    endif()
elseif(NOT EXISTS "${_metadata}" OR NOT EXISTS "${_page}")
    message(FATAL_ERROR
        "Lua wet Export did not publish metadata and page\n--stdout--\n${_stdout}")
endif()
