# Exact CLI JSON contract. This driver deliberately preserves raw formatting and
# key order. It masks only elapsed time and exact absolute roots supplied by the
# build, after separately asserting their absolute form and path suffixes.

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(run_capture OUT_VAR EXPECTED_EXIT)
    execute_process(COMMAND "${EXE}" ${ARGN}
        RESULT_VARIABLE _exit OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT "${_exit}" STREQUAL "${EXPECTED_EXIT}")
        message(FATAL_ERROR
            "exit ${_exit} != ${EXPECTED_EXIT}: ${ARGN}\n"
            "--stdout--\n${_stdout}\n--stderr--\n${_stderr}")
    endif()
    set(${OUT_VAR} "${_stdout}" PARENT_SCOPE)
endfunction()

function(assert_exact ACTUAL EXPECTED_FILE)
    file(READ "${EXPECTED_FILE}" _expected)
    if(WIN32)
        set(_expected_native_sep "\\\\")
    else()
        set(_expected_native_sep "/")
    endif()
    string(REPLACE "<NATIVE_SEP>" "${_expected_native_sep}"
           _expected "${_expected}")
    string(REGEX REPLACE "(\r\n|\n)$" "" _actual_trimmed "${ACTUAL}")
    string(REGEX REPLACE "(\r\n|\n)$" "" _expected_trimmed "${_expected}")
    if(NOT "${_actual_trimmed}" STREQUAL "${_expected_trimmed}")
        message(FATAL_ERROR
            "payload differs from ${EXPECTED_FILE}\n"
            "--actual--\n${ACTUAL}\n--expected--\n${_expected}")
    endif()
endfunction()

function(check_json NAME PAYLOAD)
    set(_path "${WORK}/${NAME}.json")
    file(WRITE "${_path}" "${PAYLOAD}")
    execute_process(COMMAND "${CHECKER}" "${_path}" ${ARGN}
        RESULT_VARIABLE _exit OUTPUT_VARIABLE _stdout ERROR_VARIABLE _stderr)
    if(NOT _exit EQUAL 0)
        message(FATAL_ERROR
            "${NAME} JSON check failed\n${_stdout}${_stderr}\n${PAYLOAD}")
    endif()
endfunction()

function(assert_occurrences HAYSTACK NEEDLE EXPECTED LABEL)
    set(_rest "${HAYSTACK}")
    set(_count 0)
    string(LENGTH "${NEEDLE}" _needle_len)
    while(1)
        string(FIND "${_rest}" "${NEEDLE}" _at)
        if(_at EQUAL -1)
            break()
        endif()
        math(EXPR _after "${_at} + ${_needle_len}")
        string(SUBSTRING "${_rest}" ${_after} -1 _rest)
        math(EXPR _count "${_count} + 1")
    endwhile()
    if(NOT _count EQUAL EXPECTED)
        message(FATAL_ERROR
            "${LABEL}: root occurrence count ${_count} != ${EXPECTED}")
    endif()
endfunction()

function(replace_first HAYSTACK NEEDLE REPLACEMENT OUT_VAR)
    string(FIND "${HAYSTACK}" "${NEEDLE}" _at)
    if(_at EQUAL -1)
        message(FATAL_ERROR "replace_first: prefix not found: ${NEEDLE}")
    endif()
    string(LENGTH "${NEEDLE}" _needle_len)
    math(EXPR _after "${_at} + ${_needle_len}")
    string(SUBSTRING "${HAYSTACK}" 0 ${_at} _before)
    string(SUBSTRING "${HAYSTACK}" ${_after} -1 _after_text)
    set(${OUT_VAR} "${_before}${REPLACEMENT}${_after_text}" PARENT_SCOPE)
endfunction()

# Manifest: no dynamic fields.
run_capture(_version 0 version --json)
check_json(version "${_version}" manifest)
assert_exact("${_version}" "${EXPECTED}/version-manifest.expected.json")

# Inspect: the file-system boundary intentionally exposes a native project path,
# while project_dir/source/sprite absolute paths use canonical forward slashes.
if(NOT IS_ABSOLUTE "${TD}")
    message(FATAL_ERROR "TD must be absolute: ${TD}")
endif()
file(TO_NATIVE_PATH "${TD}" _td_native)
file(TO_NATIVE_PATH "${TD}/clean.ntpacker_project" _project_native)
string(REPLACE "\\" "\\\\" _td_native_json "${_td_native}")
string(REPLACE "\\" "\\\\" _project_native_json "${_project_native}")
run_capture(_inspect 0 inspect "${_project_native}" --json)
check_json(inspect "${_inspect}" inspect sprites=2 schema=4)
string(FIND "${_inspect}" "\"path\": \"${_project_native_json}\""
            _project_path_pos)
if(_project_path_pos EQUAL -1)
    message(FATAL_ERROR "inspect project.path absolute/native suffix changed")
endif()
string(FIND "${_inspect}" "\"project_dir\": \"${TD}\"" _project_dir_pos)
string(FIND "${_inspect}" "\"abs\": \"${TD}/sprites\"" _source_dir_pos)
string(FIND "${_inspect}" "${TD}/sprites/coin.png" _coin_pos)
string(FIND "${_inspect}" "${TD}/sprites/hero.png" _hero_pos)
if(_project_dir_pos EQUAL -1 OR _source_dir_pos EQUAL -1 OR
   _coin_pos EQUAL -1 OR _hero_pos EQUAL -1)
    message(FATAL_ERROR "inspect canonical absolute path suffix changed")
endif()
if(WIN32)
    assert_occurrences("${_inspect}" "${_td_native_json}" 1
                       "inspect native root")
    assert_occurrences("${_inspect}" "${TD}" 4
                       "inspect canonical root")
    string(REPLACE "${_td_native_json}" "<TESTDATA_NATIVE>"
           _inspect_norm "${_inspect}")
else()
    assert_occurrences("${_inspect}" "${TD}" 5 "inspect POSIX root")
    replace_first("${_inspect}" "${TD}" "<TESTDATA_NATIVE>" _inspect_norm)
endif()
string(REPLACE "${TD}" "<TESTDATA>" _inspect_norm "${_inspect_norm}")
assert_exact("${_inspect_norm}" "${EXPECTED}/inspect-clean.expected.json")

# Validate is already independently pinned by P1-04; repeat it here to bind the
# exact schema number into the complete CLI payload matrix.
run_capture(_validate 0 validate "${TD}/problems.ntpacker_project" --json)
check_json(validate "${_validate}" validate error=2 warning=0)
assert_exact("${_validate}" "${EXPECTED}/validate-problems.expected.json")

# Pack: dry-run keeps the test side-effect free. Validate raw JSON and every
# absolute output suffix before replacing the known root and elapsed scalar.
get_filename_component(_pack_root "${WORK}/pack-root" ABSOLUTE)
run_capture(_pack 0 pack "${TD}/clean.ntpacker_project" --dry-run --json
    --out-dir "${_pack_root}" --quiet)
check_json(pack "${_pack}" pack dry_run=1 targets_ok=1 targets_failed=0)
foreach(_suffix IN ITEMS
        "/out/clean.json"
        "/out/clean.json.json"
        "/out/clean.json-0.png")
    string(FIND "${_pack}" "${_pack_root}${_suffix}" _suffix_pos)
    if(_suffix_pos EQUAL -1)
        message(FATAL_ERROR "pack absolute output suffix missing: ${_suffix}")
    endif()
endforeach()

# Pre-publication Save failure is a real error with its own exit code at the
# command boundary. Pin the complete typed JSON object independently of OS I/O.
execute_process(COMMAND "${FILE_IO_EXE}"
    RESULT_VARIABLE _file_io_exit OUTPUT_VARIABLE _file_io
    ERROR_VARIABLE _file_io_err)
if(NOT _file_io_exit EQUAL 8 OR NOT "${_file_io_err}" STREQUAL "")
    message(FATAL_ERROR
        "file I/O payload fixture failed: exit=${_file_io_exit} err=${_file_io_err}")
endif()
assert_exact("${_file_io}" "${EXPECTED}/file-io-error.expected.json")
assert_occurrences("${_pack}" "${_pack_root}" 3 "pack output root")
string(REPLACE "${_pack_root}" "<PACK_ROOT>" _pack_norm "${_pack}")
string(REGEX MATCHALL "\"total\": [-+0-9.eE]+" _timing_fields
       "${_pack_norm}")
list(LENGTH _timing_fields _timing_count)
if(NOT _timing_count EQUAL 1)
    message(FATAL_ERROR
        "pack timings_ms.total field count ${_timing_count} != 1")
endif()
list(GET _timing_fields 0 _timing_field)
string(REPLACE "${_timing_field}" "\"total\": 0" _pack_norm "${_pack_norm}")
assert_exact("${_pack_norm}" "${EXPECTED}/pack-clean-dry.expected.json")

# Mutation success and reject: fixed project IDs make the command deterministic;
# the successful scalar edit changes only the isolated WORK copy.
set(_mutation_dir "${WORK}/mutation")
file(MAKE_DIRECTORY "${_mutation_dir}")
configure_file("${TD}/clean.ntpacker_project"
               "${_mutation_dir}/project.ntpacker_project" COPYONLY)
file(COPY "${TD}/sprites" DESTINATION "${_mutation_dir}")
set(_mutation_project "${_mutation_dir}/project.ntpacker_project")
run_capture(_mutation_ok 0 set "${_mutation_project}" clean max_size=512 --json)
check_json(mutation-success "${_mutation_ok}" mutation count=1)
assert_exact("${_mutation_ok}" "${EXPECTED}/mutation-success.expected.json")
run_capture(_mutation_error 2 set "${_mutation_project}" clean max_size=99999 --json)
assert_exact("${_mutation_error}" "${EXPECTED}/mutation-error.expected.json")

# Post-publication durability is injected below the process boundary; the core
# failure semantics have their own tests. Pin omission and file-before-recovery
# ordering for each supported notice combination.
foreach(_mode IN ITEMS file recovery both)
    execute_process(COMMAND "${DURABILITY_EXE}" "${_mode}"
        RESULT_VARIABLE _notice_exit OUTPUT_VARIABLE _notice
        ERROR_VARIABLE _notice_err)
    if(NOT _notice_exit EQUAL 0 OR NOT "${_notice_err}" STREQUAL "")
        message(FATAL_ERROR
            "durability payload fixture (${_mode}) failed: ${_notice_err}")
    endif()
    assert_exact("${_notice}"
        "${EXPECTED}/mutation-durability-${_mode}.expected.json")
endforeach()
