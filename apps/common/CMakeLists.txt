# apps/common: the small modules BOTH shipped clients need (UTF-8 process/FS
# ingress, the shared product version header, app scratch-root policy, and the
# process-owned format-catalog startup workflow).
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

set(_app_paths_sources test_app_paths.c app_paths.c)
set(_app_paths_libs tp_core unity)
if(WIN32)
    list(APPEND _app_paths_sources nt_utf8_argv.c)
    list(APPEND _app_paths_libs shell32)
endif()
tp_add_test(NAME app_paths
    SOURCES ${_app_paths_sources}
    LIBS ${_app_paths_libs}
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEFINES _CRT_SECURE_NO_WARNINGS
    SANITIZE
    TIMEOUT 30)

tp_add_test(NAME app_automation_policy
    SOURCES test_app_automation_policy.c app_automation_policy.c app_json.c nt_utf8_fs.c
    LIBS tp_core cjson unity
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEFINES
        APP_AUTOMATION_TEST_ROOT="${CMAKE_CURRENT_BINARY_DIR}/automation-policy-test"
        _CRT_SECURE_NO_WARNINGS
    SANITIZE
    TIMEOUT 30)

set(_app_format_catalog_test_root
    "${CMAKE_CURRENT_BINARY_DIR}/app_format_catalog_test")
set(_app_format_catalog_empty_root
    "${_app_format_catalog_test_root}/empty")
set(_app_format_catalog_missing_root
    "${_app_format_catalog_test_root}/missing")
set(_app_format_catalog_file_root
    "${_app_format_catalog_test_root}/not-a-directory")
file(REMOVE_RECURSE "${_app_format_catalog_test_root}")
file(MAKE_DIRECTORY "${_app_format_catalog_empty_root}")
file(WRITE "${_app_format_catalog_file_root}" "not a directory")
tp_add_test(NAME app_format_catalog
    SOURCES test_app_format_catalog.c app_format_catalog.c
    LIBS tp_build unity
    INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    DEFINES
        APP_FORMAT_CATALOG_EMPTY_ROOT="${_app_format_catalog_empty_root}"
        APP_FORMAT_CATALOG_MISSING_ROOT="${_app_format_catalog_missing_root}"
        APP_FORMAT_CATALOG_FILE_ROOT="${_app_format_catalog_file_root}"
        APP_FORMAT_CATALOG_FIXTURE_ROOT="${CMAKE_SOURCE_DIR}/packer/tests/fixtures/format-packages"
        _CRT_SECURE_NO_WARNINGS
    SANITIZE
    TIMEOUT 30)
