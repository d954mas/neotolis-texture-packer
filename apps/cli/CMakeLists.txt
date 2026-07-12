# ntpacker: AI-first CLI frontend (plan docs/plans/op-layer-and-cli.md, step B1).
# Console exe. Links tp_build now (pack/export land in B3); tp_build PUBLIC-links
# tp_core + nt_builder, so the model/export API + exporter registry come
# transitively. CRT: -U_DLL to match tp_build/nt_builder's static CRT (same pin
# as apps/smoke and the builder-linking tests).
set(CLI_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/cli/${NT_PRESET_NAME}")

add_executable(ntpacker main.c cli_out.c cli_project.c cli_inspect.c cli_validate.c cli_pack.c cli_mutate.c)
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

# (i5) F1-03 §5.6 sprite-record integrity: a v4 fixture with a migrated override whose
# stored source id is absent from the atlas (sprite_bad_source, error) and another whose
# (source,key) resolves to no scanned sprite (orphan_sprite, warning). No --strict -> exit 0.
add_test(NAME cli_validate_sprite_records COMMAND ${CMAKE_COMMAND}
    -DEXE=$<TARGET_FILE:ntpacker> -DEXPECT=0 "-DARGS=validate ${_td}/v4_orphan.ntpacker_project --json"
    -DCHECKER=$<TARGET_FILE:cli_json_check>
    "-DCHECK_ARGS=validate code=sprite_bad_source code=orphan_sprite"
    -DOUTFILE=${CMAKE_CURRENT_BINARY_DIR}/cli_validate_sprite_records.json
    -P "${_run}")

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
