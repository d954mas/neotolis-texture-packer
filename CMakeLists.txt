cmake_minimum_required(VERSION 3.25)
project(neotolis-texture-packer LANGUAGES C)

# Preset name for output layout (build/<area>/<target>/<preset>). The engine derives
# this in its own scope; set it at our root so our targets see it too.
if(NOT NT_PRESET_NAME)
    get_filename_component(NT_PRESET_NAME "${CMAKE_BINARY_DIR}" NAME)
endif()

# ccache: transparently cache compiles when it's installed (local dev + CI), set BEFORE the
# engine subdir so both trees inherit it. No-op when ccache isn't on PATH, so a clean-clone
# build never depends on it; the NOT-already-set guard leaves CI's env-exported launcher alone.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM AND NOT CMAKE_C_COMPILER_LAUNCHER)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# Universal-packer max page size (owner 2026-07-10): lift the engine's mobile-safe
# 4096 default to 16384. NT_BUILD_MAX_TEXTURE_SIZE is #ifndef-guarded in nt_builder.h,
# so defining it here BEFORE any add_subdirectory makes every TU -- engine builder AND
# tp_core/tp_build (which read it via TP_PACK_MAX_PAGE_DIM) -- agree on one value. The
# .ntpack format is unaffected (source dims are u16 <= 65535; UVs are normalized u16).
add_compile_definitions(NT_BUILD_MAX_TEXTURE_SIZE=16384)

# Engine submodule: libraries only (examples/tests/tools are top-level-guarded out).
add_subdirectory(external/neotolis-engine)

# nt_builder (packing pipeline) is a top-level-only engine target; the tool is
# native-only, so pull it in explicitly.
if(EMSCRIPTEN)
    message(FATAL_ERROR "neotolis-texture-packer is a native desktop tool; WASM builds are not supported.")
endif()
add_subdirectory(external/neotolis-engine/tools/builder nt_builder)

enable_testing()
add_custom_target(report_loc_inventory
    COMMAND ${CMAKE_COMMAND}
        -DROOT=${CMAKE_SOURCE_DIR}
        -P ${CMAKE_SOURCE_DIR}/scripts/report_loc_inventory.cmake
    COMMENT "Reporting production translation-unit LOC (non-gating)"
    VERBATIM)
add_subdirectory(apps/smoke)
add_subdirectory(packer)
# apps/gui and apps/cli link tp_core/tp_build (packer/), so add them after.
add_subdirectory(apps/gui)
add_subdirectory(apps/cli)
