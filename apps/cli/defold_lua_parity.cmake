if(NOT DEFINED EXE OR NOT DEFINED SOURCE OR NOT DEFINED WORK)
    message(FATAL_ERROR "EXE, SOURCE, and WORK are required")
endif()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/project/sprites")
file(COPY
    "${SOURCE}/apps/cli/testdata/sprites/hero.png"
    "${SOURCE}/apps/cli/testdata/sprites/coin.png"
    DESTINATION "${WORK}/project/sprites")

file(WRITE "${WORK}/project/parity.ntpacker_project" [=[
{
  "version": 5,
  "atlases": [
    {
      "id": "atlas_0000000000000000000000000000d401",
      "name": "defold-parity",
      "allow_transform": false,
      "padding": 1,
      "sources": [
        {
          "id": "source_0000000000000000000000000000d402",
          "path": "sprites"
        }
      ],
      "targets": [
        {
          "exporter_id": "defold-tpinfo-2",
          "id": "target_0000000000000000000000000000d404",
          "out_path": "out/atlas"
        }
      ]
    }
  ]
}
]=])

execute_process(
    COMMAND "${EXE}" pack "${WORK}/project/parity.ntpacker_project"
            --json --out-dir "${WORK}/output"
    RESULT_VARIABLE _code
    OUTPUT_VARIABLE _stdout
    ERROR_VARIABLE _stderr)
if(NOT _code EQUAL 0)
    message(FATAL_ERROR
        "Defold CLI parity exited ${_code}\n--stdout--\n${_stdout}\n--stderr--\n${_stderr}")
endif()

set(_files
    "${WORK}/output/out/atlas.tpinfo"
    "${WORK}/output/out/atlas.tpatlas"
    "${WORK}/output/out/atlas-0.png")
# Frozen from the native Defold producer before its removal. This remains an
# independent byte oracle instead of preserving a second implementation.
set(_native_sha256
    a63ed315867a7a3514fe3eede3ee9d62dc3623ed2f8605cc487906f92e23d15d
    2b81d93a16241a90d39d5e975c371a86dda5e8122d201e85f321861211136eef
    cc950d1f8098785cd23952a162edd4cb6ba04f8a6c4e61069ac106a6452b80f8)
foreach(_index RANGE 0 2)
    list(GET _files ${_index} _file)
    list(GET _native_sha256 ${_index} _expected)
    if(NOT EXISTS "${_file}")
        message(FATAL_ERROR "Defold CLI parity output is missing: ${_file}")
    endif()
    file(SHA256 "${_file}" _actual)
    if(NOT _actual STREQUAL _expected)
        message(FATAL_ERROR
            "Defold CLI output differs from the frozen native bytes: ${_actual} != ${_expected}")
    endif()
endforeach()

string(REGEX MATCHALL "\"exporter_id\"[ \t\r\n]*:[ \t\r\n]*\"defold-tpinfo-2\"" _lua_rows "${_stdout}")
list(LENGTH _lua_rows _lua_count)
if(NOT _lua_count EQUAL 1)
    message(FATAL_ERROR "Defold CLI report lacks its runtime format row\n${_stdout}")
endif()

string(REGEX MATCHALL "could not locate game[.]project above the output" _missing_notices "${_stdout}")
list(LENGTH _missing_notices _missing_notice_count)
if(NOT _missing_notice_count EQUAL 1)
    message(FATAL_ERROR
        "Defold CLI notice differs from the native oracle: got ${_missing_notice_count}\n${_stdout}")
endif()
