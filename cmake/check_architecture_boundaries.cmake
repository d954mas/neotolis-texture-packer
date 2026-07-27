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
    ASYNC_RAW_SESSION
    HOST_QUEUE_RAW_SESSION_STORAGE)

foreach(_rule IN LISTS _arch_rules)
    set_property(GLOBAL PROPERTY "ARCH_HITS_${_rule}" "")
endforeach()

function(_arch_hit rule relative_path line_number)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    list(APPEND _hits "${relative_path}:${line_number}")
    set_property(GLOBAL PROPERTY "ARCH_HITS_${rule}" "${_hits}")
endfunction()

if(DEFINED ARCH_EXPECT_RULE)
    file(GLOB_RECURSE _arch_sources LIST_DIRECTORIES false
        "${_arch_scan_root}/*.c"
        "${_arch_scan_root}/*.h")
else()
    file(GLOB_RECURSE _arch_sources LIST_DIRECTORIES false
        "${_arch_root}/apps/*.c"
        "${_arch_root}/apps/*.h"
        "${_arch_root}/packer/*.c"
        "${_arch_root}/packer/*.h")
endif()

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
    # gui_host_queue borrows the active session only during host-thread drain.
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
            # Match the symbol token, not the opening parenthesis, so a
            # multiline call cannot bypass the boundary.
            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_session_(apply|undo|redo|save|save_as|save_new|discard|invalidate_sources|require_recovery|pack_job_start|export_start|job_cancel|job_take_result)|gui_project_(new|open|save|save_as|discard|undo|redo|invalidate_sources)|gui_session_client_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_ADMISSION "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_scan_[A-Za-z0-9_]*|tp_image_[A-Za-z0-9_]*|tp_pack_input_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_IO "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(ShellExecute[A-Z]*|CreateProcess[A-Z]*|nt_clipboard_[A-Za-z0-9_]*|tinyfd_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_PLATFORM "${_relative}" "${_line_number}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_model_[A-Za-z0-9_]*|tp_project_[A-Za-z0-9_]*|tp_validate_[A-Za-z0-9_]*|tp_exporter_(count|at)|gui_pack_(preview_diff|result|find_sprite_ref))([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_MODEL_POLICY "${_relative}" "${_line_number}")
            endif()
        endif()
    endforeach()

    # Directives, calls, and declarations are scanned as normalized whole-file
    # text so line continuations cannot weaken a dependency boundary.
    file(READ "${_source}" _scan)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _scan "${_scan}")
    string(REGEX REPLACE "//[^\r\n]*" " " _scan "${_scan}")
    set(_directives "${_scan}")
    string(REGEX REPLACE "\\\\[ \t]*[\r\n]+" "" _directives "${_directives}")
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](tp_core/tp_(session|job)\\.h|gui_project\\.h|gui_session_client\\.h)[>\"]")
        _arch_hit(VIEW_ADMISSION "${_relative}" "0")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](tp_core/tp_(scan|image|input|pack_hash)\\.h|gui_scan\\.h)[>\"]")
        _arch_hit(VIEW_IO "${_relative}" "0")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](windows\\.h|clipboard/nt_clipboard\\.h|tinyfiledialogs\\.h)[>\"]")
        _arch_hit(VIEW_PLATFORM "${_relative}" "0")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"]tp_core/tp_(model|project|operation|validate|client_capability|export)\\.h[>\"]")
        _arch_hit(VIEW_MODEL_POLICY "${_relative}" "0")
    endif()
    if(_is_core
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*(apps/|gui_|cli_|mcp_|devapi_|dev_api_|jsonrpc|transport)")
        _arch_hit(CORE_FRONTEND "${_relative}" "0")
    endif()

    # String literals are irrelevant to symbol/call checks below.
    string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" _scan "${_scan}")
    if(_is_view
       AND _scan MATCHES "(^|[^A-Za-z0-9_])(fopen|open|stat|opendir|readdir|FindFirstFile[A-Z]*)[ \t\r\n]*\\(")
        _arch_hit(VIEW_IO "${_relative}" "0")
    endif()
    if(_is_view
       AND _scan MATCHES "(^|[^A-Za-z0-9_])(system|popen)[ \t\r\n]*\\(")
        _arch_hit(VIEW_PLATFORM "${_relative}" "0")
    endif()
    if(_is_core
       AND _scan MATCHES "(^|[^A-Za-z0-9_])(gui_|cli_|mcp_|devapi_)[A-Za-z0-9_]*[ \t\r\n]*\\(")
        _arch_hit(CORE_FRONTEND "${_relative}" "0")
    endif()
    if(_is_async
       AND _scan MATCHES "(^|[^A-Za-z0-9_])((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*")
        _arch_hit(ASYNC_RAW_SESSION "${_relative}" "0")
    endif()
    if(_relative MATCHES "^apps/gui/gui_host_queue\\.(c|h)$"
       AND _scan MATCHES "typedef[ \t\r\n]+struct[ \t\r\n]+gui_host_queue[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*;")
        _arch_hit(
            HOST_QUEUE_RAW_SESSION_STORAGE
            "${_relative}" "0")
    endif()
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

# A rule may have temporary debt only in the named production paths. There are
# deliberately no positive occurrence counts: deleting debt must stay green.
function(_arch_assert_rule rule remove_in)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    set(_allowed_paths ${ARGN})
    foreach(_hit IN LISTS _hits)
        set(_allowed false)
        foreach(_allowed_path IN LISTS _allowed_paths)
            if(_hit MATCHES "^${_allowed_path}:[0-9]+$")
                set(_allowed true)
                break()
            endif()
        endforeach()
        if(NOT _allowed)
            message(FATAL_ERROR
                "${rule} violation outside ${remove_in} debt paths: ${_hit}")
        endif()
    endforeach()
endfunction()

_arch_assert_rule(VIEW_ADMISSION "R2c/R2d")
_arch_assert_rule(VIEW_IO "SR-BASE/PV-tree-list")
_arch_assert_rule(VIEW_PLATFORM "PLATFORM-SEAM/PV-chrome"
                  "apps/gui/gui_view_chrome\\.c")
_arch_assert_rule(VIEW_MODEL_POLICY "PV-settings/RESULT-INDEX"
                  "apps/gui/gui_view_canvas\\.c"
                  "apps/gui/gui_view_chrome\\.c"
                  "apps/gui/gui_view_settings\\.c")
_arch_assert_rule(CORE_FRONTEND "R1a/R1b")
_arch_assert_rule(ASYNC_RAW_SESSION "R1c/R2b")
_arch_assert_rule(
    HOST_QUEUE_RAW_SESSION_STORAGE
    "R2b no retained raw session")

# Zero-only deletion guard. Behavioral tests prove presence and sequencing;
# this gate only prevents a removed path from returning.
function(_arch_assert_absent relative_path symbol remove_in)
    set(_path "${_arch_root}/${relative_path}")
    if(NOT EXISTS "${_path}")
        return()
    endif()
    file(READ "${_path}" _source)
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _source "${_source}")
    string(REGEX REPLACE "//[^\r\n]*" " " _source "${_source}")
    if(_source MATCHES
       "(^|[^A-Za-z0-9_])${symbol}([^A-Za-z0-9_]|$)")
        message(FATAL_ERROR
            "${remove_in} deletion regressed: ${relative_path} contains ${symbol}")
    endif()
endfunction()

# Completed R1c/R2b worker and host-admission cuts.
foreach(_entry IN ITEMS
        "thrd_t" "thrd_create" "thrd_join" "job_join"
        "pack_worker" "export_worker" "job_start_thread"
        "job_thread_start" "job_thread_start_context"
        "fail_next_thread_create")
    _arch_assert_absent("packer/src/tp_job.c" "${_entry}" "R1c")
endforeach()
_arch_assert_absent("packer/src/tp_job.c"
                    "tp_session[ \t]*\\*[ \t]*session[ \t]*;" "R1c")
_arch_assert_absent("apps/gui/gui_pack_jobs.c"
                    "tp_session[ \t]*\\*" "R2b")
_arch_assert_absent("apps/gui/gui_pack_jobs.c" "wait_for_job" "R2b")
foreach(_symbol IN ITEMS
        job_session
        tp_session_job_active
        tp_session_job_poll
        tp_session_job_take_result
        tp_session_job_cancel
        tp_session_pack_job_start
        tp_session_export_start)
    _arch_assert_absent("apps/gui/gui_pack_jobs.c" "${_symbol}" "R2b")
endforeach()
foreach(_path IN ITEMS apps/gui/gui_project.c apps/gui/gui_project.h
                       apps/gui/gui_actions.c)
    _arch_assert_absent("${_path}" "gui_project_session_for_jobs" "R2b")
endforeach()
_arch_assert_absent("apps/gui/gui_actions.c"
                    "s_refresh_fingerprint_session" "R2b")
_arch_assert_absent("apps/gui/main.c" "gui_pack_worker_active" "R2b")
_arch_assert_absent("apps/gui/main.c" "gui_pack_poll" "R2b")
_arch_assert_absent(
    "apps/gui/gui_host_queue.h"
    "tp_session[ \t]*\\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;"
    "R2b no retained raw session")

file(GLOB _gui_shipping_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/gui*.c"
    "${_arch_root}/apps/gui/main.c")

foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_host_queue.c"
       OR _filename STREQUAL "gui_selftest.c")
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
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "R2b single GUI host admission owner")
    endforeach()
endforeach()

# Completed R2a observation ownership. Recovery may take a temporary immutable
# snapshot for candidate preflight and post-Save-As metadata; it never observes.
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
    _arch_assert_absent("${_path}" "${_symbol}" "R2a")
endforeach()

file(GLOB _gui_observation_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/*.c"
    "${_arch_root}/apps/gui/*.h")
foreach(_source IN LISTS _gui_observation_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    if(_relative MATCHES
       "^apps/gui/(gui_session_client|gui_selftest|tp_bench_[^/]*|test_[^/]*|client_parity_[^/]*)\\.(c|h)$")
        continue()
    endif()
    foreach(_symbol IN ITEMS tp_session_observe
                             tp_session_observation_destroy)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "R2a single GUI observation owner")
    endforeach()
    if(NOT _relative STREQUAL "apps/gui/gui_project_recovery.c")
        foreach(_symbol IN ITEMS tp_session_snapshot_create
                                 tp_session_snapshot_destroy)
            _arch_assert_absent(
                "${_relative}" "${_symbol}"
                "R2a single GUI snapshot owner")
        endforeach()
    endif()
endforeach()

# Completed R2c mutation cut.
_arch_assert_absent("apps/gui/gui_session_adapter.c" "tp_session_apply" "R2c")
_arch_assert_absent("apps/gui/gui_session_adapter.c" "\"project\\.edit\"" "R2c")
_arch_assert_absent("apps/gui/gui_session_adapter.h" "tp_session[ \t]*\\*" "R2c")
foreach(_path IN ITEMS apps/gui/gui_project.c
                       apps/gui/gui_project_mutations.c)
    _arch_assert_absent(
        "${_path}" "gui_project__refresh_after_session_commit" "R2c")
    _arch_assert_absent(
        "${_path}" "gui_project__next_transaction_id" "R2c")
endforeach()
_arch_assert_absent("apps/gui/gui_project_internal.h" "txn_seq" "R2c")
foreach(_path IN ITEMS apps/gui/gui_view_chrome.c apps/gui/main.c)
    _arch_assert_absent("${_path}" "do_undo" "R2c")
    _arch_assert_absent("${_path}" "do_redo" "R2c")
endforeach()
_arch_assert_absent("apps/gui/main.c"
                    "gui_project_anim_remove_frame"
                    "R2c frame-pinned view ingress")

foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_session_client.c"
       OR _filename STREQUAL "gui_selftest.c")
        continue()
    endif()
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    _arch_assert_absent(
        "${_relative}" "tp_session_apply"
        "R2c single GUI mutation owner")
endforeach()

# Completed R2d lifecycle cut.
_arch_assert_absent("apps/gui/gui_project_file.c" "install_session" "R2d")
foreach(_symbol IN ITEMS
        gui_project__session_client
        gui_project__host_queue)
    _arch_assert_absent(
        "apps/gui/gui_project_internal.h"
        "${_symbol}" "R2d")
    _arch_assert_absent(
        "apps/gui/gui_project_file.c"
        "${_symbol}" "R2d")
endforeach()
foreach(_path IN ITEMS
        apps/gui/gui_project.h
        apps/gui/gui_project_internal.h
        apps/gui/gui_project_file.c
        apps/gui/gui_actions_dialogs.c
        apps/gui/main.c)
    foreach(_symbol IN ITEMS
            gui_project_lifecycle_receipt
            gui_project_lifecycle_take_receipt)
        _arch_assert_absent(
            "${_path}" "${_symbol}" "R2d")
    endforeach()
endforeach()
foreach(_symbol IN ITEMS
        gui_host_binding_receipt
        gui_host_command_kind
        has_pending_start
        drain_cancel_pending)
    _arch_assert_absent(
        "apps/gui/gui_host_queue.h"
        "${_symbol}" "R2d")
    _arch_assert_absent(
        "apps/gui/gui_host_binding.h"
        "${_symbol}" "R2d")
endforeach()
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "tp_session[ \t\r\n]*\\*[ \t\r\n]*session[ \t\r\n]*;"
    "R2d single session storage")
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_session_client[ \t\r\n]+client[ \t\r\n]*;"
    "R2d single client storage")
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_host_queue[ \t\r\n]+host_queue[ \t\r\n]*;"
    "R2d single host queue storage")
foreach(_path IN ITEMS
        apps/gui/gui_host_queue.c
        apps/gui/gui_host_queue.h
        apps/gui/gui_project.c
        apps/gui/gui_project.h)
    foreach(_symbol IN ITEMS
            gui_host_queue_cancelling
            gui_project_job_cancelling)
        _arch_assert_absent(
            "${_path}" "${_symbol}" "R2d")
    endforeach()
endforeach()
_arch_assert_absent("apps/gui/gui_project_file.c" "tp_session_discard" "R2d")
_arch_assert_absent("apps/gui/gui_actions_dialogs.c" "gui_project_new" "R2d")
_arch_assert_absent("apps/gui/gui_actions_dialogs.c" "gui_project_open" "R2d")
_arch_assert_absent("apps/gui/main.c" "gui_project_open" "R2d")
_arch_assert_absent("apps/gui/gui_pack_jobs.c"
                    "gui_project_lifecycle_begin_shutdown" "R2d")
foreach(_path IN ITEMS apps/gui/gui_host_queue.c apps/gui/gui_host_queue.h)
    _arch_assert_absent("${_path}" "gui_host_queue_can_replace" "R2d")
endforeach()

foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    if(_relative MATCHES
       "^apps/gui/(gui_host_binding|gui_host_queue|gui_session_client|gui_selftest)\\.c$")
        continue()
    endif()
    foreach(_symbol IN ITEMS
            gui_host_queue_open
            gui_host_queue_begin_drain
            gui_host_queue_commit_cutover
            gui_host_queue_commit_close
            gui_session_client_attach
            gui_session_client_detach)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "R2d single lifecycle owner")
    endforeach()
endforeach()

# R3a atlas scalar drafts have one view-local reducer owner. The former action
# array and broad ready-operation route must not return.
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "CK_ATLAS_SETTING"
    "R3a atlas draft owner")
_arch_assert_absent(
    "apps/gui/gui_project_mutations.c"
    "make_atlas_key|gui_project_set_atlas_setting"
    "R3a atlas draft submit")
_arch_assert_absent(
    "apps/gui/gui_actions_internal.h"
    "atlas_setting_intent"
    "R3a action mirror deletion")
foreach(_path IN ITEMS
        apps/gui/gui_actions.h
        apps/gui/gui_actions_edits.c
        apps/gui/gui_view_settings.c
        apps/gui/gui_selftest.c
        apps/gui/test_gui_action_trace.c
        apps/gui/test_gui_canonical_identity.c)
    _arch_assert_absent(
        "${_path}"
        "gui_queue_atlas_setting|gui_project_set_atlas_setting"
        "R3a legacy atlas ingress deletion")
endforeach()

# R3c completes the value-edit cutover. The action draft reducer is the sole
# retained value owner; project-level broad operations, timers, and mirror
# queues must not return.
_arch_assert_absent(
    "apps/gui/CMakeLists.txt"
    "gui_project_pending\\.c"
    "R3c pending owner deletion")
foreach(_path IN ITEMS
        apps/gui/gui_project.h
        apps/gui/gui_project.c
        apps/gui/gui_project_file.c
        apps/gui/gui_project_internal.h
        apps/gui/gui_project_mutations.c
        apps/gui/gui_project_test_driver.h
        apps/gui/main.c)
    _arch_assert_absent(
        "${_path}"
        "gui_project_(flush_pending|pending_route|pending_offer|pending_discard|peek_pending_slice9|flush_elapsed|tick|flush_error)"
        "R3c pending API deletion")
endforeach()
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_coalesce_(kind|key)|pending_(valid|key|op|time|expected_revision|preview_stale_before)"
    "R3c pending storage deletion")
foreach(_path IN ITEMS
        apps/gui/gui_actions.h
        apps/gui/gui_actions.c
        apps/gui/gui_actions_dialogs.c
        apps/gui/gui_actions_edits.c
        apps/gui/gui_actions_internal.h
        apps/gui/gui_actions_pack.c
        apps/gui/gui_selftest.c
        apps/gui/test_gui_action_trace.c
        apps/gui/test_gui_canonical_identity.c)
    _arch_assert_absent(
        "${_path}"
        "gui_actions__flush_failed|gui_edit_target[ \t]*\\(|gui_project_set_(sprite|anim)|gui_project_set_target[ \t]*\\(|SPRITE_INTENT_|ANIMATION_INTENT_(FPS|PLAYBACK|FLIP)|TARGET_INTENT_FULL"
        "R3c legacy edit route deletion")
endforeach()
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    _arch_assert_absent(
        "${_relative}"
        "gui_actions__flush_failed|gui_edit_target[ \t]*\\(|gui_project_(flush_pending|pending_route|pending_offer|pending_discard|peek_pending_slice9|flush_elapsed|tick|flush_error)|gui_project_set_(sprite|anim)|gui_project_set_target[ \t]*\\(|SPRITE_INTENT_|ANIMATION_INTENT_(FPS|PLAYBACK|FLIP)|TARGET_INTENT_FULL"
        "R3c shipping legacy edit route deletion")
endforeach()

message(STATUS "architecture boundaries hold")
