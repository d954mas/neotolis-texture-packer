# E2E proof for the two aggregation-only exit codes. A directory placed at the
# exact metadata-file path is a portable, deterministic exporter write failure.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/project")
configure_file("${TD}/clean.ntpacker_project"
               "${WORK}/project/project.ntpacker_project" COPYONLY)
file(COPY "${TD}/sprites" DESTINATION "${WORK}/project")
set(_project "${WORK}/project/project.ntpacker_project")

execute_process(
    COMMAND "${EXE}" target add "${_project}" clean json-neotolis out/bad
            --quiet
    RESULT_VARIABLE _add_exit OUTPUT_VARIABLE _add_out ERROR_VARIABLE _add_err)
if(NOT _add_exit EQUAL 0)
    message(FATAL_ERROR
        "could not prepare second target (${_add_exit})\n${_add_out}\n${_add_err}")
endif()

function(run_pack_case NAME EXPECTED_EXIT EXPECTED_OK EXPECTED_FAILED ROOT)
    execute_process(
        COMMAND "${EXE}" pack "${_project}" --json --quiet --out-dir "${ROOT}"
        RESULT_VARIABLE _exit OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT "${_exit}" STREQUAL "${EXPECTED_EXIT}")
        message(FATAL_ERROR
            "${NAME}: exit ${_exit} != ${EXPECTED_EXIT}\n${_out}\n${_err}")
    endif()
    set(_json "${WORK}/${NAME}.json")
    file(WRITE "${_json}" "${_out}")
    execute_process(COMMAND "${CHECKER}" "${_json}" pack
        "targets_ok=${EXPECTED_OK}" "targets_failed=${EXPECTED_FAILED}"
        RESULT_VARIABLE _check OUTPUT_VARIABLE _check_out ERROR_VARIABLE _check_err)
    if(NOT _check EQUAL 0)
        message(FATAL_ERROR
            "${NAME}: invalid JSON contract\n${_check_out}${_check_err}\n${_out}")
    endif()
endfunction()

set(_partial_root "${WORK}/partial")
file(MAKE_DIRECTORY "${_partial_root}/out/bad.json")
run_pack_case(partial 6 1 1 "${_partial_root}")

set(_failed_root "${WORK}/failed")
file(MAKE_DIRECTORY
    "${_failed_root}/out/clean.json.json"
    "${_failed_root}/out/bad.json")
run_pack_case(failed 5 0 2 "${_failed_root}")
