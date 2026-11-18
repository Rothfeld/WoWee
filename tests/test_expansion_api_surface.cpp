// Functions the 2.4.3 client does not define must read nil on a TBC login,
// and stay defined for the expansions that have them.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

#include <string>

#include "addons/expansion_api_surface.hpp"

using wowee::addons::apiAbsentFrom;
using wowee::addons::removeApiAbsentFrom;

namespace {

bool isFunction(lua_State* L, const char* name) {
    lua_getglobal(L, name);
    const bool fn = lua_isfunction(L, -1);
    lua_pop(L, 1);
    return fn;
}

int noop(lua_State*) { return 0; }

struct Fixture {
    lua_State* L = luaL_newstate();
    Fixture() {
        for (const char* name : {"SortBGList", "VehicleAimUpStart", "IsFlyableArea",
                                 "GMSurveyQuestion", "GMSurveyAnswer"}) {
            lua_pushcfunction(L, noop);
            lua_setglobal(L, name);
        }
    }
    ~Fixture() { lua_close(L); }
};

} // namespace

TEST_CASE("TBC drops functions the 2.4.3 client lacks", "[addons][api_surface]") {
    Fixture f;
    removeApiAbsentFrom(f.L, "tbc");
    CHECK_FALSE(isFunction(f.L, "SortBGList"));
    CHECK_FALSE(isFunction(f.L, "VehicleAimUpStart"));
    CHECK_FALSE(isFunction(f.L, "GMSurveyAnswer"));
    // Present on 2.4.3: measured as functions there.
    CHECK(isFunction(f.L, "IsFlyableArea"));
    CHECK(isFunction(f.L, "GMSurveyQuestion"));
    // A call raises the way the native client does.
    CHECK(luaL_dostring(f.L, "SortBGList()") != 0);
    CHECK(std::string(lua_tostring(f.L, -1)).find("nil value") != std::string::npos);
}

TEST_CASE("Unmeasured expansions keep every function", "[addons][api_surface]") {
    Fixture f;
    removeApiAbsentFrom(f.L, "wotlk");
    CHECK(isFunction(f.L, "SortBGList"));
    CHECK(isFunction(f.L, "VehicleAimUpStart"));
    CHECK(apiAbsentFrom("wotlk").empty());
    CHECK_FALSE(apiAbsentFrom("tbc").empty());
}
