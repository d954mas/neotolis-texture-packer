# ctest driver (plan B4): drive a SEQUENCE of ntpacker mutation commands against a
# FRESH project created under a scratch WORK dir, asserting the per-command exit code
# and the resulting project-file state. Mutations touch ONLY WORK -- never the
# checked-in testdata. One invocation per verb FAMILY keeps the ctest count sane.
#
#   cmake -DEXE=<ntpacker> -DCHECKER=<cli_json_check> -DWORK=<scratch>
#         -DTD=<testdata_dir> -DFAMILY=<new|source|set|sprite|anim|target|atlas|stable>
#         -P cli_mutate_family.cmake

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(PROJ "${WORK}/p.ntpacker_project")
set(SPRITES "${TD}/sprites")

# _run(<expected-exit> <ntpacker args...>): run EXE with the args, assert the exit code, and
# expose captured stdout to the CALLER as ${_out}. Single home for the execute_process +
# exit-check + failure-diagnostics so run/run_json/run_match never drift apart.
function(_run EXPECT)
    execute_process(COMMAND "${EXE}" ${ARGN}
        RESULT_VARIABLE _c OUTPUT_VARIABLE _o ERROR_VARIABLE _e)
    if(NOT "${_c}" STREQUAL "${EXPECT}")
        message(FATAL_ERROR "[${FAMILY}] '${ARGN}'\n  exit ${_c} != expected ${EXPECT}\n--stdout--\n${_o}\n--stderr--\n${_e}")
    endif()
    set(_out "${_o}" PARENT_SCOPE)
endfunction()

# run(<expected-exit> <ntpacker args...>): fail loudly on an exit-code mismatch.
function(run EXPECT)
    _run(${EXPECT} ${ARGN})
endfunction()

# run_json(<expected-exit> <checker-mode+assertions;-list> <ntpacker args...>):
# runs the command, writes stdout to a temp file, and validates it with cli_json_check.
function(run_json EXPECT CHECKARGS)
    _run(${EXPECT} ${ARGN})
    set(_f "${WORK}/_out.json")
    file(WRITE "${_f}" "${_out}")
    execute_process(COMMAND "${CHECKER}" "${_f}" ${CHECKARGS}
        RESULT_VARIABLE _cc OUTPUT_VARIABLE _co ERROR_VARIABLE _ce)
    if(NOT _cc EQUAL 0)
        message(FATAL_ERROR "[${FAMILY}] json check failed on '${ARGN}': ${_co}${_ce}\n--payload--\n${_out}")
    endif()
endfunction()

# run_match(<expected-exit> <stdout-regex> <ntpacker args...>): run, assert exit, assert stdout matches.
function(run_match EXPECT REGEX)
    _run(${EXPECT} ${ARGN})
    if(NOT "${_out}" MATCHES "${REGEX}")
        message(FATAL_ERROR "[${FAMILY}] '${ARGN}'\n  stdout did not match /${REGEX}/\n--stdout--\n${_out}")
    endif()
endfunction()

function(assert_contains NEEDLE)
    file(READ "${PROJ}" _content)
    string(FIND "${_content}" "${NEEDLE}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "[${FAMILY}] project file missing expected substring: ${NEEDLE}\n--file--\n${_content}")
    endif()
endfunction()

function(assert_absent NEEDLE)
    file(READ "${PROJ}" _content)
    string(FIND "${_content}" "${NEEDLE}" _pos)
    if(NOT _pos EQUAL -1)
        message(FATAL_ERROR "[${FAMILY}] project file must NOT contain: ${NEEDLE}\n--file--\n${_content}")
    endif()
endfunction()

function(assert_count NEEDLE EXPECTED)
    file(READ "${PROJ}" _content)
    string(REGEX MATCHALL "${NEEDLE}" _matches "${_content}")
    list(LENGTH _matches _count)
    if(NOT _count EQUAL EXPECTED)
        message(FATAL_ERROR "[${FAMILY}] expected ${EXPECTED} matches for ${NEEDLE}, got ${_count}\n--file--\n${_content}")
    endif()
endfunction()

# assert_order(FIRST SECOND): both substrings present AND FIRST occurs before SECOND in
# the saved file. Used to pin sprite-array ORDER (a record must stay in place, not get
# dropped and re-appended at the end).
function(assert_order FIRST SECOND)
    file(READ "${PROJ}" _content)
    string(FIND "${_content}" "${FIRST}" _p1)
    string(FIND "${_content}" "${SECOND}" _p2)
    if(_p1 EQUAL -1)
        message(FATAL_ERROR "[${FAMILY}] ordering check: substring absent: ${FIRST}\n--file--\n${_content}")
    endif()
    if(_p2 EQUAL -1)
        message(FATAL_ERROR "[${FAMILY}] ordering check: substring absent: ${SECOND}\n--file--\n${_content}")
    endif()
    if(NOT _p1 LESS _p2)
        message(FATAL_ERROR "[${FAMILY}] wrong order: '${FIRST}' (@${_p1}) must precede '${SECOND}' (@${_p2})\n--file--\n${_content}")
    endif()
endfunction()

# ---------------------------------------------------------------------------

if(FAMILY STREQUAL "new")
    run(0 new "${PROJ}")
    if(NOT EXISTS "${PROJ}")
        message(FATAL_ERROR "[new] project file was not created")
    endif()
    assert_contains("json-neotolis")           # seeded default target
    assert_contains("\"name\": \"atlas1\"")     # default atlas
    run(3 new "${PROJ}")                        # refuse to overwrite -> exit 3
    run_json(0 "inspect;sprites=0;schema=4" inspect "${PROJ}" --json)  # F1-03: schema-4 contract pinned (sprites gained sprite_id + source)

elseif(FAMILY STREQUAL "source")
    run(0 new "${PROJ}")
    run_json(0 "mutation;count=1" add "${PROJ}" atlas1 "${SPRITES}" --json)
    run_json(0 "inspect;sprites=2" inspect "${PROJ}" --json)   # folder expands to 2 sprites
    run(0 add "${PROJ}" atlas1 "${SPRITES}")                   # duplicate -> ok no-op
    run(0 add "${PROJ}" atlas1 "${SPRITES}/../sprites")        # lexical alias -> same source, ok no-op
    run_json(0 "inspect;sprites=2" inspect "${PROJ}" --json)   # still 2 (deduped)
    run(0 remove "${PROJ}" atlas1 "${SPRITES}")
    run_json(0 "inspect;sprites=0" inspect "${PROJ}" --json)
    run(3 remove "${PROJ}" atlas1 "no/such/source")            # not found -> exit 3
    string(REPEAT "oversized_segment_" 260 _oversized_source)
    run_match(2 "\"id\":\"out_of_bounds\"" remove "${PROJ}" atlas1
        "${_oversized_source}" --json)                          # malformed path -> usage, not not-found
    # M5 regression: the old 1024-byte frontend buffers silently persisted only
    # the first 999 bytes while reporting success. The full argument must survive.
    string(REPEAT "long_segment_" 92 _long_source)
    run(0 add "${PROJ}" atlas1 "${WORK}/${_long_source}")
    assert_contains("${_long_source}")

elseif(FAMILY STREQUAL "set")
    run(0 new "${PROJ}")
    run_json(0 "mutation" set "${PROJ}" atlas1 max_size=512 padding=4 shape=1 --json)
    assert_contains("\"max_size\": 512")
    assert_contains("\"padding\": 4")
    run(0 set "${PROJ}" atlas1 allow_transform=false power_of_two=true pixels_per_unit=50)
    assert_contains("\"pixels_per_unit\": 50")
    run(2 set "${PROJ}" atlas1 max_size=99999)                 # out of range -> usage
    # H/P2-12: the --json reject payload must localize the offending op -- carry the core
    # error's `field` (closed-vocabulary key) + `op_index` (0 = first/only op), not just id+message.
    # `.*` between the two keys tolerates a future added key without pinning exact adjacency.
    run_match(2 "\"field\":\"max_size\".*\"op_index\":0" set "${PROJ}" atlas1 max_size=99999 --json)
    run(2 set "${PROJ}" atlas1 bogus=1)                        # unknown key -> usage
    run(2 set "${PROJ}" atlas1 max_size=notanumber)            # malformed value -> usage
    run(2 set "${PROJ}" atlas1 name=x)                         # steer to atlas rename -> usage
    run(3 set "${PROJ}" ghost max_size=1)                      # unknown atlas -> project

elseif(FAMILY STREQUAL "sprite")
    run(0 new "${PROJ}")
    set(LOCAL_SPRITES "${WORK}/local_sprites")
    file(MAKE_DIRECTORY "${LOCAL_SPRITES}")
    foreach(_sprite hero coin s0 s1 s2 img)
        file(WRITE "${LOCAL_SPRITES}/${_sprite}.png" "scan-only fixture")
    endforeach()
    run(0 add "${PROJ}" atlas1 "${LOCAL_SPRITES}")
    run(0 sprite set "${PROJ}" atlas1 hero origin=0.25,0.75 slice9=1,2,3,4)
    assert_contains("\"key\": \"hero.png\"")
    assert_contains("\"origin\"")
    assert_contains("\"slice9\"")
    run(0 sprite set "${PROJ}" atlas1 hero shape=0 max_vertices=6)
    assert_contains("\"max_vertices\"")
    # inherit-clear every field -> the whole override entry prunes away (sparse).
    run(0 sprite set "${PROJ}" atlas1 hero origin=inherit slice9=inherit shape=inherit max_vertices=inherit)
    assert_absent("\"key\": \"hero.png\"")
    # rename + unset
    run(0 sprite set "${PROJ}" atlas1 coin rename=bar)
    assert_contains("\"rename\": \"bar\"")
    run(0 sprite unset "${PROJ}" atlas1 coin)
    assert_absent("\"key\": \"coin.png\"")
    run(3 sprite unset "${PROJ}" atlas1 never_existed)         # unresolved selectors never create pending records
    # F1: renaming a MID-array sprite while clearing its LAST override must NOT reorder the
    # sprites array (the pre-cutover inline path edited in place). Build three retained
    # records s0/s1/s2, then in ONE command clear s1's only override AND rename it: s1 must
    # STAY between s0 and s2. The pre-fix op order (override.set then name.set) pruned the
    # now-default s1 record and re-appended it at the END on the rename -> array reorder ->
    # different saved bytes. Emitting name.set FIRST keeps it in place.
    run(0 sprite set "${PROJ}" atlas1 s0 margin=5)
    run(0 sprite set "${PROJ}" atlas1 s1 extrude=7)
    run(0 sprite set "${PROJ}" atlas1 s2 margin=9)
    run(0 sprite set "${PROJ}" atlas1 s1 extrude=inherit rename=mid1)  # clear last override + rename
    assert_contains("\"rename\": \"mid1\"")
    assert_order("\"key\": \"s0.png\"" "\"key\": \"s1.png\"")   # s1 did not jump ahead of s0
    assert_order("\"key\": \"s1.png\"" "\"key\": \"s2.png\"")   # s1 stayed in place, before s2
    # Canonical runtime resolution keeps the normalized extension-bearing source key.
    run(0 sprite set "${PROJ}" atlas1 img.png origin=0.1,0.2)
    assert_contains("\"key\": \"img.png\"")
    run(0 sprite unset "${PROJ}" atlas1 img.png)
    assert_absent("\"key\": \"img.png\"")

    # Architecture-foundation parity: a same-key selector in two sources is rejected
    # before mutation. A source-id compound resolves to the same canonical
    # {source_ref, source-local key} as the headless session resolver, survives a
    # separate-process reload, and unsetting one address leaves only the other.
    set(DUP_A "${WORK}/dup_a")
    set(DUP_B "${WORK}/dup_b")
    file(MAKE_DIRECTORY "${DUP_A}" "${DUP_B}")
    file(WRITE "${DUP_A}/shared.png" "a")
    file(WRITE "${DUP_B}/shared.png" "b")
    run(0 add "${PROJ}" atlas1 "${DUP_A}")
    run(0 add "${PROJ}" atlas1 "${DUP_B}")
    configure_file("${PROJ}" "${WORK}/before_ambiguous.bak" COPYONLY)
    _run(3 sprite set "${PROJ}" atlas1 shared origin=0.1,0.2 --json)
    if(NOT "${_out}" MATCHES "\"id\":\"ambiguous_selector\"" OR
       NOT "${_out}" MATCHES "\"candidates\":\\[" OR
       NOT "${_out}" MATCHES "\"id\":\"sprite_[0-9a-f]+\"" OR
       NOT "${_out}" MATCHES "\"kind\":\"sprite\"" OR
       NOT "${_out}" MATCHES "\"label\":\"sprite")
        message(FATAL_ERROR "[sprite] ambiguity payload lacks canonical candidate ids/kinds/labels\n${_out}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${WORK}/before_ambiguous.bak" "${PROJ}" RESULT_VARIABLE _ambiguous_changed)
    if(NOT _ambiguous_changed EQUAL 0)
        message(FATAL_ERROR "[sprite] ambiguous selector mutated the saved project")
    endif()
    file(READ "${PROJ}" _with_sources)
    string(REGEX MATCHALL "\"id\": \"source_[0-9a-f]+\"" _source_ids "${_with_sources}")
    list(LENGTH _source_ids _source_count)
    math(EXPR _source_a_index "${_source_count} - 2")
    math(EXPR _source_b_index "${_source_count} - 1")
    list(GET _source_ids ${_source_a_index} _source_a)
    list(GET _source_ids ${_source_b_index} _source_b)
    string(REGEX REPLACE ".*\"(source_[0-9a-f]+)\"" "\\1" _source_a "${_source_a}")
    string(REGEX REPLACE ".*\"(source_[0-9a-f]+)\"" "\\1" _source_b "${_source_b}")
    run(0 sprite set "${PROJ}" atlas1 "${_source_a}:shared.png" origin=0.1,0.2)
    run(0 sprite set "${PROJ}" atlas1 "${_source_b}:shared.png" origin=0.8,0.9)
    run(0 anim create "${PROJ}" atlas1 pick "${_source_b}:shared.png")
    assert_contains("\"source\": \"${_source_a}\"")
    assert_contains("\"source\": \"${_source_b}\"")
    assert_count("\\\"key\\\": \\\"shared\\.png\\\"" 3)
    assert_count("\\\"source\\\": \\\"${_source_b}\\\"" 2)
    run_json(0 "inspect;sprites=8;schema=4" inspect "${PROJ}" --json)
    run(0 sprite unset "${PROJ}" atlas1 "${_source_a}:shared.png")
    assert_absent("\"source\": \"${_source_a}\"")
    assert_contains("\"source\": \"${_source_b}\"")
    assert_count("\\\"key\\\": \\\"shared\\.png\\\"" 2)
    # Lexical parsing succeeds; the shared core owns semantic ranges and emits
    # the same typed field/status as headless clients.
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"ov_shape\""
        sprite set "${PROJ}" atlas1 hero shape=3 --json)
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"ov_shape\""
        sprite set "${PROJ}" atlas1 hero shape=65536 --json)
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"slice9_l\""
        sprite set "${PROJ}" atlas1 hero slice9=-1,0,0,0 --json)
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"ov_margin\""
        sprite set "${PROJ}" atlas1 hero margin=65535 --json)
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"ov_extrude\""
        sprite set "${PROJ}" atlas1 hero extrude=65535 --json)
    run_match(2 "\"id\":\"out_of_range\".*\"field\":\"ov_max_vertices\""
        sprite set "${PROJ}" atlas1 hero max_vertices=65542 --json)
    run(2 sprite set "${PROJ}" atlas1 hero origin=1)           # origin needs 2 comps -> usage
    run(2 sprite set "${PROJ}" atlas1 hero bogus=1)            # unknown field -> usage

elseif(FAMILY STREQUAL "anim")
    run(0 new "${PROJ}")
    file(MAKE_DIRECTORY "${WORK}/anim_sprites")
    foreach(_frame IN ITEMS f1 f2 f3 f_at overflow)
        file(WRITE "${WORK}/anim_sprites/${_frame}.png" "scan-only fixture")
    endforeach()
    run(0 add "${PROJ}" atlas1 "${WORK}/anim_sprites")
    run_json(0 "mutation;count=3" anim create "${PROJ}" atlas1 walk f1 f2 f3 --json)
    assert_contains("\"key\": \"f1.png\"")
    run_json(0 "anim;count=1;schema=4" anim list "${PROJ}" atlas1 --json)  # F1-03: shared query schema 4
    run(0 anim add-frame "${PROJ}" atlas1 walk f_at --at 1)     # insert at index 1
    run(2 anim add-frame "${PROJ}" atlas1 walk overflow --at 2147483648) # long->int overflow is usage
    run(0 anim move-frame "${PROJ}" atlas1 walk 0 3)           # move first to last
    run(0 anim remove-frame "${PROJ}" atlas1 walk f2)          # by key
    run(0 anim remove-frame "${PROJ}" atlas1 walk 0)           # by index
    run(0 anim set "${PROJ}" atlas1 walk fps=12 playback=loop_forward flip_h=1)
    assert_contains("\"fps\": 12")
    assert_contains("\"playback\": 1")
    assert_contains("\"flip_h\": true")
    run(2 anim set "${PROJ}" atlas1 walk playback=9)           # out of range -> usage
    run(2 anim set "${PROJ}" atlas1 walk fps=abc)              # malformed -> usage
    run(3 anim remove "${PROJ}" atlas1 ghost)                  # not found -> project
    run(3 anim add-frame "${PROJ}" atlas1 ghost k)             # anim not found -> project
    # H/P1-2: `anim rename` end-to-end -- SUCCESS (rename applies + persists) plus the SAME
    # error contracts `atlas rename` uses, mapped through the shared exit-code split:
    # arg-count/collision -> usage(2), unknown animation id -> project(3).
    run(0 anim create "${PROJ}" atlas1 idle)                   # a 2nd animation for the collision case
    run(2 anim create "${PROJ}" atlas1 idle)                   # duplicate create is core-owned
    run(0 anim rename "${PROJ}" atlas1 walk stroll)            # success: rename applies + saves
    assert_contains("\"name\": \"stroll\"")                    # new name persisted (deterministic save)
    assert_absent("\"name\": \"walk\"")                        # old name gone
    # Empty is a syntactically present value, so the typed operation/core rule
    # owns the rejection and its structured field (not a CLI-only rule fork).
    execute_process(
        COMMAND "${EXE}" anim rename "${PROJ}" atlas1 stroll "" --json
        RESULT_VARIABLE _empty_rc OUTPUT_VARIABLE _empty_out
        ERROR_VARIABLE _empty_err)
    if(NOT "${_empty_rc}" STREQUAL "2" OR
       NOT "${_empty_out}" MATCHES "\"id\":\"invalid_argument\"" OR
       NOT "${_empty_out}" MATCHES "\"field\":\"name\"")
        message(FATAL_ERROR
            "[anim] empty rename must be rejected by core\n"
            "  exit=${_empty_rc}\n--stdout--\n${_empty_out}\n--stderr--\n${_empty_err}")
    endif()
    run(2 anim rename "${PROJ}" atlas1 stroll idle)            # collides with another animation -> usage(2)
    run(3 anim rename "${PROJ}" atlas1 ghost whatever)         # unknown animation id -> project(3)
    run(2 anim rename "${PROJ}" atlas1 stroll)                 # missing <new> (arg count) -> usage(2)
    assert_contains("\"name\": \"stroll\"")                    # rejects left the model unchanged
    run(0 anim remove "${PROJ}" atlas1 stroll)
    assert_absent("\"name\": \"stroll\"")

elseif(FAMILY STREQUAL "target")
    run(0 new "${PROJ}")                                        # seeds 1 json-neotolis target
    run_match(3 "file_exists" new "${PROJ}" --json)
    run(0 target add "${PROJ}" atlas1 defold out/atlas1_def)
    assert_contains("\"exporter_id\": \"defold\"")
    run(2 target add "${PROJ}" atlas1 not-an-exporter out/x)    # unknown exporter -> usage
    run(0 target set "${PROJ}" atlas1 1 enabled=0 out=out/def2)
    assert_contains("\"out_path\": \"out/def2\"")
    assert_contains("\"enabled\": false")
    run(2 target set "${PROJ}" atlas1 1 exporter=nope)          # unknown exporter -> usage
    run(3 target set "${PROJ}" atlas1 9 enabled=1)              # bad index -> project
    run(0 target remove "${PROJ}" atlas1 defold)               # by id
    assert_absent("\"exporter_id\": \"defold\"")
    run(3 target remove "${PROJ}" atlas1 5)                    # bad index -> project
    run(0 target add "${PROJ}" atlas1 json-neotolis out/other)
    run_match(3 "ambiguous_selector" target remove "${PROJ}" atlas1 json-neotolis --json)

elseif(FAMILY STREQUAL "atlas")
    run(0 new "${PROJ}")
    run(0 atlas add "${PROJ}" two)
    assert_contains("\"name\": \"two\"")
    run(2 atlas add "${PROJ}" atlas1)                          # duplicate -> usage
    run(0 atlas rename "${PROJ}" two three)
    assert_contains("\"name\": \"three\"")
    assert_absent("\"name\": \"two\"")
    run(2 atlas rename "${PROJ}" three atlas1)                 # collides -> usage
    run(3 atlas rename "${PROJ}" ghost x)                      # not found -> project
    run(0 atlas remove "${PROJ}" three)
    assert_absent("\"name\": \"three\"")
    run(3 atlas remove "${PROJ}" ghost)                        # not found -> project

elseif(FAMILY STREQUAL "stable")
    # Build a non-trivial project, then a NET-ZERO atlas add/remove round-trip must
    # re-save byte-for-byte identical (the canonical writer through the mutation path).
    run(0 new "${PROJ}")
    file(MAKE_DIRECTORY "${WORK}/stable_sprites")
    file(WRITE "${WORK}/stable_sprites/s.png" "scan-only fixture")
    run(0 add "${PROJ}" atlas1 "${WORK}/stable_sprites")
    run(0 set "${PROJ}" atlas1 max_size=512 padding=2)
    run(0 sprite set "${PROJ}" atlas1 s origin=0.1,0.2)
    run(0 anim create "${PROJ}" atlas1 a s s)
    run(0 target add "${PROJ}" atlas1 defold out/d)
    configure_file("${PROJ}" "${WORK}/before.bak" COPYONLY)
    run(0 atlas add "${PROJ}" _tmp_)
    run(0 atlas remove "${PROJ}" _tmp_)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${WORK}/before.bak" "${PROJ}"
        RESULT_VARIABLE _cmp)
    if(NOT _cmp EQUAL 0)
        message(FATAL_ERROR "[stable] re-save is NOT byte-identical after a net-zero mutation")
    endif()

else()
    message(FATAL_ERROR "unknown FAMILY '${FAMILY}'")
endif()

message(STATUS "cli_mutate_family[${FAMILY}]: OK")
