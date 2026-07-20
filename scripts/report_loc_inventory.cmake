cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ROOT)
    get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

file(GLOB_RECURSE production_sources LIST_DIRECTORIES false
    "${ROOT}/packer/src/*.c"
    "${ROOT}/apps/*.c")

set(inventory "")
set(checked 0)
set(total_lines 0)
foreach(source IN LISTS production_sources)
    file(RELATIVE_PATH relative "${ROOT}" "${source}")
    string(REPLACE "\\" "/" relative "${relative}")
    if(relative MATCHES "/deps/|/generated/|/(test_[^/]*|[^/]*_test|tp_bench_[^/]*|gui_selftest|client_parity_(manifest|replay)|cli_[^/]*_payload_fixture|build_packs)\\.c$")
        continue()
    endif()

    file(READ "${source}" contents)
    string(REGEX MATCHALL "\n" line_breaks "${contents}")
    list(LENGTH line_breaks lines)
    if(NOT contents MATCHES "\n$")
        math(EXPR lines "${lines} + 1")
    endif()

    list(APPEND inventory "${relative}: ${lines} LOC")
    math(EXPR checked "${checked} + 1")
    math(EXPR total_lines "${total_lines} + ${lines}")
endforeach()

list(SORT inventory)
message(STATUS "Production LOC inventory (${checked} translation units, ${total_lines} total LOC)")
foreach(entry IN LISTS inventory)
    message(STATUS "  ${entry}")
endforeach()
