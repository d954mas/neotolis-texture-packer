# ntpacker: AI-first CLI frontend (plan docs/plans/op-layer-and-cli.md, step B1).
# Console exe. Links tp_build now (pack/export land in B3); tp_build PUBLIC-links
# tp_core + nt_builder, so the model/export API + exporter registry come
# transitively. CRT: -U_DLL to match tp_build/nt_builder's static CRT (same pin
# as apps/smoke and the builder-linking tests).
set(CLI_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/cli/${NT_PRESET_NAME}")

add_executable(ntpacker main.c cli_out.c cli_project.c cli_inspect.c cli_validate.c cli_pack.c)
target_link_libraries(ntpacker PRIVATE tp_build)
target_include_directories(ntpacker PRIVATE "${CMAKE_SOURCE_DIR}/apps/common") # shared ntpacker_version.h
target_compile_definitions(ntpacker PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(ntpacker PRIVATE -U_DLL)
nt_set_warning_flags(ntpacker) # AGENTS.md invariant: engine warning flags, zero warnings
nt_set_sanitizer_flags(ntpacker)
set_target_properties(ntpacker PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CLI_OUTPUT_DIR}")

# --- e2e ctest: run the built exe, assert the exit-code contract + JSON payload.
# No GL/display needed, so these give the op layer its first cross-OS CI coverage.

# JSON validity checker (test-only helper; parses a captured payload via cJSON).
# cJSON is engine-vendored, static-CRT -> -U_DLL (same as the packer tests that
# link it). Warning flags follow the packer/tests convention (helpers exempt).
add_executable(cli_json_check cli_json_check.c)
target_link_libraries(cli_json_check PRIVATE cjson)
target_compile_definitions(cli_json_check PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(cli_json_check PRIVATE -U_DLL)

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

# (e) inspect --json on the clean fixture -> exit 0, parses, atlas[0] has 2 sprites.
add_test(NAME cli_inspect_json COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=inspect ${_td}/clean.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check> "-DCHECK_ARGS=inspect sprites=2"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_inspect.json
    -P "${_run}")

# (f) inspect on a missing file -> project load error, exit 3.
add_test(NAME cli_inspect_missing COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=3 "-DARGS=inspect ${_td}/no_such.ntpacker_project" -P "${_run}")

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
    -P "${_run}")

# (i) same fixture WITH --strict -> exit 7 (error-severity findings present).
add_test(NAME cli_validate_strict COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=7 "-DARGS=validate ${_td}/problems.ntpacker_project --strict" -P "${_run}")

# (j) validate on an unloadable file -> exit 3.
add_test(NAME cli_validate_unloadable COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=3 "-DARGS=validate ${_td}/bad.ntpacker_project" -P "${_run}")

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
