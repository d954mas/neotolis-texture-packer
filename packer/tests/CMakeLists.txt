add_executable(tp_test_arena test_arena.c)
target_link_libraries(tp_test_arena PRIVATE tp_core unity)
target_compile_options(tp_test_arena PRIVATE -U_DLL)
add_test(NAME tp_arena COMMAND tp_test_arena)

add_executable(tp_test_name_map test_name_map.c)
target_link_libraries(tp_test_name_map PRIVATE tp_core unity)
target_compile_options(tp_test_name_map PRIVATE -U_DLL)
add_test(NAME tp_name_map COMMAND tp_test_name_map)

# B1: tp_status_id() stable machine tokens (--json error-id contract), pinned per
# enum value so a rename can't slip through without failing here.
add_executable(tp_test_status_id test_status_id.c)
target_link_libraries(tp_test_status_id PRIVATE tp_core unity)
target_compile_options(tp_test_status_id PRIVATE -U_DLL)
add_test(NAME tp_status_id COMMAND tp_test_status_id)

# A1 op-layer: canonical sprite-name key + natural order + common prefix.
# Pins the dot-basename key fix that the showcase draw-hash gate cannot see.
add_executable(tp_test_names test_names.c)
target_link_libraries(tp_test_names PRIVATE tp_core unity)
target_compile_options(tp_test_names PRIVATE -U_DLL)
add_test(NAME tp_names COMMAND tp_test_names)

# A2 op-layer: tp_scan directory recursion + image-ext whitelist, moved out of the
# GUI. Builds a fixture tree under a scratch dir at runtime (argv[1]).
add_executable(tp_test_scan test_scan.c)
target_link_libraries(tp_test_scan PRIVATE tp_core unity)
target_compile_options(tp_test_scan PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_scan_test")
add_test(NAME tp_scan COMMAND tp_test_scan "${CMAKE_BINARY_DIR}/tp_scan_test")

# A3a op-layer: tp_input project->pack-input build (assemble/desc_add moved out of
# the GUI), the per-sprite effective-shape rule, and the non-RECT extrude clamp in
# tp_project_atlas_to_settings. Links tp_build so the CONCAVE+extrude clamp
# regression can pack + export (tp_pack/tp_export_run); the pure input-build asserts
# only need tp_core. Builds a fixture tree under a scratch dir at runtime (argv[1]).
add_executable(tp_test_input test_input.c)
target_link_libraries(tp_test_input PRIVATE tp_build unity)
target_compile_options(tp_test_input PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_input_test")
add_test(NAME tp_input COMMAND tp_test_input "${CMAKE_BINARY_DIR}/tp_input_test")

# Synthetic D4 transform-decode test (plan §3.6). Includes the private helper
# header from src/ directly -- no public API pollution.
add_executable(tp_test_transform_decode test_transform_decode.c)
target_link_libraries(tp_test_transform_decode PRIVATE tp_core unity)
target_include_directories(tp_test_transform_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_transform_decode PRIVATE -U_DLL)
add_test(NAME tp_transform_decode COMMAND tp_test_transform_decode)

# Fixture-driven golden round-trip: builds every fixture case (tp_fixtures) via
# nt_builder (transitive through tp_core) then parses it back. Needs a scratch
# dir for the temp .ntpack files.
add_executable(tp_test_pack_read test_pack_read.c tp_fixtures.c)
target_link_libraries(tp_test_pack_read PRIVATE tp_build unity)  # tp_build: fixtures pack via nt_builder
target_compile_options(tp_test_pack_read PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_test")
add_test(NAME tp_pack_read COMMAND tp_test_pack_read "${CMAKE_BINARY_DIR}/tp_test")

# UV recovery property test (plan §3.3): pure functions, no builder. Includes
# the private helper header from src/ directly.
add_executable(tp_test_uv_property test_uv_property.c)
target_link_libraries(tp_test_uv_property PRIVATE tp_core unity)
target_include_directories(tp_test_uv_property PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_uv_property PRIVATE -U_DLL)
add_test(NAME tp_uv_property COMMAND tp_test_uv_property)

# Phase 1b tp_pack invariants + determinism (plan §3.4): drives nt_builder
# (transitive through tp_core) then parses back. Needs a scratch dir for the
# session .ntpack files.
add_executable(tp_test_pack test_pack.c)
target_link_libraries(tp_test_pack PRIVATE tp_build unity)  # tp_build: drives nt_builder
target_compile_options(tp_test_pack PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_pack_test")
add_test(NAME tp_pack COMMAND tp_test_pack "${CMAKE_BINARY_DIR}/tp_pack_test")

# Phase 3 tp_project load/save: round-trip, sparse, determinism, version refusal,
# path relativization, to_settings bridge. Needs a scratch dir for project files.
add_executable(tp_test_project test_project.c)
target_link_libraries(tp_test_project PRIVATE tp_core unity)
target_compile_options(tp_test_project PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_test")
add_test(NAME tp_project COMMAND tp_test_project "${CMAKE_BINARY_DIR}/tp_project_test")

# Phase 2 normalization unit tests: ext/folder strip, rename override, animation
# auto-group + explicit merge, collision, alias, final-name sort, scale. Pure
# tp_core (synthetic tp_results, no builder).
add_executable(tp_test_normalize test_normalize.c)
target_link_libraries(tp_test_normalize PRIVATE tp_core unity)
target_compile_options(tp_test_normalize PRIVATE -U_DLL)
add_test(NAME tp_normalize COMMAND tp_test_normalize)

# Phase 2 json-neotolis golden fidelity + determinism: builds every fixture via
# nt_builder (tp_build), exports json-neotolis, parses back with cJSON.
add_executable(tp_test_export_json test_export_json.c tp_fixtures.c)
target_link_libraries(tp_test_export_json PRIVATE tp_build unity cjson)
target_compile_options(tp_test_export_json PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_json_test")
add_test(NAME tp_export_json COMMAND tp_test_export_json "${CMAKE_BINARY_DIR}/tp_export_json_test")

# Phase 2 PNG page export: pack -> write pages -> decode with stb_image, probe a
# pixel, prove straight-alpha default + premultiply toggle.
add_executable(tp_test_export_png test_export_png.c)
target_link_libraries(tp_test_export_png PRIVATE tp_build unity stb_image)
target_compile_options(tp_test_export_png PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_png_test")
add_test(NAME tp_export_png COMMAND tp_test_export_png "${CMAKE_BINARY_DIR}/tp_export_png_test")

# Phase 2 per-target packing: test-only capability-restricted descriptors prove
# effective-settings repack, shared runs, and metadata-loss notices.
add_executable(tp_test_export_run test_export_run.c)
target_link_libraries(tp_test_export_run PRIVATE tp_build unity cjson)
target_compile_options(tp_test_export_run PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_run_test")
add_test(NAME tp_export_run COMMAND tp_test_export_run "${CMAKE_BINARY_DIR}/tp_export_run_test")

# A5 op-layer: tp_export_predict_loss consistency -- project-knowable axes from a
# NULL prep, pack-dependent axes (alias/multipage) with a prep, and every
# writer-emitted project-knowable notice field_id covered by predict. Links tp_build
# to pack + export the fixture through the real path (tp_export_run).
add_executable(tp_test_predict test_predict.c)
target_link_libraries(tp_test_predict PRIVATE tp_build unity)
target_compile_options(tp_test_predict PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_predict_test")
add_test(NAME tp_predict COMMAND tp_test_predict "${CMAKE_BINARY_DIR}/tp_predict_test")

# Phase 5 Defold exporter: golden .tpinfo/.tpatlas bytes, rotated-geometry,
# determinism over real fixtures, per-target identity repack + 9-slice notice,
# playback enum mapping, and a clean run over the real defold-demo atlases
# (basic/rotate/anim_trim). Builds fixtures via nt_builder (tp_build) and decodes
# the demo PNGs via stb_image.
add_executable(tp_test_export_defold test_export_defold.c tp_fixtures.c)
target_link_libraries(tp_test_export_defold PRIVATE tp_build unity stb_image)
target_compile_options(tp_test_export_defold PRIVATE -U_DLL)
target_compile_definitions(tp_test_export_defold PRIVATE
    TP_DEFOLD_DEMO_DIR="${CMAKE_SOURCE_DIR}/examples/defold-demo")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_defold_test")
add_test(NAME tp_export_defold
    COMMAND tp_test_export_defold "${CMAKE_BINARY_DIR}/tp_export_defold_test" "${CMAKE_SOURCE_DIR}/examples/defold-demo")

# C0-03 task 6: raw-RGBA ownership regression. Drives nt_builder_atlas_add_raw
# directly (links tp_build), mutates+frees the caller buffer immediately after the
# add, packs, and asserts the packed page is unaffected -> the builder deep-copies
# (nt_builder_atlas.c:771-775). Pins a lifetime the public header does not promise
# (see packer/spike/c0/upstream-issue-raw-lifetime.md). Needs a scratch dir.
add_executable(tp_test_raw_ownership test_raw_ownership.c)
target_link_libraries(tp_test_raw_ownership PRIVATE tp_build unity)
target_compile_options(tp_test_raw_ownership PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_raw_ownership)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_raw_ownership_test")
add_test(NAME tp_raw_ownership COMMAND tp_test_raw_ownership "${CMAKE_BINARY_DIR}/tp_raw_ownership_test")

# Dev driver / smoke tool (NOT a ctest): load a `.ntpacker_project`, assemble each
# atlas's sprites through the shared op layer (tp_pack_input_build), and run every
# enabled target -> the project's out_paths. Regenerates the Defold demo's ntpacker
# exports deterministically and serves as the CLI parity oracle (byte-identical to
# `ntpacker pack`). Input assembly + decode live in tp_core now (no stb here).
#   tp_demo_export <project.ntpacker_project> [work_dir]
add_executable(tp_demo_export tp_demo_export.c)
target_link_libraries(tp_demo_export PRIVATE tp_build)
target_compile_options(tp_demo_export PRIVATE -U_DLL)
