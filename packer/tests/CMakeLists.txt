add_executable(tp_test_arena test_arena.c)
target_link_libraries(tp_test_arena PRIVATE tp_core unity)
target_compile_options(tp_test_arena PRIVATE -U_DLL)
add_test(NAME tp_arena COMMAND tp_test_arena)

add_executable(tp_test_name_map test_name_map.c)
target_link_libraries(tp_test_name_map PRIVATE tp_core unity)
target_compile_options(tp_test_name_map PRIVATE -U_DLL)
add_test(NAME tp_name_map COMMAND tp_test_name_map)

# Synthetic D4 transform-decode test (plan §3.6). Includes the private helper
# header from src/ directly -- no public API pollution.
add_executable(tp_test_transform_decode test_transform_decode.c)
target_link_libraries(tp_test_transform_decode PRIVATE tp_core unity)
target_include_directories(tp_test_transform_decode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_transform_decode PRIVATE -U_DLL)
add_test(NAME tp_transform_decode COMMAND tp_test_transform_decode)

# Fixture-driven golden round-trip: builds every fixture case (tp_fixtures) via
# nt_builder (transitive through tp_core) then parses it back. Needs a scratch
# dir for the temp .ntpack files.
add_executable(tp_test_pack_read test_pack_read.c tp_fixtures.c)
target_link_libraries(tp_test_pack_read PRIVATE tp_core unity)
target_compile_options(tp_test_pack_read PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_test")
add_test(NAME tp_pack_read COMMAND tp_test_pack_read "${CMAKE_BINARY_DIR}/tp_test")

# UV recovery property test (plan §3.3): pure functions, no builder. Includes
# the private helper header from src/ directly.
add_executable(tp_test_uv_property test_uv_property.c)
target_link_libraries(tp_test_uv_property PRIVATE tp_core unity)
target_include_directories(tp_test_uv_property PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_options(tp_test_uv_property PRIVATE -U_DLL)
add_test(NAME tp_uv_property COMMAND tp_test_uv_property)

# Phase 1b tp_pack invariants + determinism (plan §3.4): drives nt_builder
# (transitive through tp_core) then parses back. Needs a scratch dir for the
# session .ntpack files.
add_executable(tp_test_pack test_pack.c)
target_link_libraries(tp_test_pack PRIVATE tp_core unity)
target_compile_options(tp_test_pack PRIVATE -U_DLL)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/tp_pack_test")
add_test(NAME tp_pack COMMAND tp_test_pack "${CMAKE_BINARY_DIR}/tp_pack_test")
