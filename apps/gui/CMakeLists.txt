# ntpacker-gui: native GUI frontend over the shared session/core contracts.
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

# The engine sets these build-wide via add_compile_definitions in its OWN directory
# scope; a submodule consumer doesn't inherit that, yet engine public headers
# (nt_resource.h) test them under our -Wundef/-Werror. Mirror the engine's resolved
# values so every target that pulls an engine UI header agrees with its libs.
set(_tp_gui_introspect_defines
    NT_INTROSPECT_ENABLED=$<BOOL:${NT_INTROSPECT_ENABLED}>
    NT_INTROSPECT_WRITE_ENABLED=$<BOOL:${NT_INTROSPECT_WRITE_ENABLED}>)

# tinyfiledialogs (vendored in-repo, apps/gui/deps, zlib licence) -- OS-native open/save/
# folder dialogs. Its own small static lib compiled -w so our -Werror never
# applies to third-party code; default CRT (matches the GUI). Windows needs comdlg32/ole32/
# shell32 for the native dialogs.
add_library(tinyfiledialogs STATIC deps/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(tinyfiledialogs PUBLIC deps/tinyfiledialogs)
target_compile_definitions(tinyfiledialogs PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tinyfiledialogs PRIVATE -w)   # vendored: exempt from our -Werror
if(WIN32)
    target_link_libraries(tinyfiledialogs PUBLIC comdlg32 ole32 shell32 user32)
endif()

add_executable(ntpacker-gui main.c gui_state.c gui_widgets.c gui_actions.c gui_actions_dialogs.c gui_actions_edits.c gui_actions_intents.c gui_actions_pack.c gui_actions_preview.c gui_actions_recovery.c gui_rows.c gui_selftest.c gui_startup.c gui_paths.c gui_log_file.c gui_crash.c gui_bootstrap.c gui_project.c gui_project_file.c gui_project_mutations.c gui_project_recovery.c gui_project_operations.c gui_edit_state.c gui_canvas.c gui_canvas_resources.c gui_pack.c gui_pack_jobs.c gui_pack_preview.c gui_view_settings.c gui_view_lists.c gui_view_canvas.c gui_view_chrome.c
    "${CMAKE_SOURCE_DIR}/apps/common/app_format_catalog.c"
    "${CMAKE_SOURCE_DIR}/apps/common/app_paths.c"
    "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_fs.c"
    "${CMAKE_SOURCE_DIR}/apps/common/app_scratch.c")
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
# Interface-impl stubs last, same rule and reason as _tp_gui_headless_libs below:
# an archive listed before tp_build cannot satisfy an undefined that tp_build's
# closure introduces, and ELF linkers do not rescan.
target_link_libraries(ntpacker-gui PRIVATE
    nt_core nt_app nt_input nt_window nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch nt_shape_renderer
    tp_build tinyfiledialogs
    tinycthread # thread-safe GUI log sink; already in-closure via nt_builder
    nt_gfx nt_http_stub nt_fs nt_clipboard
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
target_compile_definitions(ntpacker-gui PRIVATE
    _CRT_SECURE_NO_WARNINGS
    ${_tp_gui_introspect_defines}
)

# Dev-only seams that must NOT ship in the interactive binary: the --shot screenshot
# capture (gui_shot.c), the --bench-perf headless perf probe (gui_bench.c), main()'s
# --auto-pack / --selftest-crash argv branches, and gui_pack_debug_force_busy. OFF by
# default so the shipped exe carries none of that code (nor its argv strings); the
# native-debug / native-tests-debug presets turn it ON, and CI's perf-probes job
# configures native-release with -DNTPACKER_GUI_DEV_SEAMS=ON for its --bench-perf leg.
# Same shape as NTPACKER_GUI_SELFTEST below: option + gated sources + gated call sites.
option(NTPACKER_GUI_DEV_SEAMS "Compile the --shot / --bench-perf / --auto-pack dev seams into ntpacker-gui" OFF)

# Headless smoke of the project ops (create/add/decode+upload/save/reload) + a timed
# auto-quit, for CI/verification only. OFF by default -- the shipped app is interactive.
option(NTPACKER_GUI_SELFTEST "Run the startup project-ops self-test then auto-quit" OFF)

# The self-test oracle drives gui_pack_debug_force_busy, so a self-test build implies the
# dev seams. One derived variable, so the two flags cannot drift out of step.
if(NTPACKER_GUI_DEV_SEAMS OR NTPACKER_GUI_SELFTEST)
    # Whole-TU gating: the shipped build never compiles the seam sources at all, which is
    # what keeps their argv strings and fixture paths out of the binary. nt_fpng is the
    # --shot framebuffer PNG encoder and is linked only with them.
    target_sources(ntpacker-gui PRIVATE gui_shot.c gui_bench.c)
    target_link_libraries(ntpacker-gui PRIVATE nt_fpng)
    target_compile_definitions(ntpacker-gui PRIVATE NTPACKER_GUI_DEV_SEAMS=1)
endif()

if(NTPACKER_GUI_SELFTEST)
    target_compile_definitions(ntpacker-gui PRIVATE NTPACKER_GUI_SELFTEST=1)
    # The selftest's recovery-degradation regression drives the journal's memory-io fault seams
    # declared in packer/src/tp_journal_internal.h -- the same src/-internal include the packer
    # test_journal.c uses. Scoped to the selftest build ONLY (the shipped GUI never sees packer/src),
    # and gui_selftest.c is the boundary-excluded dev-seam TU (check_boundaries.sh), like the CLI tests.
    target_include_directories(ntpacker-gui PRIVATE "${CMAKE_SOURCE_DIR}/packer/src")
endif()

# CI headless build. ON turns three behaviours into compile-time facts instead of runtime environment
# reads: the self-test skips its GL render/layout VISUAL phases 1-15 (the GitHub Linux runner has no
# real GL -- under xvfb+llvmpipe the engine's materials/shaders/font atlas never reach "ready"), the
# crash handler is not installed (the job runs under ASan+UBSan, whose OWN SIGSEGV/SIGABRT handlers
# must not be overridden), and the file log is not installed (no writable app-data dir needed).
# Pinned OFF in the base preset (so a reused build dir cannot keep a stray ON in the CMake cache);
# only CI's job-local -D on the configure line turns it ON, exactly like the NTPACKER_GUI_DEV_SEAMS
# override in the perf-probes job -- so a local native-tests-debug run keeps the visual phases HARD
# on a real GPU and the shipped binary has ZERO env-controlled behaviour.
option(NTPACKER_GUI_HEADLESS_CI "CI headless build: skip the GL visual self-test phases, the crash handler and the file log" OFF)
if(NTPACKER_GUI_HEADLESS_CI)
    target_compile_definitions(ntpacker-gui PRIVATE NTPACKER_GUI_HEADLESS_CI=1)
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
tp_stage_format_packages(ntpacker-gui)
# Shipped Windows GUI is a windows-subsystem app (no console window behind the
# GUI; javaw pattern). Debug keeps the console -- logs, NT_ASSERT text and the
# self-test output stay visible in a terminal. Entry stays plain main().
if(WIN32 AND CMAKE_BUILD_TYPE STREQUAL "Release")
    set_target_properties(ntpacker-gui PROPERTIES WIN32_EXECUTABLE ON)
    target_link_options(ntpacker-gui PRIVATE "LINKER:/entry:mainCRTStartup")
endif()

tp_add_gui_test(NAME gui_edit_state
    SOURCES
        test_gui_edit_state.c
        gui_edit_state.c
    LIBS tp_core unity)

# Windows resolves argv through the shared UTF-8 process ingress, which the path
# helpers under test read back.
set(_tp_gui_paths_sources test_gui_paths.c gui_paths.c
    "${CMAKE_SOURCE_DIR}/apps/common/app_paths.c")
set(_tp_gui_paths_libs tp_core unity)
if(WIN32)
    list(APPEND _tp_gui_paths_sources
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
    list(APPEND _tp_gui_paths_libs shell32)
endif()
tp_add_gui_test(NAME gui_paths
    SOURCES ${_tp_gui_paths_sources}
    LIBS ${_tp_gui_paths_libs}
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/apps/common")

# POSIX shell-quote unit test (U-02): gui_shell_quote.h's gui_shell_squote() wraps a user-derived path
# as ONE single-quoted shell word (apostrophes -> '\''), the single source of truth for the system()-
# dispatched "open in the OS file manager" helpers. Header-only + POSIX-only, so the test body is
# #ifndef _WIN32; the local Windows build compiles a trivially-passing stub while Linux CI runs the real
# assertions under ASan+UBSan. Minimal deps -- only unity (the header pulls in nothing else).
tp_add_gui_test(NAME gui_shell_quote LIBS unity)

set(_tp_gui_headless_sources
    "${CMAKE_SOURCE_DIR}/apps/common/app_format_catalog.c"
    "${CMAKE_SOURCE_DIR}/apps/common/app_paths.c"
    gui_state.c
    gui_actions.c
    gui_actions_dialogs.c
    gui_actions_edits.c
    gui_actions_intents.c
    gui_actions_pack.c
    gui_actions_preview.c
    gui_actions_recovery.c
    gui_rows.c
    gui_project.c
    gui_project_file.c
    gui_project_mutations.c
    gui_project_recovery.c
    gui_project_operations.c
    gui_edit_state.c
    gui_canvas.c
    gui_canvas_resources.c
    gui_pack.c
    gui_pack_jobs.c
    gui_pack_preview.c
    gui_paths.c
)
if(WIN32)
    list(APPEND _tp_gui_headless_sources
        "${CMAKE_SOURCE_DIR}/apps/common/nt_utf8_argv.c")
endif()

# The headless harness link set and include path: the production GUI TUs above
# pull the engine UI stack even without a window, so every headless target links
# and includes exactly the same thing.
# The concrete impls for the engine's *_interface deps (nt_gfx, nt_http_stub,
# nt_fs, nt_clipboard) come LAST: their consumers reach them through interface
# targets, so CMake cannot see the dependency and emits the list as written --
# and an ELF linker never rescans an archive. tp_build's nt_builder closure can
# pull nt_resource objects late, so a stub listed before tp_build is a
# Linux/macOS undefined-reference (first caught by CI run 30392773049).
set(_tp_gui_headless_libs
    nt_core nt_app nt_window nt_input nt_math nt_log nt_time nt_resource
    nt_material nt_shared nt_atlas
    nt_sprite_renderer nt_text_renderer nt_font nt_ui
    nt_render nt_hash nt_mem_scratch nt_shape_renderer
    tp_build stb_image tinyfiledialogs tinycthread unity
    nt_gfx nt_http_stub nt_fs nt_clipboard)
set(_tp_gui_headless_includes
    "${CMAKE_SOURCE_DIR}/packer/src"
    "${CMAKE_SOURCE_DIR}/apps/common")

# Canonical GUI identity regression: two distinct sources may expose the same
# source-local key.  The headless target exercises the production row,
# selection/action and project/session adapters without creating a window.
# tp_job.c is rebuilt here with TP_ENABLE_TEST_SEAMS, and its worker-timeout
# seam lives in tp_job_worker.c, which the shipped library compiles seam-free.
tp_add_gui_test(NAME gui_canonical_identity
    SOURCES
        test_gui_canonical_identity.c
        "${CMAKE_SOURCE_DIR}/packer/src/tp_job.c"
        "${CMAKE_SOURCE_DIR}/packer/src/tp_job_worker.c"
        "${CMAKE_SOURCE_DIR}/packer/src/tp_job_worker_proto.c"
        ${_tp_gui_headless_sources}
    LIBS ${_tp_gui_headless_libs}
    INCLUDE_DIRS ${_tp_gui_headless_includes}
    SEAMS
    DEFINES
        TP_GUI_IDENTITY_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_gui_identity_tmp"
        TP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        _CRT_SECURE_NO_WARNINGS
        ${_tp_gui_introspect_defines})

# U-02 sprite-tree VIEW-layer regression: the filter/sort/collapse projection
# (s_view over s_rows) is a pure function of the cached row model.  Same headless
# harness + link set as the canonical-identity target -- no window, GL, or pack job.
tp_add_gui_test(NAME gui_view
    SOURCES
        test_gui_view.c
        "${CMAKE_SOURCE_DIR}/packer/tests/tp_format_catalog_test_seam.c"
        "${CMAKE_SOURCE_DIR}/packer/src/tp_scan.c"
        ${_tp_gui_headless_sources}
    LIBS ${_tp_gui_headless_libs}
    INCLUDE_DIRS ${_tp_gui_headless_includes}
    SEAMS
    DEFINES
        TP_GUI_VIEW_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_gui_view_tmp"
        _CRT_SECURE_NO_WARNINGS
        ${_tp_gui_introspect_defines})

# P1-10 action/state trace oracle.  Deliberately separate headless targets:
# decomposition of gui_actions/gui_project must preserve between-frame timing
# and state ownership without making the broader identity corpus absorb another
# responsibility.  One TU per responsibility -- drafts/conflicts, source
# refresh, jobs/lifecycle -- over one shared fixture TU, so a failure names the
# area it belongs to.  Each target gets its OWN TP_GUI_TRACE_TEST_DIR: the trace
# cases create and delete real files under it, and `ctest -j` runs the three
# binaries concurrently.
foreach(_trace_area draft refresh job)
    set(_trace_area_sources)
    if(_trace_area STREQUAL "refresh")
        list(APPEND _trace_area_sources
            "${CMAKE_SOURCE_DIR}/packer/src/tp_refresh_job.c")
    elseif(_trace_area STREQUAL "job")
        # The job trace re-execs itself as the outer worker. Compile the worker
        # service into this seam-enabled target so worker-side timing gates are
        # real test seams instead of depending on scheduler timing.
        list(APPEND _trace_area_sources
            "${CMAKE_SOURCE_DIR}/packer/src/tp_job_worker_main.c")
    endif()
    tp_add_gui_test(NAME gui_action_trace_${_trace_area}
        SOURCES
            test_gui_action_trace_${_trace_area}.c
            test_gui_action_trace_fixture.c
            "${CMAKE_SOURCE_DIR}/packer/src/tp_scan.c"
            "${CMAKE_SOURCE_DIR}/packer/src/tp_job.c"
            "${CMAKE_SOURCE_DIR}/packer/src/tp_job_worker.c"
            "${CMAKE_SOURCE_DIR}/packer/src/tp_job_worker_proto.c"
            ${_trace_area_sources}
            ${_tp_gui_headless_sources}
        LIBS ${_tp_gui_headless_libs}
        INCLUDE_DIRS ${_tp_gui_headless_includes}
        SEAMS
        DEFINES
            TP_GUI_TRACE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/tp_gui_trace_${_trace_area}_tmp"
            TP_FORMAT_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/packer/tests/fixtures/format-packages"
            TP_FORMAT_BUNDLED_ROOT="${CMAKE_SOURCE_DIR}/formats"
            TP_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
            _CRT_SECURE_NO_WARNINGS
            ${_tp_gui_introspect_defines})
endforeach()

# The replay binary is driven by the tp_client_parity_real ctest below, which is a
# cmake -P script (it runs the CLI and the replay exe against each other), so the
# executable registers no test of its own.
tp_add_gui_test(NAME client_parity_replay TARGET tp_client_parity_replay
    SOURCES
        client_parity_replay.c
        gui_project_operations.c
        "${CMAKE_SOURCE_DIR}/apps/common/app_format_catalog.c"
    LIBS tp_build
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/apps/common"
    SEAMS
    NO_CTEST)
tp_stage_format_packages(tp_client_parity_replay)

add_test(NAME tp_client_parity_real COMMAND ${CMAKE_COMMAND}
    -DNTPACKER=$<TARGET_FILE:ntpacker>
    -DREPLAY=$<TARGET_FILE:tp_client_parity_replay>
    -DSOURCE=${CMAKE_SOURCE_DIR}
    -DWORK=${CMAKE_CURRENT_BINARY_DIR}/tp_client_parity_real_tmp
    -P "${CMAKE_CURRENT_SOURCE_DIR}/client_parity_real.cmake")
set_tests_properties(tp_client_parity_real PROPERTIES TIMEOUT 180)

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
