# Test-target declaration helper.
#
# Every first-party test target is the same handful of commands: an executable, a
# link line, private include dirs, the -U_DLL static-CRT pin that matches
# nt_builder, the engine warning flags AGENTS.md requires on all our targets, and
# the ctest registration. tp_add_test() owns that shape, so each target declares
# only what is genuinely specific to it and a new test cannot silently miss the
# warning flags or the CRT pin.
#
#   NAME <n>         identity: ctest `tp_<n>`, target `tp_test_<n>`, and the
#                    default source `test_<n>.c`
#   TARGET <t>       target-name override, for the bench/tool executables whose
#                    binary name is not derived from the test they register
#   SOURCES ...      full source list, replacing the derived `test_<n>.c`
#   LIBS ...         PRIVATE link libraries
#   PRIVATE_SRC      put the calling directory's sibling `../src` on the include
#                    path (packer/src private headers: seams, tp_hex.h)
#   INCLUDE_DIRS ... further PRIVATE include directories
#   SEAMS            compile with TP_ENABLE_TEST_SEAMS=1
#   DEFINES ...      further PRIVATE compile definitions
#   MAKE_DIRS ...    directories created at configure time (test scratch roots)
#   ARGS ...         arguments appended to the ctest COMMAND
#   TIMEOUT <s>      ctest TIMEOUT property
#   SANITIZE         also apply nt_set_sanitizer_flags (see tp_add_gui_test)
#   NO_CTEST         build the executable, register no test
function(tp_add_test)
    cmake_parse_arguments(PARSE_ARGV 0 T
        "SEAMS;NO_CTEST;PRIVATE_SRC;SANITIZE"
        "NAME;TARGET;TIMEOUT"
        "SOURCES;LIBS;INCLUDE_DIRS;DEFINES;MAKE_DIRS;ARGS")
    if(NOT T_NAME)
        message(FATAL_ERROR "tp_add_test: NAME is required")
    endif()
    if(T_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR
            "tp_add_test(${T_NAME}): unrecognized arguments: ${T_UNPARSED_ARGUMENTS}")
    endif()

    set(_target "tp_test_${T_NAME}")
    if(T_TARGET)
        set(_target "${T_TARGET}")
    endif()
    set(_sources "${T_SOURCES}")
    if(NOT _sources)
        set(_sources "test_${T_NAME}.c")
    endif()

    add_executable(${_target} ${_sources})
    if(T_LIBS)
        target_link_libraries(${_target} PRIVATE ${T_LIBS})
    endif()
    if(T_PRIVATE_SRC)
        target_include_directories(${_target} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    endif()
    if(T_INCLUDE_DIRS)
        target_include_directories(${_target} PRIVATE ${T_INCLUDE_DIRS})
    endif()
    if(T_SEAMS)
        target_compile_definitions(${_target} PRIVATE TP_ENABLE_TEST_SEAMS=1)
    endif()
    if(T_DEFINES)
        target_compile_definitions(${_target} PRIVATE ${T_DEFINES})
    endif()
    target_compile_options(${_target} PRIVATE -U_DLL)
    nt_set_warning_flags(${_target})   # AGENTS.md invariant: our test TUs too
    if(T_SANITIZE)
        nt_set_sanitizer_flags(${_target})
    endif()
    foreach(_dir IN LISTS T_MAKE_DIRS)
        file(MAKE_DIRECTORY "${_dir}")
    endforeach()
    if(NOT T_NO_CTEST)
        add_test(NAME "tp_${T_NAME}" COMMAND ${_target} ${T_ARGS})
        if(T_TIMEOUT)
            set_tests_properties("tp_${T_NAME}" PROPERTIES TIMEOUT ${T_TIMEOUT})
        endif()
    endif()
endfunction()

# GUI test targets always carry sanitizer instrumentation: apps/gui has no
# directory-wide sanitizer sweep like packer/tests does, because that directory
# also holds the shipped app, the vendored dialog library, and the pack builder.
# The wrapper is what makes "a GUI test is sanitized" a property of the mechanism
# instead of a keyword twelve call sites must remember.
function(tp_add_gui_test)
    tp_add_test(${ARGV} SANITIZE)
endfunction()
