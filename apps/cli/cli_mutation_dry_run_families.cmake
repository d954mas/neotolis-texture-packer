file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/extra")
set(PROJ "${WORK}/project.ntpacker_project")
configure_file("${TD}/clean.ntpacker_project" "${PROJ}" COPYONLY)
file(COPY "${TD}/sprites" DESTINATION "${WORK}")
configure_file("${TD}/sprites/hero.png" "${WORK}/extra/anim.png" COPYONLY)
configure_file("${TD}/sprites/coin.png" "${WORK}/extra/new.png" COPYONLY)

# Prepare one saved animation so both animation create and remove can be previewed
# independently against the same immutable baseline.
execute_process(
    COMMAND "${EXE}" add "${PROJ}" clean "${WORK}/extra/anim.png" --kind file
    RESULT_VARIABLE SETUP_RC OUTPUT_VARIABLE SETUP_OUT ERROR_VARIABLE SETUP_ERR)
if(NOT SETUP_RC EQUAL 0)
    message(FATAL_ERROR "dry-run source setup failed: ${SETUP_RC}\n${SETUP_OUT}\n${SETUP_ERR}")
endif()
execute_process(
    COMMAND "${EXE}" anim create "${PROJ}" clean existing anim
    RESULT_VARIABLE SETUP_RC OUTPUT_VARIABLE SETUP_OUT ERROR_VARIABLE SETUP_ERR)
if(NOT SETUP_RC EQUAL 0)
    message(FATAL_ERROR "dry-run family setup failed: ${SETUP_RC}\n${SETUP_OUT}\n${SETUP_ERR}")
endif()

function(snapshot_state PREFIX)
    file(SHA256 "${PROJ}" HASH)
    file(GLOB_RECURSE LIST RELATIVE "${WORK}" "${WORK}/*")
    list(SORT LIST)
    set("${PREFIX}_HASH" "${HASH}" PARENT_SCOPE)
    set("${PREFIX}_LIST" "${LIST}" PARENT_SCOPE)
endfunction()

function(assert_state_unchanged PREFIX LABEL)
    snapshot_state(AFTER)
    if(NOT "${${PREFIX}_HASH}" STREQUAL "${AFTER_HASH}")
        message(FATAL_ERROR "${LABEL} changed project bytes")
    endif()
    if(NOT "${${PREFIX}_LIST}" STREQUAL "${AFTER_LIST}")
        message(FATAL_ERROR "${LABEL} changed directory listing\nbefore=${${PREFIX}_LIST}\nafter=${AFTER_LIST}")
    endif()
endfunction()

function(run_preview LABEL COMMAND EXPECT_GENERATED)
    execute_process(
        COMMAND "${EXE}" ${ARGN} --dry-run --json
        RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
    if(NOT RC EQUAL 0)
        message(FATAL_ERROR "${LABEL} failed: ${RC}\n${OUT}\n${ERR}")
    endif()
    string(JSON ACTUAL_COMMAND GET "${OUT}" command)
    string(JSON DRY GET "${OUT}" dry_run)
    string(JSON GENERATED_LEN LENGTH "${OUT}" generated_ids)
    string(JSON AFFECTED_LEN LENGTH "${OUT}" affected_ids)
    string(JSON NOTICES_LEN LENGTH "${OUT}" notices)
    if(NOT ACTUAL_COMMAND STREQUAL "${COMMAND}" OR NOT DRY OR
       NOT GENERATED_LEN EQUAL EXPECT_GENERATED OR AFFECTED_LEN LESS 1 OR
       NOT NOTICES_LEN EQUAL 0)
        message(FATAL_ERROR "${LABEL} preview contract mismatch: ${OUT}")
    endif()
    if(EXPECT_GENERATED EQUAL 1)
        string(JSON GENERATED_ID GET "${OUT}" generated_ids 0)
        set(EXPECTED_PREFIX "${COMMAND}")
        if("${COMMAND}" STREQUAL "anim")
            set(EXPECTED_PREFIX "anim")
        endif()
        if(NOT GENERATED_ID MATCHES "^${EXPECTED_PREFIX}_[0-9a-f]+$")
            message(FATAL_ERROR "${LABEL} generated id has wrong kind: ${OUT}")
        endif()
        set(GENERATED_IS_AFFECTED FALSE)
        math(EXPR AFFECTED_LAST "${AFFECTED_LEN} - 1")
        foreach(INDEX RANGE 0 ${AFFECTED_LAST})
            string(JSON AFFECTED_ID GET "${OUT}" affected_ids ${INDEX})
            if(AFFECTED_ID STREQUAL GENERATED_ID)
                set(GENERATED_IS_AFFECTED TRUE)
            endif()
        endforeach()
        if(NOT GENERATED_IS_AFFECTED)
            message(FATAL_ERROR "${LABEL} generated id is not the candidate result id: ${OUT}")
        endif()
    endif()
    assert_state_unchanged(BASE "${LABEL}")
endfunction()

function(run_error_parity LABEL EXPECT_EXIT)
    execute_process(
        COMMAND "${EXE}" ${ARGN} --dry-run --json
        RESULT_VARIABLE DRY_RC OUTPUT_VARIABLE DRY_OUT ERROR_VARIABLE DRY_ERR)
    execute_process(
        COMMAND "${EXE}" ${ARGN} --json
        RESULT_VARIABLE LIVE_RC OUTPUT_VARIABLE LIVE_OUT ERROR_VARIABLE LIVE_ERR)
    if(NOT DRY_RC EQUAL EXPECT_EXIT OR NOT LIVE_RC EQUAL EXPECT_EXIT OR
       NOT DRY_OUT STREQUAL LIVE_OUT OR NOT DRY_ERR STREQUAL LIVE_ERR)
        message(FATAL_ERROR
            "${LABEL} dry/live rejection mismatch\n"
            "dry(${DRY_RC})=${DRY_OUT}${DRY_ERR}\n"
            "live(${LIVE_RC})=${LIVE_OUT}${LIVE_ERR}")
    endif()
    assert_state_unchanged(BASE "${LABEL}")
endfunction()

snapshot_state(BASE)
execute_process(
    COMMAND "${EXE}" add "${PROJ}" clean "${WORK}/extra/new.png" --kind file --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "source add dry-run failed: ${RC}\n${OUT}\n${ERR}")
endif()
string(JSON GENERATED_LEN LENGTH "${OUT}" generated_ids)
string(JSON GENERATED_ID GET "${OUT}" generated_ids 0)
string(JSON AFFECTED_LEN LENGTH "${OUT}" affected_ids)
string(JSON NOTICES_LEN LENGTH "${OUT}" notices)
set(GENERATED_IS_AFFECTED FALSE)
math(EXPR AFFECTED_LAST "${AFFECTED_LEN} - 1")
foreach(INDEX RANGE 0 ${AFFECTED_LAST})
    string(JSON AFFECTED_ID GET "${OUT}" affected_ids ${INDEX})
    if(AFFECTED_ID STREQUAL GENERATED_ID)
        set(GENERATED_IS_AFFECTED TRUE)
    endif()
endforeach()
if(NOT GENERATED_LEN EQUAL 1 OR NOT GENERATED_ID MATCHES "^source_[0-9a-f]+$" OR
   NOT GENERATED_IS_AFFECTED OR
   NOT NOTICES_LEN EQUAL 0)
    message(FATAL_ERROR "source add preview contract mismatch: ${OUT}")
endif()
assert_state_unchanged(BASE "source add dry-run")

run_preview("source remove dry-run" "remove" 0
    remove "${PROJ}" clean "${WORK}/sprites")
run_error_parity("source selector miss" 3
    remove "${PROJ}" clean "${WORK}/missing")

run_preview("sprite set dry-run" "sprite" 0
    sprite set "${PROJ}" clean coin origin=0.25,0.75)
run_error_parity("sprite selector miss" 3
    sprite set "${PROJ}" clean missing origin=0.25,0.75)

run_preview("animation create dry-run" "anim" 1
    anim create "${PROJ}" clean preview coin hero)
run_preview("animation remove dry-run" "anim" 0
    anim remove "${PROJ}" clean existing)
run_error_parity("animation selector miss" 3
    anim remove "${PROJ}" clean missing)

run_preview("target create dry-run" "target" 1
    target add "${PROJ}" clean defold out/preview)
run_preview("target remove dry-run" "target" 0
    target remove "${PROJ}" clean json-neotolis)
run_error_parity("target selector miss" 3
    target remove "${PROJ}" clean 9)

run_preview("atlas create dry-run" "atlas" 1
    atlas add "${PROJ}" preview)
run_preview("atlas remove dry-run" "atlas" 0
    atlas remove "${PROJ}" clean)
run_error_parity("atlas selector miss" 3
    atlas remove "${PROJ}" missing)
