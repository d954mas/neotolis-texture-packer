# ntpacker-gui: native GUI shell (roadmap Phase 6 grows this). Skeleton only.
# Native-only: the root CMakeLists fatal-errors on EMSCRIPTEN, but guard anyway so
# this file is self-contained if ever pulled into a wasm-configured tree.
if(NOT EMSCRIPTEN)

set(GUI_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/gui/${NT_PRESET_NAME}")
set(GUI_ASSETS_DIR "${GUI_OUTPUT_DIR}/assets")
# Shared pack location -- builder writes here once; every preset copies from here.
set(GUI_PACK_DIR "${CMAKE_SOURCE_DIR}/build/apps/gui")
set(GUI_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
# Pack builder reads engine shader/font sources by short relative paths, so it runs
# from the engine root (keeps generated macro names identical to the engine examples).
set(GUI_ENGINE_ROOT "${CMAKE_SOURCE_DIR}/external/neotolis-engine")

# tinyfiledialogs (vendored in-repo, apps/gui/deps, zlib licence) -- OS-native open/save/
# folder dialogs (ux.md gap 3). Its own small static lib compiled -w so our -Werror never
# applies to third-party code; default CRT (matches the GUI). Windows needs comdlg32/ole32/
# shell32 for the native dialogs.
add_library(tinyfiledialogs STATIC deps/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(tinyfiledialogs PUBLIC deps/tinyfiledialogs)
target_compile_definitions(tinyfiledialogs PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tinyfiledialogs PRIVATE -w)   # vendored: exempt from our -Werror
if(WIN32)
    target_link_libraries(tinyfiledialogs PUBLIC comdlg32 ole32 shell32 user32)
endif()

add_executable(ntpacker-gui main.c gui_project.c gui_scan.c gui_canvas.c gui_history.c)
# Lean link set derived from examples/ui_showcase: dropped nt_shape_renderer (no 3D/
# shape draws), nt_debug_overlay + nt_metrics (no HUD), nt_platform (no mem probe).
# Kept the concrete impls the engine's *_interface deps require at final link:
# nt_gfx (font/ui), nt_fs + nt_http_stub (resource), nt_clipboard (ui).
#
# tp_core is now linked (issue #282 resolved for the GUI by the packer/ split): tp_core is
# the read/model/edit core WITHOUT nt_builder, so it carries no nt_basisu_encoder and no
# static-CRT pin -- it links cleanly alongside nt_gfx. tp_build (the pack orchestrator that
# DOES pull nt_builder) is deliberately NOT linked; in-process packing still waits on #282.
# stb_image decodes the selected source image for the canvas preview (Task 5).
target_link_libraries(ntpacker-gui PRIVATE
    nt_core nt_app nt_input nt_window nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch
    nt_gfx nt_http_stub nt_fs nt_clipboard
    tp_core stb_image tinyfiledialogs
)
# Generated asset header from the pack builder (ntpacker_ui_assets.h).
target_include_directories(ntpacker-gui PRIVATE "${GUI_GENERATED_DIR}")
# The engine sets these build-wide via add_compile_definitions in its OWN directory scope;
# a submodule consumer doesn't inherit that, yet engine public headers (nt_resource.h) test
# them under our -Wundef/-Werror. Mirror the engine's resolved values so we agree with its libs.
target_compile_definitions(ntpacker-gui PRIVATE
    _CRT_SECURE_NO_WARNINGS
    NT_INTROSPECT_ENABLED=$<BOOL:${NT_INTROSPECT_ENABLED}>
    NT_INTROSPECT_WRITE_ENABLED=$<BOOL:${NT_INTROSPECT_WRITE_ENABLED}>
)

# Headless smoke of the project ops (create/add/decode+upload/save/reload) + a timed
# auto-quit, for CI/verification only. OFF by default -- the shipped app is interactive.
option(NTPACKER_GUI_SELFTEST "Run the startup project-ops self-test then auto-quit" OFF)
if(NTPACKER_GUI_SELFTEST)
    target_compile_definitions(ntpacker-gui PRIVATE NTPACKER_GUI_SELFTEST=1)
endif()

nt_set_warning_flags(ntpacker-gui)
nt_set_sanitizer_flags(ntpacker-gui)
set_target_properties(ntpacker-gui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GUI_OUTPUT_DIR}")

set(GUI_PACK_NAMES ntpacker_ui.ntpack)

# Pack builder + copy wiring (mirrors examples/ui_showcase): the builder runs at BUILD
# time to produce the pack + generated header, then the pack copies into the preset's
# assets/. The copy DEPENDS on the pack, which DEPENDS on the builder target, so a clean
# configure+build always produces + copies the pack with no reconfigure step.
add_executable(build_ntpacker_gui_packs build_packs.c)
target_link_libraries(build_ntpacker_gui_packs PRIVATE nt_builder nt_log)
target_compile_definitions(build_ntpacker_gui_packs PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(build_ntpacker_gui_packs PRIVATE -U_DLL)  # static CRT (match smoke/builder)
nt_set_warning_flags(build_ntpacker_gui_packs)
nt_set_sanitizer_flags(build_ntpacker_gui_packs)
set_target_properties(build_ntpacker_gui_packs PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GUI_OUTPUT_DIR}")

foreach(PACK ${GUI_PACK_NAMES})
    list(APPEND GUI_PACK_FILES "${GUI_PACK_DIR}/${PACK}")
endforeach()
add_custom_command(
    OUTPUT ${GUI_PACK_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GUI_PACK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GUI_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:build_ntpacker_gui_packs>" "${GUI_PACK_DIR}" "${GUI_GENERATED_DIR}"
    DEPENDS build_ntpacker_gui_packs
    WORKING_DIRECTORY "${GUI_ENGINE_ROOT}"
    COMMENT "Building ntpacker-gui pack(s)"
)

foreach(PACK ${GUI_PACK_NAMES})
    add_custom_command(
        OUTPUT "${GUI_ASSETS_DIR}/${PACK}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GUI_ASSETS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GUI_PACK_DIR}/${PACK}" "${GUI_ASSETS_DIR}/${PACK}"
        DEPENDS "${GUI_PACK_DIR}/${PACK}"
        COMMENT "Copying ${PACK} -> ${NT_PRESET_NAME}/assets/"
    )
    list(APPEND GUI_PACK_OUTPUTS "${GUI_ASSETS_DIR}/${PACK}")
endforeach()
add_custom_target(ntpacker_gui_packs DEPENDS ${GUI_PACK_OUTPUTS})
# Ordering: packs (and thus the generated header) build before the exe compiles/links.
add_dependencies(ntpacker-gui ntpacker_gui_packs)

endif()  # NOT EMSCRIPTEN
