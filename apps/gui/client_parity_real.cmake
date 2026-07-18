file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}" "${WORK}/assets")
file(WRITE "${WORK}/assets/hero.png" "parity sprite hero")
file(WRITE "${WORK}/assets/coin.png" "parity sprite coin")

function(run_ok)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc
        OUTPUT_VARIABLE stdout ERROR_VARIABLE stderr)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "command failed (${rc}): ${ARGN}\n${stdout}\n${stderr}")
    endif()
endfunction()

function(compare_family family)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${WORK}/${family}/cli.ntpacker_project"
        "${WORK}/${family}/gui.ntpacker_project"
        RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "${family}: CLI and GUI canonical bytes differ")
    endif()
endfunction()

set(BASE "${WORK}/base.ntpacker_project")
run_ok("${REPLAY}" seed "${BASE}")

# Atlas create/rename/masked settings, then remove from the same shared-ID base.
file(MAKE_DIRECTORY "${WORK}/atlas")
configure_file("${BASE}" "${WORK}/atlas/base.ntpacker_project" COPYONLY)
configure_file("${BASE}" "${WORK}/atlas/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" atlas add "${WORK}/atlas/cli.ntpacker_project" two)
run_ok("${NTPACKER}" target add "${WORK}/atlas/cli.ntpacker_project" two
    json-neotolis out/two)
run_ok("${NTPACKER}" atlas rename "${WORK}/atlas/cli.ntpacker_project" two golden)
run_ok("${NTPACKER}" set "${WORK}/atlas/cli.ntpacker_project" golden padding=4 margin=2)
run_ok("${REPLAY}" replay atlas "${WORK}/atlas/base.ntpacker_project"
    "${WORK}/atlas/cli.ntpacker_project" "${WORK}/atlas/gui.ntpacker_project")
compare_family(atlas)

file(MAKE_DIRECTORY "${WORK}/atlas_remove")
configure_file("${WORK}/atlas/cli.ntpacker_project"
    "${WORK}/atlas_remove/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/atlas/cli.ntpacker_project"
    "${WORK}/atlas_remove/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" atlas remove "${WORK}/atlas_remove/cli.ntpacker_project" golden)
run_ok("${REPLAY}" replay atlas_remove
    "${WORK}/atlas_remove/base.ntpacker_project"
    "${WORK}/atlas_remove/cli.ntpacker_project"
    "${WORK}/atlas_remove/gui.ntpacker_project")
compare_family(atlas_remove)

# Source add harvests the CLI-generated structural ID; removal starts from those
# exact shared bytes. The second add is the file-CLI's documented no-op path.
file(MAKE_DIRECTORY "${WORK}/source")
configure_file("${BASE}" "${WORK}/source/base.ntpacker_project" COPYONLY)
configure_file("${BASE}" "${WORK}/source/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" add "${WORK}/source/cli.ntpacker_project" atlas1 "${WORK}/assets")
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/source/before_noop.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" add "${WORK}/source/cli.ntpacker_project" atlas1 "${WORK}/assets")
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${WORK}/source/before_noop.ntpacker_project"
    "${WORK}/source/cli.ntpacker_project" RESULT_VARIABLE noop_rc)
if(NOT noop_rc EQUAL 0)
    message(FATAL_ERROR "source duplicate add was not a canonical-byte no-op")
endif()
run_ok("${REPLAY}" replay source "${WORK}/source/base.ntpacker_project"
    "${WORK}/source/cli.ntpacker_project" "${WORK}/source/gui.ntpacker_project")
compare_family(source)

file(MAKE_DIRECTORY "${WORK}/source_remove")
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/source_remove/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/source_remove/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" remove "${WORK}/source_remove/cli.ntpacker_project" atlas1 "${WORK}/assets")
run_ok("${REPLAY}" replay source_remove
    "${WORK}/source_remove/base.ntpacker_project"
    "${WORK}/source_remove/cli.ntpacker_project"
    "${WORK}/source_remove/gui.ntpacker_project")
compare_family(source_remove)

# Sprite name plus every override field in one real CLI intent. The GUI replay
# uses the same canonical {source_id,key}; omitted atlas/source fields remain so.
file(MAKE_DIRECTORY "${WORK}/sprite")
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/sprite/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/sprite/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" sprite set "${WORK}/sprite/cli.ntpacker_project" atlas1 hero
    rename=hero_final origin=0.25,0.75 slice9=1,2,3,4 shape=0
    allow_rotate=0 max_vertices=6 margin=5 extrude=2)
run_ok("${REPLAY}" replay sprite "${WORK}/sprite/base.ntpacker_project"
    "${WORK}/sprite/cli.ntpacker_project" "${WORK}/sprite/gui.ntpacker_project")
compare_family(sprite)

file(MAKE_DIRECTORY "${WORK}/sprite_clear")
configure_file("${WORK}/sprite/cli.ntpacker_project"
    "${WORK}/sprite_clear/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/sprite/cli.ntpacker_project"
    "${WORK}/sprite_clear/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" sprite unset "${WORK}/sprite_clear/cli.ntpacker_project" atlas1 hero)
run_ok("${REPLAY}" replay sprite_clear
    "${WORK}/sprite_clear/base.ntpacker_project"
    "${WORK}/sprite_clear/cli.ntpacker_project"
    "${WORK}/sprite_clear/gui.ntpacker_project")
compare_family(sprite_clear)

# Animation covers create, frame add/move/remove, full settings mask and rename.
file(MAKE_DIRECTORY "${WORK}/animation")
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/animation/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/source/cli.ntpacker_project"
    "${WORK}/animation/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" anim create "${WORK}/animation/cli.ntpacker_project" atlas1 walk hero coin)
run_ok("${NTPACKER}" anim add-frame "${WORK}/animation/cli.ntpacker_project" atlas1 walk hero)
run_ok("${NTPACKER}" anim move-frame "${WORK}/animation/cli.ntpacker_project" atlas1 walk 0 2)
run_ok("${NTPACKER}" anim remove-frame "${WORK}/animation/cli.ntpacker_project" atlas1 walk 1)
run_ok("${NTPACKER}" anim set "${WORK}/animation/cli.ntpacker_project" atlas1 walk
    fps=12 playback=loop_forward flip_h=1 flip_v=0)
run_ok("${NTPACKER}" anim rename "${WORK}/animation/cli.ntpacker_project" atlas1 walk stroll)
run_ok("${REPLAY}" replay animation "${WORK}/animation/base.ntpacker_project"
    "${WORK}/animation/cli.ntpacker_project" "${WORK}/animation/gui.ntpacker_project")
compare_family(animation)

file(MAKE_DIRECTORY "${WORK}/animation_remove")
configure_file("${WORK}/animation/cli.ntpacker_project"
    "${WORK}/animation_remove/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/animation/cli.ntpacker_project"
    "${WORK}/animation_remove/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" anim remove "${WORK}/animation_remove/cli.ntpacker_project" atlas1 stroll)
run_ok("${REPLAY}" replay animation_remove
    "${WORK}/animation_remove/base.ntpacker_project"
    "${WORK}/animation_remove/cli.ntpacker_project"
    "${WORK}/animation_remove/gui.ntpacker_project")
compare_family(animation_remove)

# Target create and partial SET intentionally omit exporter, then remove.
file(MAKE_DIRECTORY "${WORK}/target")
configure_file("${BASE}" "${WORK}/target/base.ntpacker_project" COPYONLY)
configure_file("${BASE}" "${WORK}/target/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" target add "${WORK}/target/cli.ntpacker_project" atlas1 defold out/d)
run_ok("${NTPACKER}" target set "${WORK}/target/cli.ntpacker_project" atlas1 defold
    out=out/d2 enabled=0)
run_ok("${REPLAY}" replay target "${WORK}/target/base.ntpacker_project"
    "${WORK}/target/cli.ntpacker_project" "${WORK}/target/gui.ntpacker_project")
compare_family(target)

file(MAKE_DIRECTORY "${WORK}/target_remove")
configure_file("${WORK}/target/cli.ntpacker_project"
    "${WORK}/target_remove/base.ntpacker_project" COPYONLY)
configure_file("${WORK}/target/cli.ntpacker_project"
    "${WORK}/target_remove/cli.ntpacker_project" COPYONLY)
run_ok("${NTPACKER}" target remove "${WORK}/target_remove/cli.ntpacker_project" atlas1 defold)
run_ok("${REPLAY}" replay target_remove
    "${WORK}/target_remove/base.ntpacker_project"
    "${WORK}/target_remove/cli.ntpacker_project"
    "${WORK}/target_remove/gui.ntpacker_project")
compare_family(target_remove)

message(STATUS "real CLI/GUI mutation parity: OK")
