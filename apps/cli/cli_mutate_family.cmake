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
    run_json(0 "inspect;sprites=2" inspect "${PROJ}" --json)   # still 2 (deduped)
    run(0 remove "${PROJ}" atlas1 "${SPRITES}")
    run_json(0 "inspect;sprites=0" inspect "${PROJ}" --json)
    run(3 remove "${PROJ}" atlas1 "no/such/source")            # not found -> exit 3

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
    run(0 sprite set "${PROJ}" atlas1 hero origin=0.25,0.75 slice9=1,2,3,4)
    assert_contains("\"name\": \"hero\"")
    assert_contains("\"origin\"")
    assert_contains("\"slice9\"")
    run(0 sprite set "${PROJ}" atlas1 hero shape=0 max_vertices=6)
    assert_contains("\"max_vertices\"")
    # inherit-clear every field -> the whole override entry prunes away (sparse).
    run(0 sprite set "${PROJ}" atlas1 hero origin=inherit slice9=inherit shape=inherit max_vertices=inherit)
    assert_absent("\"name\": \"hero\"")
    # rename + unset
    run(0 sprite set "${PROJ}" atlas1 foo rename=bar)
    assert_contains("\"rename\": \"bar\"")
    run(0 sprite unset "${PROJ}" atlas1 foo)
    assert_absent("\"name\": \"foo\"")
    run(0 sprite unset "${PROJ}" atlas1 never_existed)         # idempotent clear -> ok
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
    assert_order("\"name\": \"s0\"" "\"name\": \"s1\"")        # s1 did not jump ahead of s0
    assert_order("\"name\": \"s1\"" "\"name\": \"s2\"")        # s1 stayed in place, before s2
    # F2: an override key that carries an extension is stored + cleared VERBATIM (the
    # pre-cutover bytes), NOT stripped to the export bridge. `sprite set img.png` stores
    # "img.png" (not "img"); `sprite unset img.png` finds+removes that verbatim record.
    run(0 sprite set "${PROJ}" atlas1 img.png origin=0.1,0.2)
    assert_contains("\"name\": \"img.png\"")                   # verbatim -- a bridged store would write "img"
    run(0 sprite unset "${PROJ}" atlas1 img.png)               # keys on the verbatim record, not "img"
    assert_absent("\"name\": \"img.png\"")                     # cleared (would silently no-op if it keyed on "img")
    run(2 sprite set "${PROJ}" atlas1 x origin=1)              # origin needs 2 comps -> usage
    run(2 sprite set "${PROJ}" atlas1 x bogus=1)               # unknown field -> usage

elseif(FAMILY STREQUAL "anim")
    run(0 new "${PROJ}")
    run_json(0 "mutation;count=3" anim create "${PROJ}" atlas1 walk f1 f2 f3 --json)
    run_json(0 "anim;count=1;schema=4" anim list "${PROJ}" atlas1 --json)  # F1-03: shared query schema 4
    run(0 anim add-frame "${PROJ}" atlas1 walk f_at --at 1)     # insert at index 1
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
    run(0 anim rename "${PROJ}" atlas1 walk stroll)            # success: rename applies + saves
    assert_contains("\"name\": \"stroll\"")                    # new name persisted (deterministic save)
    assert_absent("\"name\": \"walk\"")                        # old name gone
    run(2 anim rename "${PROJ}" atlas1 stroll idle)            # collides with another animation -> usage(2)
    run(3 anim rename "${PROJ}" atlas1 ghost whatever)         # unknown animation id -> project(3)
    run(2 anim rename "${PROJ}" atlas1 stroll)                 # missing <new> (arg count) -> usage(2)
    assert_contains("\"name\": \"stroll\"")                    # rejects left the model unchanged
    run(0 anim remove "${PROJ}" atlas1 stroll)
    assert_absent("\"name\": \"stroll\"")

elseif(FAMILY STREQUAL "target")
    run(0 new "${PROJ}")                                        # seeds 1 json-neotolis target
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
    run(0 set "${PROJ}" atlas1 max_size=512 padding=2)
    run(0 sprite set "${PROJ}" atlas1 s origin=0.1,0.2)
    run(0 anim create "${PROJ}" atlas1 a f1 f2)
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
