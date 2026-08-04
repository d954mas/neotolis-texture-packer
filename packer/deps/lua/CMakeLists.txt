# PUC Lua 5.5.0 with the pinned official fixes listed in VENDOR.md.
# This is an internal, offline-only dependency: no interpreter, compiler,
# dynamic loader, install rule, or upstream build logic is included.
include("${CMAKE_CURRENT_LIST_DIR}/verify_source.cmake")

add_library(tp_lua_vendor STATIC
    src/lapi.c
    src/lauxlib.c
    src/lbaselib.c
    src/lcode.c
    src/lctype.c
    src/ldebug.c
    src/ldo.c
    src/ldump.c
    src/lfunc.c
    src/lgc.c
    src/llex.c
    src/lmathlib.c
    src/lmem.c
    src/lobject.c
    src/lopcodes.c
    src/lparser.c
    src/lstate.c
    src/lstring.c
    src/lstrlib.c
    src/ltable.c
    src/ltm.c
    src/lundump.c
    src/lutf8lib.c
    src/lvm.c
    src/lzio.c)

target_include_directories(tp_lua_vendor SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(tp_lua_vendor PRIVATE
    LUA_NOCVTN2S
    LUA_NOCVTS2N
    _CRT_SECURE_NO_WARNINGS)
target_compile_features(tp_lua_vendor PRIVATE c_std_17)

# Vendored third-party code is exempt from first-party warning-as-error policy.
if(MSVC)
    target_compile_options(tp_lua_vendor PRIVATE /w)
else()
    target_compile_options(tp_lua_vendor PRIVATE -w)
endif()
