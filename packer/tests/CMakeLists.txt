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

# F1-02: source-key normalization / case-fold collision / portability validation
# against the production tp_status model and master spec §5.3.
add_executable(tp_test_srckey test_srckey.c)
target_link_libraries(tp_test_srckey PRIVATE tp_core unity)
target_compile_options(tp_test_srckey PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_srckey)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_srckey COMMAND tp_test_srckey)

add_executable(tp_test_source_path_text test_source_path_text.c)
target_link_libraries(tp_test_source_path_text PRIVATE tp_core unity)
target_include_directories(tp_test_source_path_text PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_source_path_text PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_source_path_text)
add_test(NAME tp_source_path_text COMMAND tp_test_source_path_text)

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

add_executable(tp_test_pack_constraints test_pack_constraints.c)
target_link_libraries(tp_test_pack_constraints PRIVATE tp_core unity)
target_include_directories(tp_test_pack_constraints PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_pack_constraints PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_pack_constraints)
add_test(NAME tp_pack_constraints COMMAND tp_test_pack_constraints)

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

# Owner refcount invariants must remain executable in Release, where libc
# assert() would compile away. The parent probe succeeds only when its child
# terminates on the production generation owner's invalid-release path.
add_executable(tp_test_project_generation_assert
    test_project_generation_assert.c)
target_link_libraries(tp_test_project_generation_assert PRIVATE tp_core)
target_include_directories(tp_test_project_generation_assert PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(tp_test_project_generation_assert PRIVATE NDEBUG)
target_compile_options(tp_test_project_generation_assert PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project_generation_assert)
add_test(NAME tp_project_generation_assert
    COMMAND tp_test_project_generation_assert)

# Save I/O propagation at the live owner boundary. This stays separate from the
# broad session battery so the identity/fingerprint/history outcome table is
# easy to review when persistence changes.
add_executable(tp_test_session_save_io_contract test_session_save_io_contract.c)
target_link_libraries(tp_test_session_save_io_contract PRIVATE tp_core unity)
target_include_directories(tp_test_session_save_io_contract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_session_save_io_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_session_save_io_contract)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_session_save_io_contract_test")
add_test(NAME tp_session_save_io_contract COMMAND
    tp_test_session_save_io_contract
    "${CMAKE_BINARY_DIR}/tp_session_save_io_contract_test")

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

# Independent ordered validation oracle over a checked-in, platform-neutral
# multi-atlas corpus. Keep this separate from rule-level and filesystem cases.
add_executable(tp_test_validate_contract test_validate_contract.c)
target_link_libraries(tp_test_validate_contract PRIVATE tp_core unity)
target_compile_definitions(tp_test_validate_contract PRIVATE
    TP_CLI_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/apps/cli/testdata"
    TP_VALIDATE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_validate_contract_test"
    _CRT_SECURE_NO_WARNINGS)
target_compile_options(tp_test_validate_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_validate_contract)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_validate_contract_test")
add_test(NAME tp_validate_contract COMMAND tp_test_validate_contract)

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
# inverse, reference-integrity rejection for source.remove, inverse-apply alloc-failure rollback (sweep), capture-alloc atomicity,
# redo + redo-branch discard, and corrupted-diff structured errors. Includes the
# private src/ internal header for the diff alloc seam + record/history internals.
add_executable(tp_test_diff test_diff.c)
target_link_libraries(tp_test_diff PRIVATE tp_core unity)
target_include_directories(tp_test_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_diff PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_diff)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_diff COMMAND tp_test_diff)

# P1-09: compact independent contract for semantic Undo/Redo, exact history
# accounting, Redo-branch discard, and journal-failure non-publication at the
# session event boundary.
add_executable(tp_test_history_semantics_contract
    test_history_semantics_contract.c)
target_link_libraries(tp_test_history_semantics_contract PRIVATE tp_core unity)
target_include_directories(tp_test_history_semantics_contract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_history_semantics_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_history_semantics_contract)
add_test(NAME tp_history_semantics_contract
    COMMAND tp_test_history_semantics_contract)

add_executable(tp_test_history_codec_contract test_history_codec_contract.c)
target_link_libraries(tp_test_history_codec_contract PRIVATE tp_core unity)
target_include_directories(tp_test_history_codec_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_history_codec_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_history_codec_contract)
add_test(NAME tp_history_codec_contract COMMAND tp_test_history_codec_contract
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/history_v1_forward.hex"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/history_v1_reverse.hex"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/history_v1_collections_forward.hex"
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/history_v1_collections_reverse.hex")

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

add_executable(tp_test_journal_wire_contract test_journal_wire_contract.c)
target_link_libraries(tp_test_journal_wire_contract PRIVATE tp_core unity)
target_include_directories(tp_test_journal_wire_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_journal_wire_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_journal_wire_contract)
add_test(NAME tp_journal_wire_contract COMMAND tp_test_journal_wire_contract
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/journal_v4_mixed.hex")

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

# F1-00 project identity: LEXICAL canonical-path fixtures.
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

# P1-11 missing recovery oracle: ranking must not inherit filesystem directory
# enumeration order, including equal timestamps at the bounded candidate cap.
add_executable(tp_test_recovery_ranking_contract
    test_recovery_ranking_contract.c)
target_link_libraries(tp_test_recovery_ranking_contract PRIVATE tp_core unity)
target_include_directories(tp_test_recovery_ranking_contract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_recovery_ranking_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_recovery_ranking_contract)
add_test(NAME tp_recovery_ranking_contract
    COMMAND tp_test_recovery_ranking_contract)

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

# P1-06: exact Win32 filesystem conversion boundaries live separately from the
# cross-platform scan suite so platform backend moves retain an independent pin.
if(WIN32)
    add_executable(tp_test_fs_win32_contract test_fs_win32_contract.c)
    target_link_libraries(tp_test_fs_win32_contract PRIVATE tp_core unity)
    target_include_directories(tp_test_fs_win32_contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(tp_test_fs_win32_contract PRIVATE
        _CRT_SECURE_NO_WARNINGS)
    target_compile_options(tp_test_fs_win32_contract PRIVATE -U_DLL)
    nt_set_warning_flags(tp_test_fs_win32_contract)
    add_test(NAME tp_fs_win32_contract COMMAND tp_test_fs_win32_contract)
endif()

if(NOT WIN32)
    add_executable(tp_test_project_save_posix_contract
        test_project_save_posix_contract.c)
    target_link_libraries(tp_test_project_save_posix_contract PRIVATE tp_core unity)
    target_include_directories(tp_test_project_save_posix_contract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(tp_test_project_save_posix_contract PRIVATE
        TP_PROJECT_SAVE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_project_save_posix_test")
    target_compile_options(tp_test_project_save_posix_contract PRIVATE -U_DLL)
    nt_set_warning_flags(tp_test_project_save_posix_contract)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_save_posix_test")
    add_test(NAME tp_project_save_posix_contract
        COMMAND tp_test_project_save_posix_contract)
endif()

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
target_include_directories(tp_test_pack PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_pack PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_pack)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_pack_test")
add_test(NAME tp_pack COMMAND tp_test_pack "${CMAKE_BINARY_DIR}/tp_pack_test")

# H0.3-a: versioned build-worker protocol codec (decision 0018). Encode/decode
# round-trip across representative inputs plus the fail-closed matrix (truncated,
# oversized declared length, wrong magic, unsupported version, corrupt payload).
add_executable(tp_test_build_proto test_build_proto.c)
target_link_libraries(tp_test_build_proto PRIVATE tp_build unity)  # tp_build owns the worker/protocol seam
target_include_directories(tp_test_build_proto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_build_proto PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_build_proto)   # AGENTS.md invariant: our test TUs too
add_test(NAME tp_build_proto COMMAND tp_test_build_proto)

# H0.3-a: byte-identical oracle. The extracted driver (and tp_pack) produce a
# .ntpack identical to a direct nt_builder invocation for the same inputs; the
# permanent normal-result regression for the worker cutover. Needs a scratch dir.
add_executable(tp_test_build_driver_oracle test_build_driver_oracle.c)
target_link_libraries(tp_test_build_driver_oracle PRIVATE tp_build unity)  # tp_build: driver + direct nt_builder
target_include_directories(tp_test_build_driver_oracle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_build_driver_oracle PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_build_driver_oracle)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_build_driver_oracle_test")
add_test(NAME tp_build_driver_oracle COMMAND tp_test_build_driver_oracle "${CMAKE_BINARY_DIR}/tp_build_driver_oracle_test")

# H0.3-b: private build-worker process boundary (decision 0018). Fault-injecting
# worker binaries -- one per mode via a test-only compile def, the same pattern as
# the CLI NTPACKER_CLI_*_FAULT_SEAM binaries (production exes have no such mode) --
# drive the crash / malformed-reply / non-zero-exit outcome mapping. The test exe
# is BOTH the parent and the normal worker child (its main dispatches __build-worker).
# HANG (H0.4) joins the fault set: drains stdin then never replies/exits, so the
# cancel + safety-timeout paths can tree-kill it and clean up staging. H0.5 adds
# NOWRITE (full-disk/sink write failure: partial staging file + BUILDER_FAILED),
# OKNOART (exit 0 + valid OK reply + missing artifact -> BUILDER_CRASHED), and
# BIGLEN/TRUNC (giant declared / short truncated reply frame -> BUILDER_FAILED),
# proving the PARENT path fails closed through the process (codec-level is #43).
foreach(_mode CRASH MALFORMED NONZERO HANG NOWRITE OKNOART BIGLEN TRUNC)
    string(TOLOWER "${_mode}" _lc)
    add_executable(tp_build_worker_${_lc} tp_build_worker_fault.c)
    target_link_libraries(tp_build_worker_${_lc} PRIVATE tp_build)
    target_include_directories(tp_build_worker_${_lc} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    target_compile_definitions(tp_build_worker_${_lc} PRIVATE TP_WORKER_FAULT_${_mode}=1)
    target_compile_options(tp_build_worker_${_lc} PRIVATE -U_DLL)
    nt_set_warning_flags(tp_build_worker_${_lc})   # AGENTS.md invariant: our test TUs too
endforeach()

add_executable(tp_test_build_worker test_build_worker.c)
target_link_libraries(tp_test_build_worker PRIVATE tp_build unity)  # tp_build owns the worker boundary + nt_builder oracle
target_include_directories(tp_test_build_worker PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_build_worker PRIVATE -U_DLL)
# Compile-def (NOT env): the timeout test drives the worker with a tiny safety
# timeout so it can assert a builder_crashed timeout without a 5-minute wait.
target_compile_definitions(tp_test_build_worker PRIVATE TP_TEST_WORKER_TIMEOUT_MS=400)
nt_set_warning_flags(tp_test_build_worker)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_build_worker_test")
add_test(NAME tp_build_worker COMMAND tp_test_build_worker
    "${CMAKE_BINARY_DIR}/tp_build_worker_test"
    $<TARGET_FILE:tp_build_worker_crash>
    $<TARGET_FILE:tp_build_worker_malformed>
    $<TARGET_FILE:tp_build_worker_nonzero>
    $<TARGET_FILE:tp_build_worker_hang>
    $<TARGET_FILE:tp_build_worker_nowrite>
    $<TARGET_FILE:tp_build_worker_oknoart>
    $<TARGET_FILE:tp_build_worker_biglen>
    $<TARGET_FILE:tp_build_worker_trunc>)
# A regression that failed to kill the hang worker must not wedge CI for the job's
# full 6h budget: bound the whole worker suite well above its sub-second normal run
# (the timeout test overrides the 5-min safety timeout down to 400ms).
set_tests_properties(tp_build_worker PROPERTIES TIMEOUT 120)

# Shared strict-UTF-8 raster ingress: memory decode, bounded resources and
# explicit allocator ownership.  The scratch tree includes a Cyrillic filename.
add_executable(tp_test_image test_image.c)
target_link_libraries(tp_test_image PRIVATE tp_core unity)
target_include_directories(tp_test_image PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_image PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_image)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_image_test")
add_test(NAME tp_image COMMAND tp_test_image "${CMAKE_BINARY_DIR}/tp_image_test")

# Phase 3 tp_project load/save: round-trip, sparse, determinism, version refusal,
# path relativization, to_settings bridge. Needs a scratch dir for project files.
add_executable(tp_test_project test_project.c)
target_link_libraries(tp_test_project PRIVATE tp_core unity)
target_include_directories(tp_test_project PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_test")
add_test(NAME tp_project COMMAND tp_test_project "${CMAKE_BINARY_DIR}/tp_project_test")

# Canonical-only schema v5 gate: exact version admission and save-time
# canonicality are independent of the broader project round-trip battery.
add_executable(tp_test_schema_v5 test_schema_v5.c)
target_link_libraries(tp_test_schema_v5 PRIVATE tp_core unity)
target_include_directories(tp_test_schema_v5 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_schema_v5 PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_schema_v5)
add_test(NAME tp_schema_v5 COMMAND tp_test_schema_v5)

# Canonical structural identity: atomic/idempotent assignment and exact loaded IDs.
add_executable(tp_test_project_ids test_project_ids.c)
target_link_libraries(tp_test_project_ids PRIVATE tp_core unity)
target_include_directories(tp_test_project_ids PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project_ids PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project_ids)   # AGENTS.md invariant: our test TUs too
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_project_ids_test")
add_test(NAME tp_project_ids COMMAND tp_test_project_ids "${CMAKE_BINARY_DIR}/tp_project_ids_test")

# Independent canonical-v5 bytes/model oracles. This deliberately does not use
# writer->loader round-trip as its assertion boundary.
add_executable(tp_test_project_contract test_project_contract.c)
target_link_libraries(tp_test_project_contract PRIVATE tp_core unity)
target_include_directories(tp_test_project_contract PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_project_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_project_contract)
add_test(NAME tp_project_contract COMMAND tp_test_project_contract
    "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_v5_rich.golden")

# Exact pre-publication Save outcome matrix: typed phase/path/native cause,
# unchanged destination, and caller-controlled retry for every I/O phase.
add_executable(tp_test_save_io_contract test_save_io_contract.c)
target_link_libraries(tp_test_save_io_contract PRIVATE tp_core unity)
target_include_directories(tp_test_save_io_contract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_save_io_contract PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_save_io_contract)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_save_io_contract_test")
add_test(NAME tp_save_io_contract COMMAND tp_test_save_io_contract
    "${CMAKE_BINARY_DIR}/tp_save_io_contract_test")

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

# Raw-RGBA ownership regression. Drives nt_builder_atlas_add_raw
# directly (links tp_build), mutates+frees the caller buffer immediately after the
# add, packs, and asserts the packed page is unaffected -> the builder deep-copies
# (nt_builder_atlas.c:771-775). Pins a lifetime the public header does not promise
# (see docs/research/neotolis-engine-raw-rgba-lifetime.md). Needs a scratch dir.
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
#   tp_bench_foundation --project <project> [scratch_dir] [iterations] [recovery_ops]
#   tp_bench_foundation --project-load-scaling [iterations]
#   tp_bench_foundation --batch-scaling <project> [iterations]
add_executable(tp_bench_foundation
    tp_bench_foundation.c
    tp_bench_project_load.c)
target_link_libraries(tp_bench_foundation PRIVATE tp_core nt_time cjson)
target_include_directories(tp_bench_foundation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_bench_foundation PRIVATE -U_DLL)
target_compile_definitions(tp_bench_foundation PRIVATE _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_bench_foundation)

# Deterministic checked-in large project: 100 atlases x 10 file sources. The
# generator uses the core's white-box model helpers; the ctest comes back
# through the public snapshot/load boundary and proves committed bytes,
# structural counts, source tags, and relative asset paths.
add_executable(tp_generate_large_project tp_generate_large_project.c)
target_link_libraries(tp_generate_large_project PRIVATE tp_core)
target_include_directories(tp_generate_large_project PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_generate_large_project PRIVATE -U_DLL)
target_compile_definitions(tp_generate_large_project PRIVATE
    _CRT_SECURE_NO_WARNINGS)
nt_set_warning_flags(tp_generate_large_project)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_large_project_contract")
add_test(NAME tp_large_project_contract
    COMMAND tp_generate_large_project --check
        "${CMAKE_SOURCE_DIR}/examples/projects/large-synthetic.ntpacker_project"
        "${CMAKE_BINARY_DIR}/tp_large_project_contract")

# Tests are executable boundaries, so sanitizer instrumentation must be linked
# on each one rather than only on tp_core/tp_build. Keep this directory-wide
# gate at the end so newly added first-party tests cannot silently miss it.
get_property(_tp_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_tp_test_target IN LISTS _tp_test_targets)
    nt_set_sanitizer_flags(${_tp_test_target})
endforeach()
