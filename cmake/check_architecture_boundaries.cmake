cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED ARCH_ROOT OR ARCH_ROOT STREQUAL "")
    message(FATAL_ERROR "ARCH_ROOT is required")
endif()

cmake_path(ABSOLUTE_PATH ARCH_ROOT NORMALIZE OUTPUT_VARIABLE _arch_root)
if(DEFINED ARCH_SCAN_ROOT AND NOT ARCH_SCAN_ROOT STREQUAL "")
    cmake_path(ABSOLUTE_PATH ARCH_SCAN_ROOT NORMALIZE
               OUTPUT_VARIABLE _arch_scan_root)
else()
    set(_arch_scan_root "${_arch_root}")
endif()

set(_arch_rules
    VIEW_ADMISSION
    VIEW_IO
    VIEW_PLATFORM
    VIEW_MODEL_POLICY
    CORE_FRONTEND
    ASYNC_RAW_SESSION)

foreach(_rule IN LISTS _arch_rules)
    set_property(GLOBAL PROPERTY "ARCH_HITS_${_rule}" "")
endforeach()

function(_arch_hit rule relative_path line_number)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    list(APPEND _hits "${relative_path}:${line_number}")
    set_property(GLOBAL PROPERTY "ARCH_HITS_${rule}" "${_hits}")
endfunction()

function(_arch_count_matches input_file regex out_count)
    file(READ "${input_file}" _content)
    string(REGEX MATCHALL "${regex}" _matches "${_content}")
    list(LENGTH _matches _count)
    set("${out_count}" "${_count}" PARENT_SCOPE)
endfunction()

file(GLOB_RECURSE _arch_sources LIST_DIRECTORIES false
    "${_arch_scan_root}/*.c"
    "${_arch_scan_root}/*.h")

foreach(_source IN LISTS _arch_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_scan_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)

    if(NOT DEFINED ARCH_EXPECT_RULE)
        if(_relative MATCHES "^(external|build)/"
           OR _relative MATCHES "^cmake/fixtures/"
           OR _relative MATCHES "/(deps|generated)/"
           OR _relative MATCHES "(^|/)(test_[^/]*|gui_selftest|tp_bench_[^/]*|client_parity_[^/]*)\\.(c|h)$")
            continue()
        endif()
    endif()

    set(_is_view false)
    if(_relative MATCHES "^apps/gui/gui_view_[^/]*\\.(c|h)$")
        set(_is_view true)
    endif()

    set(_is_core false)
    if(_relative MATCHES "^packer/(include/tp_core|src)/.*\\.(c|h)$")
        set(_is_core true)
    endif()

    set(_is_async false)
    if(_relative MATCHES "(^|/)[^/]*(worker|transport|dev[_-]?api|mcp|host_queue)[^/]*\\.(c|h)$")
        set(_is_async true)
    endif()
    # The GUI host admission owner borrows the live session only during its
    # host-thread drain call. Its no-retained-field rule and exclusive direct
    # admission symbols are pinned below; it is not background async code.
    if(_relative MATCHES "^apps/gui/gui_host_queue\\.(c|h)$")
        set(_is_async false)
    endif()

    file(STRINGS "${_source}" _lines)
    set(_line_number 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _line_number "${_line_number} + 1")
        string(STRIP "${_line}" _trimmed)
        if(_trimmed MATCHES "^//"
           OR _trimmed MATCHES "^/\\*"
           OR _trimmed MATCHES "^\\*")
            continue()
        endif()

        if(_is_view)
            if(_trimmed MATCHES "^#[ \t]*include[ \t]*[<\"](tp_core/tp_(session|job)\\.h|gui_project\\.h|gui_session_client\\.h)[>\"]"
               OR _trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_session_(apply|undo|redo|save|save_as|save_new|discard|invalidate_sources|require_recovery|pack_job_start|export_start|job_cancel|job_take_result)|gui_project_(new|open|save|save_as|discard|undo|redo|invalidate_sources)|gui_session_client_[A-Za-z0-9_]*)[ \t]*\\(")
                _arch_hit(VIEW_ADMISSION "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "^#[ \t]*include[ \t]*[<\"](tp_core/tp_(scan|image|input|pack_hash)\\.h|gui_scan\\.h)[>\"]"
               OR _trimmed MATCHES "(^|[^A-Za-z0-9_])(fopen|open|stat|opendir|readdir|FindFirstFile[A-Z]*|tp_scan_[A-Za-z0-9_]*|tp_image_[A-Za-z0-9_]*|tp_pack_input_[A-Za-z0-9_]*)[ \t]*\\(")
                _arch_hit(VIEW_IO "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "^#[ \t]*include[ \t]*[<\"](windows\\.h|clipboard/nt_clipboard\\.h|tinyfiledialogs\\.h)[>\"]"
               OR _trimmed MATCHES "(^|[^A-Za-z0-9_])(ShellExecute[A-Z]*|CreateProcess[A-Z]*|system|popen|nt_clipboard_[A-Za-z0-9_]*|tinyfd_[A-Za-z0-9_]*)[ \t]*\\(")
                _arch_hit(VIEW_PLATFORM "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "^#[ \t]*include[ \t]*[<\"]tp_core/tp_(model|project|operation|validate|client_capability|export)\\.h[>\"]"
               OR _trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_model_[A-Za-z0-9_]*|tp_project_[A-Za-z0-9_]*|tp_validate_[A-Za-z0-9_]*|tp_exporter_(count|at)|gui_pack_(preview_diff|result|find_sprite_ref))[ \t]*\\(")
                _arch_hit(VIEW_MODEL_POLICY "${_relative}" "${_line_number}")
            endif()
        endif()

        if(_is_core
           AND (_trimmed MATCHES "^#[ \t]*include[ \t]*[<\"][^>\"]*(apps/|gui_|cli_|mcp_|devapi_|dev_api_|jsonrpc|transport)"
                OR _trimmed MATCHES "(^|[^A-Za-z0-9_])(gui_|cli_|mcp_|devapi_)[A-Za-z0-9_]*[ \t]*\\("))
            _arch_hit(CORE_FRONTEND "${_relative}" "${_line_number}")
        endif()

        if(_is_async
           AND _trimmed MATCHES "(^|[^A-Za-z0-9_])((const[ \t]+)?tp_session)[ \t]*\\*[ \t]*[A-Za-z_][A-Za-z0-9_]*")
            _arch_hit(ASYNC_RAW_SESSION "${_relative}" "${_line_number}")
        endif()
    endforeach()
endforeach()

if(DEFINED ARCH_EXPECT_RULE AND NOT ARCH_EXPECT_RULE STREQUAL "")
    if(NOT ARCH_EXPECT_RULE IN_LIST _arch_rules)
        message(FATAL_ERROR "unknown ARCH_EXPECT_RULE=${ARCH_EXPECT_RULE}")
    endif()
    foreach(_rule IN LISTS _arch_rules)
        get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${_rule}")
        list(LENGTH _hits _count)
        if(_rule STREQUAL ARCH_EXPECT_RULE)
            if(NOT _count EQUAL 1)
                message(FATAL_ERROR
                    "${_rule} negative fixture expected exactly one hit; got ${_count}: ${_hits}")
            endif()
        elseif(NOT _count EQUAL 0)
            message(FATAL_ERROR
                "${ARCH_EXPECT_RULE} fixture unexpectedly hit ${_rule}: ${_hits}")
        endif()
    endforeach()
    message(STATUS "architecture negative fixture detected ${ARCH_EXPECT_RULE}")
    return()
endif()

function(_arch_assert_rule rule expected_count remove_in)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    list(LENGTH _hits _count)
    if(NOT _count EQUAL expected_count)
        message(FATAL_ERROR
            "${rule} debt changed: expected ${expected_count} hit(s) owned by ${remove_in}, got ${_count}: ${_hits}")
    endif()
    set(_allowed_paths ${ARGN})
    foreach(_hit IN LISTS _hits)
        set(_allowed false)
        foreach(_allowed_path IN LISTS _allowed_paths)
            if(_hit MATCHES "^${_allowed_path}:[0-9]+$")
                set(_allowed true)
            endif()
        endforeach()
        if(NOT _allowed)
            message(FATAL_ERROR
                "${rule} new violation outside ${remove_in} exact debt paths: ${_hit}")
        endif()
    endforeach()
endfunction()

_arch_assert_rule(VIEW_ADMISSION 0 "R2c/R2d")
_arch_assert_rule(VIEW_IO 0 "SR-BASE/PV-tree-list")
_arch_assert_rule(VIEW_PLATFORM 10 "PLATFORM-SEAM/PV-chrome"
                  "apps/gui/gui_view_chrome\\.c")
_arch_assert_rule(VIEW_MODEL_POLICY 20 "PV-settings/RESULT-INDEX"
                  "apps/gui/gui_view_canvas\\.c"
                  "apps/gui/gui_view_chrome\\.c"
                  "apps/gui/gui_view_settings\\.c")
_arch_assert_rule(CORE_FRONTEND 0 "R1a/R1b")
_arch_assert_rule(ASYNC_RAW_SESSION 0 "R1c/R2b")

function(_arch_assert_symbol relative_path symbol expected_count remove_in)
    set(_path "${_arch_root}/${relative_path}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "${remove_in} manifest owner disappeared before cutover: ${relative_path}")
    endif()
    file(STRINGS "${_path}" _symbol_lines)
    set(_count 0)
    foreach(_line IN LISTS _symbol_lines)
        string(STRIP "${_line}" _trimmed)
        if(_trimmed MATCHES "^//"
           OR _trimmed MATCHES "^/\\*"
           OR _trimmed MATCHES "^\\*")
            continue()
        endif()
        string(REPLACE ";" " " _match_line "${_line}")
        string(REGEX MATCHALL
            "(^|[^A-Za-z0-9_])${symbol}([^A-Za-z0-9_]|$)"
            _matches "${_match_line}")
        list(LENGTH _matches _line_count)
        math(EXPR _count "${_count} + ${_line_count}")
    endforeach()
    if(NOT _count EQUAL expected_count)
        message(FATAL_ERROR
            "${remove_in} manifest ${relative_path}::${symbol} expected ${expected_count}, got ${_count}")
    endif()
endfunction()

function(_arch_assert_function_symbol relative_path function_name symbol
         expected_count remove_in)
    set(_path "${_arch_root}/${relative_path}")
    file(STRINGS "${_path}" _lines)
    set(_active false)
    set(_opened false)
    set(_depth 0)
    set(_count 0)
    foreach(_line IN LISTS _lines)
        if(NOT _active
           AND _line MATCHES "^[A-Za-z_][A-Za-z0-9_ \t*]*[ \t]${function_name}[ \t]*\\(")
            set(_active true)
        endif()
        if(_active)
            string(STRIP "${_line}" _trimmed)
            if(NOT (_trimmed MATCHES "^//"
                    OR _trimmed MATCHES "^/\\*"
                    OR _trimmed MATCHES "^\\*"))
                string(REPLACE ";" " " _match_line "${_line}")
                string(REGEX MATCHALL
                    "(^|[^A-Za-z0-9_])${symbol}([^A-Za-z0-9_]|$)"
                    _matches "${_match_line}")
                list(LENGTH _matches _line_count)
                math(EXPR _count "${_count} + ${_line_count}")
            endif()
            string(REGEX MATCHALL "\\{" _opens "${_line}")
            string(REGEX MATCHALL "\\}" _closes "${_line}")
            list(LENGTH _opens _open_count)
            list(LENGTH _closes _close_count)
            if(_open_count GREATER 0)
                set(_opened true)
            endif()
            math(EXPR _depth "${_depth} + ${_open_count} - ${_close_count}")
            if(_opened AND _depth EQUAL 0)
                set(_active false)
                break()
            endif()
        endif()
    endforeach()
    if(NOT _count EQUAL expected_count)
        message(FATAL_ERROR
            "${remove_in} manifest ${relative_path}::${function_name}/${symbol} expected ${expected_count}, got ${_count}")
    endif()
endfunction()

# Deferred presentation debts are pinned by exact token as well as by the
# generic detector above. Replacing one forbidden call with another cannot keep
# the gate green merely because the line count stayed constant.
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "nt_clipboard_available" 1
                    "PLATFORM-SEAM/PV-chrome")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "nt_clipboard_set_text" 1
                    "PLATFORM-SEAM/PV-chrome")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "ShellExecuteA" 1
                    "PLATFORM-SEAM/PV-chrome")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "ShellExecuteW" 1
                    "PLATFORM-SEAM/PV-chrome")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "system" 4
                    "PLATFORM-SEAM/PV-chrome")
_arch_assert_symbol("apps/gui/gui_view_canvas.c" "tp_exporter_count" 1
                    "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_view_canvas.c" "tp_exporter_at" 3
                    "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_view_canvas.c" "gui_pack_preview_diff" 1
                    "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_view_canvas.c" "gui_pack_result" 1
                    "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "tp_exporter_count" 1
                    "PV-chrome/dialogs")
_arch_assert_symbol("apps/gui/gui_view_chrome.c" "tp_exporter_at" 1
                    "PV-chrome/dialogs")
_arch_assert_symbol("apps/gui/gui_view_settings.c" "tp_exporter_count" 1
                    "PV-settings")
_arch_assert_symbol("apps/gui/gui_view_settings.c" "tp_exporter_at" 3
                    "PV-settings")
_arch_assert_symbol("apps/gui/gui_view_settings.c"
                    "tp_project_sprite_effective_shape" 1 "PV-settings")
_arch_assert_symbol("apps/gui/gui_view_settings.c"
                    "tp_validate_session_snapshot_target" 1 "PV-settings")
_arch_assert_symbol("apps/gui/gui_view_settings.c" "gui_pack_result" 1
                    "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_view_settings.c"
                    "gui_pack_find_sprite_ref" 1 "RESULT-INDEX")
_arch_assert_symbol("apps/gui/gui_rows.h" "tp_scan\\.h" 1
                    "SR-BASE/PV-tree-list")

# tp_job.c is the host-side process owner and does not match the filename-based
# async scan. Pin the completed R1c deletion explicitly so a raw session alias
# or an in-process thread authority cannot be reintroduced there.
set(_tp_job "${_arch_root}/packer/src/tp_job.c")
file(STRINGS "${_tp_job}" _tp_job_session_field_lines
     REGEX "^[ \t]+tp_session[ \t]*\\*[ \t]*session;")
list(LENGTH _tp_job_session_field_lines _tp_job_session_fields)
if(NOT _tp_job_session_fields EQUAL 0)
    message(FATAL_ERROR
        "R1c raw-session deletion regressed: expected 0, got ${_tp_job_session_fields}")
endif()

set(_gui_pack_jobs "${_arch_root}/apps/gui/gui_pack_jobs.c")
_arch_count_matches(
    "${_gui_pack_jobs}"
    "tp_session[ \t]*\\*"
    _gui_pack_raw_sessions)
if(NOT _gui_pack_raw_sessions EQUAL 0)
    message(FATAL_ERROR
        "R2b raw-session deletion regressed: expected 0, got ${_gui_pack_raw_sessions}")
endif()

# Completed R1c deletion manifest. These zero-count pins make the cutover
# irreversible without an explicit architecture decision.
foreach(_entry IN ITEMS
        "thrd_t" "thrd_create" "thrd_join" "job_join"
        "pack_worker" "export_worker" "job_start_thread"
        "job_thread_start" "job_thread_start_context"
        "fail_next_thread_create")
    _arch_assert_symbol("packer/src/tp_job.c" "${_entry}" 0
                        "R1c completed")
endforeach()
_arch_assert_symbol("apps/gui/gui_pack_jobs.c" "gui_pack_shutdown" 1 "R2b completed")
_arch_assert_symbol("apps/gui/gui_pack_jobs.c" "wait_for_job" 0 "R2b completed")
_arch_assert_symbol("apps/gui/main.c" "gui_pack_shutdown" 1 "R2b completed")

foreach(_symbol IN ITEMS
        job_session
        tp_session_job_active
        tp_session_job_poll
        tp_session_job_take_result
        tp_session_job_cancel
        tp_session_pack_job_start
        tp_session_export_start)
    _arch_assert_symbol("apps/gui/gui_pack_jobs.c" "${_symbol}" 0 "R2b completed")
endforeach()
_arch_assert_symbol("apps/gui/gui_project.c"
                    "gui_project_session_for_jobs" 0 "R2b completed")
_arch_assert_symbol("apps/gui/gui_project.h"
                    "gui_project_session_for_jobs" 0 "R2b completed")
_arch_assert_symbol("apps/gui/gui_actions.c"
                    "gui_project_session_for_jobs" 0 "R2b completed")
_arch_assert_symbol("apps/gui/gui_actions.c"
                    "s_refresh_fingerprint_session" 0 "R2b completed")
_arch_assert_symbol("apps/gui/main.c"
                    "gui_pack_worker_active" 0 "R2b completed")
_arch_assert_symbol("apps/gui/main.c"
                    "gui_pack_poll" 0 "R2b completed")

foreach(_entry IN ITEMS
        "tp_session_pack_job_start|1"
        "tp_session_export_start|1"
        "tp_session_job_poll|1"
        "tp_session_job_take_result|1"
        "tp_session_job_cancel|1")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _symbol)
    list(GET _parts 1 _count)
    _arch_assert_symbol("apps/gui/gui_host_queue.c"
                        "${_symbol}" "${_count}" "R2b single host owner")
endforeach()
file(GLOB _gui_host_owned_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/gui*.c"
    "${_arch_root}/apps/gui/main.c")
foreach(_source IN LISTS _gui_host_owned_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_host_queue.c")
        continue()
    endif()
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    foreach(_symbol IN ITEMS
            tp_session_pack_job_start
            tp_session_export_start
            tp_session_job_active
            tp_session_job_poll
            tp_session_job_take_result
            tp_session_job_cancel)
        _arch_assert_symbol(
            "${_relative}" "${_symbol}" 0
            "R2b single GUI host admission owner")
    endforeach()
endforeach()
_arch_assert_symbol(
    "apps/gui/gui_host_queue.h"
    "tp_session[ \t]*\\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;"
    0 "R2b no retained raw session")
_arch_count_matches("${_arch_root}/apps/gui/CMakeLists.txt"
                    "(^|[ \t(])gui_host_queue\\.c([ \t\r\n)]|$)"
                    _host_queue_source_registrations)
if(NOT _host_queue_source_registrations EQUAL 5)
    message(FATAL_ERROR
        "R2b gui_host_queue.c registrations expected 5, got ${_host_queue_source_registrations}")
endif()

# Completed R2a deletion manifest. GUI observation and immutable snapshot
# lifetime have one owner; presentation does not assemble or refresh snapshots.
foreach(_entry IN ITEMS
        "apps/gui/gui_project.c|tp_session_snapshot_create"
        "apps/gui/gui_project.c|tp_session_snapshot_destroy"
        "apps/gui/gui_project.c|tp_session_observe"
        "apps/gui/gui_project_file.c|tp_session_observe"
        "apps/gui/gui_project_internal.h|tp_session_snapshot[ \t]*\\*[ \t]*snapshot"
        "apps/gui/gui_project_internal.h|snapshot_lifetime_generation"
        "apps/gui/gui_rows.c|tp_session_snapshot_source_generation"
        "apps/gui/gui_project_file.c|recompute_name")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _path)
    list(GET _parts 1 _symbol)
    _arch_assert_symbol("${_path}" "${_symbol}" 0 "R2a completed")
endforeach()
_arch_assert_symbol("apps/gui/gui_session_client.c"
                    "tp_session_observe" 1 "R2a completed")

file(GLOB _gui_shipping_observation_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/*.c"
    "${_arch_root}/apps/gui/*.h")
foreach(_source IN LISTS _gui_shipping_observation_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST
               _relative NORMALIZE)
    if(_relative MATCHES
       "^apps/gui/(gui_session_client|gui_selftest|tp_bench_[^/]*|test_[^/]*|client_parity_[^/]*)\\.(c|h)$")
        continue()
    endif()
    foreach(_symbol IN ITEMS
            tp_session_observe
            tp_session_observation_destroy
            tp_session_snapshot_create
            tp_session_snapshot_destroy)
        _arch_assert_symbol(
            "${_relative}" "${_symbol}" 0
            "R2a single GUI observation owner")
    endforeach()
endforeach()

_arch_count_matches("${_arch_root}/apps/gui/CMakeLists.txt"
                    "(^|[ \t(])gui_session_client\\.c([ \t\r\n)]|$)"
                    _client_source_registrations)
if(NOT _client_source_registrations EQUAL 8)
    message(FATAL_ERROR
        "R2c manifest gui_session_client.c registrations expected 8, got ${_client_source_registrations}")
endif()

_arch_assert_symbol("apps/gui/gui_session_adapter.c" "tp_session_apply" 0 "R2c complete")
_arch_assert_symbol("apps/gui/gui_session_adapter.c" "\"project\\.edit\"" 0 "R2c complete")
_arch_assert_symbol("apps/gui/gui_session_client.c" "tp_session_apply" 1 "R2c owner")
_arch_assert_symbol("apps/gui/gui_session_adapter.h" "tp_session[ \t]*\\*" 0 "R2c complete")
foreach(_entry IN ITEMS
        "apps/gui/gui_project.c|gui_project__refresh_after_session_commit|0"
        "apps/gui/gui_project_pending.c|gui_project__refresh_after_session_commit|0"
        "apps/gui/gui_project_mutations.c|gui_project__refresh_after_session_commit|0"
        "apps/gui/gui_project_internal.h|txn_seq|0"
        "apps/gui/gui_project.c|gui_project__next_transaction_id|0"
        "apps/gui/gui_project_pending.c|gui_project__next_transaction_id|0"
        "apps/gui/gui_project_mutations.c|gui_project__next_transaction_id|0")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _path)
    list(GET _parts 1 _symbol)
    list(GET _parts 2 _count)
    _arch_assert_symbol("${_path}" "${_symbol}" "${_count}" "R2c complete")
endforeach()
_arch_assert_function_symbol(
    "apps/gui/gui_project.c" "gui_project__snapshot_drop"
    "gui_project__snapshot_drop" 1 "SR-BASE")
_arch_assert_function_symbol(
    "apps/gui/gui_project.c" "gui_project_invalidate_sources"
    "gui_project__snapshot_drop" 1 "SR-BASE")
foreach(_function IN ITEMS gui_project_undo gui_project_redo gui_project_save
                           gui_project_save_as)
    _arch_assert_function_symbol("apps/gui/gui_project_file.c" "${_function}"
                                 "gui_project__snapshot_drop" 0 "R2c complete")
    _arch_assert_function_symbol("apps/gui/gui_project_file.c" "${_function}"
                                 "gui_project__sync_recovery_notice" 0 "R2c complete")
endforeach()
foreach(_path IN ITEMS apps/gui/gui_view_chrome.c apps/gui/main.c)
    _arch_assert_symbol("${_path}" "do_undo" 0 "R2c complete")
    _arch_assert_symbol("${_path}" "do_redo" 0 "R2c complete")
endforeach()

file(READ "${_arch_root}/apps/gui/main.c" _main_source)
string(FIND "${_main_source}" "gui_bench_tick();" _bench_tick_position)
string(FIND "${_main_source}" "gui_project_frame_begin(" _frame_begin_position)
if(_bench_tick_position LESS 0 OR _frame_begin_position LESS 0 OR
   _bench_tick_position GREATER _frame_begin_position)
    message(FATAL_ERROR
        "R2c benchmark ingress must execute before the frame observation is pinned")
endif()

_arch_assert_symbol("apps/gui/gui_project_file.c" "install_session" 3 "R2d")
foreach(_function IN ITEMS install_session fresh_init
                           gui_project_shutdown gui_project_new
                           gui_project_open)
    _arch_assert_function_symbol("apps/gui/gui_project_file.c" "${_function}"
                                 "gui_project__snapshot_drop" 1 "R2d")
endforeach()
_arch_assert_function_symbol("apps/gui/gui_project_file.c" "install_session"
                             "s_project\\.session[ \t]*=" 1 "R2d")
_arch_assert_function_symbol("apps/gui/gui_project_file.c"
                             "gui_project_shutdown"
                             "s_project\\.session[ \t]*=" 1 "R2d")
_arch_assert_symbol("apps/gui/gui_project_file.c" "tp_session_destroy" 4 "R2d")
_arch_assert_symbol("apps/gui/gui_project_file.c" "tp_session_discard" 2 "R2d")
_arch_assert_function_symbol("apps/gui/gui_project_file.c" "gui_project_new"
                             "gui_project_pending_discard" 1 "R2d")
_arch_assert_function_symbol("apps/gui/gui_project_file.c" "gui_project_open"
                             "gui_project_pending_discard" 1 "R2d")
foreach(_symbol IN ITEMS request_new request_open request_exit)
    _arch_assert_symbol("apps/gui/gui_view_chrome.c" "${_symbol}" 1 "R2d")
endforeach()
_arch_assert_symbol("apps/gui/gui_actions_dialogs.c" "gui_project_new" 2 "R2d")
_arch_assert_symbol("apps/gui/gui_actions_dialogs.c" "gui_project_open" 1 "R2d")
_arch_assert_symbol("apps/gui/main.c" "gui_project_open" 1 "R2d")
_arch_assert_symbol("apps/gui/main.c" "gui_project_shutdown" 1 "R2d")

foreach(_symbol IN ITEMS pending_valid pending_key pending_op pending_time
                         pending_expected_revision
                         pending_preview_stale_before)
    _arch_assert_symbol("apps/gui/gui_project_internal.h" "${_symbol}" 1 "R3d")
endforeach()
foreach(_entry IN ITEMS
        "gui_project_flush_pending|3" "gui_project_pending_route|1"
        "gui_project_pending_offer|1" "gui_project_peek_pending_slice9|1"
        "gui_project_flush_elapsed|1" "gui_project_pending_discard|1")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _symbol)
    list(GET _parts 1 _count)
    _arch_assert_symbol("apps/gui/gui_project_pending.c"
                        "${_symbol}" "${_count}" "R3d")
endforeach()
_arch_assert_symbol("apps/gui/gui_actions_dialogs.c"
                    "gui_actions__flush_failed" 6 "R3d")
foreach(_entry IN ITEMS
        "apps/gui/gui_actions_dialogs.c|gui_project_flush_pending|2"
        "apps/gui/gui_actions.c|gui_project_flush_pending|1"
        "apps/gui/gui_project_mutations.c|gui_project_flush_pending|17"
        "apps/gui/gui_project_file.c|gui_project_flush_pending|5"
        "apps/gui/gui_project.h|gui_project_flush_pending|1"
        "apps/gui/gui_project_mutations.c|gui_project_pending_route|8"
        "apps/gui/gui_project_internal.h|gui_project_pending_route|1"
        "apps/gui/gui_project_mutations.c|gui_project_pending_offer|4"
        "apps/gui/gui_project_internal.h|gui_project_pending_offer|1"
        "apps/gui/main.c|gui_project_peek_pending_slice9|1"
        "apps/gui/gui_project.h|gui_project_peek_pending_slice9|1"
        "apps/gui/main.c|gui_project_flush_elapsed|1"
        "apps/gui/gui_project.h|gui_project_flush_elapsed|1"
        "apps/gui/gui_project_file.c|gui_project_pending_discard|4"
        "apps/gui/gui_project_internal.h|gui_project_pending_discard|1")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _path)
    list(GET _parts 1 _symbol)
    list(GET _parts 2 _count)
    _arch_assert_symbol("${_path}" "${_symbol}" "${_count}" "R3d")
endforeach()
_arch_count_matches("${_arch_root}/apps/gui/CMakeLists.txt"
                    "gui_project_pending\\.c" _pending_source_registrations)
if(NOT _pending_source_registrations EQUAL 4)
    message(FATAL_ERROR
        "R3d manifest gui_project_pending.c registrations expected 4, got ${_pending_source_registrations}")
endif()
foreach(_symbol IN ITEMS s_edit_kind s_edit_atlas s_edit_anim
                         s_edit_sprite s_edit_buf)
    _arch_assert_symbol("apps/gui/gui_state.c" "${_symbol}" 1 "R3d")
    _arch_assert_symbol("apps/gui/gui_state.h" "${_symbol}" 1 "R3d")
endforeach()
foreach(_symbol IN ITEMS edit_atlas_id edit_atlas_revision edit_anim_atlas_id
                         edit_anim_id edit_anim_revision edit_sprite_atlas_id
                         edit_sprite_source_id edit_sprite_revision
                         edit_sprite_source_key)
    _arch_assert_symbol("apps/gui/gui_actions_internal.h" "${_symbol}" 1 "R3d")
endforeach()
foreach(_entry IN ITEMS
        "s_nb_pad|3" "s_nb_margin|3" "s_nb_extrude|3" "s_nb_maxv|3"
        "s_nb_ppu|3" "s_nb_alpha|3" "s_nb_ox|3" "s_nb_oy|3"
        "s_nb_s9|2" "s_nb_ov_margin|3" "s_nb_ov_extrude|3"
        "s_nb_target_path|5" "s_nb_anim_fps|3")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _symbol)
    list(GET _parts 1 _count)
    _arch_assert_symbol("apps/gui/gui_view_settings.c"
                        "${_symbol}" "${_count}" "R3d")
endforeach()

foreach(_symbol IN ITEMS s_sel_atlas s_sel_src s_sel_child s_sel_anim
                         s_sel_anim_frame s_sel_anchor_row s_focus_view
                         s_sel_abs s_sel_missing s_reselect_pending
                         s_reselect_source_id s_reselect_key
                         s_reselect_atlas_id)
    _arch_assert_symbol("apps/gui/gui_state.c" "${_symbol}" 1 "R4")
endforeach()
foreach(_entry IN ITEMS
        "s_reselect_pending|3" "s_reselect_source_id|5"
        "s_reselect_key|7" "s_reselect_atlas_id|2")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _symbol)
    list(GET _parts 1 _count)
    _arch_assert_symbol("apps/gui/gui_rows.c" "${_symbol}" "${_count}" "R4")
endforeach()

message(STATUS "architecture boundaries and debt counts are stable")
