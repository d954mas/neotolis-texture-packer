cmake_minimum_required(VERSION 3.25)
project(neotolis-texture-packer LANGUAGES C)

# Preset name for output layout (build/<area>/<target>/<preset>). The engine derives
# this in its own scope; set it at our root so our targets see it too.
if(NOT NT_PRESET_NAME)
    get_filename_component(NT_PRESET_NAME "${CMAKE_BINARY_DIR}" NAME)
endif()

# Engine submodule: libraries only (examples/tests/tools are top-level-guarded out).
add_subdirectory(external/neotolis-engine)

# nt_builder (packing pipeline) is a top-level-only engine target; the tool is
# native-only, so pull it in explicitly.
if(EMSCRIPTEN)
    message(FATAL_ERROR "neotolis-texture-packer is a native desktop tool; WASM builds are not supported.")
endif()
add_subdirectory(external/neotolis-engine/tools/builder nt_builder)

enable_testing()
add_subdirectory(apps/smoke)
