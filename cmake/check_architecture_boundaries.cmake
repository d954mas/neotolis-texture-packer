cmake_minimum_required(VERSION 3.25)

# Rule-id namespace: `A<n>` (A1a..A5 structural cuts, A6 the job-owner test-seam
# fence, A7 the view/action-state fence). Both boundary checkers now launch the same way (ctest), so their ids may
# not collide: `R<n>` belongs to scripts/check_boundaries.sh (R1..R22) and nothing
# here. The A-ids trace to the packet ids of the ui-session refactor plan whose cut
# each rule pins (plan R1a -> A1a, ... plan R5 -> A5, plan P6 -> A6); the letter is
# the only thing that changed.

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
    VIEW_ACTION_STATE
    VIEW_IO
    VIEW_PLATFORM
    VIEW_MODEL_POLICY
    CORE_FRONTEND
    ASYNC_RAW_SESSION)

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
    apps/gui/gui_project_pending.c
    apps/gui/gui_scan.c
    apps/gui/gui_scan.h
    apps/gui/tp_bench_gui_rows.c
    apps/gui/client_parity_manifest.c
    apps/gui/client_parity_manifest.h
    apps/gui/gui_host_binding.c
    apps/gui/gui_host_binding.h
    apps/gui/gui_host_queue.c
    apps/gui/gui_host_queue.h
    apps/gui/gui_session_client.c
    apps/gui/gui_session_client.h
    apps/gui/test_client_parity.c
    apps/gui/test_gui_host_binding.c
    apps/gui/test_gui_host_queue.c
    apps/gui/test_gui_session_adapter.c
    apps/gui/test_gui_session_client.c
    packer/src/tp_session_job_observation.c
    packer/src/tp_session_job_observation_internal.h
    packer/src/tp_session_observation.c
    packer/tests/test_session_job_observation.c
    packer/tests/test_session_observation.c)

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

# Every forbidden SITE is judged on its own. A whole-file `MATCHES` answers only
# "does this file contain one?" and reports the FIRST capture, so the second and
# later sites of the same rule in one file were never recorded -- and a first
# site covered by a per-symbol debt allowance therefore muted the rest of the
# file. `MATCHALL` makes a scan report one hit per site, so every site meets the
# allowances alone; the per-match re-`MATCHES` is only how CMake hands back the
# capture group, and a match the same pattern rejects is a checker bug that
# fails closed.
#
# `text` must already carry the `_symbols` rewrite below: `MATCHALL` returns a
# CMake list, so a `;` inside a matched site would split that site across two
# elements.
#
# T9 replaces this regex scanner with a parser-backed gate, which is why this
# generalization is a deliberate, recorded double-spend: a scan that silently
# reports only its first hit is a live hole in every rule until then.
function(_arch_scan_all rule relative_path text pattern group prefix)
    string(REGEX MATCHALL "${pattern}" _matches "${text}")
    foreach(_match IN LISTS _matches)
        if(NOT _match MATCHES "${pattern}")
            message(FATAL_ERROR
                "${rule}: MATCHALL yielded '${_match}', which the same pattern "
                "rejects, so the scan cannot name the symbol it found.")
        endif()
        _arch_hit("${rule}" "${relative_path}" "0"
                  "${prefix}${CMAKE_MATCH_${group}}")
    endforeach()
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
    # thread.
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

            # Deferred action state belongs to the actions layer alone. A view
            # DECLARES an intent through gui_request_*(); it never writes the
            # queue, a pending flag, or the actions state struct. This used to
            # be unenforceable because the pending flags were extern globals
            # any view TU could assign -- S16 folded all of them into ONE
            # private queue, which is what makes the token rule real.
            if(_trimmed MATCHES "(^|[^A-Za-z0-9_])(s_pending_[A-Za-z0-9_]*|s_actions)([^A-Za-z0-9_]|$)")
                _arch_hit(VIEW_ACTION_STATE "${_relative}" "${_line_number}"
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
    file(READ "${_source}" _raw)
    # Comments and literals are ONE lexical layer, so they are removed in ONE
    # alternation whose leftmost match wins: whichever token opens first
    # consumes the others. Removing them in ordered passes is precisely how a
    # scan quietly stops finding things -- a `'"'` character literal opens a
    # string that runs to the next quote ANYWHERE in the file (which is what hid
    # two `system()` calls in gui_view_chrome.c from every whole-file scan), and
    # a `//` inside a string erases the rest of a real line. No symbol, call, or
    # declaration scan reads a literal, so blanking all of them loses nothing.
    string(REGEX REPLACE
           "/\\*([^*]|\\*+[^*/])*\\*+/|//[^\r\n]*|\"([^\"\\\\]|\\\\.)*\"|'([^'\\\\\r\n]|\\\\.)*'"
           " " _code "${_raw}")
    # An include names its header inside a STRING, so the directive text must
    # keep string literals and can only drop comments. It therefore cannot share
    # the pass above -- CMake rejects backreferences under alternation, and a
    # literal-PRESERVING single pass needs one -- and a `/*` or `//` written
    # inside a string literal is read here as a comment opener.
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _directives "${_raw}")
    string(REGEX REPLACE "//[^\r\n]*" " " _directives "${_directives}")
    string(REGEX REPLACE "\\\\[ \t]*[\r\n]+" "" _directives "${_directives}")
    # ...so the residual is CAUGHT rather than assumed. `_code` is lexed
    # correctly, so it holds the true directive count; if the ordered pass above
    # ends up with fewer, a comment opener inside a literal swallowed real
    # directives and the include scan is blind to them. Fail closed: a scan that
    # cannot see its input must not report "no violations". (A `#include`
    # written INSIDE a string literal survives in `_directives` and not in
    # `_code`, which is why only a LOSS is an error.)
    string(REGEX MATCHALL "#[ \t]*include" _code_includes "${_code}")
    string(REGEX MATCHALL "#[ \t]*include" _directive_includes "${_directives}")
    list(LENGTH _code_includes _code_include_count)
    list(LENGTH _directive_includes _directive_include_count)
    if(_directive_include_count LESS _code_include_count)
        message(FATAL_ERROR
            "include scan lost a directive to a comment/literal desync: "
            "${_relative} has ${_code_include_count} include directive(s) but "
            "the comment strip left ${_directive_include_count}. A `/*` or "
            "`//` inside a string literal is being read as a comment opener, "
            "so the include rules cannot see this file.")
    endif()
    # EVERY include directive is judged on its own. A single whole-file MATCHES
    # answers only "does this file contain one?" and reports the FIRST capture,
    # so the second and later forbidden includes of the same rule were never
    # recorded -- and a first include covered by a per-symbol debt allowance
    # therefore muted the whole file. MATCHALL turns the scan into one hit per
    # directive, so each forbidden include meets the allowances alone.
    string(REGEX MATCHALL "#[ \t]*include[ \t]*[<\"][^<>\"\r\n]*[>\"]"
           _include_directives "${_directives}")
    foreach(_directive IN LISTS _include_directives)
        if(_is_view
           AND _directive MATCHES "[<\"](tp_core/tp_(session|job)\\.h|gui_project\\.h|gui_session_client\\.h)[>\"]")
            _arch_hit(VIEW_ADMISSION "${_relative}" "0"
                      "#include:${CMAKE_MATCH_1}")
        endif()
        if(_is_view
           AND _directive MATCHES "[<\"](tp_core/tp_(scan|image|input|pack_hash)\\.h|gui_scan\\.h)[>\"]")
            _arch_hit(VIEW_IO "${_relative}" "0"
                      "#include:${CMAKE_MATCH_1}")
        endif()
        if(_is_view
           AND _directive MATCHES "[<\"](windows\\.h|clipboard/nt_clipboard\\.h|tinyfiledialogs\\.h)[>\"]")
            _arch_hit(VIEW_PLATFORM "${_relative}" "0"
                      "#include:${CMAKE_MATCH_1}")
        endif()
        if(_is_view
           AND _directive MATCHES "[<\"](tp_core/tp_(pack_result|project|operation|validate|client_capability|export)\\.h)[>\"]")
            _arch_hit(VIEW_MODEL_POLICY "${_relative}" "0"
                      "#include:${CMAKE_MATCH_1}")
        endif()
        if(_is_core
           AND _directive MATCHES "[<\"][^>\"]*(apps/|gui_|cli_|mcp_|devapi_|dev_api_|jsonrpc|transport)")
            _arch_hit(CORE_FRONTEND "${_relative}" "0"
                      "#include:${CMAKE_MATCH_1}")
        endif()
    endforeach()

    # `MATCHALL` returns a CMake list, so a `;` inside a matched site would
    # split that site across two elements. The statement separator is REWRITTEN
    # rather than blanked because the declarator scans below need a terminator
    # to tell a retained member from a cast or a parameter; `@` is not C outside
    # comments and literals, both of which are already gone.
    string(REPLACE ";" "@" _symbols "${_code}")
    if(_is_view)
        _arch_scan_all(VIEW_IO "${_relative}" "${_symbols}"
            "(^|[^A-Za-z0-9_])(fopen|open|stat|opendir|readdir|FindFirstFile[A-Z]*)[ \t\r\n]*\\("
            2 "")
        _arch_scan_all(VIEW_PLATFORM "${_relative}" "${_symbols}"
            "(^|[^A-Za-z0-9_])(system|popen)[ \t\r\n]*\\("
            2 "")
    endif()
    if(_is_core)
        _arch_scan_all(CORE_FRONTEND "${_relative}" "${_symbols}"
            "(^|[^A-Za-z0-9_])((gui_|cli_|mcp_|devapi_)[A-Za-z0-9_]*)[ \t\r\n]*\\("
            2 "")
    endif()
    if(_is_async)
        # The declared NAME is part of the symbol: two raw sessions in one async
        # TU are two distinct debts, and identical hit records would be folded
        # back into one by the debt report's de-duplication.
        _arch_scan_all(ASYNC_RAW_SESSION "${_relative}" "${_symbols}"
            "(^|[^A-Za-z0-9_])((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*[ \t\r\n]*([A-Za-z_][A-Za-z0-9_]*)"
            4 "tp_session-pointer:")
    endif()
endforeach()

# A6 seam-fence walker (rationale and the real-tree invocations sit at the
# bottom of this file; defined here so the SEAM_FENCE fixture dispatch below
# can call it). Half 1 of A6 used to be a whole-file substring search for
# "TP_ENABLE_TEST_SEAMS", which any unrelated seam elsewhere in the file
# satisfied — including the very comment describing the rule. This check is
# bound to the symbol instead: the file is walked with a conditional-directive
# depth counter, and every non-comment line naming the symbol must sit inside
# a live `#ifdef TP_ENABLE_TEST_SEAMS` (or `#if defined(TP_ENABLE_TEST_SEAMS)`)
# region. The depth model is deliberately simple — it tracks nesting and treats
# an `#else`/`#elif` at the guard's own depth as leaving the fence — and both
# guard forms are matched whole-line (end-anchored): a compound condition such
# as `#if defined(TP_ENABLE_TEST_SEAMS) || X` is NOT a fence (it can compile
# the seam into every build), so a symbol under it reads as unfenced and fails.
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
               OR _trimmed MATCHES "^#[ \t]*if[ \t]+defined[ \t]*\\([ \t]*TP_ENABLE_TEST_SEAMS[ \t]*\\)[ \t]*$")
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

# SEAM_FENCE negative fixture: proves the walker REJECTS a compound guard
# condition. The fixture file guards the seam symbol only with
# `#if defined(TP_ENABLE_TEST_SEAMS) || ...`, so the expected (passing) outcome
# is the walker's OUTSIDE-a-fence FATAL_ERROR; the registering ctest asserts
# that message via PASS_REGULAR_EXPRESSION. Reaching the message below means
# the walker accepted the compound form as a fence — the regression this
# fixture exists to catch.
if(DEFINED ARCH_EXPECT_RULE AND ARCH_EXPECT_RULE STREQUAL "SEAM_FENCE")
    set(_arch_root "${_arch_scan_root}")
    _arch_assert_seam_fenced("seam_compound.c" "tp_session_job_attach_internal"
        "A6 seam-fence negative fixture")
    message(FATAL_ERROR
        "SEAM_FENCE fixture: the walker ACCEPTED a compound #if condition, "
        "so an always-true guard would pass as a seam fence.")
endif()

if(DEFINED ARCH_EXPECT_RULE AND NOT ARCH_EXPECT_RULE STREQUAL "")
    if(NOT ARCH_EXPECT_RULE IN_LIST _arch_rules)
        message(FATAL_ERROR "unknown ARCH_EXPECT_RULE=${ARCH_EXPECT_RULE}")
    endif()
    # One seeded violation per fixture is the norm, so the expected count
    # defaults to 1. A fixture that exists to prove EVERY violation is reported
    # (not just the first) declares its own count with ARCH_EXPECT_HITS.
    if(NOT DEFINED ARCH_EXPECT_HITS OR ARCH_EXPECT_HITS STREQUAL "")
        set(ARCH_EXPECT_HITS 1)
    endif()
    foreach(_rule IN LISTS _arch_rules)
        get_property(_hits GLOBAL PROPERTY "ARCH_HITS_${_rule}")
        list(LENGTH _hits _count)
        if(_rule STREQUAL ARCH_EXPECT_RULE)
            if(NOT _count EQUAL ARCH_EXPECT_HITS)
                message(FATAL_ERROR
                    "${_rule} negative fixture expected exactly ${ARCH_EXPECT_HITS} hit(s); got ${_count}: ${_hits}")
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
# the symbol form "#include:<header>"; call scans use the call name; declarator
# scans use "<family>:<declared name>". Every scan reports EVERY site, so an
# entry must list every forbidden symbol its file legitimately names — no
# allowed first site masks a forbidden later one, and reordering code cannot
# move which hit is reported.
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

# USA-25 partial: owner: gate. Spec §16's "boundary checks reject mutation,
# filesystem, platform, and business policy in views" is a statement ABOUT a
# build check, so its owner is this file's four VIEW_* rules -- there is no
# runtime test that can prove it. apps/gui/test_gui_canonical_identity.c carries
# the closest behavioural companion (no source decode on the UI thread), but a
# companion is not an owner. scripts/check_boundaries.sh R22 reads this line.
#
# "partial:" is load-bearing here, exactly as on a test-class tag: only two of
# the four rules are hard assertions (VIEW_ADMISSION for mutation, VIEW_IO for
# filesystem). VIEW_PLATFORM and VIEW_MODEL_POLICY are debt REPORTS, and their
# per-symbol allowances below permit the very residue USA-25 forbids:
#   - platform policy in a view: gui_view_chrome.c reaches windows.h, the
#     clipboard seam, `system`, and ShellExecute* directly;
#   - model/business policy in a view: gui_view_canvas.c, gui_view_chrome.c and
#     gui_view_settings.c read the exporter registry, tp_validate, pack results,
#     and the effective-shape projection.
# Those sites are pinned per symbol (a NEW symbol of either family still fails),
# so the id has not lost its owner -- but the gate proves USA-25 for every view
# EXCEPT that allowlist, which the PLATFORM-SEAM / SR-BASE cuts retire.
_arch_assert_rule(VIEW_ADMISSION "A2c/A2d")
# A7: zero debt by construction. The deferred-intent queue is private to the
# actions layer, so the names this rule forbids have no external linkage at
# all -- a view that trips it could not link even if the rule were removed.
_arch_assert_rule(VIEW_ACTION_STATE "A7 one deferred-intent queue")
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
_arch_assert_rule(CORE_FRONTEND "A1a/A1b")
_arch_assert_rule(ASYNC_RAW_SESSION "A1c/A2b")

# The zero-only guards and their embedded negative fixtures share this token
# matcher. The fixtures use header includes because a forbidden driver reached
# through a header is exactly as callable as one included by a .c file.
function(_arch_source_contains source symbol out_contains)
    set(_source "${source}")
    string(REGEX REPLACE "/\\*([^*]|\\*+[^*/])*\\*+/" " " _source "${_source}")
    string(REGEX REPLACE "//[^\r\n]*" " " _source "${_source}")
    if(_source MATCHES
       "(^|[^A-Za-z0-9_])${symbol}([^A-Za-z0-9_]|$)")
        set(${out_contains} true PARENT_SCOPE)
    else()
        set(${out_contains} false PARENT_SCOPE)
    endif()
endfunction()

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
    _arch_source_contains("${_source}" "${symbol}" _contains)
    if(_contains)
        message(FATAL_ERROR
            "${remove_in} deletion regressed: ${relative_path} contains ${symbol}")
    endif()
endfunction()

# Local negative fixtures for the two transitive-header holes guarded below.
# Keep them in this checker: no production fixture file should need to carry a
# deliberately forbidden include merely to prove the boundary walker sees it.
_arch_source_contains(
    "#include \"gui_actions_driver.h\"\n"
    "gui_actions_driver[.]h" _seed_view_driver)
if(NOT _seed_view_driver)
    message(FATAL_ERROR
        "A7-selftest: view-header fixture escaped gui_actions_driver.h detection")
endif()
string(CONCAT _arch_seed_project_driver_source
    "#include \"gui_project_test_driver.h\"\n"
    "static bool second_driver(void) { return gui_project_test_new() }\n")
_arch_source_contains(
    "${_arch_seed_project_driver_source}"
    "gui_project_test_driver[.]h" _seed_project_driver)
if(NOT _seed_project_driver)
    message(FATAL_ERROR
        "A2d-selftest: wrapper-based project-driver fixture escaped detection")
endif()
string(CONCAT _arch_seed_project_fixture_source
    "#include \"test_gui_action_trace_fixture.h\"\n"
    "static bool second_driver(void) { return gui_project_test_new() }\n")
_arch_source_contains(
    "${_arch_seed_project_fixture_source}"
    "test_gui_[A-Za-z0-9_]*[.]h" _seed_project_fixture)
if(NOT _seed_project_fixture)
    message(FATAL_ERROR
        "A2d-selftest: transitive GUI test-fixture include escaped detection")
endif()

# Completed A1c/A2b worker and host-admission cuts.
foreach(_entry IN ITEMS
        "thrd_t" "thrd_create" "thrd_join" "job_join"
        "pack_worker" "export_worker" "job_start_thread"
        "job_thread_start" "job_thread_start_context"
        "fail_next_thread_create")
    _arch_assert_absent("packer/src/tp_job.c" "${_entry}" "A1c")
endforeach()
_arch_assert_absent("packer/src/tp_job.c"
                    "tp_session[ \t]*\\*[ \t]*session[ \t]*;" "A1c")
_arch_assert_absent("apps/gui/gui_pack_jobs.c"
                    "tp_session[ \t]*\\*" "A2b")
_arch_assert_absent("apps/gui/gui_pack_jobs.c" "wait_for_job" "A2b")
foreach(_symbol IN ITEMS
        job_session
        tp_session_job_active
        tp_session_job_poll
        tp_session_job_take_result
        tp_session_job_cancel
        tp_session_pack_job_start
        tp_session_export_start)
    _arch_assert_absent("apps/gui/gui_pack_jobs.c" "${_symbol}" "A2b")
endforeach()
foreach(_path IN ITEMS apps/gui/gui_project.c apps/gui/gui_project.h
                       apps/gui/gui_actions.c)
    _arch_assert_absent("${_path}" "gui_project_session_for_jobs" "A2b")
endforeach()
_arch_assert_absent("apps/gui/gui_actions.c"
                    "s_refresh_fingerprint_session" "A2b")
_arch_assert_absent("apps/gui/main.c" "gui_pack_worker_active" "A2b")
_arch_assert_absent("apps/gui/main.c" "gui_pack_poll" "A2b")
file(GLOB _gui_shipping_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/gui*.c"
    "${_arch_root}/apps/gui/main.c")
file(GLOB _gui_shipping_headers LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/*.h")

foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_project.c"
       OR _filename STREQUAL "gui_project_file.c"
       OR _filename STREQUAL "gui_selftest.c")
        continue()
    endif()
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    foreach(_symbol IN ITEMS
            tp_session_pack_job_start
            tp_session_export_start
            tp_session_refresh_start
            tp_session_job_active
            tp_session_job_poll
            tp_session_job_take_result
            tp_session_job_cancel
            tp_session_update)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "A2b single GUI host admission owner")
    endforeach()
endforeach()
_arch_assert_absent(
    "apps/gui/gui_project_file.c"
    "tp_session_refresh_start"
    "A2b Refresh admission belongs to gui_project.c")

# Completed A2a observation ownership. Recovery may take a temporary immutable
# snapshot for candidate preflight and post-Save-As metadata; it never observes.
foreach(_entry IN ITEMS
        "apps/gui/gui_project.c|tp_session_snapshot_create"
        "apps/gui/gui_project.c|tp_session_snapshot_destroy"
        "apps/gui/gui_project.c|tp_session_observe"
        "apps/gui/gui_project_file.c|tp_session_observe"
        "apps/gui/gui_project_internal.h|tp_session_snapshot[ \t]*\\*[ \t]*snapshot"
        "apps/gui/gui_rows.c|tp_session_snapshot_source_generation"
        "apps/gui/gui_project_file.c|recompute_name")
    string(REPLACE "|" ";" _parts "${_entry}")
    list(GET _parts 0 _path)
    list(GET _parts 1 _symbol)
    _arch_assert_absent("${_path}" "${_symbol}" "A2a")
endforeach()

file(GLOB _gui_observation_sources LIST_DIRECTORIES false
    "${_arch_root}/apps/gui/*.c"
    "${_arch_root}/apps/gui/*.h")
foreach(_source IN LISTS _gui_observation_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    cmake_path(CONVERT "${_relative}" TO_CMAKE_PATH_LIST _relative NORMALIZE)
    if(_relative MATCHES
       "^apps/gui/(gui_selftest|tp_bench_[^/]*|test_[^/]*|client_parity_[^/]*)\\.(c|h)$")
        continue()
    endif()
    foreach(_symbol IN ITEMS tp_session_observe
                             tp_session_observation_destroy)
        _arch_assert_absent(
            "${_relative}" "${_symbol}"
            "A2a single GUI observation owner")
    endforeach()
    if(NOT _relative STREQUAL "apps/gui/gui_project_recovery.c")
        foreach(_symbol IN ITEMS tp_session_snapshot_create
                                 tp_session_snapshot_destroy)
            _arch_assert_absent(
                "${_relative}" "${_symbol}"
                "A2a single GUI snapshot owner")
        endforeach()
    endif()
endforeach()

# Completed A2c mutation cut. The explicit operation-lowering module is the
# sole GUI operation-batch ingress and therefore calls the session directly.
_arch_assert_absent("apps/gui/gui_project_operations.c" "\"project\\.edit\"" "A2c")
foreach(_path IN ITEMS apps/gui/gui_project.c
                       apps/gui/gui_project_mutations.c)
    _arch_assert_absent(
        "${_path}" "gui_project__refresh_after_session_commit" "A2c")
    _arch_assert_absent(
        "${_path}" "gui_project__next_transaction_id" "A2c")
endforeach()
_arch_assert_absent("apps/gui/gui_project_internal.h" "txn_seq" "A2c")
foreach(_path IN ITEMS apps/gui/gui_view_chrome.c apps/gui/main.c)
    _arch_assert_absent("${_path}" "do_undo" "A2c")
    _arch_assert_absent("${_path}" "do_redo" "A2c")
endforeach()
_arch_assert_absent("apps/gui/main.c"
                    "gui_project_anim_remove_frame"
                    "A2c frame-pinned view ingress")

foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_project_operations.c"
       OR _filename STREQUAL "gui_selftest.c")
        continue()
    endif()
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    _arch_assert_absent(
        "${_relative}" "tp_session_apply"
        "A2c single GUI mutation owner")
    if(NOT _filename STREQUAL "gui_project_operations.c")
        _arch_assert_absent(
            "${_relative}" "gui_session_client_submit"
            "A5 single GUI submit owner")
    endif()
    _arch_assert_absent(
        "${_relative}" "gui_project__refresh_after_session_commit"
        "A5 observation-owned mutation refresh")
endforeach()

# Completed A2d lifecycle cut.
_arch_assert_absent("apps/gui/gui_project_file.c" "install_session" "A2d")
foreach(_symbol IN ITEMS
        gui_project_step
        gui_project_step_result)
    _arch_assert_absent(
        "apps/gui/gui_project.h" "${_symbol}"
        "A2d project FSM driver is internal")
endforeach()
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    if(NOT _filename STREQUAL "gui_project.c"
       AND NOT _filename STREQUAL "gui_actions.c")
        _arch_assert_absent(
            "${_relative}" "gui_project_step"
            "A2d one internal project FSM driver")
    endif()
    if(NOT _filename STREQUAL "gui_selftest.c")
        _arch_assert_absent(
            "${_relative}" "gui_project_test_driver[.]h"
            "A2d shipping sources cannot import the test project driver")
        _arch_assert_absent(
            "${_relative}" "test_gui_[A-Za-z0-9_]*[.]h"
            "A2d shipping sources cannot import GUI test fixtures")
    endif()
    if(_filename MATCHES "^gui_view_.*\\.c$")
        _arch_assert_absent(
            "${_relative}" "gui_actions_step"
            "A7 views submit requests and never drive the host")
    endif()
    if(NOT _filename STREQUAL "gui_actions.c"
       AND NOT _filename STREQUAL "gui_selftest.c")
        _arch_assert_absent(
            "${_relative}" "gui_actions__test_drain_intents"
            "A7 half-step remains test-only")
    endif()
    foreach(_legacy IN ITEMS
            apply_pending
            gui_project_frame_begin
            gui_project_frame_end
            gui_project_take_completion
            gui_project_lifecycle_pump
            gui_actions_poll_host_completion
            gui_actions_pump_lifecycle
            gui_pack_poll)
        _arch_assert_absent(
            "${_relative}" "${_legacy}"
            "A2d/A7 deleted manual lifecycle protocol")
    endforeach()
endforeach()

# A view header is part of the view's compile-time surface: including a driver
# there exposes it transitively to every implementing TU. Scan the complete,
# explicitly declared view role (both .c and .h), not only shipping .c files.
foreach(_view IN LISTS _arch_view_files)
    _arch_assert_absent(
        "${_view}" "gui_actions_driver[.]h"
        "A7 views cannot include the actions FSM driver")
    _arch_assert_absent(
        "${_view}" "gui_actions_dev[.]h"
        "A7 views cannot include dev host adapters")
endforeach()

# Header-transitive project drivers are equally real drivers. The four owners
# below are explicit: the project driver declares the primitive, the actions
# and project internals expose it only to their implementations, and the test
# driver is a test-only inline harness. Every other production GUI header is
# forbidden from naming the primitive or importing either driver header.
set(_gui_project_driver_header_owners
    gui_actions_internal.h
    gui_project_driver.h
    gui_project_internal.h
    gui_project_test_driver.h)
foreach(_header IN LISTS _gui_shipping_headers)
    cmake_path(GET _header FILENAME _filename)
    if(_filename MATCHES "^test_.*[.]h$")
        continue()
    endif()
    if(_filename IN_LIST _gui_project_driver_header_owners)
        continue()
    endif()
    cmake_path(RELATIVE_PATH _header BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    _arch_assert_absent(
        "${_relative}" "gui_project_step"
        "A2d production headers cannot introduce a second project FSM driver")
    _arch_assert_absent(
        "${_relative}" "gui_project_driver[.]h"
        "A2d production headers cannot import the project FSM driver")
    _arch_assert_absent(
        "${_relative}" "gui_project_test_driver[.]h"
        "A2d production headers cannot import the test project driver")
    _arch_assert_absent(
        "${_relative}" "test_gui_[A-Za-z0-9_]*[.]h"
        "A2d production headers cannot import GUI test fixtures")
endforeach()
foreach(_symbol IN ITEMS
        gui_actions_step
        gui_actions_host_open
        gui_actions_host_shutdown_step
        gui_actions_shutdown
        do_pack_blocking
        do_undo
        do_redo
        gui_actions_refresh_diff_headless)
    _arch_assert_absent(
        "apps/gui/gui_actions.h" "${_symbol}"
        "A7 view ingress cannot expose FSM drivers or direct executors")
endforeach()
_arch_assert_absent(
    "apps/gui/gui_pack_jobs.c" "gui_project_driver[.]h"
    "A7 blocking adapters drive the actions FSM, never the project FSM")
foreach(_symbol IN ITEMS
        gui_project__session_client
        gui_project__host_queue)
    _arch_assert_absent(
        "apps/gui/gui_project_internal.h"
        "${_symbol}" "A2d")
    _arch_assert_absent(
        "apps/gui/gui_project_file.c"
        "${_symbol}" "A2d")
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
            "${_path}" "${_symbol}" "A2d")
    endforeach()
endforeach()
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_session_client[ \t\r\n]+client[ \t\r\n]*;"
    "A2d single client storage")
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_host_queue[ \t\r\n]+host_queue[ \t\r\n]*;"
    "A2d single host queue storage")
foreach(_path IN ITEMS
        apps/gui/gui_project.c
        apps/gui/gui_project.h)
    foreach(_symbol IN ITEMS
            gui_host_queue_cancelling
            gui_project_job_cancelling)
        _arch_assert_absent(
            "${_path}" "${_symbol}" "A2d")
    endforeach()
endforeach()
_arch_assert_absent("apps/gui/gui_actions_dialogs.c" "gui_project_new" "A2d")
_arch_assert_absent("apps/gui/gui_actions_dialogs.c" "gui_project_open" "A2d")
_arch_assert_absent("apps/gui/main.c" "gui_project_open" "A2d")
_arch_assert_absent("apps/gui/gui_pack_jobs.c"
                    "gui_project_lifecycle_begin_shutdown" "A2d")
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    if(_relative MATCHES
       "^apps/gui/gui_selftest\\.c$")
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
            "A2d single lifecycle owner")
    endforeach()
endforeach()

# History, identity, and source-runtime commands belong to the small project
# host, not to whichever file happens to hold a borrowed view. The host owns the
# sole active-session pointer, so these session entry points may appear only in
# gui_project.c/gui_project_file.c.
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(GET _source FILENAME _filename)
    if(_filename STREQUAL "gui_project.c"
       OR _filename STREQUAL "gui_project_file.c")
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
            "A2d single host command owner")
    endforeach()
endforeach()

# The active session is stored by the small project host. Other GUI modules may
# borrow it only through the narrow internal accessor.
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
    _arch_assert_absent(
        "${_relative}" "gui_host_queue_[A-Za-z0-9_]*"
        "A2d deleted host queue does not return")
    _arch_assert_absent(
        "${_relative}" "gui_host_binding_[A-Za-z0-9_]*"
        "A2d deleted host binding does not return")
    _arch_assert_absent(
        "${_relative}" "gui_session_client_[A-Za-z0-9_]*"
        "A2d deleted pseudo-client does not return")
    if(NOT "${_relative}" IN_LIST _arch_borrow_session_owners)
        _arch_assert_absent(
            "${_relative}" "gui_project__borrow_active_session"
            "A2d single session storage borrows in one place")
    endif()
endforeach()

# A3a atlas scalar drafts have one view-local reducer owner. The former action
# array and broad ready-operation route must not return.
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "CK_ATLAS_SETTING"
    "A3a atlas draft owner")
_arch_assert_absent(
    "apps/gui/gui_project_mutations.c"
    "make_atlas_key|gui_project_set_atlas_setting"
    "A3a atlas draft submit")
_arch_assert_absent(
    "apps/gui/gui_actions_internal.h"
    "atlas_setting_intent"
    "A3a action mirror deletion")
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
        "A3a legacy atlas ingress deletion")
endforeach()

# A3c completes the value-edit cutover. The action draft reducer is the sole
# retained value owner; project-level broad operations, timers, and mirror
# queues must not return.
_arch_assert_absent(
    "apps/gui/CMakeLists.txt"
    "gui_project_pending\\.c"
    "A3c pending owner deletion")
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
        "A3c pending API deletion")
endforeach()
_arch_assert_absent(
    "apps/gui/gui_project_internal.h"
    "gui_coalesce_(kind|key)|pending_(valid|key|op|time|expected_revision|preview_stale_before)"
    "A3c pending storage deletion")
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
        "A3c legacy edit route deletion")
endforeach()
foreach(_source IN LISTS _gui_shipping_sources)
    cmake_path(RELATIVE_PATH _source BASE_DIRECTORY "${_arch_root}"
               OUTPUT_VARIABLE _relative)
    _arch_assert_absent(
        "${_relative}"
        "gui_actions__flush_failed|gui_edit_target[ \t]*\\(|gui_project_(flush_pending|pending_route|pending_offer|pending_discard|peek_pending_slice9|flush_elapsed|tick|flush_error)|gui_project_set_(sprite|anim)|gui_project_set_target[ \t]*\\(|SPRITE_INTENT_|ANIMATION_INTENT_(FPS|PLAYBACK|FLIP)|TARGET_INTENT_FULL"
        "A3c shipping legacy edit route deletion")
endforeach()

# gui_project_pending.c's return is caught by the _arch_deleted_files registry
# at the top of this file (one place owns "intentionally absent").

# A4/A5 cutover hardening. Stable ids are the only cross-frame identity;
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
            "A4/A5 legacy identity and mutation route deletion")
    endforeach()
endforeach()

_arch_assert_absent(
    "apps/gui/client_parity_replay.c"
    "TP_TF_OUT_PATH"
    "A5 target path uses the narrow identified endpoint")
_arch_assert_absent(
    "apps/gui/gui_project_operations.c"
    "gui_session_client_snapshot"
    "A5 submit receipts have one owner")
_arch_assert_absent(
    "apps/gui/main.c"
    "s_shown_result"
    "A5 result identity uses stable atlas and publication version")
_arch_assert_absent(
    "apps/gui/gui_canvas.c"
    "ref[ \t]*->[ \t]*result"
    "A5 double-click identity uses result generation")
_arch_assert_absent(
    "apps/gui/gui_canvas.h"
    "typedef[ \t\r\n]+struct[ \t\r\n]+gui_canvas_double_click_ref[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_result)[ \t\r\n]*\\*"
    "A5 no retained result-pointer identity")
_arch_assert_absent(
    "apps/gui/gui_pack_preview.c"
    "s_preview[ \t]*\\.[ \t]*atlas_index"
    "A5 preview identity uses stable atlas id")
_arch_assert_absent(
    "packer/src/tp_job.c"
    "typedef[ \t\r\n]+struct[ \t\r\n]+tp_live_job[ \t\r\n]*\\{[^}]*((const[ \t\r\n]+)?tp_session)[ \t\r\n]*\\*"
    "A5 worker job owns immutable input, never raw session")
_arch_assert_absent(
    "packer/src/tp_job.c"
    "tp_arena_destroy[ \t\r\n]*\\([ \t\r\n]*result[ \t\r\n]*->[ \t\r\n]*pack[ \t\r\n]*\\.[ \t\r\n]*arena"
    "A5 job result lifetime has one retained owner")

# A6 job-owner test seam. tp_session_job_attach_internal adopts the active-job
# lease for a test-owned job. Production has no route to it; the sole
# production start path spawns fail-atomically and rejects a second live owner.
#
# Two halves, because either one alone can be satisfied trivially:
# 1. the guarded SYMBOL is inside a TP_ENABLE_TEST_SEAMS conditional in each of
#    the four files that own the seam (the `_arch_assert_seam_fenced` walker,
#    defined above the fixture dispatch), and
# 2. no other shipping TU names either symbol.

foreach(_path IN ITEMS packer/src/tp_session.c
                       packer/src/tp_job_owner_internal.h)
    _arch_assert_seam_fenced(
        "${_path}" "tp_session_job_attach_internal"
        "A6 job-owner seams stay compiled out of shipping")
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
            "A6 job attach is a test seam, not a production entry point")
    endif()
    _arch_assert_absent(
        "${_relative}" "tp_session_job_observation_begin_internal"
        "A6 deleted job observation seam does not return")
endforeach()

message(STATUS "architecture boundaries hold")
