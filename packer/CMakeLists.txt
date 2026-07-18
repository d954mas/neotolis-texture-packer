# --- tp_core: read/model/edit core (NO packing, NO nt_builder) ------------------
# Split rationale: tp_pack.c/tp_export_run.c are the only TUs that need nt_builder.
# Historically the split ALSO firewalled basisu away from the GUI (engine issue
# #282); #283 fixed that upstream and the GUI links tp_build directly now (see
# apps/gui/CMakeLists.txt). The split stays for layering (model-only consumers:
# most tests, inspect-only tools) and CRT hygiene (tp_build pins -U_DLL to match
# the static-CRT builder; tp_core stays default-CRT for the render runtime).
# Vendored utf8proc supplies Unicode NFC and case-folding to source keys.
add_subdirectory(deps/utf8proc)
find_package(Threads REQUIRED)

add_library(tp_core STATIC
    src/tp_arena.c src/tp_fs_io.c src/tp_image.c src/tp_json_internal.c src/tp_utf8.c src/tp_names.c src/tp_name_map.c src/tp_pack_read.c src/tp_project.c src/tp_project_parse.c src/tp_project_path.c src/tp_project_save.c src/tp_project_write.c src/tp_project_pack_bridge.c
    src/tp_pack_settings.c src/tp_pack_constraints.c src/tp_transform.c src/tp_scan.c src/tp_input.c src/tp_validate.c src/tp_validate_index.c src/tp_validate_report.c src/tp_validate_source.c src/tp_validate_sprite.c src/tp_validate_target_settings.c
    src/tp_export.c src/tp_normalize.c src/tp_export_png.c src/tp_export_json_neotolis.c
    src/tp_export_defold.c
    # Project identity, canonical paths, leases, source plans, and recovery.
    src/tp_id.c src/tp_identity.c src/tp_identity_session.c src/tp_source_path_text.c src/tp_source_plan.c
    src/tp_project_lease.c src/tp_recovery.c
    # Fixed client capability vocabulary and matrix.
    src/tp_client_capability.c
    # Canonical schema identity assignment and validation.
    src/tp_project_identity.c
    # Source-key normalization, collision, and portability rules.
    src/tp_srckey.c
    # Deterministic sprite IDs, index, and selector resolution.
    src/tp_sprite_index.c src/tp_selector.c
    # Typed operation catalog, validation, apply, builders, and encoding.
    src/tp_op_catalog.c src/tp_op_validate.c src/tp_op_validate_atlas.c src/tp_op_validate_source_sprite.c src/tp_op_validate_animation.c src/tp_op_apply.c src/tp_op_encode.c src/tp_op_build.c
    # Transaction ID retention, clone/apply/publish, semantic dirty, and JSON.
    src/tp_idset.c
    src/tp_project_clone.c src/tp_semantic.c src/tp_txn_idset.c src/tp_txn_apply.c
    # Synchronous orchestration and the owned immutable snapshot boundary.
    src/tp_session.c src/tp_session_snapshot.c src/tp_project_generation.c
    src/tp_txn_parse.c src/tp_txn_lower.c src/tp_txn_encode.c
    # Semantic diff, exact inverse/redo, and bounded in-memory history.
    src/tp_diff_entity.c src/tp_diff_capture.c src/tp_diff_apply.c src/tp_history_codec.c src/tp_history.c
    # Durable recovery journal and injectable I/O seam.
    src/tp_journal.c src/tp_journal_io.c)
if(WIN32)
    target_sources(tp_core PRIVATE src/tp_fs_win32.c)
else()
    target_sources(tp_core PRIVATE src/tp_fs_posix.c)
endif()
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
# stb_image / stb_image_write (engine-vendored, public domain) are the bounded
# memory decoder and PNG page writer. Linked DIRECTLY here (NOT via nt_builder):
# each is a standalone single-header implementation with no basisu concern. The
# decoder allocator is hidden behind tp_image_free, so allocation and release
# stay inside the matching library runtime.
# utf8proc (vendored, MIT) powers src/tp_srckey.c's NFC + case-fold. PRIVATE:
# tp_srckey.h never exposes utf8proc types; its allocations are freed in-TU.
target_link_libraries(tp_core PRIVATE cjson stb_image stb_image_write utf8proc)
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

enable_testing()
add_subdirectory(tests)
