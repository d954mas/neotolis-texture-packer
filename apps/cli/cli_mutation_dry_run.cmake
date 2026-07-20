file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

set(PROJ "${WORK}/project.ntpacker_project")
configure_file("${TD}/clean.ntpacker_project" "${PROJ}" COPYONLY)
file(COPY "${TD}/sprites" DESTINATION "${WORK}")
file(SHA256 "${PROJ}" BEFORE_HASH)

execute_process(
    COMMAND "${EXE}" set "${PROJ}" clean max_size=512 --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "set dry-run exit ${RC}\nstdout=${OUT}\nstderr=${ERR}")
endif()
file(SHA256 "${PROJ}" AFTER_HASH)
if(NOT BEFORE_HASH STREQUAL AFTER_HASH)
    message(FATAL_ERROR "set dry-run changed project bytes")
endif()

set(EXPLICIT_ATLAS "atlas_00000000000000000000000000000101")
execute_process(
    COMMAND "${EXE}" set "${PROJ}" "${EXPLICIT_ATLAS}" padding=3 --dry-run --json
    RESULT_VARIABLE EXPLICIT_RC OUTPUT_VARIABLE EXPLICIT_OUT ERROR_VARIABLE EXPLICIT_ERR)
if(NOT EXPLICIT_RC EQUAL 0)
    message(FATAL_ERROR "explicit-id dry-run failed: ${EXPLICIT_RC} ${EXPLICIT_OUT} ${EXPLICIT_ERR}")
endif()
string(JSON EXPLICIT_AFFECTED GET "${EXPLICIT_OUT}" affected_ids 0)
if(NOT EXPLICIT_AFFECTED STREQUAL EXPLICIT_ATLAS)
    message(FATAL_ERROR "explicit affected id was not preserved: ${EXPLICIT_OUT}")
endif()
file(SHA256 "${PROJ}" EXPLICIT_HASH)
if(NOT BEFORE_HASH STREQUAL EXPLICIT_HASH)
    message(FATAL_ERROR "explicit-id dry-run changed project bytes")
endif()

foreach(KEY schema command dry_run would_change operation_count revision_before revision_after affected_ids generated_ids notices)
    string(JSON VALUE ERROR_VARIABLE JSON_ERROR GET "${OUT}" "${KEY}")
    if(NOT JSON_ERROR STREQUAL "NOTFOUND")
        message(FATAL_ERROR "set dry-run missing ${KEY}: ${JSON_ERROR}\npayload=${OUT}")
    endif()
endforeach()
string(JSON SCHEMA GET "${OUT}" schema)
string(JSON COMMAND GET "${OUT}" command)
string(JSON DRY GET "${OUT}" dry_run)
string(JSON CHANGES GET "${OUT}" would_change)
string(JSON OPS GET "${OUT}" operation_count)
string(JSON REV_BEFORE GET "${OUT}" revision_before)
string(JSON REV_AFTER GET "${OUT}" revision_after)
string(JSON AFFECTED_LEN LENGTH "${OUT}" affected_ids)
string(JSON GENERATED_LEN LENGTH "${OUT}" generated_ids)
string(JSON NOTICES_LEN LENGTH "${OUT}" notices)
if(NOT "${SCHEMA}" EQUAL 2 OR NOT "${COMMAND}" STREQUAL "set" OR NOT DRY OR
   NOT CHANGES OR NOT "${OPS}" EQUAL 1 OR NOT "${REV_BEFORE}" EQUAL 0 OR
   NOT "${REV_AFTER}" EQUAL 1 OR "${AFFECTED_LEN}" LESS 1 OR
   NOT "${GENERATED_LEN}" EQUAL 0 OR NOT "${NOTICES_LEN}" EQUAL 0)
    message(FATAL_ERROR "set dry-run contract mismatch: ${OUT}")
endif()

execute_process(
    COMMAND "${EXE}" set "${PROJ}" clean max_size=2048 --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 0)
    message(FATAL_ERROR "no-op dry-run failed: ${RC} ${OUT} ${ERR}")
endif()
string(JSON CHANGES GET "${OUT}" would_change)
string(JSON REV_AFTER GET "${OUT}" revision_after)
if(CHANGES OR NOT "${REV_AFTER}" EQUAL 0)
    message(FATAL_ERROR "semantic no-op was reported as a change: ${OUT}")
endif()
file(SHA256 "${PROJ}" NOOP_HASH)
if(NOT BEFORE_HASH STREQUAL NOOP_HASH)
    message(FATAL_ERROR "no-op dry-run changed project bytes")
endif()

execute_process(
    COMMAND "${EXE}" set "${PROJ}" missing max_size=512 --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
set(DRY_ERROR_OUT "${OUT}")
set(DRY_ERROR_ERR "${ERR}")
if(NOT RC EQUAL 3)
    message(FATAL_ERROR "selector-miss dry-run exit=${RC}: ${OUT} ${ERR}")
endif()
execute_process(
    COMMAND "${EXE}" set "${PROJ}" missing max_size=512 --json
    RESULT_VARIABLE LIVE_RC OUTPUT_VARIABLE LIVE_OUT ERROR_VARIABLE LIVE_ERR)
if(NOT LIVE_RC EQUAL 3 OR NOT DRY_ERROR_OUT STREQUAL LIVE_OUT OR
   NOT DRY_ERROR_ERR STREQUAL LIVE_ERR)
    message(FATAL_ERROR "selector-miss dry/live mismatch\ndry=${DRY_ERROR_OUT}${DRY_ERROR_ERR}\nlive=${LIVE_OUT}${LIVE_ERR}")
endif()
string(JSON ERROR_ID GET "${OUT}" error id)
if(NOT ERROR_ID STREQUAL "not_found")
    message(FATAL_ERROR "selector-miss dry-run error mismatch: ${OUT}")
endif()

execute_process(
    COMMAND "${EXE}" set "${PROJ}" clean max_size=99999 --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
set(DRY_ERROR_OUT "${OUT}")
set(DRY_ERROR_ERR "${ERR}")
if(NOT RC EQUAL 2)
    message(FATAL_ERROR "validation-reject dry-run exit=${RC}: ${OUT} ${ERR}")
endif()
execute_process(
    COMMAND "${EXE}" set "${PROJ}" clean max_size=99999 --json
    RESULT_VARIABLE LIVE_RC OUTPUT_VARIABLE LIVE_OUT ERROR_VARIABLE LIVE_ERR)
if(NOT LIVE_RC EQUAL 2 OR NOT DRY_ERROR_OUT STREQUAL LIVE_OUT OR
   NOT DRY_ERROR_ERR STREQUAL LIVE_ERR)
    message(FATAL_ERROR "validation dry/live mismatch\ndry=${DRY_ERROR_OUT}${DRY_ERROR_ERR}\nlive=${LIVE_OUT}${LIVE_ERR}")
endif()
string(JSON ERROR_ID GET "${OUT}" error id)
if(NOT ERROR_ID STREQUAL "out_of_range")
    message(FATAL_ERROR "validation-reject dry-run error mismatch: ${OUT}")
endif()

set(NEW_PROJ "${WORK}/new.ntpacker_project")
execute_process(
    COMMAND "${EXE}" new "${NEW_PROJ}" --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 0 OR EXISTS "${NEW_PROJ}")
    message(FATAL_ERROR "new dry-run published or failed: exit=${RC}\nstdout=${OUT}\nstderr=${ERR}")
endif()
set(FIRST_NEW_OUT "${OUT}")
execute_process(
    COMMAND "${EXE}" new "${NEW_PROJ}" --dry-run --json
    RESULT_VARIABLE SECOND_RC OUTPUT_VARIABLE SECOND_OUT ERROR_VARIABLE SECOND_ERR)
if(NOT SECOND_RC EQUAL 0 OR EXISTS "${NEW_PROJ}" OR
   NOT FIRST_NEW_OUT STREQUAL SECOND_OUT)
    message(FATAL_ERROR
        "identical new dry-runs were not byte-stable or had side effects\n"
        "first=${FIRST_NEW_OUT}\nsecond=${SECOND_OUT}\nstderr=${SECOND_ERR}")
endif()
string(JSON COMMAND GET "${OUT}" command)
string(JSON DRY GET "${OUT}" dry_run)
string(JSON CHANGES GET "${OUT}" would_change)
string(JSON OPS GET "${OUT}" operation_count)
string(JSON GENERATED_LEN LENGTH "${OUT}" generated_ids)
string(JSON GENERATED_SEMANTICS GET "${OUT}" generated_ids_semantics)
string(JSON NOTICES_LEN LENGTH "${OUT}" notices)
if(NOT "${COMMAND}" STREQUAL "new" OR NOT DRY OR NOT CHANGES OR
   NOT "${OPS}" EQUAL 0 OR NOT "${GENERATED_LEN}" EQUAL 0 OR
   NOT "${GENERATED_SEMANTICS}" STREQUAL "assigned_on_apply" OR
   NOT "${NOTICES_LEN}" EQUAL 0)
    message(FATAL_ERROR "new dry-run contract mismatch: ${OUT}")
endif()

execute_process(
    COMMAND "${EXE}" new "${PROJ}" --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
set(DRY_ERROR_OUT "${OUT}")
set(DRY_ERROR_ERR "${ERR}")
if(NOT RC EQUAL 3)
    message(FATAL_ERROR "existing-destination dry-run exit=${RC}: ${OUT} ${ERR}")
endif()
execute_process(
    COMMAND "${EXE}" new "${PROJ}" --json
    RESULT_VARIABLE LIVE_RC OUTPUT_VARIABLE LIVE_OUT ERROR_VARIABLE LIVE_ERR)
if(NOT LIVE_RC EQUAL 3 OR NOT DRY_ERROR_OUT STREQUAL LIVE_OUT OR
   NOT DRY_ERROR_ERR STREQUAL LIVE_ERR)
    message(FATAL_ERROR "destination dry/live mismatch\ndry=${DRY_ERROR_OUT}${DRY_ERROR_ERR}\nlive=${LIVE_OUT}${LIVE_ERR}")
endif()
string(JSON ERROR_ID GET "${OUT}" error id)
if(NOT ERROR_ID STREQUAL "file_exists")
    message(FATAL_ERROR "existing-destination dry-run error mismatch: ${OUT}")
endif()

set(HUMAN_PROJ "${WORK}/human.ntpacker_project")
execute_process(
    COMMAND "${EXE}" new "${HUMAN_PROJ}" --dry-run
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 0 OR EXISTS "${HUMAN_PROJ}" OR
   NOT OUT MATCHES "^Dry run: would create project ")
    message(FATAL_ERROR
        "human new dry-run was misleading: exit=${RC}\nstdout=${OUT}\nstderr=${ERR}")
endif()
