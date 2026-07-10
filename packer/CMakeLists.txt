add_library(tp_core STATIC
    src/tp_arena.c src/tp_name_map.c src/tp_pack_read.c src/tp_pack.c)
target_include_directories(tp_core PUBLIC include)
# nt_builder is PUBLIC-linked so tp_core drives the builder (1b) AND transitively gets
# shared/include (nt_pack_format.h, nt_atlas_format.h, nt_texture_format.h) + hash/nt_hash.h.
target_link_libraries(tp_core PUBLIC nt_builder nt_log)
target_compile_definitions(tp_core PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tp_core PRIVATE -U_DLL)   # match builder: static CRT (see apps/smoke)
nt_set_warning_flags(tp_core)      # AGENTS.md invariant
nt_set_sanitizer_flags(tp_core)
enable_testing()
add_subdirectory(tests)
