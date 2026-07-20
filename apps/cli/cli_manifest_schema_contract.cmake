file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/extra")

execute_process(
    COMMAND "${EXE}" version --json
    RESULT_VARIABLE MANIFEST_RC
    OUTPUT_VARIABLE MANIFEST
    ERROR_VARIABLE MANIFEST_ERR)
if(NOT MANIFEST_RC EQUAL 0)
    message(FATAL_ERROR "version manifest failed: ${MANIFEST_RC}\n${MANIFEST}\n${MANIFEST_ERR}")
endif()

string(JSON MANIFEST_SCHEMA ERROR_VARIABLE MANIFEST_SCHEMA_ERROR GET "${MANIFEST}" schema)
if(MANIFEST_SCHEMA_ERROR OR NOT MANIFEST_SCHEMA EQUAL 2)
    message(FATAL_ERROR "version manifest schema must be 2: ${MANIFEST}")
endif()

function(run_variant FAMILY VARIANT)
    set(ARGS ${ARGN})
    if(VARIANT STREQUAL "dry_run")
        list(APPEND ARGS --dry-run)
    endif()
    list(APPEND ARGS --json)
    execute_process(
        COMMAND "${EXE}" ${ARGS}
        RESULT_VARIABLE RC
        OUTPUT_VARIABLE OUT
        ERROR_VARIABLE ERR)
    if(NOT RC EQUAL 0)
        message(FATAL_ERROR "${FAMILY}.${VARIANT} failed: ${RC}\n${OUT}\n${ERR}")
    endif()
    string(JSON ACTUAL_SCHEMA ERROR_VARIABLE ACTUAL_ERROR GET "${OUT}" schema)
    string(JSON ADVERTISED_SCHEMA ERROR_VARIABLE ADVERTISED_ERROR
        GET "${MANIFEST}" verbs "${FAMILY}" "${VARIANT}")
    if(ACTUAL_ERROR OR ADVERTISED_ERROR)
        message(FATAL_ERROR
            "${FAMILY}.${VARIANT} schema is not machine-comparable\n"
            "manifest=${MANIFEST}\nresponse=${OUT}")
    endif()
    if(NOT ACTUAL_SCHEMA EQUAL ADVERTISED_SCHEMA)
        message(FATAL_ERROR
            "${FAMILY}.${VARIANT} advertises schema ${ADVERTISED_SCHEMA} "
            "but emits ${ACTUAL_SCHEMA}")
    endif()
endfunction()

set(NEW_APPLY "${WORK}/new-apply.ntpacker_project")
set(NEW_PREVIEW "${WORK}/new-preview.ntpacker_project")
run_variant(new apply new "${NEW_APPLY}")
run_variant(new dry_run new "${NEW_PREVIEW}")

set(PROJ "${WORK}/project.ntpacker_project")
configure_file("${TD}/clean.ntpacker_project" "${PROJ}" COPYONLY)
file(COPY "${TD}/sprites" DESTINATION "${WORK}")
configure_file("${TD}/sprites/hero.png" "${WORK}/extra/apply.png" COPYONLY)
configure_file("${TD}/sprites/coin.png" "${WORK}/extra/preview.png" COPYONLY)

run_variant(add apply add "${PROJ}" clean "${WORK}/extra/apply.png" --kind file)
run_variant(add dry_run add "${PROJ}" clean "${WORK}/extra/preview.png" --kind file)
run_variant(remove apply remove "${PROJ}" clean "${WORK}/extra/apply.png")
run_variant(remove dry_run remove "${PROJ}" clean "${WORK}/sprites")
run_variant(set apply set "${PROJ}" clean padding=3)
run_variant(set dry_run set "${PROJ}" clean padding=4)
run_variant(sprite apply sprite set "${PROJ}" clean coin origin=0.25,0.75)
run_variant(sprite dry_run sprite set "${PROJ}" clean hero origin=0.75,0.25)
run_variant(anim apply anim create "${PROJ}" clean applied coin hero)
run_variant(anim dry_run anim create "${PROJ}" clean preview coin hero)
run_variant(target apply target add "${PROJ}" clean defold out/applied)
run_variant(target dry_run target set "${PROJ}" clean defold enabled=0)
run_variant(atlas apply atlas add "${PROJ}" applied)
run_variant(atlas dry_run atlas add "${PROJ}" preview)

execute_process(
    COMMAND "${EXE}" anim list "${PROJ}" clean --json
    RESULT_VARIABLE LIST_RC
    OUTPUT_VARIABLE LIST_OUT
    ERROR_VARIABLE LIST_ERR)
if(NOT LIST_RC EQUAL 0)
    message(FATAL_ERROR "anim.list failed: ${LIST_RC}\n${LIST_OUT}\n${LIST_ERR}")
endif()
string(JSON LIST_SCHEMA ERROR_VARIABLE LIST_SCHEMA_ERROR GET "${LIST_OUT}" schema)
string(JSON ADVERTISED_LIST_SCHEMA ERROR_VARIABLE ADVERTISED_LIST_ERROR
    GET "${MANIFEST}" verbs anim list)
if(LIST_SCHEMA_ERROR OR ADVERTISED_LIST_ERROR OR
   NOT LIST_SCHEMA EQUAL ADVERTISED_LIST_SCHEMA)
    message(FATAL_ERROR
        "anim.list schema is not advertised independently from mutation variants\n"
        "manifest=${MANIFEST}\nresponse=${LIST_OUT}")
endif()
