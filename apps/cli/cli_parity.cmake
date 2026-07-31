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
    COMMAND "${NTPACKER}" pack "${B}/${_srcname}" --out-dir "${A}" --json --quiet
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "CLI pack failed (${_rc})\n--stdout--\n${_o}\n--stderr--\n${_e}")
endif()

# (2) Public live-session path -> B. The driver owns no mutable project or
# direct export orchestration; it opens the same saved project and drains one
# typed Export receipt.
execute_process(
    COMMAND "${DEMO}" "${B}/${_srcname}" "${WORK}" --json
    RESULT_VARIABLE _rc2 OUTPUT_VARIABLE _o2 ERROR_VARIABLE _e2)
if(NOT _rc2 EQUAL 0)
    message(FATAL_ERROR "live-session driver failed (${_rc2})\n--stdout--\n${_o2}\n--stderr--\n${_e2}")
endif()

# (3) compare the success aggregate shared by both client contracts. The live
# receipt intentionally carries extra publication fields; this parity fixture
# proves the exact overlapping counters instead of reconstructing failure
# semantics in the test.
string(JSON _cli_targets GET "${_o}" totals targets_ok)
string(JSON _cli_files GET "${_o}" totals files_written)
string(JSON _live_targets GET "${_o2}" targets)
string(JSON _live_files GET "${_o2}" files)
string(JSON _live_notices GET "${_o2}" notices)
string(JSON _live_atlases_ok GET "${_o2}" atlases_ok)
string(JSON _live_atlases_failed GET "${_o2}" atlases_failed)
string(JSON _live_atlases_skipped GET "${_o2}" atlases_skipped)
string(JSON _live_partial GET "${_o2}" partial_publication)
string(JSON _live_uncertain GET "${_o2}" publication_uncertain)

set(_cli_notices 0)
string(JSON _atlas_count LENGTH "${_o}" atlases)
if(_atlas_count GREATER 0)
    math(EXPR _atlas_last "${_atlas_count} - 1")
    foreach(_atlas RANGE 0 ${_atlas_last})
        string(JSON _target_count LENGTH "${_o}" atlases ${_atlas} targets)
        if(_target_count GREATER 0)
            math(EXPR _target_last "${_target_count} - 1")
            foreach(_target RANGE 0 ${_target_last})
                string(JSON _notice_count LENGTH "${_o}" atlases ${_atlas} targets ${_target} notices)
                math(EXPR _cli_notices "${_cli_notices} + ${_notice_count}")
            endforeach()
        endif()
    endforeach()
endif()

if(NOT _cli_targets EQUAL _live_targets OR
   NOT _cli_files EQUAL _live_files OR
   NOT _cli_notices EQUAL _live_notices)
    message(FATAL_ERROR
        "structured result mismatch:\n"
        "  CLI  targets=${_cli_targets} files=${_cli_files} notices=${_cli_notices}\n"
        "  live targets=${_live_targets} files=${_live_files} notices=${_live_notices}")
endif()
if(NOT _live_atlases_ok EQUAL 1 OR
   NOT _live_atlases_failed EQUAL 0 OR
   NOT _live_atlases_skipped EQUAL 0 OR
   _live_partial OR _live_uncertain)
    message(FATAL_ERROR
        "unexpected live success metadata: ${_o2}")
endif()

# (4) byte-compare every produced file.
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

# (5) Repeat both client shapes with the same deterministic publication
# failure. A directory occupies the second target's metadata path, after an
# earlier target has already published. Both contracts must preserve the same
# successful-target/file counters and expose the partial failure.
set(_af "${A}_failure")
set(_bf "${B}_failure")
set(_wf "${WORK}_failure")
file(REMOVE_RECURSE "${_af}" "${_bf}" "${_wf}")
file(MAKE_DIRECTORY "${_af}" "${_bf}")
file(COPY "${SRCPROJ}" DESTINATION "${_bf}")
file(COPY "${SRCDIR}/parity_sprites" DESTINATION "${_bf}")
file(MAKE_DIRECTORY
    "${_af}/out/nested/deep_json.json"
    "${_bf}/out/nested/deep_json.json")

execute_process(
    COMMAND "${NTPACKER}" pack "${_bf}/${_srcname}"
            --out-dir "${_af}" --json --quiet
    RESULT_VARIABLE _frc
    OUTPUT_VARIABLE _fo
    ERROR_VARIABLE _fe)
if(_frc EQUAL 0)
    message(FATAL_ERROR
        "CLI failure fixture unexpectedly succeeded\n${_fo}")
endif()
execute_process(
    COMMAND "${DEMO}" "${_bf}/${_srcname}" "${_wf}" --json
    RESULT_VARIABLE _frc2
    OUTPUT_VARIABLE _fo2
    ERROR_VARIABLE _fe2)
if(_frc2 EQUAL 0)
    message(FATAL_ERROR
        "live failure fixture unexpectedly succeeded\n${_fo2}")
endif()

string(JSON _f_cli_ok GET "${_fo}" totals targets_ok)
string(JSON _f_cli_failed GET "${_fo}" totals targets_failed)
string(JSON _f_cli_files GET "${_fo}" totals files_written)
string(JSON _f_live_status GET "${_fo2}" status)
string(JSON _f_live_targets GET "${_fo2}" targets)
string(JSON _f_live_files GET "${_fo2}" files)
string(JSON _f_live_failed GET "${_fo2}" atlases_failed)
string(JSON _f_live_partial GET "${_fo2}" partial_publication)
if(NOT _f_cli_ok EQUAL _f_live_targets OR
   NOT _f_cli_files EQUAL _f_live_files OR
   _f_cli_failed LESS 1 OR
   _f_live_failed LESS 1 OR
   _f_live_status EQUAL 0 OR
   NOT _f_live_partial)
    message(FATAL_ERROR
        "structured partial-failure mismatch:\n"
        "  CLI rc=${_frc} ok=${_f_cli_ok} failed=${_f_cli_failed} files=${_f_cli_files}\n"
        "  live rc=${_frc2} status=${_f_live_status} targets=${_f_live_targets} "
        "failed_atlases=${_f_live_failed} files=${_f_live_files} partial=${_f_live_partial}\n"
        "--CLI stderr--\n${_fe}\n--live stderr--\n${_fe2}")
endif()

message(STATUS "cli_parity: OK -- success bytes and partial-failure receipts agree")
