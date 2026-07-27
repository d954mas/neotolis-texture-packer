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

# A hit records WHICH forbidden symbol fired, not only where. The per-site debt
# allowances below are expressed in exactly that vocabulary, so an exempted file
# cannot quietly grow a new symbol of the same forbidden family.
function(_arch_hit rule relative_path line_number symbol)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    list(APPEND _hits "${relative_path}:${line_number}:${symbol}")
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
                _arch_hit(VIEW_ADMISSION "${_relative}" "${_line_number}"
                          "${CMAKE_MATCH_2}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_scan_[A-Za-z0-9_]*|tp_image_[A-Za-z0-9_]*|tp_pack_input_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_IO "${_relative}" "${_line_number}"
                          "${CMAKE_MATCH_2}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(ShellExecute[A-Z]*|CreateProcess[A-Z]*|nt_clipboard_[A-Za-z0-9_]*|tinyfd_[A-Za-z0-9_]*)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_PLATFORM "${_relative}" "${_line_number}"
                          "${CMAKE_MATCH_2}")
            endif()

            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(tp_model_[A-Za-z0-9_]*|tp_project_[A-Za-z0-9_]*|tp_validate_[A-Za-z0-9_]*|tp_exporter_(count|at)|gui_pack_(preview_diff|result|find_sprite_ref))([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_MODEL_POLICY "${_relative}" "${_line_number}"
                          "${CMAKE_MATCH_2}")
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
        _arch_hit(VIEW_ADMISSION "${_relative}" "0"
                  "#include:${CMAKE_MATCH_1}")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](tp_core/tp_(scan|image|input|pack_hash)\\.h|gui_scan\\.h)[>\"]")
        _arch_hit(VIEW_IO "${_relative}" "0"
                  "#include:${CMAKE_MATCH_1}")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](windows\\.h|clipboard/nt_clipboard\\.h|tinyfiledialogs\\.h)[>\"]")
        _arch_hit(VIEW_PLATFORM "${_relative}" "0"
                  "#include:${CMAKE_MATCH_1}")
    endif()
    if(_is_view
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"](tp_core/tp_(model|project|operation|validate|client_capability|export)\\.h)[>\"]")
        _arch_hit(VIEW_MODEL_POLICY "${_relative}" "0"
                  "#include:${CMAKE_MATCH_1}")
    endif()
    if(_is_core
       AND _directives MATCHES "#[ \t]*include[ \t]*[<\"][^>\"]*(apps/|gui_|cli_|mcp_|devapi_|dev_api_|jsonrpc|transport)")
        _arch_hit(CORE_FRONTEND "${_relative}" "0"
                  "#include:${CMAKE_MATCH_1}")
    endif()

    # String literals are irrelevant to symbol/call checks below.
    string(REGEX REPLACE "\"([^\"\\\\]|\\\\.)*\"" "\"\"" _scan "${_scan}")
    if(_is_view
       AND _scan MATCHES "(^|[^A-Za-z0-9_])(fopen|open|stat|opendir|readdir|FindFirstFile[A-Z]*)[ \t\r\n]*\\(")
        _arch_hit(VIEW_IO "${_relative}" "0"
                  "${CMAKE_MATCH_2}")
    endif()
    if(_is_view
       AND _scan MATCHES "(^|[^A-Za-z0-9_])(system|popen)[ \t\r\n]*\\(")
        _arch_hit(VIEW_PLATFORM "${_relative}" "0"
                  "${CMAKE_MATCH_2}")
    endif()
    if(_is_core
       AND _scan MATCHES "(^|[^A-Za-z0-9_])((gui_|cli_|mcp_|devapi_)[A-Za-z0-9_]*)[ \t\r\n]*\\(")
        _arch_hit(CORE_FRONTEND "${_relative}" "0"
                  "${CMAKE_MATCH_2}")
    endif()
    if(_is_async
       AND _scan MATCHES "(^|[^A-Za-z0-9_])((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*")
        _arch_hit(ASYNC_RAW_SESSION "${_relative}" "0"
                  "tp_session-pointer")
    endif()
    if(_relative MATCHES "^apps/gui/gui_host_queue\\.(c|h)$"
       AND _scan MATCHES "typedef[ \t\r\n]+struct[ \t\r\n]+gui_host_queue[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*[ \t\r\n]*;")
        _arch_hit(
            HOST_QUEUE_RAW_SESSION_STORAGE
            "${_relative}" "0"
            "retained-tp_session-member")
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
            if(_hit MATCHES "^${_allowed_path}:[0-9]+:")
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
# gate) with PER-SITE allowances. A whole-file exemption neutralizes its own
# rule twice over: it makes the rule unable to fail on that file, AND it lets
# the exempted view accrue unlimited NEW debt of the same forbidden family.
#
# So the allowance is not "how many" (AGENTS.md forbids occurrence counts and
# ratchets outright) but "WHICH": every entry pins one file to the exact
# forbidden-API symbols that file already uses, with a rationale. Consequences:
#   - deleting debt stays green (nothing is compared against a baseline);
#   - a NEW symbol of the forbidden family fails even inside an exempted file;
#   - a file that is not listed at all must still have zero hits.
#
# Entry format: "<file-regex>|<symbol>,<symbol>,...". Include-directive hits use
# the symbol form "#include:<header>"; whole-file call scans use the call name.
# The whole-file scans report the FIRST match in a file, so an entry lists every
# forbidden header/call that file legitimately has — otherwise merely reordering
# includes would move the reported hit and fail a green tree. Per-LINE symbol
# hits are reported individually and carry no such caveat.
function(_arch_report_debt rule note)
    get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${rule}")
    set(_entries "${ARGN}")
    set(_sites "")
    foreach(_hit IN LISTS _hits)
        if(NOT _hit MATCHES "^([^:]+):([0-9]+):(.*)$")
            message(FATAL_ERROR
                "${rule}: malformed hit record '${_hit}'")
        endif()
        set(_file "${CMAKE_MATCH_1}")
        set(_line "${CMAKE_MATCH_2}")
        set(_symbol "${CMAKE_MATCH_3}")
        set(_allowed false)
        foreach(_entry IN LISTS _entries)
            if(NOT _entry MATCHES "^([^|]+)\\|(.*)$")
                message(FATAL_ERROR
                    "${rule}: malformed debt entry '${_entry}'")
            endif()
            set(_entry_file "${CMAKE_MATCH_1}")
            set(_entry_symbols "${CMAKE_MATCH_2}")
            if(NOT _file MATCHES "^${_entry_file}$")
                continue()
            endif()
            string(REPLACE "," ";" _allowed_symbols
                   "${_entry_symbols}")
            if("${_symbol}" IN_LIST _allowed_symbols)
                set(_allowed true)
            endif()
            break()
        endforeach()
        if(NOT _allowed)
            message(FATAL_ERROR
                "${rule} violation: ${_file}:${_line} uses '${_symbol}', "
                "which is not an allowed ${note} debt site. Exemptions name "
                "the exact symbols a file already uses, never the whole file, "
                "so a NEW symbol of this family fails here too. Remove the "
                "dependency, or add the symbol with a written rationale.")
        endif()
        list(APPEND _sites "${_file} ${_symbol}")
    endforeach()
    if(NOT _sites)
        message(STATUS "debt ${rule}: none")
        return()
    endif()
    list(SORT _sites)
    list(REMOVE_DUPLICATES _sites)
    foreach(_site IN LISTS _sites)
        message(STATUS "debt ${rule}: ${_site}")
    endforeach()
endfunction()

# USA-25 owner: gate. Spec §16's "boundary checks reject mutation, filesystem,
# platform, and business policy in views" is a statement ABOUT a build check, so
# its owner is this file's four VIEW_* rules -- there is no runtime test that can
# prove it. apps/gui/test_gui_canonical_identity.c carries the closest
# behavioural companion (no source decode on the UI thread), but a companion is
# not an owner. scripts/check_boundaries.sh R22 reads this line.
_arch_assert_rule(VIEW_ADMISSION "R2c/R2d")
_arch_assert_rule(VIEW_IO "SR-BASE/PV-tree-list")
# pre-SR-BASE debt: the chrome view owns the menu/dialog seam and reaches the
# OS shell and clipboard directly; the seam moves behind the host owner later.
# `system` is the "reveal in file manager" helper, ShellExecute* its Win32 half.
_arch_report_debt(VIEW_PLATFORM "PLATFORM-SEAM/PV-chrome"
                  "apps/gui/gui_view_chrome\\.c|#include:windows.h,#include:clipboard/nt_clipboard.h,system,ShellExecute,ShellExecuteA,ShellExecuteW,nt_clipboard_available,nt_clipboard_set_text")
_arch_report_debt(VIEW_MODEL_POLICY "PV-settings/RESULT-INDEX"
                  # pre-SR-BASE debt: the canvas view reads pack/result model
                  # data (gui_pack_result, preview diff) to draw, and the
                  # exporter registry to fill its preview-target selector.
                  "apps/gui/gui_view_canvas\\.c|#include:tp_core/tp_export.h,tp_exporter_count,tp_exporter_at,gui_pack_result,gui_pack_preview_diff"
                  # pre-SR-BASE debt: the chrome view reads the exporter
                  # registry for the export modal's target dropdown.
                  "apps/gui/gui_view_chrome\\.c|#include:tp_core/tp_export.h,tp_exporter_count,tp_exporter_at"
                  # pre-SR-BASE debt: tp_validate/tp_exporter reads plus the
                  # effective-shape projection over the pending PV-settings
                  # slice live in the settings view.
                  "apps/gui/gui_view_settings\\.c|#include:tp_core/tp_export.h,#include:tp_core/tp_validate.h,tp_exporter_count,tp_exporter_at,tp_validate_session_snapshot_target,tp_project_sprite_effective_shape,gui_pack_result,gui_pack_find_sprite_ref")
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

# P5: history, identity, and source-runtime commands belong to the host owner
# (spec 6.1), not to whichever file happens to hold a borrowed session. The
# binding owns the sole active-session pointer, so these session entry points
# may appear only there; every other GUI TU goes through
# gui_host_binding_undo/redo/save/save_as/invalidate_sources.
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_host_binding.c")
        continue()
    endif()
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    foreach(_symbol IN ITEMS
            tp_session_undo
            tp_session_redo
            tp_session_save
            tp_session_save_as
            tp_session_invalidate_sources
            tp_session_can_undo
            tp_session_can_redo
            tp_session_undo_depth
            tp_session_redo_depth)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "R2d single host command owner")
    endforeach()
endforeach()

# The host queue is the host owner's private ingress, not a GUI-wide API.
# This containment sweep replaces the old async-family carve-out: instead of
# pretending the queue is async and then exempting it, the queue keeps its
# host-thread session calls and its symbols stay inside the owner set.
# P5 closed the last bypass: gui_project.c's direct queue calls (enqueue,
# take_completion, busy, active_kind, the staged-completion test seam) are now
# gui_host_binding_* ingress functions, so the binding pair IS the owner set.
set(_arch_host_queue_owners
    apps/gui/gui_host_queue.c
    apps/gui/gui_host_queue.h
    apps/gui/gui_host_binding.c
    apps/gui/gui_host_binding.h)

# The active session is borrowed, never held. P5 shrank this list: the
# lifecycle/file owner and the mutation owner ask the host binding (commands)
# or gui_session_client_is_attached (liveness) instead of borrowing.
# gui_project.c keeps the accessor itself, gui_project_internal.h its
# declaration, and gui_project_recovery.c the borrow the session-scoped core
# recovery API genuinely needs.
set(_arch_borrow_session_owners
    apps/gui/gui_project.c
    apps/gui/gui_project_internal.h
    apps/gui/gui_project_recovery.c)

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
        apps/gui/test_gui_action_trace_draft.c
        apps/gui/test_gui_action_trace_refresh.c
        apps/gui/test_gui_action_trace_job.c
        apps/gui/test_gui_action_trace_fixture.c
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
        apps/gui/test_gui_action_trace_draft.c
        apps/gui/test_gui_action_trace_refresh.c
        apps/gui/test_gui_action_trace_job.c
        apps/gui/test_gui_action_trace_fixture.c
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

# P6 job-owner test seams. Two internal entry points exist ONLY for tests:
# tp_session_job_attach_internal (adopts the active-job lease for a job it did
# not start) and tp_session_job_observation_begin_internal (publishes a second
# live observed owner). Production has no route to either -- the sole production
# start path, tp_session_job_start_internal, spawns fail-atomically and rejects a
# second live owner -- but the USA-07 reverse/superseded admission table is only
# reachable through them, so they are kept and fenced instead of deleted. Both
# declaration and definition sit behind TP_ENABLE_TEST_SEAMS; this rule is what
# makes the fence load-bearing.
#
# Two halves, because either one alone can be satisfied trivially:
# 1. the guarded SYMBOL is inside a TP_ENABLE_TEST_SEAMS conditional in each of
#    the four files that own the seam, and
# 2. no other shipping TU names either symbol.
#
# Half 1 used to be a whole-file substring search for "TP_ENABLE_TEST_SEAMS",
# which any unrelated seam elsewhere in the file satisfied — including the very
# comment describing the rule. The check below is bound to the symbol instead:
# the file is walked with a conditional-directive depth counter, and every
# non-comment line naming the symbol must sit inside a live
# `#ifdef TP_ENABLE_TEST_SEAMS` (or `#if defined(TP_ENABLE_TEST_SEAMS)`) region.
# The depth model is deliberately simple — it tracks nesting and treats an
# `#else`/`#elif` at the guard's own depth as leaving the fence — and it does not
# evaluate compound conditions beyond a leading `defined(...)` test.
function(_arch_assert_seam_fenced relative_path symbol why)
    set(_path "${_arch_root}/${relative_path}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR
            "${why} guard lost its file: ${relative_path} does not exist.")
    endif()
    file(STRINGS "${_path}" _lines)
    set(_depth 0)
    set(_seam_depths "")
    set(_found false)
    set(_line_number 0)
    foreach(_line IN LISTS _lines)
        math(EXPR _line_number "${_line_number} + 1")
        string(STRIP "${_line}" _trimmed)
        if(_trimmed MATCHES "^#[ \t]*(if|ifdef|ifndef)([^A-Za-z0-9_]|$)")
            math(EXPR _depth "${_depth} + 1")
            if(_trimmed MATCHES "^#[ \t]*ifdef[ \t]+TP_ENABLE_TEST_SEAMS[ \t]*$"
               OR _trimmed MATCHES "^#[ \t]*if[ \t]+defined[ \t]*\\([ \t]*TP_ENABLE_TEST_SEAMS[ \t]*\\)")
                list(APPEND _seam_depths "${_depth}")
            endif()
        elseif(_trimmed MATCHES "^#[ \t]*endif")
            list(REMOVE_ITEM _seam_depths "${_depth}")
            if(_depth GREATER 0)
                math(EXPR _depth "${_depth} - 1")
            endif()
        elseif(_trimmed MATCHES "^#[ \t]*(else|elif)")
            list(REMOVE_ITEM _seam_depths "${_depth}")
        elseif(_trimmed MATCHES "^(//|/\\*|\\*)")
            # comment line: never a declaration site
        elseif(_trimmed MATCHES
               "(^|[^A-Za-z0-9_])${symbol}([^A-Za-z0-9_]|$)")
            set(_found true)
            if(NOT _seam_depths)
                message(FATAL_ERROR
                    "${why}: ${relative_path}:${_line_number} names "
                    "${symbol} OUTSIDE a #ifdef TP_ENABLE_TEST_SEAMS fence, "
                    "so the seam is reachable from a shipping build.")
            endif()
        endif()
    endforeach()
    if(NOT _found)
        message(FATAL_ERROR
            "${why}: ${relative_path} no longer names ${symbol}, so the fence "
            "around it proves nothing. Retarget or delete the rule.")
    endif()
endfunction()

foreach(_path IN ITEMS packer/src/tp_session.c
                       packer/src/tp_job_owner_internal.h)
    _arch_assert_seam_fenced(
        "${_path}" "tp_session_job_attach_internal"
        "P6 job-owner seams stay compiled out of shipping")
endforeach()
foreach(_path IN ITEMS packer/src/tp_session_job_observation.c
                       packer/src/tp_session_job_observation_internal.h)
    _arch_assert_seam_fenced(
        "${_path}" "tp_session_job_observation_begin_internal"
        "P6 job-owner seams stay compiled out of shipping")
endforeach()

file(GLOB_RECURSE _job_seam_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/*.c"
    "${_arch_root}/apps/*.h"
    "${_arch_root}/packer/include/tp_core/*.h"
    "${_arch_root}/packer/src/*.c"
    "${_arch_root}/packer/src/*.h")
foreach(_source IN LISTS _job_seam_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    if(_relative MATCHES "/(deps|generated)/"
       OR _relative MATCHES "(^|/)(test_[^/]*|gui_selftest|tp_bench_[^/]*|client_parity_[^/]*)\\.(c|h)$")
        continue()
    endif()
    if(NOT _relative STREQUAL "packer/src/tp_session.c"
       AND NOT _relative STREQUAL "packer/src/tp_job_owner_internal.h")
        _arch_assert_absent(
            "${_relative}" "tp_session_job_attach_internal"
            "P6 job attach is a test seam, not a production entry point")
    endif()
    if(NOT _relative STREQUAL "packer/src/tp_session_job_observation.c"
       AND NOT _relative STREQUAL
               "packer/src/tp_session_job_observation_internal.h")
        _arch_assert_absent(
            "${_relative}" "tp_session_job_observation_begin_internal"
            "P6 job observation begin is a test seam, not a production entry point")
    endif()
endforeach()

message(STATUS "architecture boundaries hold")
