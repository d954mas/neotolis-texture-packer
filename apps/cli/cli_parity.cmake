# B3b parity ctest driver: prove `ntpacker pack` emits BYTE-IDENTICAL output to the
# direct-core path (tp_pack_input_build -> tp_export_run, driven by tp_demo_export --
# the GUI's exact entry points, no GL needed). Both pack the SAME fixture project;
# the only variable is the frontend, so any drift (naming, float locale, path
# handling, notice-vs-write ordering) shows up as a file mismatch.
#
#   cmake -DNTPACKER=.. -DDEMO=.. -DSRCPROJ=.. -DSRCDIR=.. -DA=.. -DB=.. -DWORK=..
#         -DFILES="rel1;rel2;..." -P cli_parity.cmake
#
# A = CLI output tree (pack --out-dir A, relative out_paths re-rooted under A).
# B = direct-core output tree (a COPY of the fixture whose relative out_paths resolve
#     against B; tp_demo_export writes there). FILES are the produced paths relative
#     to each tree; every one is compared A/<f> vs B/<f>.

file(REMOVE_RECURSE "${A}" "${B}")
file(MAKE_DIRECTORY "${A}")
file(MAKE_DIRECTORY "${B}")

# Stage the fixture into B so the direct driver resolves sources + out_paths there.
# file(COPY) preserves dotfiles (the .mask.png dotfile-override sprite), which we
# assert explicitly so a copy regression fails loudly instead of as a mismatch.
get_filename_component(_srcname "${SRCPROJ}" NAME)
file(COPY "${SRCPROJ}" DESTINATION "${B}")
file(COPY "${SRCDIR}/parity_sprites" DESTINATION "${B}")
if(NOT EXISTS "${B}/parity_sprites/.mask.png")
    message(FATAL_ERROR "fixture copy dropped the .mask.png dotfile -- parity would be meaningless")
endif()

# (1) CLI path -> A.
execute_process(
    COMMAND "${NTPACKER}" pack "${SRCPROJ}" --out-dir "${A}" --quiet
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "CLI pack failed (${_rc})\n--stdout--\n${_o}\n--stderr--\n${_e}")
endif()

# (2) direct-core path -> B (tp_demo_export packs B's copy to its relative out_paths).
execute_process(
    COMMAND "${DEMO}" "${B}/${_srcname}" "${WORK}"
    RESULT_VARIABLE _rc2 OUTPUT_VARIABLE _o2 ERROR_VARIABLE _e2)
if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "direct-core driver failed (${_rc2})\n--stdout--\n${_o2}\n--stderr--\n${_e2}")
endif()

# (3) byte-compare every produced file.
foreach(_f IN LISTS FILES)
    if(NOT EXISTS "${A}/${_f}")
        message(FATAL_ERROR "CLI did not produce ${_f} under A")
    endif()
    if(NOT EXISTS "${B}/${_f}")
        message(FATAL_ERROR "direct-core did not produce ${_f} under B")
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E compare_files "${A}/${_f}" "${B}/${_f}"
        RESULT_VARIABLE _cmp)
    if(NOT _cmp EQUAL 0)
        message(FATAL_ERROR "PARITY MISMATCH: ${_f}\n  A=${A}/${_f}\n  B=${B}/${_f}")
    endif()
endforeach()

message(STATUS "cli_parity: OK -- CLI and direct-core output byte-identical")
