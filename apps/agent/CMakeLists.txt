tp_add_test(NAME agent_json
    SOURCES test_agent_json.c ../common/app_json.c
    LIBS tp_core cjson unity
    INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/apps/common"
    SANITIZE)

add_executable(agent_test_recover test_recover.c ../common/app_paths.c)
target_link_libraries(agent_test_recover PRIVATE tp_core)
target_include_directories(agent_test_recover PRIVATE "${CMAKE_SOURCE_DIR}/apps/common")
target_compile_definitions(agent_test_recover PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(agent_test_recover PRIVATE -U_DLL)
if(WIN32)
    target_sources(agent_test_recover PRIVATE ../common/nt_utf8_argv.c)
    target_link_libraries(agent_test_recover PRIVATE shell32)
endif()
nt_set_warning_flags(agent_test_recover)
nt_set_sanitizer_flags(agent_test_recover)

tp_add_test(NAME agent_commands
    SOURCES test_agent_commands.c agent_commands.c
    LIBS tp_core cjson unity
    SANITIZE)
