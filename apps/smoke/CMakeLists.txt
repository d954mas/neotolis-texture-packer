set(SMOKE_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/build/apps/smoke/${NT_PRESET_NAME}")

add_executable(tp_smoke main.c)
target_link_libraries(tp_smoke PRIVATE nt_builder nt_log)
target_compile_definitions(tp_smoke PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(tp_smoke PRIVATE -U_DLL)
nt_set_warning_flags(tp_smoke)
nt_set_sanitizer_flags(tp_smoke)
set_target_properties(tp_smoke PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${SMOKE_OUTPUT_DIR}")
