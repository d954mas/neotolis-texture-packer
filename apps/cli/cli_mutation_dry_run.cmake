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
if(NOT RC EQUAL 3)
    message(FATAL_ERROR "selector-miss dry-run exit=${RC}: ${OUT} ${ERR}")
endif()
string(JSON ERROR_ID GET "${OUT}" error id)
if(NOT ERROR_ID STREQUAL "not_found")
    message(FATAL_ERROR "selector-miss dry-run error mismatch: ${OUT}")
endif()

execute_process(
    COMMAND "${EXE}" set "${PROJ}" clean max_size=99999 --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 2)
    message(FATAL_ERROR "validation-reject dry-run exit=${RC}: ${OUT} ${ERR}")
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
string(JSON COMMAND GET "${OUT}" command)
string(JSON DRY GET "${OUT}" dry_run)
string(JSON CHANGES GET "${OUT}" would_change)
string(JSON OPS GET "${OUT}" operation_count)
string(JSON GENERATED_LEN LENGTH "${OUT}" generated_ids)
string(JSON NOTICES_LEN LENGTH "${OUT}" notices)
if(NOT "${COMMAND}" STREQUAL "new" OR NOT DRY OR NOT CHANGES OR
   NOT "${OPS}" EQUAL 0 OR "${GENERATED_LEN}" LESS 2 OR NOT "${NOTICES_LEN}" EQUAL 0)
    message(FATAL_ERROR "new dry-run contract mismatch: ${OUT}")
endif()

execute_process(
    COMMAND "${EXE}" new "${PROJ}" --dry-run --json
    RESULT_VARIABLE RC OUTPUT_VARIABLE OUT ERROR_VARIABLE ERR)
if(NOT RC EQUAL 3)
    message(FATAL_ERROR "existing-destination dry-run exit=${RC}: ${OUT} ${ERR}")
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
