# --- tp_core: read/model/edit core (NO packing, NO nt_builder) ------------------
# Split rationale: tp_pack.c/tp_export_run.c are the only TUs that need nt_builder.
# Historically the split ALSO firewalled basisu away from the GUI (engine issue
# #282); #283 fixed that upstream and the GUI links tp_build directly now (see
# apps/gui/CMakeLists.txt). The split stays for layering (model-only consumers:
# most tests, inspect-only tools) and CRT hygiene (tp_build pins -U_DLL to match
# the static-CRT builder; tp_core stays default-CRT for the render runtime).
# Vendored utf8proc (Unicode NFC + case-fold): added BEFORE tp_core so the source-
# key normalizer (src/tp_srckey.c) can PRIVATE-link it. Was spike-only until F1-02
# promoted tp_srckey into production. -w vendored; no on-disk schema of its own.
add_subdirectory(deps/utf8proc)
find_package(Threads REQUIRED)

add_library(tp_core STATIC
    src/tp_arena.c src/tp_names.c src/tp_name_map.c src/tp_pack_read.c src/tp_project.c
    src/tp_pack_settings.c src/tp_scan.c src/tp_input.c src/tp_validate.c
    src/tp_export.c src/tp_normalize.c src/tp_export_png.c src/tp_export_json_neotolis.c
    src/tp_export_defold.c
    # F1-00 production project-identity boundary: 128-bit runtime ID + RNG seam,
    # canonical saved-project path (lexical + realpath), Save-As transition DTO.
    # Promoted from the accepted C0-01 spike; no engine API, no new link deps
    # (POSIX realpath in libc / Win32 kernel32 auto-linked).
    src/tp_id.c src/tp_identity.c src/tp_identity_session.c src/tp_source_plan.c
    src/tp_project_lease.c src/tp_recovery.c
    # M5: fixed client capability vocabulary/matrix. Unsupported surfaces are a
    # typed result, not ad-hoc frontend prose or a pretend live CLI session.
    src/tp_client_capability.c
    # F1-01 schema-v2 identity migration: deterministic legacy synthesis +
    # atomic writable promotion (kept out of tp_project.c's loader/serializer).
    src/tp_project_migrate.c
    # F1-02 tagged sources: source-key normalization / case-fold collision /
    # portability validation, promoted from the C0-01 srckey spike (uses utf8proc).
    src/tp_srckey.c
    # F1-03 deterministic sprite ids + canonical selectors: the resolved sprite
    # index (scan -> source-local key -> sprite_id) and the human-selector resolver
    # (id/name/path/compound -> exactly one id, ambiguity -> candidate list).
    src/tp_sprite_index.c src/tp_selector.c
    # F2-01 typed operation engine: the append-only op catalog + closed field
    # vocabulary, payload/reference validation, id-only stage-then-commit apply,
    # selector->operation builders, and the canonical byte-stable op/result encoder.
    # Promoted from the accepted C0-02 spike. CORE-TESTED groundwork -- the CLI/GUI
    # frontend cutover is F2-05 (this does not route the shipping mutators).
    src/tp_op_catalog.c src/tp_op_validate.c src/tp_op_apply.c src/tp_op_encode.c src/tp_op_build.c
    # F2-02 atomic transactions, revision & semantic dirty: an OOM-safe project
    # deep-clone (the atomicity primitive), the semantic-state identity hash + dirty
    # model, the pluggable transaction-id idempotency store, the atomic clone/apply/
    # swap core over the F2-01 op engine, and the versioned transaction request/result
    # JSON contract (decode/lower/encode; cJSON stays a PRIVATE dep in the .c files).
    # Promoted from the accepted C0-02 spike. CORE-TESTED groundwork -- the CLI/GUI
    # frontend cutover is F2-05 (this does not route the shipping mutators).
    # F2-04 fix: the 32-hex idempotency id-set shared by the idstore + the journal's
    # retained-id index (one place for the membership + growth logic).
    src/tp_idset.c
    src/tp_project_clone.c src/tp_semantic.c src/tp_txn_idset.c src/tp_txn_apply.c
    # Architecture foundation M1: synchronous orchestration owner and the one
    # public owned immutable snapshot boundary. Business validation remains in
    # the operation/model modules; no GUI, protocol, recovery backend, or jobs.
    src/tp_session.c
    # P-01 (plan §17): arena-backed alternative to the malloc project deep-clone --
    # a memcpy-bound clone off one tp_arena, byte-identical to tp_project_clone.
    # Integration into the live model stays deferred; M0's authoritative normal
    # transaction allocation baseline lives in tests/tp_bench_foundation.
    src/tp_project_clone_arena.c
    src/tp_txn_parse.c src/tp_txn_lower.c src/tp_txn_encode.c
    # F2-03 semantic diff / exact inverse (Undo) + redo replay + a minimal in-memory
    # undo/redo history: per-op state-capture diff (before/after entity data + position),
    # element deep-copy + positional collection primitives, inverse/redo apply
    # (stage-then-commit via the F2-02 clone/swap), and the history stack. Extends the
    # F2-02 commit to capture a diff without breaking its atomicity. CORE-TESTED
    # groundwork -- the GUI Undo cutover is F3-02, frontend routing F2-05, journal F2-04.
    src/tp_diff_entity.c src/tp_diff_capture.c src/tp_diff_apply.c src/tp_history.c
    # F2-04 minimum recovery journal: a versioned, length-prefixed, CRC-checksummed
    # durable log over an injectable I/O seam (in-memory for the fault suite, real file
    # for production). A commit is not acknowledged until its record is durably appended
    # (§7.1); the retained transaction-id set + committed state are recoverable from
    # checkpoint + journal replay (§7.2), which is UB-clean on arbitrary/corrupt/torn
    # bytes. Sidecar: does NOT change project serialization. Model glue + the commit
    # acknowledgement gate live in tp_txn_apply.c; the coordinator interface in tp_transaction.h.
    src/tp_journal.c src/tp_journal_io.c)
target_include_directories(tp_core PUBLIC include)
# tp_pack_settings.c includes nt_builder.h for the header-only nt_atlas_opts_defaults()
# inline (the single source of the packing defaults). HEADERS ONLY: this adds the builder
# include dir but does NOT link nt_builder, so no nt_basisu_encoder / static-CRT pin leaks
# into tp_core's own closure. nt_builder.h's other includes (nt_hash, nt_shared format
# headers) are already on tp_core's include path via the PUBLIC links below.
target_include_directories(tp_core PRIVATE
    $<TARGET_PROPERTY:nt_builder,INTERFACE_INCLUDE_DIRECTORIES>)
# stb_image_write.h for the PNG page writer (src/tp_export_png.c). Header path
# only; the library itself is linked PRIVATE below.
target_include_directories(tp_core PRIVATE
    $<TARGET_PROPERTY:stb_image_write,INTERFACE_INCLUDE_DIRECTORIES>)
# No nt_builder in tp_core's link closure: nt_shared is INTERFACE headers only
# (nt_pack_format.h / nt_atlas_format.h / nt_texture_format.h), nt_hash is the small
# hash lib (hash/nt_hash.h), nt_log is logging. None pull basisu, so tp_core is safe
# to link into the GPU runtime (the GUI).
target_link_libraries(tp_core PUBLIC nt_shared nt_hash nt_log PRIVATE Threads::Threads)
# cJSON (engine-vendored, MIT) parses .ntpacker_project. PRIVATE: tp_project.h never
# exposes cJSON; Save uses a hand-rolled deterministic writer.
# stb_image_write (engine-vendored, public domain) is the PNG page writer. Linked
# DIRECTLY here (NOT via nt_builder): it is a standalone single-header impl with no
# basisu / no static-CRT-duplicate concern, so it is safe in tp_core's closure and
# the GUI (which links tp_core) picks it up transitively -- same PRIVATE static-lib
# propagation cjson already relies on. Its own -U_DLL objects mix with tp_core's
# default-CRT objects exactly like the GUI's existing stb_image dependency.
# utf8proc (vendored, MIT) powers src/tp_srckey.c's NFC + case-fold. PRIVATE:
# tp_srckey.h never exposes utf8proc types; its allocations are freed in-TU.
target_link_libraries(tp_core PRIVATE cjson stb_image_write utf8proc)
target_compile_definitions(tp_core PRIVATE _CRT_SECURE_NO_WARNINGS)
# CRT: tp_core no longer links the static-CRT builder, so it DROPS -U_DLL and uses the
# engine runtime's default CRT -- matching its own deps (nt_hash/nt_log/cjson) and the
# render runtime the GUI links (nt_gfx et al.). This lets ntpacker-gui link tp_core
# without a CRT mismatch. tp_build + the builder-linking tests keep -U_DLL to match
# nt_builder/nt_basisu_encoder (static CRT). Mixing default-CRT and -U_DLL objects in
# one image is already proven on this clang/windows-msvc toolchain (every engine example
# links GLFW's -U_DLL objects against default-CRT runtime libs).
nt_set_warning_flags(tp_core)      # AGENTS.md invariant
nt_set_sanitizer_flags(tp_core)

# --- tp_build: pack orchestration (drives nt_builder) ---------------------------
# Isolated so ONLY pack-time consumers (GUI, CLI, smoke, pack tests) pull
# nt_builder -> nt_basisu_encoder. Since #283 the GUI links tp_build directly.
add_library(tp_build STATIC src/tp_pack.c src/tp_export_run.c src/tp_job.c)
# nt_builder BEFORE tp_core: GNU ld resolves static archives left-to-right and
# nt_builder needs nt_log_write from tp_core's PUBLIC nt_log; the old order
# flattened to ...libnt_log.a...libnt_builder.a and the Linux release link
# failed (lld/ld64 don't care about order). nt_log repeated after nt_builder
# as belt-and-suspenders against future reordering.
target_link_libraries(tp_build PUBLIC nt_builder tp_core nt_log)
target_compile_definitions(tp_build PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tp_build PRIVATE -U_DLL)   # static CRT: match nt_builder (see apps/smoke)
nt_set_warning_flags(tp_build)
nt_set_sanitizer_flags(tp_build)

# The C0-01 contract-spike primitives (tp_c0). Depends on tp_core's interface
# includes + utf8proc (added above). Spike-only: not linked into apps, no on-disk
# schema change.
add_subdirectory(spike/c0)

enable_testing()
add_subdirectory(tests)
