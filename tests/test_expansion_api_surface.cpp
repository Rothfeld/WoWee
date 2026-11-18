// Functions the 2.4.3 client does not define must read nil on a TBC login,
// and stay defined for the expansions that have them.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <cstring>
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

namespace {

int answersTrue(lua_State* L) { lua_pushboolean(L, 1); return 1; }
int answersFalse(lua_State* L) { lua_pushboolean(L, 0); return 1; }
int answersClass(lua_State* L) {
    lua_pushstring(L, "Rogue");
    lua_pushstring(L, "ROGUE");
    lua_pushnumber(L, 4);
    return 3;
}
std::string run(lua_State* L, const char* code) {
    if (luaL_dostring(L, code) != 0) return std::string("error: ") + lua_tostring(L, -1);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "nil";
    lua_settop(L, 0);
    return out;
}

struct ConventionFixture {
    lua_State* L = luaL_newstate();
    ConventionFixture() {
        luaopen_base(L);
        lua_settop(L, 0);
        lua_register(L, "UnitExists", answersTrue);
        lua_register(L, "UnitIsDead", answersFalse);
        lua_register(L, "UnitClass", answersClass);
        wowee::addons::applyApiConventions(L, "tbc");
    }
    ~ConventionFixture() { lua_close(L); }
};

} // namespace

TEST_CASE("TBC answers flags as 1 or nil", "[addons][api_surface]") {
    ConventionFixture f;
    CHECK(run(f.L, "return tostring(UnitExists('player'))") == "1");
    CHECK(run(f.L, "return tostring(UnitIsDead('player'))") == "nil");
    CHECK(run(f.L, "return select('#', UnitIsDead('player'))") == "1");
}

TEST_CASE("TBC result counts", "[addons][api_surface]") {
    ConventionFixture f;
    CHECK(run(f.L, "return select('#', UnitClass('player'))") == "2");
    CHECK(run(f.L, "return select(2, UnitClassBase('player'))") == "ROGUE");
    CHECK(run(f.L, "return select('#', UnitClassBase('player'))") == "2");
}

TEST_CASE("Unmeasured expansions keep their answers", "[addons][api_surface]") {
    lua_State* L = luaL_newstate();
    luaopen_base(L);
    lua_settop(L, 0);
    lua_register(L, "UnitExists", answersTrue);
    wowee::addons::applyApiConventions(L, "wotlk");
    CHECK(run(L, "return tostring(UnitExists('player'))") == "true");
    lua_close(L);
}

TEST_CASE("TBC widget methods answer flags as 1 or nil", "[addons][api_surface]") {
    lua_State* L = luaL_newstate();
    luaopen_base(L);
    lua_settop(L, 0);
    lua_newtable(L);
    lua_pushcfunction(L, answersFalse);
    lua_setfield(L, -2, "IsShown");
    lua_setglobal(L, "__WoweeFrameMT");
    wowee::addons::applyApiConventions(L, "tbc");
    CHECK(run(L, "return tostring(__WoweeFrameMT.IsShown({}))") == "nil");
    lua_close(L);
}
