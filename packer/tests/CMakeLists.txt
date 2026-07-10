add_executable(tp_test_arena test_arena.c)
target_link_libraries(tp_test_arena PRIVATE tp_core unity)
target_compile_options(tp_test_arena PRIVATE -U_DLL)
add_test(NAME tp_arena COMMAND tp_test_arena)

add_executable(tp_test_name_map test_name_map.c)
target_link_libraries(tp_test_name_map PRIVATE tp_core unity)
target_compile_options(tp_test_name_map PRIVATE -U_DLL)
add_test(NAME tp_name_map COMMAND tp_test_name_map)
