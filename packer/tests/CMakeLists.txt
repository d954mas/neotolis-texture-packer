tp_add_test(NAME arena LIBS tp_core unity)

tp_add_test(NAME name_map LIBS tp_core unity PRIVATE_SRC)

# B1: tp_status_id() stable machine tokens (--json error-id contract), pinned per
# enum value so a rename can't slip through without failing here.
tp_add_test(NAME status_id LIBS tp_core unity)

# F1-00 project identity: 128-bit runtime ID + injectable RNG seam (deterministic).
# PRIVATE_SRC: tp_hex.h (shared drift-guard encoder).
tp_add_test(NAME identity_id LIBS tp_core unity PRIVATE_SRC)

# F1-01 Part A: promoted structural-ID primitive surface (shape-ID parse/format,
# versioned stable hash + sprite_id byte-goldens, production tp_status mapping).
tp_add_test(NAME id LIBS tp_core unity PRIVATE_SRC)

# F1-02: source-key normalization / case-fold collision / portability validation
# against the production tp_status and source-key contracts.
tp_add_test(NAME srckey LIBS tp_core unity)

tp_add_test(NAME source_path_text LIBS tp_core unity PRIVATE_SRC)

# F1-03: resolved sprite index (scan -> source-local key -> sprite_id) + lookups.
# Needs a scratch dir (argv[1]) for the on-disk scan fixtures.
tp_add_test(NAME sprite_index LIBS tp_core unity PRIVATE_SRC
    ARGS "${CMAKE_BINARY_DIR}/tp_sprite_index_test")

# F2-01: typed operation engine -- catalog + closed field vocabulary, canonical
# byte-stable encoder goldens, and payload/reference validation (invalid id/type/
# range/reference each -> the right structured status).
tp_add_test(NAME operation LIBS tp_core unity PRIVATE_SRC)

tp_add_test(NAME pack_constraints LIBS tp_core unity PRIVATE_SRC)

# F2-01: id-only apply (forward + error per op kind), stage-then-commit allocator-
# fault safety, selector->operation builders, and the parity proof (op apply ==
# existing mutator, byte-identical). Includes the private src/ internal header for
# the staging-alloc fault seam (same pattern as test_id.c including tp_hex.h).
tp_add_test(NAME op_apply LIBS tp_core unity PRIVATE_SRC)

# F2-02: atomic transactions, revision, semantic dirty, idempotency, and the
# versioned transaction request/result JSON contract. Deep-clone byte-identity +
# OOM-safe fault sweep, atomicity (op N fails / clone-alloc / per-op-alloc leave the
# model byte-unchanged), expected_revision below/above/equal, idempotent retry,
# dirty=identity (edit->save->edit->inverse->clean at higher revision), validation
# ordering, the CLI batch JSON golden + batch==one-by-one property, and UB-free
# numbers. Includes the private src/ internal headers for the clone + op alloc-fault
# seams (same pattern as test_op_apply.c).
# The clone allocation-fault seam and the serialization work probes are compiled
# out of the shipped library, so their TUs are recompiled here with
# TP_ENABLE_TEST_SEAMS.
tp_add_test(NAME transaction
    SOURCES test_transaction.c ../src/tp_project_clone.c ../src/tp_project_write.c
    LIBS tp_core unity PRIVATE_SRC SEAMS)

# Architecture foundation M1: the synchronous session is the only live model
# owner; snapshots are independently owned and rejected commits publish nothing.
# tinycthread: the owner-thread contract test drives a whole session lifecycle
# on a spawned owner, proving the owner is the creating thread.
# The snapshot accounting/fault seams, the generation-owner allocation fault, the
# save-I/O faults, and the clone allocation fault are all compiled out of the
# shipped library, so their TUs are recompiled here with TP_ENABLE_TEST_SEAMS.
tp_add_test(NAME session
    SOURCES test_session.c ../src/tp_session_snapshot.c
        ../src/tp_project_generation.c ../src/tp_project_save.c
        ../src/tp_project_clone.c
    LIBS tp_core tinycthread unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_session_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_session_test")

# Owner refcount invariants must remain executable in Release, where libc
# assert() would compile away. The parent probe succeeds only when its child
# terminates on the production generation owner's invalid-release path.
tp_add_test(NAME project_generation_assert LIBS tp_core PRIVATE_SRC
    DEFINES NDEBUG)

# Save I/O propagation at the live owner boundary. This stays separate from the
# broad session battery so the identity/fingerprint/history outcome table is
# easy to review when persistence changes.
tp_add_test(NAME session_save_io_contract
    SOURCES test_session_save_io_contract.c ../src/tp_project_save.c
    LIBS tp_core unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_session_save_io_contract_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_session_save_io_contract_test")

# Owner refcount/detach coverage adopts a synthetic job through
# tp_session_job_attach_internal, a seam compiled out of the shipped library
# (nothing in production adopts a job it did not start), so tp_session.c is
# recompiled here with TP_ENABLE_TEST_SEAMS.
tp_add_test(NAME job_owner
    SOURCES test_job_owner.c ../src/tp_session.c
    LIBS tp_core tinycthread unity PRIVATE_SRC SEAMS)

# Architecture foundation job freshness: a result captures both immutable model
# and source-runtime generations. Generation zero is data, never a stale sentinel.
# The job transport's seams are compiled out of the shipped library, so the whole
# worker TU chain is rebuilt here with TP_ENABLE_TEST_SEAMS: tp_job.c needs the
# host-side worker timeout override, and tp_job_worker_main.c carries the
# TP_TEST_JOB_WORKER_BLOCK_MS block that makes cancel linearization deterministic.
# tp_build: F3-03 T2 extension calls tp_session_pack_input_hash (job/pack surface).
# PRIVATE_SRC: single-decode seam (tp_image_internal.h) + scratch teardown
# (tp_fs_internal.h).
tp_add_test(NAME job_input_token
    SOURCES test_job_input_token.c
        ../src/tp_scan.c ../src/tp_export_run.c ../src/tp_job.c
        ../src/tp_job_worker.c ../src/tp_job_worker_proto.c
        ../src/tp_job_worker_main.c
    LIBS tp_build unity PRIVATE_SRC SEAMS
    DEFINES _CRT_SECURE_NO_WARNINGS)

# M5 thin-boundary validation: all project validation rules are core-owned and
# exposed as an owned typed report over a file-oriented entry point.
tp_add_test(NAME validate LIBS tp_core unity PRIVATE_SRC
    DEFINES
        TP_CLI_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/apps/cli/testdata"
        TP_VALIDATE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_validate_test"
        _CRT_SECURE_NO_WARNINGS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_validate_test")

# Independent ordered validation oracle over a checked-in, platform-neutral
# multi-atlas corpus. Keep this separate from rule-level and filesystem cases.
tp_add_test(NAME validate_contract LIBS tp_core unity
    DEFINES
        TP_CLI_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/apps/cli/testdata"
        TP_VALIDATE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_validate_contract_test"
        _CRT_SECURE_NO_WARNINGS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_validate_contract_test")

# Validation resource/complexity gate: a real saved project with 1k resolved
# sprites, overrides, and frames. Elapsed time is evidence; deterministic probe
# growth is the hard cross-platform contract.
tp_add_test(NAME validate_large TARGET tp_bench_validate
    SOURCES tp_bench_validate.c
    LIBS tp_core nt_time PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_bench_validate"
    ARGS "${CMAKE_BINARY_DIR}/tp_bench_validate")

# F2-03: semantic diff / exact inverse (Undo) + redo replay + in-memory history.
# The ORACLE suite: A->forward->B->inverse->A' byte-identical for EVERY op kind (and
# == the legacy full-snapshot restore), reference cascade, a 100-anim batch as one
# inverse, reference-integrity rejection for source.remove, inverse-apply alloc-failure rollback (sweep), capture-alloc atomicity,
# redo + redo-branch discard, and corrupted-diff structured errors. Includes the
# private src/ internal header for the diff alloc seam + record/history internals.
tp_add_test(NAME diff LIBS tp_core unity PRIVATE_SRC)

# P1-09: compact independent contract for semantic Undo/Redo, exact history
# accounting, Redo-branch discard, and journal-failure non-publication at the
# session event boundary.
tp_add_test(NAME history_semantics_contract LIBS tp_core unity PRIVATE_SRC)

tp_add_test(NAME history_codec_contract LIBS tp_core unity PRIVATE_SRC
    ARGS
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
tp_add_test(NAME journal
    SOURCES test_journal.c ../src/tp_project_clone.c ../src/tp_project_write.c
    LIBS tp_core unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_journal_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_journal_test")

tp_add_test(NAME journal_wire_contract LIBS tp_core unity PRIVATE_SRC
    ARGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/journal_v4_mixed.hex")

# M0 benchmark measurement contract: deterministic percentile math and the
# fail-closed rule that a rejected operation is never accepted as a timing sample.
tp_add_test(NAME bench_support LIBS nt_time unity)

# F1-03: canonical selector resolver (id/name/path/compound -> exactly one id).
tp_add_test(NAME selector LIBS tp_core unity PRIVATE_SRC
    ARGS "${CMAKE_BINARY_DIR}/tp_selector_test")

# F1-00 project identity: LEXICAL canonical-path fixtures.
# Includes the src-only internal header so BOTH host rule sets run on every CI OS.
tp_add_test(NAME identity_path LIBS tp_core unity PRIVATE_SRC)

tp_add_test(NAME file_fingerprint LIBS tp_core unity
    DEFINES _CRT_SECURE_NO_WARNINGS)

# F1-00 project identity: FILESYSTEM realpath/symlink resolution + atomic Save-As
# transition. Uses a created scratch dir (argv[1]); no baked absolute paths.
# PRIVATE_SRC: tp_hex.h (shared drift-guard encoder).
tp_add_test(NAME identity_session LIBS tp_core unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_identity_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_identity_test")

# Architecture foundation M4/A1: OS-backed exclusive canonical project lease.
# The executable re-enters itself as a child process to prove cross-process busy
# detection while keeping the test independent from GUI/session/recovery code.
tp_add_test(NAME project_lease LIBS tp_core unity PRIVATE_SRC
    ARGS "${CMAKE_BINARY_DIR}/tp_project_lease_test" "$<TARGET_FILE:ntpacker>")

tp_add_test(NAME recovery_store LIBS tp_core unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_recovery_store_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_recovery_store_test")

# P1-11 missing recovery oracle: ranking must not inherit filesystem directory
# enumeration order, including equal timestamps at the bounded candidate cap.
tp_add_test(NAME recovery_ranking_contract LIBS tp_core unity PRIVATE_SRC)

# A1 op-layer: canonical sprite-name key + natural order + common prefix.
# Pins the dot-basename key fix that the showcase draw-hash gate cannot see.
tp_add_test(NAME names LIBS tp_core unity)

# A2 op-layer: tp_scan directory recursion + image-ext whitelist, moved out of the
# GUI. Builds a fixture tree under a scratch dir at runtime (argv[1]).
tp_add_test(NAME scan
    SOURCES test_scan.c ../src/tp_scan.c
    LIBS tp_core unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_scan_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_scan_test")

# P1-06: exact Win32 filesystem conversion boundaries live separately from the
# cross-platform scan suite so platform backend moves retain an independent pin.
if(WIN32)
    tp_add_test(NAME fs_win32_contract LIBS tp_core unity PRIVATE_SRC
        DEFINES _CRT_SECURE_NO_WARNINGS)
endif()

if(NOT WIN32)
    tp_add_test(NAME project_save_posix_contract LIBS tp_core unity PRIVATE_SRC
        DEFINES
            TP_PROJECT_SAVE_TEST_DIR="${CMAKE_BINARY_DIR}/tp_project_save_posix_test"
        MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_project_save_posix_test")
endif()

# A3a op-layer: tp_input project->pack-input build (assemble/desc_add moved out of
# the GUI), the per-sprite effective-shape rule, and the non-RECT extrude clamp in
# tp_project_atlas_to_settings. Links tp_build so the CONCAVE+extrude clamp
# regression can pack + export (tp_pack/tp_export_run); the pure input-build asserts
# only need tp_core. Builds a fixture tree under a scratch dir at runtime (argv[1]).
tp_add_test(NAME input
    SOURCES test_input.c ../src/tp_scan.c
    LIBS tp_build unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_input_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_input_test")

# Synthetic D4 transform-decode test. Includes the private helper
# header from src/ directly -- no public API pollution.
tp_add_test(NAME transform_decode LIBS tp_core unity PRIVATE_SRC)

# Fixture-driven golden round-trip: builds every fixture case (tp_fixtures) via
# nt_builder (transitive through tp_core) then parses it back. Needs a scratch
# dir for the temp .ntpack files. tp_build: fixtures pack via nt_builder.
tp_add_test(NAME pack_read
    SOURCES test_pack_read.c tp_fixtures.c
    LIBS tp_build unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_test")

# UV recovery property test: pure functions, no builder. Includes
# the private helper header from src/ directly.
tp_add_test(NAME uv_property LIBS tp_core unity PRIVATE_SRC)

# tp_pack invariants + determinism: drives nt_builder
# (transitive through tp_core) then parses back. Needs a scratch dir for the
# session .ntpack files. tp_build: drives nt_builder.
tp_add_test(NAME pack LIBS tp_build unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_pack_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_pack_test")

# F3-03 T1: canonical semantic pack_input_hash + per-file image-hash cache.
# Pure tp_core; links stb_image_write to lay down a real PNG for the
# identical-pixels-changed-mtime case. Needs a scratch dir (argv[1]).
tp_add_test(NAME pack_input_hash LIBS tp_core unity stb_image_write
    INCLUDE_DIRS
        $<TARGET_PROPERTY:stb_image_write,INTERFACE_INCLUDE_DIRECTORIES>
        "${CMAKE_SOURCE_DIR}/packer/src"
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_pack_input_hash_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_pack_input_hash_test")

# F3-03 T4/T5/T6 + S18 + S28: memory-only pack-result cache (byte-budget LRU +
# selection + the compressed cold tier) and the Undo/Redo cache probe. Links
# tp_build so one case can pack a real atlas through tp_pack (the binary is also
# its own build worker, dispatched in main). Needs a scratch dir (argv[1]).
# The damaged-blob containment path has no production route -- the store writes
# and reads its own blobs in one process -- so tp_pack_result_cache.c is
# recompiled here with TP_ENABLE_TEST_SEAMS (same pattern as
# tp_test_session_observation recompiling tp_session_observation.c).
tp_add_test(NAME pack_result_cache
    SOURCES test_pack_result_cache.c ../src/tp_pack_result_cache.c
    LIBS tp_build unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_pack_result_cache_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_pack_result_cache_test")

# Versioned build-worker protocol codec. Encode/decode
# round-trip across representative inputs plus the fail-closed matrix (truncated,
# oversized declared length, wrong magic, unsupported version, corrupt payload).
# tp_build owns the worker/protocol seam.
tp_add_test(NAME build_proto LIBS tp_build unity PRIVATE_SRC)

# H0.3-a: byte-identical oracle. The extracted driver (and tp_pack) produce a
# .ntpack identical to a direct nt_builder invocation for the same inputs; the
# permanent normal-result regression for the worker cutover. Needs a scratch dir.
tp_add_test(NAME build_driver_oracle LIBS tp_build unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_build_driver_oracle_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_build_driver_oracle_test")

# Private build-worker process boundary. Fault-injecting
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
foreach(_mode CRASH MALFORMED NONZERO HANG NOWRITE OKNOART BIGLEN TRUNC FLOOD)
    string(TOLOWER "${_mode}" _lc)
    tp_add_test(NAME build_worker_${_lc} TARGET tp_build_worker_${_lc}
        SOURCES tp_build_worker_fault.c
        LIBS tp_build PRIVATE_SRC
        DEFINES TP_WORKER_FAULT_${_mode}=1
        NO_CTEST)
endforeach()

# tp_build owns the worker boundary + nt_builder oracle. The TP_TEST_WORKER_TIMEOUT_MS
# compile-def (NOT env) drives the worker with a tiny safety timeout so the test can
# assert a builder_crashed timeout without a 5-minute wait.
# A regression that failed to kill the hang worker must not wedge CI for the job's
# full 6h budget: TIMEOUT bounds the whole worker suite well above its sub-second
# normal run.
# The fault-injection controls (explicit worker exe, tiny timeout, lowered reply
# cap) are compiled out of the shipped library, so tp_build_worker.c is recompiled
# here with TP_ENABLE_TEST_SEAMS.
tp_add_test(NAME build_worker
    SOURCES test_build_worker.c ../src/tp_build_worker.c
    LIBS tp_build unity PRIVATE_SRC SEAMS
    DEFINES TP_TEST_WORKER_TIMEOUT_MS=400
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_build_worker_test"
    ARGS
        "${CMAKE_BINARY_DIR}/tp_build_worker_test"
        $<TARGET_FILE:tp_build_worker_crash>
        $<TARGET_FILE:tp_build_worker_malformed>
        $<TARGET_FILE:tp_build_worker_nonzero>
        $<TARGET_FILE:tp_build_worker_hang>
        $<TARGET_FILE:tp_build_worker_nowrite>
        $<TARGET_FILE:tp_build_worker_oknoart>
        $<TARGET_FILE:tp_build_worker_biglen>
        $<TARGET_FILE:tp_build_worker_trunc>
        $<TARGET_FILE:tp_build_worker_flood>
    TIMEOUT 120)

# Shared strict-UTF-8 raster ingress: memory decode, bounded resources and
# explicit allocator ownership.  The scratch tree includes a Cyrillic filename.
tp_add_test(NAME image LIBS tp_core unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_image_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_image_test")

# Phase 3 tp_project load/save: round-trip, sparse, determinism, version refusal,
# path relativization, to_settings bridge. Needs a scratch dir for project files.
# Save-I/O faults, the writer size cap, the serialization/load work probes, and
# the JSON-limit writer override are all compiled out of the shipped library, so
# their four TUs are recompiled here with TP_ENABLE_TEST_SEAMS.
# cjson: tp_project_parse.c parses through cJSON (header path only here).
tp_add_test(NAME project
    SOURCES test_project.c ../src/tp_project_save.c ../src/tp_project_write.c
        ../src/tp_project_parse.c ../src/tp_project_identity.c
    LIBS tp_core unity cjson PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_project_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_project_test")

# Canonical-only schema v5 gate: exact version admission and save-time
# canonicality are independent of the broader project round-trip battery.
tp_add_test(NAME schema_v5 LIBS tp_core unity PRIVATE_SRC)

# Canonical structural identity: atomic/idempotent assignment and exact loaded IDs.
tp_add_test(NAME project_ids LIBS tp_core unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_project_ids_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_project_ids_test")

# Independent canonical-v5 bytes/model oracles. This deliberately does not use
# writer->loader round-trip as its assertion boundary.
tp_add_test(NAME project_contract LIBS tp_core unity PRIVATE_SRC
    ARGS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/project_v5_rich.golden")

# Exact pre-publication Save outcome matrix: typed phase/path/native cause,
# unchanged destination, and caller-controlled retry for every I/O phase.
tp_add_test(NAME save_io_contract
    SOURCES test_save_io_contract.c ../src/tp_project_save.c
    LIBS tp_core unity PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_save_io_contract_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_save_io_contract_test")

# Phase 2 normalization unit tests: ext/folder strip, rename override, animation
# auto-group + explicit merge, collision, alias, final-name sort, scale. Pure
# tp_core (synthetic tp_results, no builder).
tp_add_test(NAME normalize LIBS tp_core unity)

# Phase 2 json-neotolis golden fidelity + determinism: builds every fixture via
# nt_builder (tp_build), exports json-neotolis, parses back with cJSON.
tp_add_test(NAME export_json
    SOURCES test_export_json.c tp_fixtures.c
    LIBS tp_build unity cjson PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_export_json_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_export_json_test")

# Phase 2 PNG page export: pack -> write pages -> decode with stb_image, probe a
# pixel, prove straight-alpha default + premultiply toggle.
tp_add_test(NAME export_png LIBS tp_build unity stb_image
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_export_png_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_export_png_test")

# Export output-SET publication: declared-list preflight (outside-directory and
# case-insensitive duplicate rejections before the writer runs), unlisted/missing
# output verdicts before the first rename, two-phase-swap rollback, and the
# dead-owner reaper including crash-mid-swap recovery. Drives
# tp_export_write_and_publish_set directly over a fixture exporter (no pack).
# The rename fault seam is compiled out of the shipped library, so tp_export_png.c
# is rebuilt here with TP_ENABLE_TEST_SEAMS (same pattern as tp_test_export_run
# recompiling tp_export_run.c); that TU carries the PNG page writer, hence the
# stb_image_write include path and link.
tp_add_test(NAME export_set_publish
    SOURCES test_export_set_publish.c ../src/tp_export_png.c
    LIBS tp_core unity stb_image_write PRIVATE_SRC SEAMS
    INCLUDE_DIRS
        $<TARGET_PROPERTY:stb_image_write,INTERFACE_INCLUDE_DIRECTORIES>
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_export_set_publish_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_export_set_publish_test")

# Phase 2 per-target packing: test-only capability-restricted descriptors prove
# effective-settings repack, shared runs, and metadata-loss notices.
tp_add_test(NAME export_run
    SOURCES test_export_run.c ../src/tp_export_run.c
    LIBS tp_build unity cjson PRIVATE_SRC SEAMS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_export_run_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_export_run_test")

# A5 op-layer: tp_export_predict_loss consistency -- project-knowable axes from a
# NULL prep, pack-dependent axes (alias/multipage) with a prep, and every
# writer-emitted project-knowable notice field_id covered by predict. Links tp_build
# to pack + export the fixture through the real path (tp_export_run).
tp_add_test(NAME predict LIBS tp_build unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_predict_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_predict_test")

# Phase 5 Defold exporter: golden .tpinfo/.tpatlas bytes, rotated-geometry,
# determinism over real fixtures, per-target identity repack + 9-slice notice,
# playback enum mapping, and a clean run over the real defold-demo atlases
# (basic/rotate/anim_trim). Builds fixtures via nt_builder (tp_build) and decodes
# the demo PNGs via stb_image.
tp_add_test(NAME export_defold
    SOURCES test_export_defold.c tp_fixtures.c
    LIBS tp_build unity stb_image PRIVATE_SRC
    DEFINES TP_DEFOLD_DEMO_DIR="${CMAKE_SOURCE_DIR}/examples/defold-demo"
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_export_defold_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_export_defold_test"
         "${CMAKE_SOURCE_DIR}/examples/defold-demo")

# Raw-RGBA ownership regression. Drives nt_builder_atlas_add_raw
# directly (links tp_build), mutates+frees the caller buffer immediately after the
# add, packs, and asserts the packed page is unaffected -> the builder deep-copies
# (nt_builder_atlas.c:771-775). Pins a lifetime the public header does not promise
# (see docs/architecture/sources-and-raster.md). Needs a scratch dir.
tp_add_test(NAME raw_ownership LIBS tp_build unity PRIVATE_SRC
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_raw_ownership_test"
    ARGS "${CMAKE_BINARY_DIR}/tp_raw_ownership_test")

# Dev driver / smoke tool (NOT a ctest): load a `.ntpacker_project`, assemble each
# atlas's sprites through the shared op layer (tp_pack_input_build), and run every
# enabled target -> the project's out_paths. Regenerates the Defold demo's ntpacker
# exports deterministically and serves as the CLI parity oracle (byte-identical to
# `ntpacker pack`). Input assembly + decode live in tp_core now (no stb here).
#   tp_demo_export <project.ntpacker_project> [work_dir]
tp_add_test(NAME demo_export TARGET tp_demo_export
    SOURCES tp_demo_export.c
    LIBS tp_build
    NO_CTEST)

# M0 production-path baseline. The superseded component clone, full-snapshot-as-
# commit, and throwaway diff-spike executables were removed: this is the single
# authoritative core measurement path. Timing remains advisory; status, sample,
# fixture, allocation and byte accounting fail closed.
#   tp_bench_foundation [scratch_dir] [iterations] [recovery_ops]
#   tp_bench_foundation --project <project> [scratch_dir] [iterations] [recovery_ops]
#   tp_bench_foundation --project-load-scaling [iterations]
#   tp_bench_foundation --batch-scaling <project> [iterations]
#   tp_bench_foundation --pack-hash-probe <project> [iterations]
# tp_build (not just tp_core): the --pack-hash-probe mode calls the session probe
# tp_session_pack_input_hash, which lives in tp_job.c (tp_build). tp_build PUBLIC-
# links tp_core, so the other modes keep their symbols.
# The snapshot/clone accounting and the project-load work probes are compiled out
# of the shipped library, so their TUs are recompiled here with
# TP_ENABLE_TEST_SEAMS.
tp_add_test(NAME bench_foundation TARGET tp_bench_foundation
    SOURCES tp_bench_foundation.c tp_bench_project_load.c
        ../src/tp_session_snapshot.c ../src/tp_project_generation.c
        ../src/tp_project_clone.c ../src/tp_project_parse.c
        ../src/tp_project_identity.c
    LIBS tp_build nt_time cjson tinycthread PRIVATE_SRC SEAMS
    DEFINES _CRT_SECURE_NO_WARNINGS
    NO_CTEST)

# Premise gate: serialized `.ntpack` blob bytes vs the raw RGBA8 page
# bytes they inflate into, plus the inflate cost, at 1024/2048/4096 synthetic
# page scales and one 4096 atlas built from the real Kenney bench assets. NOT a
# ctest: it drives the real nt_builder and allocates hundreds of MiB.
# Also carries the S27 phase-2 cold-tier codec sweep (stb PNG levels 1/3/5/8 +
# forced filter-none, vs raw miniz deflate levels 1/3/6) over the two 4096 page
# fixtures. miniz comes in transitively via nt_builder; named here because this
# TU includes miniz.h directly.
#   tp_bench_pack_blob <scratch_dir> [iterations] [bench_assets_root]
tp_add_test(NAME bench_pack_blob TARGET tp_bench_pack_blob
    SOURCES tp_bench_pack_blob.c
    LIBS tp_build nt_time miniz PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS
    NO_CTEST)

# Deterministic checked-in large project: 100 atlases x 10 file sources. The
# generator uses the core's white-box model helpers; the ctest comes back
# through the public snapshot/load boundary and proves committed bytes,
# structural counts, source tags, and relative asset paths.
tp_add_test(NAME large_project_contract TARGET tp_generate_large_project
    SOURCES tp_generate_large_project.c
    LIBS tp_core PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_large_project_contract"
    ARGS --check
        "${CMAKE_SOURCE_DIR}/examples/projects/large-synthetic.ntpacker_project"
        "${CMAKE_BINARY_DIR}/tp_large_project_contract")

# Deterministic checked-in owner-scale bench project (U-01): 37 atlases
# assembled from the 2,020-image Kenney CC0 manifest
# (examples/bench-assets/manifest.tsv) across six composition groups
# (homogeneous per-pack, mixed/shared-source, duplicate-source, proto
# multi-page, size-varied, folder-source) -- 5,481 total source memberships.
# Same white-box-construct / public-snapshot-validate split as
# tp_generate_large_project.
tp_add_test(NAME bench_project_contract TARGET tp_generate_bench_project
    SOURCES tp_generate_bench_project.c
    LIBS tp_core PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS
    MAKE_DIRS "${CMAKE_BINARY_DIR}/tp_bench_project_contract"
    ARGS --manifest
        "${CMAKE_SOURCE_DIR}/examples/bench-assets/manifest.tsv" --check
        "${CMAKE_SOURCE_DIR}/examples/projects/bench-owner-scale.ntpacker_project"
        "${CMAKE_BINARY_DIR}/tp_bench_project_contract")

tp_add_test(NAME proc_job_transport LIBS tp_build unity PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS)

tp_add_test(NAME job_worker_proto LIBS tp_build unity PRIVATE_SRC
    DEFINES _CRT_SECURE_NO_WARNINGS)

if(WIN32)
    set(_tp_proc_backend ../src/tp_proc_win32.c)
else()
    set(_tp_proc_backend ../src/tp_proc_posix.c)
endif()
tp_add_test(NAME job_worker_process
    SOURCES test_job_worker_process.c
        ../src/tp_job_worker.c ../src/tp_job_worker_proto.c
        ${_tp_proc_backend}
    LIBS tp_core tinycthread unity PRIVATE_SRC SEAMS
    DEFINES _CRT_SECURE_NO_WARNINGS)

# Tests are executable boundaries, so sanitizer instrumentation must be linked
# on each one rather than only on tp_core/tp_build. Keep this directory-wide
# gate at the end so newly added first-party tests cannot silently miss it.
get_property(_tp_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_tp_test_target IN LISTS _tp_test_targets)
    nt_set_sanitizer_flags(${_tp_test_target})
endforeach()
