add_executable(tp_test_arena test_arena.c)
target_link_libraries(tp_test_arena PRIVATE tp_core unity)
target_compile_options(tp_test_arena PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_arena)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_arena COMMAND tp_test_arena)

add_executable(tp_test_name_map test_name_map.c)
target_link_libraries(tp_test_name_map PRIVATE tp_core unity)
target_compile_options(tp_test_name_map PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_name_map)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_name_map COMMAND tp_test_name_map)

# B1: tp_status_id() stable machine tokens (--json error-id contract), pinned per
# enum value so a rename can't slip through without failing here.
add_executable(tp_test_status_id test_status_id.c)
target_link_libraries(tp_test_status_id PRIVATE tp_core unity)
target_compile_options(tp_test_status_id PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_status_id)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_status_id COMMAND tp_test_status_id)

# F1-00 project identity: 128-bit runtime ID + injectable RNG seam (deterministic).
add_executable(tp_test_identity_id test_identity_id.c)
target_link_libraries(tp_test_identity_id PRIVATE tp_core unity)
target_include_directories(tp_test_identity_id PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # tp_hex.h (shared drift-guard encoder)
target_compile_options(tp_test_identity_id PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_identity_id)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_identity_id COMMAND tp_test_identity_id)

# F1-01 Part A: promoted structural-ID primitive surface (shape-ID parse/format,
# versioned stable hash + sprite_id byte-goldens, production tp_status mapping).
add_executable(tp_test_id test_id.c)
target_link_libraries(tp_test_id PRIVATE tp_core unity)
target_include_directories(tp_test_id PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # tp_hex.h (shared drift-guard encoder)
target_compile_options(tp_test_id PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_id)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_id COMMAND tp_test_id)

# F1-02: source-key normalization / case-fold collision / portability validation,
# promoted from the C0-01 srckey spike onto the production tp_status model.
add_executable(tp_test_srckey test_srckey.c)
target_link_libraries(tp_test_srckey PRIVATE tp_core unity)
target_compile_options(tp_test_srckey PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_srckey)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_srckey COMMAND tp_test_srckey)

# F1-03: resolved sprite index (scan -> source-local key -> sprite_id) + lookups.
# Needs a scratch dir (argv[1]) for the on-disk scan fixtures.
add_executable(tp_test_sprite_index test_sprite_index.c)
target_link_libraries(tp_test_sprite_index PRIVATE tp_core unity)
target_include_directories(tp_test_sprite_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_sprite_index PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_sprite_index)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_sprite_index COMMAND tp_test_sprite_index "${CMAKE_BINARY_DIR}/tp_sprite_index_test")

# F2-01: typed operation engine -- catalog + closed field vocabulary, canonical
# byte-stable encoder goldens, and payload/reference validation (invalid id/type/
# range/reference each -> the right structured status).
add_executable(tp_test_operation test_operation.c)
target_link_libraries(tp_test_operation PRIVATE tp_core unity)
target_include_directories(tp_test_operation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_operation PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_operation)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_operation COMMAND tp_test_operation)

# F2-01: id-only apply (forward + error per op kind), stage-then-commit allocator-
# fault safety, selector->operation builders, and the parity proof (op apply ==
# existing mutator, byte-identical). Includes the private src/ internal header for
# the staging-alloc fault seam (same pattern as test_id.c including tp_hex.h).
add_executable(tp_test_op_apply test_op_apply.c)
target_link_libraries(tp_test_op_apply PRIVATE tp_core unity)
target_include_directories(tp_test_op_apply PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_op_apply PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_op_apply)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_op_apply COMMAND tp_test_op_apply)

# F2-02: atomic transactions, revision, semantic dirty, idempotency, and the
# versioned transaction request/result JSON contract. Deep-clone byte-identity +
# OOM-safe fault sweep, atomicity (op N fails / clone-alloc / per-op-alloc leave the
# model byte-unchanged), expected_revision below/above/equal, idempotent retry,
# dirty=identity (edit->save->edit->inverse->clean at higher revision), validation
# ordering, the CLI batch JSON golden + batch==one-by-one property, and UB-free
# numbers. Includes the private src/ internal headers for the clone + op alloc-fault
# seams (same pattern as test_op_apply.c).
add_executable(tp_test_transaction test_transaction.c)
target_link_libraries(tp_test_transaction PRIVATE tp_core unity)
target_include_directories(tp_test_transaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_transaction PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_transaction)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_transaction COMMAND tp_test_transaction)

# Architecture foundation M1: the synchronous session is the only live model
# owner; snapshots are independently owned and rejected commits publish nothing.
add_executable(tp_test_session test_session.c)
target_link_libraries(tp_test_session PRIVATE tp_core unity)
target_include_directories(tp_test_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_session PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_session)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_session_test")
add_test(NAME tp_session COMMAND tp_test_session "${CMAKE_BINARY_DIR}/tp_session_test")

add_executable(tp_test_job_owner test_job_owner.c)
target_link_libraries(tp_test_job_owner PRIVATE tp_core tinycthread unity)
target_include_directories(tp_test_job_owner PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_job_owner PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_job_owner)
add_test(NAME tp_job_owner COMMAND tp_test_job_owner)

# Architecture foundation job freshness: a result captures both immutable model
# and source-runtime generations. Generation zero is data, never a stale sentinel.
add_executable(tp_test_job_input_token test_job_input_token.c)
target_link_libraries(tp_test_job_input_token PRIVATE tp_core unity)
target_compile_options(tp_test_job_input_token PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_job_input_token)
add_test(NAME tp_job_input_token COMMAND tp_test_job_input_token)

# M5 thin-boundary validation: all project validation rules are core-owned and
# exposed as an owned typed report over a file-oriented entry point.
add_executable(tp_test_validate test_validate.c)
target_link_libraries(tp_test_validate PRIVATE tp_core unity)
target_include_directories(tp_test_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(tp_test_validate PRIVATE
    TP_CLI_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/apps/cli/testdata"
    TP_VALIDATE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_validate_test")
target_compile_options(tp_test_validate PRIVATE -U_DLL)
target_compile_definitions(tp_test_validate PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_test_validate)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_validate_test")
add_test(NAME tp_validate COMMAND tp_test_validate)

# Validation resource/complexity gate: a real saved project with 1k resolved
# sprites, overrides, and frames. Elapsed time is evidence; deterministic probe
# growth is the hard cross-platform contract.
add_executable(tp_bench_validate tp_bench_validate.c)
target_link_libraries(tp_bench_validate PRIVATE tp_core nt_time)
target_include_directories(tp_bench_validate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_bench_validate PRIVATE -U_DLL)
target_compile_definitions(tp_bench_validate PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_bench_validate)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_bench_validate")
add_test(NAME tp_validate_large COMMAND tp_bench_validate "${CMAKE_BINARY_DIR}/tp_bench_validate")

# F2-03: semantic diff / exact inverse (Undo) + redo replay + in-memory history.
# The ORACLE suite: A->forward->B->inverse->A' byte-identical for EVERY op kind (and
# == the legacy full-snapshot restore), reference cascade, a 100-anim batch as one
# inverse, inverse-apply alloc-failure rollback (sweep), capture-alloc atomicity,
# redo + redo-branch discard, and corrupted-diff structured errors. Includes the
# private src/ internal header for the diff alloc seam + record/history internals.
add_executable(tp_test_diff test_diff.c)
target_link_libraries(tp_test_diff PRIVATE tp_core unity)
target_include_directories(tp_test_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_diff PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_diff)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_diff COMMAND tp_test_diff)

# F2-04: recovery-journal fault suite. Proves the §7.1/§7.2 contract -- acknowledged
# (durably appended) transactions recover and never duplicate after restart; torn/failed
# transactions are invisible; the reader is UB-clean on arbitrary/corrupt/short bytes
# (short write at every byte boundary, torn tail, checksum flip, absurd length prefix);
# append failure rolls back exactly; checkpoint+replay restores state + retained id set;
# duplicate retry after restart de-duplicates; a moved-project (stale-key) journal is not
# applied; the coordinator prepare/publish/abort fire at the right commit points; and a
# real on-disk file journal round-trips. Includes the private src/ internal header for the
# format constants + memory-io fault seams. argv[1] is a scratch dir for the file test.
add_executable(tp_test_journal test_journal.c)
target_link_libraries(tp_test_journal PRIVATE tp_core unity)
target_include_directories(tp_test_journal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_journal PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_journal)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_journal_test")
add_test(NAME tp_journal COMMAND tp_test_journal "${CMAKE_BINARY_DIR}/tp_journal_test")

# M0 benchmark measurement contract: deterministic percentile math and the
# fail-closed rule that a rejected operation is never accepted as a timing sample.
add_executable(tp_test_bench_support test_bench_support.c)
target_link_libraries(tp_test_bench_support PRIVATE nt_time unity)
target_compile_options(tp_test_bench_support PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_bench_support)
add_test(NAME tp_bench_support COMMAND tp_test_bench_support)

# F1-03: canonical selector resolver (id/name/path/compound -> exactly one id).
add_executable(tp_test_selector test_selector.c)
target_link_libraries(tp_test_selector PRIVATE tp_core unity)
target_include_directories(tp_test_selector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_selector PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_selector)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_selector COMMAND tp_test_selector "${CMAKE_BINARY_DIR}/tp_selector_test")

# F1-03: v4 sprite-record identity lifecycle -- lazy v3->v4 re-key, pending soft-orphan,
# stored-orphan reactivation, and v4 {source,key} save/reload round-trip.
add_executable(tp_test_sprite_migrate test_sprite_migrate.c)
target_link_libraries(tp_test_sprite_migrate PRIVATE tp_core unity)
target_include_directories(tp_test_sprite_migrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_sprite_migrate PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_sprite_migrate)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_sprite_migrate COMMAND tp_test_sprite_migrate "${CMAKE_BINARY_DIR}/tp_sprite_migrate_test")

# F1-01 Part C (low-level): deterministic legacy synthetic-ID assigner +
# collision-fallback determinism/exhaustion goldens.
add_executable(tp_test_migrate test_migrate.c)
target_link_libraries(tp_test_migrate PRIVATE tp_core unity)
target_include_directories(tp_test_migrate PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # tp_hex.h (shared drift-guard encoder)
target_compile_options(tp_test_migrate PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_migrate)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_migrate COMMAND tp_test_migrate)

# F1-00 project identity: LEXICAL canonical-path fixtures (C0-01 corpus ported).
# Includes the src-only internal header so BOTH host rule sets run on every CI OS.
add_executable(tp_test_identity_path test_identity_path.c)
target_link_libraries(tp_test_identity_path PRIVATE tp_core unity)
target_include_directories(tp_test_identity_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_identity_path PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_identity_path)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_identity_path COMMAND tp_test_identity_path)

add_executable(tp_test_file_fingerprint test_file_fingerprint.c)
target_link_libraries(tp_test_file_fingerprint PRIVATE tp_core unity)
target_compile_options(tp_test_file_fingerprint PRIVATE -U_DLL)
target_compile_definitions(tp_test_file_fingerprint PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_test_file_fingerprint)
add_test(NAME tp_file_fingerprint COMMAND tp_test_file_fingerprint)

# F1-00 project identity: FILESYSTEM realpath/symlink resolution + atomic Save-As
# transition. Uses a created scratch dir (argv[1]); no baked absolute paths.
add_executable(tp_test_identity_session test_identity_session.c)
target_link_libraries(tp_test_identity_session PRIVATE tp_core unity)
target_include_directories(tp_test_identity_session PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # tp_hex.h (shared drift-guard encoder)
target_compile_options(tp_test_identity_session PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_identity_session)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_identity_test")
add_test(NAME tp_identity_session COMMAND tp_test_identity_session "${CMAKE_BINARY_DIR}/tp_identity_test")

# Architecture foundation M4/A1: OS-backed exclusive canonical project lease.
# The executable re-enters itself as a child process to prove cross-process busy
# detection while keeping the test independent from GUI/session/recovery code.
add_executable(tp_test_project_lease test_project_lease.c)
target_link_libraries(tp_test_project_lease PRIVATE tp_core unity)
target_include_directories(tp_test_project_lease PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project_lease PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project_lease)
add_test(NAME tp_project_lease COMMAND tp_test_project_lease
    "${CMAKE_BINARY_DIR}/tp_project_lease_test" "$<TARGET_FILE:ntpacker>")

add_executable(tp_test_recovery_store test_recovery_store.c)
target_link_libraries(tp_test_recovery_store PRIVATE tp_core unity)
target_include_directories(tp_test_recovery_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_recovery_store PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_recovery_store)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_recovery_store_test")
add_test(NAME tp_recovery_store COMMAND tp_test_recovery_store "${CMAKE_BINARY_DIR}/tp_recovery_store_test")

# A1 op-layer: canonical sprite-name key + natural order + common prefix.
# Pins the dot-basename key fix that the showcase draw-hash gate cannot see.
add_executable(tp_test_names test_names.c)
target_link_libraries(tp_test_names PRIVATE tp_core unity)
target_compile_options(tp_test_names PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_names)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_names COMMAND tp_test_names)

# A2 op-layer: tp_scan directory recursion + image-ext whitelist, moved out of the
# GUI. Builds a fixture tree under a scratch dir at runtime (argv[1]).
add_executable(tp_test_scan test_scan.c)
target_link_libraries(tp_test_scan PRIVATE tp_core unity)
target_compile_options(tp_test_scan PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_scan)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_scan_test")
add_test(NAME tp_scan COMMAND tp_test_scan "${CMAKE_BINARY_DIR}/tp_scan_test")

# A3a op-layer: tp_input project->pack-input build (assemble/desc_add moved out of
# the GUI), the per-sprite effective-shape rule, and the non-RECT extrude clamp in
# tp_project_atlas_to_settings. Links tp_build so the CONCAVE+extrude clamp
# regression can pack + export (tp_pack/tp_export_run); the pure input-build asserts
# only need tp_core. Builds a fixture tree under a scratch dir at runtime (argv[1]).
add_executable(tp_test_input test_input.c)
target_link_libraries(tp_test_input PRIVATE tp_build unity)
target_include_directories(tp_test_input PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_input PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_input)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_input_test")
add_test(NAME tp_input COMMAND tp_test_input "${CMAKE_BINARY_DIR}/tp_input_test")

# Synthetic D4 transform-decode test (plan §3.6). Includes the private helper
# header from src/ directly -- no public API pollution.
add_executable(tp_test_transform_decode test_transform_decode.c)
target_link_libraries(tp_test_transform_decode PRIVATE tp_core unity)
target_include_directories(tp_test_transform_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_transform_decode PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_transform_decode)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_transform_decode COMMAND tp_test_transform_decode)

# Fixture-driven golden round-trip: builds every fixture case (tp_fixtures) via
# nt_builder (transitive through tp_core) then parses it back. Needs a scratch
# dir for the temp .ntpack files.
add_executable(tp_test_pack_read test_pack_read.c tp_fixtures.c)
target_link_libraries(tp_test_pack_read PRIVATE tp_build unity)  # tp_build: fixtures pack via nt_builder
target_compile_options(tp_test_pack_read PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_pack_read)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_test")
add_test(NAME tp_pack_read COMMAND tp_test_pack_read "${CMAKE_BINARY_DIR}/tp_test")

# UV recovery property test (plan §3.3): pure functions, no builder. Includes
# the private helper header from src/ directly.
add_executable(tp_test_uv_property test_uv_property.c)
target_link_libraries(tp_test_uv_property PRIVATE tp_core unity)
target_include_directories(tp_test_uv_property PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_uv_property PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_uv_property)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_uv_property COMMAND tp_test_uv_property)

# Phase 1b tp_pack invariants + determinism (plan §3.4): drives nt_builder
# (transitive through tp_core) then parses back. Needs a scratch dir for the
# session .ntpack files.
add_executable(tp_test_pack test_pack.c)
target_link_libraries(tp_test_pack PRIVATE tp_build unity)  # tp_build: drives nt_builder
target_compile_options(tp_test_pack PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_pack)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_pack_test")
add_test(NAME tp_pack COMMAND tp_test_pack "${CMAKE_BINARY_DIR}/tp_pack_test")

# Phase 3 tp_project load/save: round-trip, sparse, determinism, version refusal,
# path relativization, to_settings bridge. Needs a scratch dir for project files.
add_executable(tp_test_project test_project.c)
target_link_libraries(tp_test_project PRIVATE tp_core unity)
target_include_directories(tp_test_project PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_test")
add_test(NAME tp_project COMMAND tp_test_project "${CMAKE_BINARY_DIR}/tp_project_test")

# F1-01 identity migration/promotion battery: v1->v2 migration byte-golden, stable
# read-only synthesis, writable promote (once/atomic/idempotent), save-failure +
# RNG-failure invariants, duplicate/malformed/nil id rejection, id survival across
# rename/reorder/remove, and forward-compat over the checked-in v1 CLI fixtures.
add_executable(tp_test_project_ids test_project_ids.c)
target_link_libraries(tp_test_project_ids PRIVATE tp_core unity)
target_include_directories(tp_test_project_ids PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project_ids PRIVATE -U_DLL)
target_compile_definitions(tp_test_project_ids PRIVATE
    TP_CLI_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/apps/cli/testdata")
nt_set_warning_flags(tp_test_project_ids)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_ids_test")
add_test(NAME tp_project_ids COMMAND tp_test_project_ids "${CMAKE_BINARY_DIR}/tp_project_ids_test")

# Phase 2 normalization unit tests: ext/folder strip, rename override, animation
# auto-group + explicit merge, collision, alias, final-name sort, scale. Pure
# tp_core (synthetic tp_results, no builder).
add_executable(tp_test_normalize test_normalize.c)
target_link_libraries(tp_test_normalize PRIVATE tp_core unity)
target_compile_options(tp_test_normalize PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_normalize)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_normalize COMMAND tp_test_normalize)

# Phase 2 json-neotolis golden fidelity + determinism: builds every fixture via
# nt_builder (tp_build), exports json-neotolis, parses back with cJSON.
add_executable(tp_test_export_json test_export_json.c tp_fixtures.c)
target_link_libraries(tp_test_export_json PRIVATE tp_build unity cjson)
target_compile_options(tp_test_export_json PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_export_json)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_json_test")
add_test(NAME tp_export_json COMMAND tp_test_export_json "${CMAKE_BINARY_DIR}/tp_export_json_test")

# Phase 2 PNG page export: pack -> write pages -> decode with stb_image, probe a
# pixel, prove straight-alpha default + premultiply toggle.
add_executable(tp_test_export_png test_export_png.c)
target_link_libraries(tp_test_export_png PRIVATE tp_build unity stb_image)
target_compile_options(tp_test_export_png PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_export_png)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_png_test")
add_test(NAME tp_export_png COMMAND tp_test_export_png "${CMAKE_BINARY_DIR}/tp_export_png_test")

# Phase 2 per-target packing: test-only capability-restricted descriptors prove
# effective-settings repack, shared runs, and metadata-loss notices.
add_executable(tp_test_export_run test_export_run.c)
target_link_libraries(tp_test_export_run PRIVATE tp_build unity cjson)
target_include_directories(tp_test_export_run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_export_run PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_export_run)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_export_run_test")
add_test(NAME tp_export_run COMMAND tp_test_export_run "${CMAKE_BINARY_DIR}/tp_export_run_test")

# A5 op-layer: tp_export_predict_loss consistency -- project-knowable axes from a
# NULL prep, pack-dependent axes (alias/multipage) with a prep, and every
# writer-emitted project-knowable notice field_id covered by predict. Links tp_build
# to pack + export the fixture through the real path (tp_export_run).
add_executable(tp_test_predict test_predict.c)
target_link_libraries(tp_test_predict PRIVATE tp_build unity)
target_include_directories(tp_test_predict PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_predict PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_predict)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_predict_test")
add_test(NAME tp_predict COMMAND tp_test_predict "${CMAKE_BINARY_DIR}/tp_predict_test")

# Phase 5 Defold exporter: golden .tpinfo/.tpatlas bytes, rotated-geometry,
# determinism over real fixtures, per-target identity repack + 9-slice notice,
# playback enum mapping, and a clean run over the real defold-demo atlases
# (basic/rotate/anim_trim). Builds fixtures via nt_builder (tp_build) and decodes
# the demo PNGs via stb_image.
add_executable(tp_test_export_defold test_export_defold.c tp_fixtures.c)
target_link_libraries(tp_test_export_defold PRIVATE tp_build unity stb_image)
target_include_directories(tp_test_export_defold PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_export_defold PRIVATE -U_DLL)
target_compile_definitions(tp_test_export_defold PRIVATE
    TP_DEFOLD_DEMO_DIR="${CMAKE_SOURCE_DIR}/examples/defold-demo")
nt_set_warning_flags(tp_test_export_defold)   # AGENTS.md invariant: our test TUs too
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
nt_set_warning_flags(tp_demo_export)   # AGENTS.md invariant: engine warning flags on all our targets

# M0 production-path baseline. The superseded component clone, full-snapshot-as-
# commit, and throwaway diff-spike executables were removed: this is the single
# authoritative core measurement path. Timing remains advisory; status, sample,
# fixture, allocation and byte accounting fail closed.
#   tp_bench_foundation [scratch_dir] [iterations] [recovery_ops]
#   tp_bench_foundation --project-load-scaling [iterations]
add_executable(tp_bench_foundation
    tp_bench_foundation.c
    tp_bench_project_load.c)
target_link_libraries(tp_bench_foundation PRIVATE tp_core nt_time cjson)
target_include_directories(tp_bench_foundation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_bench_foundation PRIVATE -U_DLL)
target_compile_definitions(tp_bench_foundation PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_bench_foundation)
