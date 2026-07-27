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

# Role classification is DECLARED, never inferred from a filename prefix.
# Under the old prefix rule the role was a side effect of a name: renaming a
# view TU out of `gui_view_*` silently dropped every view rule, and a GUI file
# that never carried the prefix was scanned as a non-view no matter what it
# did. The list below IS the role. Two guards keep the declaration honest:
# every declared file must exist (a rename cannot unclassify a view), and
# every `gui_view_*` file on disk must be declared (a new view cannot arrive
# unclassified). A view TU that is neither declared nor named `gui_view_*`
# still needs a human to declare it — that is the cost of an explicit role,
# and it is paid once per file instead of silently every day.
set(_arch_view_files
    apps/gui/gui_view_canvas.c
    apps/gui/gui_view_canvas.h
    apps/gui/gui_view_chrome.c
    apps/gui/gui_view_chrome.h
    apps/gui/gui_view_lists.c
    apps/gui/gui_view_lists.h
    apps/gui/gui_view_settings.c
    apps/gui/gui_view_settings.h)
# gui_rows.c owns gui_view_adopt_default_atlas but is a projection module,
# not a view TU: it is deliberately NOT declared as a view.

# Files that are absent on purpose. `_arch_assert_absent` treats a missing
# guarded path as a checker bug (fail-closed) unless the path is registered
# here, and a registered path that comes back is a deletion regression.
set(_arch_deleted_files
    apps/gui/gui_project_pending.c)

if(NOT DEFINED ARCH_EXPECT_RULE)
    foreach(_view IN LISTS _arch_view_files)
        if(NOT EXISTS "${_arch_root}/${_view}")
            message(FATAL_ERROR
                "declared view TU is missing: ${_view}. A view file was "
                "renamed or deleted without updating the checker's view "
                "role list; classification must stay explicit.")
        endif()
    endforeach()
    foreach(_deleted IN LISTS _arch_deleted_files)
        if(EXISTS "${_arch_root}/${_deleted}")
            message(FATAL_ERROR
                "deletion regressed: ${_deleted} returned")
        endif()
    endforeach()
endif()

# Any `gui_view_*` file on disk must be classified. Adding a view TU without
# declaring its role fails here instead of shipping unchecked.
file(GLOB _arch_view_candidates LIST_DIRECTORIES false
    "${_arch_scan_root}/apps/gui/gui_view_*.c"
    "${_arch_scan_root}/apps/gui/gui_view_*.h")
foreach(_candidate IN LISTS _arch_view_candidates)
    cmake_path(RELATIVE_PATH _candidate BASE_DIRECTORY "${_arch_scan_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    if(NOT "${_relative}" IN_LIST _arch_view_files)
        message(FATAL_ERROR
            "unclassified view TU: ${_relative} matches gui_view_* but is "
            "not declared in the checker's view role list. Declare it (or "
            "give it a non-view name) so the view rules apply to it.")
    endif()
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
    if("${_relative}" IN_LIST _arch_view_files)
        set(_is_view true)
    endif()

    set(_is_core false)
    if(_relative MATCHES "^packer/(include/tp_core|src)/.*\\.(c|h)$")
        set(_is_core true)
    endif()

    # The async family is code that runs off the host/UI thread: job workers,
    # transports, and out-of-process clients. Its rule forbids retaining or
    # calling a raw `tp_session *`, because that session belongs to another
    # thread. gui_host_queue is deliberately NOT in this family: it runs ON
    # the host thread and legitimately passes `tp_session *` through its
    # drain/admission functions, which is why the old `host_queue` token had
    # to be exempted line-for-line the moment it was added. The two things
    # that must actually be true of the queue are expressed directly instead:
    # HOST_QUEUE_RAW_SESSION_STORAGE (it never RETAINS a session) and the
    # gui_host_queue_* containment sweep below (its symbols never leak past
    # the host owner).
    set(_is_async false)
    if(_relative MATCHES "(^|/)[^/]*(worker|transport|dev[_-]?api|mcp)[^/]*\\.(c|h)$")
        set(_is_async true)
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
            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_session_(apply|undo|redo|save|save_as|save_new|discard|invalidate_sources|require_recovery|pack_job_start|export_start|job_cancel|job_take_result)|gui_project_(new|open|save|save_as|discard|undo|redo|invalidate_sources)|gui_project_submit_[A-Za-z0-9_]*|gui_session_client_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
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

# Non-gating debt report (AGENTS.md Simplification Policy: inventory, not a
# gate). Every file listed as exempt below has pre-existing debt with a named
# rationale, so the rule as written can never fail on it — gating it would be
# a rule that neutralizes itself. Its hits are printed instead. Deleting debt
# stays green because nothing is compared against a baseline: there are no
# occurrence counts in any exemption, only in the printed report.
#
# The rule is NOT dead, though: a view TU that is not on the exemption list
# must have zero hits, so a new view cannot accrue this debt silently.
function(_arch_report_debt rule note)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    set(_exempt_paths ${ARGN})
    set(_files "")
    foreach(_hit IN LISTS _hits)
        string(REGEX REPLACE ":[0-9]+$" "" _file "${_hit}")
        set(_allowed false)
        foreach(_exempt_path IN LISTS _exempt_paths)
            if(_file MATCHES "^${_exempt_path}$")
                set(_allowed true)
                break()
            endif()
        endforeach()
        if(NOT _allowed)
            message(FATAL_ERROR
                "${rule} violation in a file with no ${note} debt "
                "exemption: ${_hit}")
        endif()
        list(APPEND _files "${_file}")
    endforeach()
    if(NOT _files)
        message(STATUS "debt ${rule}: none")
        return()
    endif()
    set(_unique ${_files})
    list(SORT _unique)
    list(REMOVE_DUPLICATES _unique)
    foreach(_file IN LISTS _unique)
        set(_count 0)
        foreach(_entry IN LISTS _files)
            if(_entry STREQUAL _file)
                math(EXPR _count "${_count} + 1")
            endif()
        endforeach()
        message(STATUS "debt ${rule}: ${_file} (${_count})")
    endforeach()
endfunction()

_arch_assert_rule(VIEW_ADMISSION "R2c/R2d")
_arch_assert_rule(VIEW_IO "SR-BASE/PV-tree-list")
_arch_report_debt(VIEW_PLATFORM "PLATFORM-SEAM/PV-chrome"
                  # pre-SR-BASE debt: the chrome view owns the menu/dialog
                  # seam and calls the platform file dialog and clipboard
                  # directly; the seam moves behind the host owner later.
                  "apps/gui/gui_view_chrome\\.c")
_arch_report_debt(VIEW_MODEL_POLICY "PV-settings/RESULT-INDEX"
                  # pre-SR-BASE debt: the canvas view reads pack/result model
                  # data (gui_pack_result, sprite-ref lookup) to draw.
                  "apps/gui/gui_view_canvas\\.c"
                  # pre-SR-BASE debt: the chrome view reads project/model
                  # state for its title and status affordances.
                  "apps/gui/gui_view_chrome\\.c"
                  # pre-SR-BASE debt: tp_validate/tp_exporter reads over the
                  # pending PV-settings slice live in the settings view.
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
        # Fail closed. A guarded file that vanished used to make its rule
        # pass silently; now the only way a guarded path may be absent is to
        # be registered as intentionally deleted.
        if("${relative_path}" IN_LIST _arch_deleted_files)
            return()
        endif()
        message(FATAL_ERROR
            "${remove_in} guard lost its file: ${relative_path} does not "
            "exist, so the guard against ${symbol} no longer proves "
            "anything. Retarget the rule, or register the path in "
            "_arch_deleted_files if the deletion is intentional.")
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
    if(NOT _filename STREQUAL "gui_session_adapter.c"
       AND NOT _filename STREQUAL "gui_session_client.c")
        _arch_assert_absent(
            "${_relative}" "gui_session_client_submit"
            "R5 single GUI submit owner")
    endif()
    _arch_assert_absent(
        "${_relative}" "gui_project__refresh_after_session_commit"
        "R5 observation-owned mutation refresh")
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

# The host queue is the host owner's private ingress, not a GUI-wide API.
# This containment sweep replaces the old async-family carve-out: instead of
# pretending the queue is async and then exempting it, the queue keeps its
# host-thread session calls and its symbols stay inside the owner set.
# P5 shrinks this list: gui_project.c's direct queue calls become
# gui_host_binding_* ingress functions.
set(_arch_host_queue_owners
    apps/gui/gui_host_queue.c
    apps/gui/gui_host_queue.h
    apps/gui/gui_host_binding.c
    apps/gui/gui_host_binding.h
    apps/gui/gui_project.c)

# The active session is borrowed, never held. P5 shrinks this list as the
# lifecycle/mutation owners consolidate.
set(_arch_borrow_session_owners
    apps/gui/gui_project.c
    apps/gui/gui_project_internal.h
    apps/gui/gui_project_file.c
    apps/gui/gui_project_recovery.c
    apps/gui/gui_project_mutations.c)

foreach(_source IN LISTS _gui_observation_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    # Tests may drive either owner directly.
    if(_relative MATCHES
       "^apps/gui/(test_[^/]*|tp_bench_[^/]*|client_parity_[^/]*)\\.(c|h)$")
        continue()
    endif()
    if(NOT "${_relative}" IN_LIST _arch_host_queue_owners)
        _arch_assert_absent(
            "${_relative}" "gui_host_queue_[A-Za-z0-9_]*"
            "R2d host queue stays inside the host owner")
    endif()
    if(NOT "${_relative}" IN_LIST _arch_borrow_session_owners)
        _arch_assert_absent(
            "${_relative}" "gui_project__borrow_active_session"
            "R2d single session storage borrows in one place")
    endif()
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

# gui_project_pending.c's return is caught by the _arch_deleted_files registry
# at the top of this file (one place owns "intentionally absent").

# R4/R5 cutover hardening. Stable ids are the only cross-frame identity;
# indices remain frame-local projections. Exact old names are guarded instead
# of broad "*_index" patterns so legitimate snapshot/view lookup stays simple.
foreach(_source IN LISTS _gui_observation_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    foreach(_symbol IN ITEMS
            s_sel_atlas
            s_sel_src
            s_sel_child
            s_sel_anim
            s_sel_anim_frame
            s_sel_anchor_row
            s_focus_view
            s_sel_abs
            s_sel_missing
            s_reselect_pending
            s_reselect_source_id
            s_reselect_key
            s_reselect_atlas_id
            s_gui_view
            gui_project_animation_ref_at
            gui_project_target_ref_at
            gui_session_rename_atlas
            gui_session_set_sprite_name
            gui_session_rename_animation
            gui_project__snapshot_drop)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "R4/R5 legacy identity and mutation route deletion")
    endforeach()
endforeach()

_arch_assert_absent(
    "apps/gui/client_parity_replay.c"
    "TP_TF_OUT_PATH"
    "R5 target path uses the narrow identified endpoint")
_arch_assert_absent(
    "apps/gui/gui_session_adapter.c"
    "gui_session_client_snapshot"
    "R5 submit receipts have one owner")
_arch_assert_absent(
    "apps/gui/main.c"
    "s_shown_result"
    "R5 result identity uses stable atlas and publication version")
_arch_assert_absent(
    "apps/gui/gui_canvas.c"
    "ref[ \t]*->[ \t]*result"
    "R5 double-click identity uses result generation")
_arch_assert_absent(
    "apps/gui/gui_canvas.h"
    "typedef[ \t\r\n]+struct[ \t\r\n]+gui_canvas_double_click_ref[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_result)[ \t\r\n]*\\*"
    "R5 no retained result-pointer identity")
_arch_assert_absent(
    "apps/gui/gui_pack_preview.c"
    "s_preview[ \t]*\\.[ \t]*atlas_index"
    "R5 preview identity uses stable atlas id")
_arch_assert_absent(
    "packer/src/tp_job.c"
    "typedef[ \t\r\n]+struct[ \t\r\n]+tp_live_job[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*"
    "R5 worker job owns immutable input, never raw session")
_arch_assert_absent(
    "packer/src/tp_job.c"
    "tp_arena_destroy[ \t\r\n]*\\([ \t\r\n]*result[ \t\r\n]*->[ \t\r\n]*pack[ \t\r\n]*\\.[ \t\r\n]*arena"
    "R5 job result lifetime has one retained owner")

message(STATUS "architecture boundaries hold")
