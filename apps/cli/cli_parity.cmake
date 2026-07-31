# Client parity ctest driver: prove ordinary saved-file CLI Export and public
# live-session Export emit byte-identical artifacts and metadata reports. Both
# clients consume the exact same staged saved project; the only variable is
# admission/delivery (synchronous CLI versus typed live terminal receipt).
#
#   cmake -DNTPACKER=.. -DDEMO=.. -DSRCPROJ=.. -DSRCDIR=.. -DA=.. -DB=.. -DWORK=..
#         -DFILES="rel1;rel2;..." -P cli_parity.cmake
#
# A = CLI output tree (pack --out-dir A, relative out_paths re-rooted under A).
# B = live-session output tree (the staged project's relative out_paths resolve
#     against B). FILES include image artifacts and exporter metadata reports;
# every one is compared A/<f> vs B/<f>.

file(REMOVE_RECURSE "${A}" "${B}")
file(MAKE_DIRECTORY "${A}")
file(MAKE_DIRECTORY "${B}")

# Stage one saved project in B so both client shapes resolve the same sources.
# file(COPY) preserves dotfiles (the .mask.png dotfile-override sprite), which we
# assert explicitly so a copy regression fails loudly instead of as a mismatch.
get_filename_component(_srcname "${SRCPROJ}" NAME)
file(COPY "${SRCPROJ}" DESTINATION "${B}")
file(COPY "${SRCDIR}/parity_sprites" DESTINATION "${B}")
if(NOT EXISTS "${B}/parity_sprites/.mask.png")
    message(FATAL_ERROR "fixture copy dropped the .mask.png dotfile -- parity would be meaningless")
endif()

# (1) Ordinary saved-file CLI path -> A, using the staged project.
execute_process(
    COMMAND "${NTPACKER}" pack "${B}/${_srcname}" --out-dir "${A}" --quiet
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "CLI pack failed (${_rc})\n--stdout--\n${_o}\n--stderr--\n${_e}")
endif()

# (2) Public live-session path -> B. The driver owns no mutable project or
# direct export orchestration; it opens the same saved project and drains one
# typed Export receipt.
execute_process(
    COMMAND "${DEMO}" "${B}/${_srcname}" "${WORK}"
    RESULT_VARIABLE _rc2 OUTPUT_VARIABLE _o2 ERROR_VARIABLE _e2)
if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "live-session driver failed (${_rc2})\n--stdout--\n${_o2}\n--stderr--\n${_e2}")
endif()

# (3) byte-compare every produced file.
foreach(_f IN LISTS FILES)
    if(NOT EXISTS "${A}/${_f}")
        message(FATAL_ERROR "CLI did not produce ${_f} under A")
    endif()
    if(NOT EXISTS "${B}/${_f}")
        message(FATAL_ERROR "live-session did not produce ${_f} under B")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${A}/${_f}" "${B}/${_f}"
        RESULT_VARIABLE _cmp)
    if(NOT _cmp EQUAL 0)
        message(FATAL_ERROR "PARITY MISMATCH: ${_f}\n  A=${A}/${_f}\n  B=${B}/${_f}")
    endif()
endforeach()

message(STATUS "cli_parity: OK -- ordinary CLI and live-session Export output byte-identical")
