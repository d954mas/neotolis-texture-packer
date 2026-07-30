# apps/common: the small modules BOTH shipped clients need (UTF-8 process/FS
# ingress, the shared product version header, the app scratch-root policy).
#
# They are listed in each client's own source list rather than linked as a
# library: nt_utf8_argv.c is Windows-only and several client binaries pin a
# different CRT/seam set, so one archive would either be linked twice into a
# target that already compiled the TU or force every consumer onto one shape.
# This file therefore declares no product target -- only the tests.

set(_app_scratch_sources test_app_scratch.c app_scratch.c nt_utf8_fs.c)
set(_app_scratch_libs tp_core unity)
if(WIN32)
    # The scratch root reads %LOCALAPPDATA% through the UTF-16 -> UTF-8 ingress.
    list(APPEND _app_scratch_sources nt_utf8_argv.c)
    list(APPEND _app_scratch_libs shell32)
endif()
tp_add_test(NAME app_scratch
    SOURCES ${_app_scratch_sources}
    LIBS ${_app_scratch_libs}
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEFINES _CRT_SECURE_NO_WARNINGS
    SANITIZE
    TIMEOUT 30)
