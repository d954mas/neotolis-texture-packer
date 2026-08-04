cmake_minimum_required(VERSION 3.25)

if(NOT DEFINED TP_STAGE_SCRIPT OR
   NOT DEFINED TP_FORMAT_FIXTURE_ROOT OR
   NOT DEFINED TP_TEST_ROOT)
    message(FATAL_ERROR
        "format package staging contract requires the stage script, fixture root, and test root")
endif()

function(assert_exists path)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "expected path does not exist: ${path}")
    endif()
endfunction()

function(assert_not_exists path)
    if(EXISTS "${path}")
        message(FATAL_ERROR "unexpected path exists: ${path}")
    endif()
endfunction()

function(assert_file_text path expected)
    assert_exists("${path}")
    file(READ "${path}" actual)
    if(NOT actual STREQUAL expected)
        message(FATAL_ERROR
            "unexpected content in '${path}': expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(write_manifest path entries)
    file(WRITE "${path}"
        "set(TP_FORMAT_PACKAGE_MANIFEST_DEFINED TRUE)\n"
        "set(TP_FORMAT_PACKAGE_MANIFEST \"${entries}\")\n")
endfunction()

function(copy_minimal_package source_root package_name)
    set(package_dir "${source_root}/${package_name}")
    file(MAKE_DIRECTORY "${package_dir}")
    file(COPY_FILE
        "${TP_FORMAT_FIXTURE_ROOT}/valid-minimal/format.json"
        "${package_dir}/format.json")
    file(COPY_FILE
        "${TP_FORMAT_FIXTURE_ROOT}/valid-minimal/export.lua"
        "${package_dir}/export.lua")
endfunction()

function(run_stage expected_success source_root destination manifest)
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DTP_FORMAT_STAGE_DEST=${destination}"
            "-DTP_FORMAT_SOURCE_ROOT=${source_root}"
            "-DTP_FORMAT_MANIFEST=${manifest}"
            -P "${TP_STAGE_SCRIPT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE stdout
        ERROR_VARIABLE stderr)
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR
            "format staging unexpectedly failed (${result}): ${stdout}${stderr}")
    endif()
    if(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "format staging unexpectedly succeeded")
    endif()
endfunction()

function(reset_case name out_root out_source out_destination out_manifest)
    set(root "${TP_TEST_ROOT}/${name}")
    set(source "${root}/source")
    set(destination "${root}/output/formats")
    set(manifest "${root}/manifest.cmake")
    file(REMOVE_RECURSE "${root}")
    file(MAKE_DIRECTORY "${source}" "${destination}")
    file(WRITE "${destination}/last-good.txt" "preserve")
    set(${out_root} "${root}" PARENT_SCOPE)
    set(${out_source} "${source}" PARENT_SCOPE)
    set(${out_destination} "${destination}" PARENT_SCOPE)
    set(${out_manifest} "${manifest}" PARENT_SCOPE)
endfunction()

function(assert_last_good destination)
    assert_file_text("${destination}/last-good.txt" "preserve")
    file(GLOB destination_entries LIST_DIRECTORIES TRUE
        RELATIVE "${destination}" "${destination}/*")
    if(NOT destination_entries STREQUAL "last-good.txt")
        message(FATAL_ERROR
            "failed staging changed the last-good tree: '${destination_entries}'")
    endif()
    assert_not_exists("${destination}.tp-stage")
    assert_not_exists("${destination}.tp-backup")
endfunction()

file(REMOVE_RECURSE "${TP_TEST_ROOT}")
file(MAKE_DIRECTORY "${TP_TEST_ROOT}")

# A package-directory spelling is diagnostic context, not a lowercase slug.
# Uppercase and underscore are both valid under the API-v1 package contract.
reset_case("valid" root source destination manifest)
copy_minimal_package("${source}" "Format_1")
file(WRITE "${source}/Format_1/source-only.txt" "must not be staged")
file(MAKE_DIRECTORY "${destination}/stale-package")
file(WRITE "${destination}/stale-package/stale.txt" "stale")
write_manifest("${manifest}" "Format_1")
run_stage(TRUE "${source}" "${destination}" "${manifest}")

file(GLOB_RECURSE staged_files LIST_DIRECTORIES FALSE
    RELATIVE "${destination}" "${destination}/*")
list(SORT staged_files)
set(expected_files "Format_1/export.lua;Format_1/format.json")
if(NOT staged_files STREQUAL expected_files)
    message(FATAL_ERROR
        "staged package set differs: expected '${expected_files}', got '${staged_files}'")
endif()
file(SHA256 "${source}/Format_1/format.json" source_descriptor_hash)
file(SHA256 "${destination}/Format_1/format.json" staged_descriptor_hash)
file(SHA256 "${source}/Format_1/export.lua" source_script_hash)
file(SHA256 "${destination}/Format_1/export.lua" staged_script_hash)
if(NOT source_descriptor_hash STREQUAL staged_descriptor_hash OR
   NOT source_script_hash STREQUAL staged_script_hash)
    message(FATAL_ERROR "staging did not preserve the two fixed package files")
endif()
assert_not_exists("${destination}.tp-stage")
assert_not_exists("${destination}.tp-backup")

# The production manifest is intentionally empty until the first runtime format
# ships. An empty stage is still a successful, complete replacement.
reset_case("empty" root source destination manifest)
file(MAKE_DIRECTORY "${destination}/stale-package")
file(WRITE "${destination}/stale-package/stale.txt" "stale")
write_manifest("${manifest}" "")
run_stage(TRUE "${source}" "${destination}" "${manifest}")
file(GLOB_RECURSE staged_files LIST_DIRECTORIES FALSE
    RELATIVE "${destination}" "${destination}/*")
if(staged_files)
    message(FATAL_ERROR
        "empty manifest staged unexpected files: '${staged_files}'")
endif()
assert_not_exists("${destination}.tp-stage")
assert_not_exists("${destination}.tp-backup")

# Duplicate entries fail before replacing the last-good destination.
reset_case("duplicate" root source destination manifest)
copy_minimal_package("${source}" "duplicate")
write_manifest("${manifest}" "duplicate;duplicate")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")

# Dot components and separator-bearing components must never address outside the
# source root or destination root.
reset_case("invalid-dot" root source destination manifest)
file(WRITE "${source}/format.json" "{}")
file(WRITE "${source}/export.lua" "return function() end")
write_manifest("${manifest}" ".")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")

reset_case("path-escape" root source destination manifest)
copy_minimal_package("${root}" "escape")
write_manifest("${manifest}" "../escape")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")

# Every fixed input is validated before replacement begins.
reset_case("missing-file" root source destination manifest)
copy_minimal_package("${source}" "valid-first")
file(MAKE_DIRECTORY "${source}/missing-file")
file(WRITE "${source}/missing-file/format.json" "{}")
write_manifest("${manifest}" "valid-first;missing-file")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")

reset_case("nonregular-file" root source destination manifest)
file(MAKE_DIRECTORY
    "${source}/nonregular-file"
    "${source}/nonregular-file/format.json")
file(WRITE "${source}/nonregular-file/export.lua" "return function() end")
write_manifest("${manifest}" "nonregular-file")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")

if(UNIX)
    reset_case("fifo-file" root source destination manifest)
    file(MAKE_DIRECTORY "${source}/fifo-file")
    file(WRITE "${source}/fifo-file/export.lua" "return function() end")
    execute_process(
        COMMAND mkfifo "${source}/fifo-file/format.json"
        RESULT_VARIABLE fifo_result)
    if(NOT fifo_result EQUAL 0)
        message(FATAL_ERROR "could not create FIFO staging fixture")
    endif()
    write_manifest("${manifest}" "fifo-file")
    run_stage(FALSE "${source}" "${destination}" "${manifest}")
    assert_last_good("${destination}")
endif()

# A crash can leave only the backup visible. Recovery belongs to the previous
# transaction and must happen even when the newly requested input is invalid.
reset_case("interrupted-backup" root source destination manifest)
file(RENAME "${destination}" "${destination}.tp-backup")
file(MAKE_DIRECTORY "${destination}.tp-stage")
file(WRITE "${destination}.tp-stage/incomplete.txt" "discard")
write_manifest("${manifest}" "missing-package")
run_stage(FALSE "${source}" "${destination}" "${manifest}")
assert_last_good("${destination}")
