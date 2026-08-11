#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "lauxlib.h"
#include "lua.h"

_Static_assert(LUA_VERSION_NUM == 505, "Packet 2 requires Lua 5.5");
_Static_assert(sizeof(lua_Integer) == 8U,
               "Packet 2 requires a 64-bit lua_Integer");
_Static_assert(sizeof(lua_Number) == 8U,
               "Packet 2 requires a binary64-sized lua_Number");

int main(void) {
    static const char source[] = "return function() end";
    lua_State *state = luaL_newstate();
    if (!state) {
        return 1;
    }
    const int status = luaL_loadbufferx(state, source, strlen(source),
                                        "@vendor-contract", "t");
    const bool loaded_function =
        status == LUA_OK && lua_type(state, -1) == LUA_TFUNCTION;
    lua_close(state);
    return loaded_function ? 0 : 2;
}
