# ntpacker: AI-first CLI frontend (plan docs/plans/op-layer-and-cli.md, step B1).
# Console exe. Links tp_build now (pack/export land in B3); tp_build PUBLIC-links
# tp_core + nt_builder, so the model/export API + exporter registry come
# transitively. CRT: -U_DLL to match tp_build/nt_builder's static CRT (same pin
# as apps/smoke and the builder-linking tests).
set(CLI_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/cli/${NT_PRESET_NAME}")

add_executable(ntpacker main.c cli_out.c)
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
