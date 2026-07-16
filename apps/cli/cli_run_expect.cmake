# ctest driver (plan B1): run EXE with ARGS, assert the exact exit code EXPECT.
# Optional: MATCH (a regex stdout must contain), CHECKER (an exe run over the
# captured stdout written to OUTFILE), and EXPECT_FILE (an exact stdout golden,
# ignoring only its optional final line ending). Asserting an EXACT non-zero code is why
# this wraps execute_process instead of ctest WILL_FAIL (which only tests != 0).
#   cmake -DEXE=.. -DEXPECT=.. [-DARGS=..] [-DMATCH=..] [-DCHECKER=.. -DOUTFILE=..] [-DEXPECT_FILE=..] -P cli_run_expect.cmake
if(NOT DEFINED ARGS)
    set(ARGS "")
endif()
separate_arguments(_args NATIVE_COMMAND "${ARGS}")

execute_process(
    COMMAND "${EXE}" ${_args}
    RESULT_VARIABLE _code
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)

if(NOT "${_code}" STREQUAL "${EXPECT}")
    message(FATAL_ERROR "exit code ${_code} != expected ${EXPECT}\n--stdout--\n${_out}\n--stderr--\n${_err}")
endif()

if(DEFINED EXPECT_STDERR_EMPTY AND EXPECT_STDERR_EMPTY AND NOT "${_err}" STREQUAL "")
    message(FATAL_ERROR "stderr must be empty\n--stderr--\n${_err}")
endif()

if(DEFINED MATCH AND NOT "${_out}" MATCHES "${MATCH}")
    message(FATAL_ERROR "stdout did not match /${MATCH}/\n--stdout--\n${_out}")
endif()

if(DEFINED EXIST_FILES)
    foreach(_f IN LISTS EXIST_FILES)
        if(NOT EXISTS "${_f}")
            message(FATAL_ERROR "expected output file missing: ${_f}\n--stderr--\n${_err}")
        endif()
    endforeach()
endif()

# NOT_EXIST_FILES: assert each path is ABSENT after the run (dry-run coverage --
# ntpacker pack --dry-run must not create any target file or directory).
if(DEFINED NOT_EXIST_FILES)
    foreach(_f IN LISTS NOT_EXIST_FILES)
        if(EXISTS "${_f}")
            message(FATAL_ERROR "path must NOT exist after a dry run: ${_f}\n--stderr--\n${_err}")
        endif()
    endforeach()
endif()

if(DEFINED CHECKER OR DEFINED EXPECT_FILE)
    if(NOT DEFINED OUTFILE)
        message(FATAL_ERROR "OUTFILE is required with CHECKER or EXPECT_FILE")
    endif()
    file(WRITE "${OUTFILE}" "${_out}")
endif()

if(DEFINED EXPECT_FILE)
    file(READ "${EXPECT_FILE}" _expected_out)
    # Console output owns one trailing newline. CMake's process capture and Git
    # may represent it as LF or CRLF; no other bytes are normalized.
    string(REGEX REPLACE "(\r\n|\n)$" "" _actual_exact "${_out}")
    string(REGEX REPLACE "(\r\n|\n)$" "" _expected_exact "${_expected_out}")
    if(NOT "${_actual_exact}" STREQUAL "${_expected_exact}")
        message(FATAL_ERROR
            "stdout differs from exact golden ${EXPECT_FILE}\n--actual--\n${_out}\n--expected--\n${_expected_out}")
    endif()
endif()

if(DEFINED CHECKER)
    # Optional CHECK_ARGS (space-separated) select the checker mode + assertions.
    if(NOT DEFINED CHECK_ARGS)
        set(CHECK_ARGS "")
    endif()
    separate_arguments(_cargs NATIVE_COMMAND "${CHECK_ARGS}")
    execute_process(
        COMMAND "${CHECKER}" "${OUTFILE}" ${_cargs}
        RESULT_VARIABLE _cc
        OUTPUT_VARIABLE _co
        ERROR_VARIABLE _ce)
    if(NOT _cc EQUAL 0)
        message(FATAL_ERROR "json checker failed (${_cc}): ${_co}${_ce}\n--payload--\n${_out}")
    endif()
endif()
