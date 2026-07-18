# ntpacker: AI-first CLI frontend (plan docs/plans/op-layer-and-cli.md, step B1).
# Console exe. Links tp_build now (pack/export land in B3); tp_build PUBLIC-links
# tp_core + nt_builder, so the model/export API + exporter registry come
# transitively. CRT: -U_DLL to match tp_build/nt_builder's static CRT (same pin
# as apps/smoke and the builder-linking tests).
set(CLI_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/cli/${NT_PRESET_NAME}")

set(_ntpacker_sources main.c cli_exit.c cli_out.c cli_project.c cli_inspect.c cli_validate.c cli_pack.c cli_mutate.c)
if(WIN32)
    list(APPEND _ntpacker_sources "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
endif()
function(ntpacker_configure_target target)
    target_link_libraries(${target} PRIVATE tp_build)
    if(WIN32)
        target_link_libraries(${target} PRIVATE shell32)
    endif()
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/apps/common")
    target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${target} PRIVATE -U_DLL)
    nt_set_warning_flags(${target})
    nt_set_sanitizer_flags(${target})
endfunction()

add_executable(ntpacker ${_ntpacker_sources})
ntpacker_configure_target(ntpacker)
set_target_properties(ntpacker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CLI_OUTPUT_DIR}")

# Test-only binary with one deterministic fault seam. Production ntpacker has
# no environment-controlled behavior; this variant proves inspect propagates a
# sprite-index failure as structured JSON + the internal-error exit code.
add_executable(ntpacker_inspect_fault ${_ntpacker_sources})
ntpacker_configure_target(ntpacker_inspect_fault)
target_compile_definitions(ntpacker_inspect_fault PRIVATE NTPACKER_CLI_INSPECT_FAULT_SEAM=1)
set_target_properties(ntpacker_inspect_fault PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CLI_OUTPUT_DIR}")

# --- e2e ctest: run the built exe, assert the exit-code contract + JSON payload.
# No GL/display needed, so these give the op layer its first cross-OS CI coverage.

# JSON validity checker (test-only helper; parses a captured payload via cJSON).
# cJSON is engine-vendored, static-CRT -> -U_DLL (same as the packer tests that
# link it). Warning flags follow the packer/tests convention (helpers exempt).
add_executable(cli_json_check cli_json_check.c)
target_link_libraries(cli_json_check PRIVATE cjson)
target_compile_definitions(cli_json_check PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cli_json_check PRIVATE -U_DLL)

add_executable(cli_out_contract_test cli_out_contract_test.c cli_out.c)
target_link_libraries(cli_out_contract_test PRIVATE tp_core)
target_compile_definitions(cli_out_contract_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cli_out_contract_test PRIVATE -U_DLL)
nt_set_warning_flags(cli_out_contract_test)
nt_set_sanitizer_flags(cli_out_contract_test)
add_test(NAME cli_out_contract COMMAND cli_out_contract_test)

# Deterministic adapter fixture for the post-publication durability notice. The
# fault itself is core-tested; this executable pins only the CLI wire payload.
add_executable(cli_durability_payload_fixture
    cli_durability_payload_fixture.c cli_out.c)
target_link_libraries(cli_durability_payload_fixture PRIVATE tp_core)
target_compile_definitions(cli_durability_payload_fixture PRIVATE
    _CRT_SECURE_NO_WARNINGS)
target_compile_options(cli_durability_payload_fixture PRIVATE -U_DLL)
nt_set_warning_flags(cli_durability_payload_fixture)

add_executable(cli_exit_contract_test test_cli_exit.c cli_exit.c)
target_link_libraries(cli_exit_contract_test PRIVATE tp_core)
target_compile_definitions(cli_exit_contract_test PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cli_exit_contract_test PRIVATE -U_DLL)
nt_set_warning_flags(cli_exit_contract_test)
nt_set_sanitizer_flags(cli_exit_contract_test)
add_test(NAME cli_exit_contract COMMAND cli_exit_contract_test)

if(WIN32)
    add_executable(cli_utf8_argv_test
        test_utf8_argv.c
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c"
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_fs.c")
    target_include_directories(cli_utf8_argv_test PRIVATE
        "${CMAKE_SOURCE_DIR}/apps/common")
    target_link_libraries(cli_utf8_argv_test PRIVATE tp_core unity shell32)
    target_compile_definitions(cli_utf8_argv_test PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(cli_utf8_argv_test PRIVATE -U_DLL)
    nt_set_warning_flags(cli_utf8_argv_test)
    nt_set_sanitizer_flags(cli_utf8_argv_test)
    add_test(NAME cli_utf8_argv COMMAND cli_utf8_argv_test)
endif()

set(_run "${CMAKE_CURRENT_SOURCE_DIR}/cli_run_expect.cmake")

# (a) no args -> usage error, exit 2.
add_test(NAME cli_no_args COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=2 "-DARGS=" -P "${_run}")

# (b) unknown verb -> usage error, exit 2.
add_test(NAME cli_unknown_verb COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=2 "-DARGS=frobnicate" -P "${_run}")

# (b2) unknown flag -> usage error, exit 2.
add_test(NAME cli_unknown_flag COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=2 "-DARGS=--bogus" -P "${_run}")

# (c) version -> exit 0, human line present on stdout.
add_test(NAME cli_version COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=version" -DMATCH=ntpacker -P "${_run}")

# (d) version --json -> exit 0 AND stdout parses as JSON with the manifest keys.
add_test(NAME cli_version_json COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=version --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_version_manifest.json
    -P "${_run}")

# --- B2 read verbs: inspect + validate over checked-in fixtures (no GL/pack). ---
# The fixtures live in the source tree; project_dir resolves sources against them.
set(_td "${CMAKE_CURRENT_SOURCE_DIR}/testdata")

# P1-05: exact schemas/payloads. Only elapsed time and the two known absolute
# test roots are normalized; path policy and every suffix are asserted first.
add_test(NAME cli_exact_payload_contract COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker>
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    -DDURABILITY_EXE=$<TARGET_FILE:cli_durability_payload_fixture>
    -DTD=${_td}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_exact_contract
    -DEXPECTED=${_td}
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_exact_contract.cmake")

# The process exit-code contract includes total export failure (5) and partial
# success (6). Force portable write failures with directories at output paths.
add_test(NAME cli_pack_exit_matrix COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker>
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    -DTD=${_td}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_exit_matrix
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_pack_exit_matrix.cmake")

# (e) inspect --json on the clean fixture -> exit 0, parses, atlas[0] has 2 sprites.
add_test(NAME cli_inspect_json COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=inspect ${_td}/clean.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check> "-DCHECK_ARGS=inspect sprites=2"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_inspect.json
    -P "${_run}")

if(WIN32)
    # Process-level proof for the Windows UTF-16 -> strict UTF-8 ingress and
    # packer filesystem boundary. CMake launches the child through CreateProcessW.
    set(_unicode_td "${CMAKE_CURRENT_BINARY_DIR}/нтпакер")
    file(MAKE_DIRECTORY "${_unicode_td}")
    configure_file("${_td}/clean.ntpacker_project"
                   "${_unicode_td}/проект.ntpacker_project" COPYONLY)
    file(COPY "${_td}/sprites" DESTINATION "${_unicode_td}")
    add_test(NAME cli_inspect_unicode_path COMMAND ${CMAKE_COMMAND}
        -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0
        "-DARGS=inspect ${_unicode_td}/проект.ntpacker_project --json"
        -DCHECKER=$<TARGET_FILE:cli_json_check>
        "-DCHECK_ARGS=inspect sprites=2"
        -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_inspect_unicode.json
        -P "${_run}")
endif()

# The documented one-shot CLI accepts project paths relative to its process CWD;
# the core identity boundary, not each verb, owns that resolution.
add_test(NAME cli_inspect_relative_project COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0
    "-DARGS=inspect testdata/clean.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check> "-DCHECK_ARGS=inspect sprites=2"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_inspect_relative.json
    -P "${_run}")
set_tests_properties(cli_inspect_relative_project PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

# (f) inspect on a missing file -> project load error, exit 3.
add_test(NAME cli_inspect_missing COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=3 "-DARGS=inspect ${_td}/no_such.ntpacker_project" -P "${_run}")

add_test(NAME cli_inspect_index_fault COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker_inspect_fault> -DEXPECT=1
    "-DARGS=inspect ${_td}/clean.ntpacker_project --json"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_inspect_index_fault.json
    -DEXPECT_FILE=${_td}/inspect-index-fault.expected.json
    -DEXPECT_STDERR_EMPTY=ON -P "${_run}")
set_tests_properties(cli_inspect_index_fault PROPERTIES
    ENVIRONMENT "NTPACKER_TEST_INSPECT_INDEX_FAIL=1")

# (g) validate --json on the clean fixture -> exit 0, counts 0/0.
add_test(NAME cli_validate_clean COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/clean.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check> "-DCHECK_ARGS=validate error=0 warning=0"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_clean.json
    -P "${_run}")

# (h) validate --json on the problems fixture (no --strict) -> exit 0, findings
# contain BOTH the missing-source and dangling-frame codes.
add_test(NAME cli_validate_problems COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/problems.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate code=missing_source code=dangling_anim_frame"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_problems.json
    -DEXPECT_FILE=${_td}/validate-problems.expected.json
    -P "${_run}")

# (i) same JSON payload WITH --strict -> exit 7 (error-severity findings present).
add_test(NAME cli_validate_strict COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=7
    "-DARGS=validate ${_td}/problems.ntpacker_project --strict --json"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_strict.json
    -DEXPECT_FILE=${_td}/validate-problems.expected.json
    -P "${_run}")

# (i2) F1-02 source-key validation: a fixture with case-folding source paths
# ("Art/hero" vs "art/hero") and a reserved-name path ("con.png") -> exit 0
# (warnings only) AND both source_collision + source_portability findings present.
add_test(NAME cli_validate_source_keys COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/collision.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate code=source_collision code=source_portability"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_source_keys.json
    -P "${_run}")

# (i3) F1-02 Fix B: './' , '//' and trailing-slash spellings of ONE folder canonicalize
# to the same key -> flagged duplicate_source (the old fixed-1024 slash-only key missed
# these). No --strict -> exit 0 (findings in the payload).
add_test(NAME cli_validate_dup_spelling COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/dupspell.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate code=duplicate_source"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_dup_spelling.json
    -P "${_run}")

# (i4) F1-02 Fix B: two DISTINCT >1024-byte source paths sharing a 1023-byte prefix must
# NOT be mistaken for a duplicate (the old fixed-1024 buffer truncated both into a spurious
# match). Canonicalized in a TP_SRCKEY_MAX buffer they stay distinct: the only warning is
# empty_atlas (both paths missing -> 2 errors); a spurious duplicate would push warning to 2.
add_test(NAME cli_validate_long_paths COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/longpaths.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate error=2 warning=1"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_long_paths.json
    -P "${_run}")

# (i5) Canonical v5 orphan integrity: both sparse records target a real source ID,
# but their source-local keys are currently absent. They remain valid orphaned
# metadata and reactivate if either key returns; no name-only bridge exists.
# Both findings are warnings, so without --strict the command exits 0.
add_test(NAME cli_validate_sprite_records COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/v5_orphan.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate error=0 warning=2 code=orphan_sprite"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_sprite_records.json
    -P "${_run}")

# (i5b) The same canonical orphan fixture with --strict still exits 0 because
# missing physical keys are warnings, not malformed project identity.
add_test(NAME cli_validate_sprite_records_strict COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/v5_orphan.ntpacker_project --strict"
    -P "${_run}")

# (i6) Deterministic multi-atlas validation contract: two independent duplicate
# output groups (cardinality 3 and 2) pin core iteration and schema-2 CLI order.
# Every source resolves and every exporter is known, so these five warnings are
# the complete platform-neutral report. No --strict -> exit 0.
add_test(NAME cli_validate_dup_target COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/dup_target.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate error=0 warning=5 code=duplicate_out_path"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_dup_target.json
    -DEXPECT_FILE=${_td}/validate-dup-target.expected.json
    -P "${_run}")

# (j) validate on an unloadable file -> exit 3.
add_test(NAME cli_validate_unloadable COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=3
    "-DARGS=validate ${_td}/bad.ntpacker_project --json" -P "${_run}")

# (k) pack the clean fixture: exit 0, report parses (timings masked), files land
# under --out-dir (never in the source tree).
add_test(NAME cli_pack_clean COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0
    "-DARGS=pack ${_td}/clean.ntpacker_project --json --out-dir ${CMAKE_CURRENT_BINARY_DIR}/cli_pack_out"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=pack targets_ok=1 targets_failed=0"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_clean.json
    "-DEXIST_FILES=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_out/out/clean.json.json;${CMAKE_CURRENT_BINARY_DIR}/cli_pack_out/out/clean.json-0.png"
    -P "${_run}")

# (l) --target filtering every target away -> ok-with-warning (agents must not
# hard-fail on a preview-only project), zero targets in the report.
add_test(NAME cli_pack_target_none COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0
    "-DARGS=pack ${_td}/clean.ntpacker_project --json --target defold --out-dir ${CMAKE_CURRENT_BINARY_DIR}/cli_pack_out2"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=pack targets_ok=0 targets_failed=0"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_none.json
    -P "${_run}")

# (m) dangling anim frame aborts the atlas before any target writes (plan L-4
# through the CLI): pack failure, exit 4.
add_test(NAME cli_pack_problems COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=4
    "-DARGS=pack ${_td}/problems.ntpacker_project --out-dir ${CMAKE_CURRENT_BINARY_DIR}/cli_pack_out3"
    -P "${_run}")

# (n) B3b dry-run: exit 0, report parses with dry_run true + would_write per target,
# and NO output file lands on disk (the out dir stays absent -- NOT_EXIST_FILES).
add_test(NAME cli_pack_dry_run COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0
    "-DARGS=pack ${_td}/clean.ntpacker_project --dry-run --json --out-dir ${CMAKE_CURRENT_BINARY_DIR}/cli_pack_dry"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=pack dry_run=1 targets_ok=1 targets_failed=0"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_dry.json
    "-DNOT_EXIST_FILES=${CMAKE_CURRENT_BINARY_DIR}/cli_pack_dry/out/clean.json.json;${CMAKE_CURRENT_BINARY_DIR}/cli_pack_dry/out/clean.json-0.png;${CMAKE_CURRENT_BINARY_DIR}/cli_pack_dry/out"
    -P "${_run}")

# (o) B3b parity: `ntpacker pack` == direct-core (tp_demo_export) BYTE-FOR-BYTE on a
# fixture exercising a nested relative out_path, a rename feeding an anim, a dotfile
# override, and a fractional pivot (float/locale canary). tp_demo_export lives in
# packer/tests; both targets are built by the top-level build. No GL -> runs on 3 OS.
add_test(NAME cli_parity COMMAND ${CMAKE_COMMAND}
    -DNTPACKER=$<TARGET_FILE:ntpacker>
    -DDEMO=$<TARGET_FILE:tp_demo_export>
    -DSRCPROJ=${_td}/parity.ntpacker_project
    -DSRCDIR=${_td}
    -DA=${CMAKE_CURRENT_BINARY_DIR}/cli_parity_A
    -DB=${CMAKE_CURRENT_BINARY_DIR}/cli_parity_B
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_parity_work
    "-DFILES=out/parity_json.json;out/parity_json-0.png;out/nested/deep_json.json;out/nested/deep_json-0.png;out/parity_def.tpinfo;out/parity_def.tpatlas;out/parity_def-0.png"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cli_parity.cmake")

# --- B4 mutation verbs: one e2e test per verb FAMILY. Each drives a sequence of
# ntpacker commands against a FRESH project in an isolated scratch dir (mutations
# never touch the checked-in testdata) and asserts exit codes + resulting file state.
# No GL/pack -> runs on 3 OS. See cli_mutate_family.cmake for the per-family script.
set(_mut "${CMAKE_CURRENT_SOURCE_DIR}/cli_mutate_family.cmake")
foreach(_fam new source set sprite anim target atlas stable)
    add_test(NAME cli_mutate_${_fam} COMMAND ${CMAKE_COMMAND}
        -DEXE=$<TARGET_FILE:ntpacker>
        -DCHECKER=$<TARGET_FILE:cli_json_check>
        -DTD=${_td}
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_mut_${_fam}
        -DFAMILY=${_fam}
        -P "${_mut}")
endforeach()
