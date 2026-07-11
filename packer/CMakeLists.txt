# --- tp_core: read/model/edit core (NO packing, NO nt_builder) ------------------
# Split rationale (engine issue #282): tp_pack.c is the ONLY TU that needs
# nt_builder, and nt_builder pulls nt_basisu_encoder -- a static-CRT-pinned basisu
# TU that duplicates the render runtime's nt_basisu_transcoder and cannot coexist
# with nt_gfx in one image. Keeping the builder out of tp_core lets the GUI link the
# project model in-process. tp_build (below) isolates the builder for the CLI, the
# smoke app, and the pack tests that actually pack.
add_library(tp_core STATIC
    src/tp_arena.c src/tp_names.c src/tp_name_map.c src/tp_pack_read.c src/tp_project.c
    src/tp_pack_settings.c src/tp_scan.c
    src/tp_export.c src/tp_normalize.c src/tp_export_png.c src/tp_export_json_neotolis.c
    src/tp_export_defold.c)
target_include_directories(tp_core PUBLIC include)
# tp_pack_settings.c includes nt_builder.h for the header-only nt_atlas_opts_defaults()
# inline (the single source of the packing defaults). HEADERS ONLY: this adds the builder
# include dir but does NOT link nt_builder, so no nt_basisu_encoder / static-CRT pin leaks
# into tp_core or the GUI (#282). nt_builder.h's other includes (nt_hash, nt_shared format
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
target_link_libraries(tp_core PUBLIC nt_shared nt_hash nt_log)
# cJSON (engine-vendored, MIT) parses .ntpacker_project. PRIVATE: tp_project.h never
# exposes cJSON; Save uses a hand-rolled deterministic writer.
# stb_image_write (engine-vendored, public domain) is the PNG page writer. Linked
# DIRECTLY here (NOT via nt_builder): it is a standalone single-header impl with no
# basisu / no static-CRT-duplicate concern, so it is safe in tp_core's closure and
# the GUI (which links tp_core) picks it up transitively -- same PRIVATE static-lib
# propagation cjson already relies on. Its own -U_DLL objects mix with tp_core's
# default-CRT objects exactly like the GUI's existing stb_image dependency.
target_link_libraries(tp_core PRIVATE cjson stb_image_write)
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
# Isolated so ONLY pack-time consumers (CLI, smoke, pack tests) pull nt_builder ->
# nt_basisu_encoder. The GUI links tp_core only and never sees basisu (#282).
add_library(tp_build STATIC src/tp_pack.c src/tp_export_run.c)
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
