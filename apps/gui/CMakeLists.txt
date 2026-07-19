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

add_executable(ntpacker-gui main.c gui_state.c gui_widgets.c gui_actions.c gui_actions_dialogs.c gui_actions_edits.c gui_actions_pack.c gui_actions_preview.c gui_actions_recovery.c gui_rows.c gui_selftest.c gui_shot.c gui_startup.c gui_paths.c gui_log_file.c gui_crash.c gui_bootstrap.c gui_parity.c gui_project.c gui_project_file.c gui_project_mutations.c gui_project_pending.c gui_project_recovery.c gui_session_adapter.c gui_scan.c gui_canvas.c gui_canvas_resources.c gui_pack.c gui_pack_jobs.c gui_pack_preview.c gui_view_settings.c gui_view_lists.c gui_view_canvas.c gui_view_chrome.c
    "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_fs.c")
if(WIN32)
    target_sources(ntpacker-gui PRIVATE "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
endif()
# Link set derived from examples/ui_showcase; nt_shape_renderer is re-added for the atlas-page
# canvas overlays (region outlines / hull polygons / pivot markers). Kept the concrete impls the
# engine's *_interface deps require at final link: nt_gfx (font/ui), nt_fs + nt_http_stub
# (resource), nt_clipboard (ui).
#
# tp_build (in-process packing) is now linked: engine issue #282 is FIXED (basisu single shared
# transcoder TU, commit 8d0133cb), so nt_builder + nt_basisu_encoder no longer duplicate the render
# runtime's transcoder symbol and coexist with nt_gfx in one image. tp_build PUBLIC-links tp_core +
# nt_builder, so the read/model/edit core comes transitively. gui_pack.c is a thin adapter over
# session-owned typed Pack/Export jobs. Source-preview raster ingress is shared
# with pack jobs through tp_core (strict UTF-8 path + bounded memory decode).
target_link_libraries(ntpacker-gui PRIVATE
    nt_core nt_app nt_input nt_window nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch nt_shape_renderer
    nt_gfx nt_http_stub nt_fs nt_clipboard
    nt_fpng # dev screenshot mode (--shot): PNG-encode the framebuffer readback
    tp_build tinyfiledialogs
    tinycthread # thread-safe GUI log sink; already in-closure via nt_builder
)
# D2 crash handler: MiniDumpWriteDump (gui_crash.c) needs DbgHelp on Windows. Target-scoped link
# addition only (no preset/toolchain change); POSIX uses libc backtrace, no extra lib.
if(WIN32)
    target_link_libraries(ntpacker-gui PRIVATE dbghelp shell32)
endif()
# Generated asset header from the pack builder (ntpacker_ui_assets.h).
target_include_directories(ntpacker-gui PRIVATE "${GUI_GENERATED_DIR}")
# Shared product version header (apps/common/ntpacker_version.h), also used by the CLI.
target_include_directories(ntpacker-gui PRIVATE "${CMAKE_SOURCE_DIR}/apps/common")
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
    # The selftest's recovery-degradation regression drives the journal's memory-io fault seams
    # declared in packer/src/tp_journal_internal.h -- the same src/-internal include the packer
    # test_journal.c uses. Scoped to the selftest build ONLY (the shipped GUI never sees packer/src),
    # and gui_selftest.c is the boundary-excluded dev-seam TU (check_boundaries.sh), like the CLI tests.
    target_include_directories(ntpacker-gui PRIVATE "${CMAKE_SOURCE_DIR}/packer/src")
endif()

# Optional: register the GUI self-test exe as a ctest. It needs NTPACKER_GUI_SELFTEST=ON (the exe then
# runs the headless self-test + auto-quits) AND a live GL context/display -- so it is OFF by default and
# GL-less CI runners stay green. Enable locally with:
#   cmake --preset native-tests-debug
#   cmake --build --preset native-tests-debug --target ntpacker-gui
#   ctest --preset native-tests-debug -R ntpacker_gui_selftest
option(NTPACKER_GUI_CTEST "Register the GUI self-test exe as a ctest (requires NTPACKER_GUI_SELFTEST=ON + a GL context)" OFF)
if(NTPACKER_GUI_SELFTEST AND NTPACKER_GUI_CTEST)
    # The self-test resolves example assets relative to the repo root (examples/defold-demo/...); a failed
    # NT_ASSERT aborts with a non-zero exit, which ctest reports as a test failure.
    add_test(NAME ntpacker_gui_selftest COMMAND ntpacker-gui)
    set_tests_properties(ntpacker_gui_selftest PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        TIMEOUT 180
        LABELS "gui;gl")
endif()

nt_set_warning_flags(ntpacker-gui)
nt_set_sanitizer_flags(ntpacker-gui)
set_target_properties(ntpacker-gui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GUI_OUTPUT_DIR}")
# Shipped Windows GUI is a windows-subsystem app (no console window behind the
# GUI; javaw pattern). Debug keeps the console -- logs, NT_ASSERT text and the
# self-test output stay visible in a terminal. Entry stays plain main().
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(ntpacker-gui PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(ntpacker-gui PRIVATE "LINKER:/entry:mainCRTStartup")
endif()

# GUI row rebuild benchmark. It compiles the production gui_rows/gui_scan
# sources with narrow stubs; no window, renderer, pack job, or event loop.
# CTest enforces only correctness/count/complexity. Absolute timing is advisory.
add_executable(tp_bench_gui_rows tp_bench_gui_rows.c gui_rows.c gui_scan.c)
target_link_libraries(tp_bench_gui_rows PRIVATE tp_core nt_time nt_ui)
target_include_directories(tp_bench_gui_rows PRIVATE
    "${CMAKE_SOURCE_DIR}/packer/tests"
    "${CMAKE_SOURCE_DIR}/packer/src")
target_compile_definitions(tp_bench_gui_rows PRIVATE
    NTPACKER_GUI_BENCH=1
    _CRT_SECURE_NO_WARNINGS
    NT_INTROSPECT_ENABLED=$<BOOL:${NT_INTROSPECT_ENABLED}>
    NT_INTROSPECT_WRITE_ENABLED=$<BOOL:${NT_INTROSPECT_WRITE_ENABLED}>
)
nt_set_warning_flags(tp_bench_gui_rows)
nt_set_sanitizer_flags(tp_bench_gui_rows)
set_target_properties(tp_bench_gui_rows PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${GUI_OUTPUT_DIR}")
add_test(NAME tp_gui_rows_complexity
    COMMAND tp_bench_gui_rows "${CMAKE_CURRENT_BINARY_DIR}/tp_gui_rows_ctest" 1)
set_tests_properties(tp_gui_rows_complexity PROPERTIES
    TIMEOUT 180
    LABELS "benchmark;gui")

# Contract probe: the production GUI atlas-rename adapter must admit the
# canonical transaction through tp_session and read the result from one owned
# snapshot. Headless: no window, renderer, dialogs, or GUI globals.
add_executable(tp_test_gui_session_adapter test_gui_session_adapter.c gui_session_adapter.c)
target_link_libraries(tp_test_gui_session_adapter PRIVATE tp_core unity)
target_compile_options(tp_test_gui_session_adapter PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_gui_session_adapter)
nt_set_sanitizer_flags(tp_test_gui_session_adapter)
add_test(NAME tp_gui_session_adapter COMMAND tp_test_gui_session_adapter)

add_executable(tp_test_gui_paths test_gui_paths.c gui_paths.c)
target_link_libraries(tp_test_gui_paths PRIVATE tp_core unity)
target_compile_options(tp_test_gui_paths PRIVATE -U_DLL)
target_include_directories(tp_test_gui_paths PRIVATE
    "${CMAKE_SOURCE_DIR}/apps/common")
if(WIN32)
    target_sources(tp_test_gui_paths PRIVATE
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
    target_link_libraries(tp_test_gui_paths PRIVATE shell32)
endif()
nt_set_warning_flags(tp_test_gui_paths)
nt_set_sanitizer_flags(tp_test_gui_paths)
add_test(NAME tp_gui_paths COMMAND tp_test_gui_paths)

# Canonical GUI identity regression: two distinct sources may expose the same
# source-local key.  The headless target exercises the production row,
# selection/action and project/session adapters without creating a window.
add_executable(tp_test_gui_canonical_identity
    test_gui_canonical_identity.c
    gui_state.c
    gui_actions.c
    gui_actions_dialogs.c
    gui_actions_edits.c
    gui_actions_pack.c
    gui_actions_preview.c
    gui_actions_recovery.c
    gui_rows.c
    gui_project.c
    gui_project_file.c
    gui_project_mutations.c
    gui_project_pending.c
    gui_project_recovery.c
    gui_session_adapter.c
    gui_scan.c
    gui_canvas.c
    gui_canvas_resources.c
    gui_pack.c
    gui_pack_jobs.c
    gui_pack_preview.c
    gui_paths.c
)
if(WIN32)
    target_sources(tp_test_gui_canonical_identity PRIVATE
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
endif()
target_link_libraries(tp_test_gui_canonical_identity PRIVATE
    nt_core nt_app nt_window nt_input nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch nt_shape_renderer
    nt_gfx nt_http_stub nt_fs nt_clipboard
    tp_build stb_image tinyfiledialogs tinycthread unity
)
target_include_directories(tp_test_gui_canonical_identity PRIVATE
    "${CMAKE_SOURCE_DIR}/packer/src"
    "${CMAKE_SOURCE_DIR}/apps/common"
)
target_compile_definitions(tp_test_gui_canonical_identity PRIVATE
    TP_GUI_IDENTITY_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_gui_identity_tmp"
    TP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    _CRT_SECURE_NO_WARNINGS
    NT_INTROSPECT_ENABLED=$<BOOL:${NT_INTROSPECT_ENABLED}>
    NT_INTROSPECT_WRITE_ENABLED=$<BOOL:${NT_INTROSPECT_WRITE_ENABLED}>
)
target_compile_options(tp_test_gui_canonical_identity PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_gui_canonical_identity)
nt_set_sanitizer_flags(tp_test_gui_canonical_identity)
add_test(NAME tp_gui_canonical_identity COMMAND tp_test_gui_canonical_identity)

# P1-10 action/state trace oracle.  This is deliberately a separate headless
# target: decomposition of gui_actions/gui_project must preserve between-frame
# timing and state ownership without making the broader identity corpus absorb
# another responsibility.
add_executable(tp_test_gui_action_trace
    test_gui_action_trace.c
    gui_state.c
    gui_actions.c
    gui_actions_dialogs.c
    gui_actions_edits.c
    gui_actions_pack.c
    gui_actions_preview.c
    gui_actions_recovery.c
    gui_rows.c
    gui_project.c
    gui_project_file.c
    gui_project_mutations.c
    gui_project_pending.c
    gui_project_recovery.c
    gui_session_adapter.c
    gui_scan.c
    gui_canvas.c
    gui_canvas_resources.c
    gui_pack.c
    gui_pack_jobs.c
    gui_pack_preview.c
    gui_paths.c
)
if(WIN32)
    target_sources(tp_test_gui_action_trace PRIVATE
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
endif()
target_link_libraries(tp_test_gui_action_trace PRIVATE
    nt_core nt_app nt_window nt_input nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch nt_shape_renderer
    nt_gfx nt_http_stub nt_fs nt_clipboard
    tp_build stb_image tinyfiledialogs tinycthread unity
)
target_include_directories(tp_test_gui_action_trace PRIVATE
    "${CMAKE_SOURCE_DIR}/packer/src"
    "${CMAKE_SOURCE_DIR}/apps/common"
)
target_compile_definitions(tp_test_gui_action_trace PRIVATE
    TP_GUI_TRACE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_gui_trace_tmp"
    _CRT_SECURE_NO_WARNINGS
    NT_INTROSPECT_ENABLED=$<BOOL:${NT_INTROSPECT_ENABLED}>
    NT_INTROSPECT_WRITE_ENABLED=$<BOOL:${NT_INTROSPECT_WRITE_ENABLED}>
)
target_compile_options(tp_test_gui_action_trace PRIVATE -U_DLL)
nt_set_warning_flags(tp_test_gui_action_trace)
nt_set_sanitizer_flags(tp_test_gui_action_trace)
add_test(NAME tp_gui_action_trace COMMAND tp_test_gui_action_trace)

# Capability/parity corpus. The second adapter is intentionally test-only:
# it calls tp_session directly and proves the future transport host needs no
# duplicate mutable session wrapper.
add_executable(tp_test_client_parity
    test_client_parity.c
    client_parity_manifest.c
    gui_session_adapter.c)
target_link_libraries(tp_test_client_parity PRIVATE tp_build unity)
target_compile_options(tp_test_client_parity PRIVATE -U_DLL)
target_compile_definitions(tp_test_client_parity PRIVATE
    TP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    TP_TEST_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_client_parity_tmp")
nt_set_warning_flags(tp_test_client_parity)
nt_set_sanitizer_flags(tp_test_client_parity)
add_test(NAME tp_client_parity COMMAND tp_test_client_parity)

add_executable(tp_client_parity_replay
    client_parity_replay.c
    gui_session_adapter.c)
target_link_libraries(tp_client_parity_replay PRIVATE tp_build)
target_compile_options(tp_client_parity_replay PRIVATE -U_DLL)
nt_set_warning_flags(tp_client_parity_replay)
nt_set_sanitizer_flags(tp_client_parity_replay)
add_test(NAME tp_client_parity_real COMMAND ${CMAKE_COMMAND}
    -DNTPACKER=$<TARGET_FILE:ntpacker>
    -DREPLAY=$<TARGET_FILE:tp_client_parity_replay>
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tp_client_parity_real_tmp
    -P "${CMAKE_CURRENT_SOURCE_DIR}/client_parity_real.cmake")

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
# Icon masks baked into ntpacker_ui_atlas; passed absolute (builder's WORKING_DIRECTORY is the engine
# root, not apps/gui). Globbed only for the DEPENDS list so editing/adding an icon repacks; the bake
# itself uses an explicit list in build_packs.c (the contract), not this glob.
set(GUI_ICONS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/icons")
file(GLOB GUI_ICON_FILES "${GUI_ICONS_DIR}/*.png")
add_custom_command(
    OUTPUT ${GUI_PACK_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GUI_PACK_DIR}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${GUI_GENERATED_DIR}"
    COMMAND "$<TARGET_FILE:build_ntpacker_gui_packs>" "${GUI_PACK_DIR}" "${GUI_GENERATED_DIR}" "${GUI_ICONS_DIR}"
    DEPENDS build_ntpacker_gui_packs ${GUI_ICON_FILES}
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
