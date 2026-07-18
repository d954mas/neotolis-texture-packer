cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ROOT)
    get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

set(default_hard_limit 800)

# Exact ratchets for pre-existing production translation units above the hard
# limit. Lower a value whenever a simplification commit shrinks that file.
set(legacy_limits
    "packer/src/tp_project.c=3387"
    "apps/gui/gui_actions.c=2477"
    "packer/src/tp_recovery.c=2004"
    "apps/gui/gui_project.c=1970"
    "apps/cli/cli_mutate.c=1805"
    "packer/src/tp_journal.c=1788"
    "packer/src/tp_txn_apply.c=1456"
    "packer/src/tp_validate.c=1341"
    "apps/gui/main.c=1340"
    "packer/src/tp_session_snapshot.c=1064"
    "apps/gui/gui_pack.c=1022"
    "apps/gui/gui_canvas.c=1004"
    "apps/gui/gui_view_settings.c=982"
    "packer/src/tp_session.c=933"
    "packer/src/tp_op_validate.c=927"
    "packer/src/tp_fs_internal.c=897"
    "packer/src/tp_history_codec.c=892"
    "packer/src/tp_txn_parse.c=830")

file(GLOB_RECURSE production_sources LIST_DIRECTORIES false
    "${ROOT}/packer/src/*.c"
    "${ROOT}/apps/*.c")

set(failures "")
set(checked 0)
foreach(source IN LISTS production_sources)
    file(RELATIVE_PATH relative "${ROOT}" "${source}")
    string(REPLACE "\\" "/" relative "${relative}")
    if(relative MATCHES "/deps/|/generated/|/(test_[^/]*|tp_bench_[^/]*|gui_selftest|build_packs)\\.c$")
        continue()
    endif()

    file(READ "${source}" contents)
    string(REGEX MATCHALL "\n" line_breaks "${contents}")
    list(LENGTH line_breaks lines)
    if(NOT contents MATCHES "\n$")
        math(EXPR lines "${lines} + 1")
    endif()

    set(limit "${default_hard_limit}")
    foreach(entry IN LISTS legacy_limits)
        string(REPLACE "=" ";" pair "${entry}")
        list(GET pair 0 legacy_path)
        list(GET pair 1 legacy_limit)
        if(relative STREQUAL legacy_path)
            set(limit "${legacy_limit}")
            break()
        endif()
    endforeach()

    if(lines GREATER limit)
        list(APPEND failures "${relative}: ${lines} LOC > ${limit}")
    endif()
    math(EXPR checked "${checked} + 1")
endforeach()

if(failures)
    list(JOIN failures "\n  " failure_text)
    message(FATAL_ERROR "Production LOC budget exceeded:\n  ${failure_text}")
endif()

message(STATUS "production LOC budget OK (${checked} translation units)")
