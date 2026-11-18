// Functions the 2.4.3 client does not define must read nil on a TBC login,
// and stay defined for the expansions that have them.
#include <catch_amalgamated.hpp>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include <algorithm>
#include <cstring>
#include <fstream>
#include <regex>
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
int countsCalls(lua_State* L) {
    lua_getglobal(L, "calls");
    lua_pushnumber(L, lua_tonumber(L, -1) + 1);
    lua_setglobal(L, "calls");
    return 0;
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
        lua_register(L, "TargetUnit", countsCalls);
        // The engine's event hook, recording what was fired.
        lua_pushcfunction(L, [](lua_State* S) -> int {
            std::string line;
            for (int i = 1; i <= lua_gettop(S); ++i) {
                if (i > 1) line += ' ';
                line += lua_tostring(S, i);
            }
            lua_pushstring(S, line.c_str());
            lua_setglobal(S, "fired");
            return 0;
        });
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_fire_event");
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

TEST_CASE("TBC refuses protected functions from a macro script", "[addons][api_surface]") {
    ConventionFixture f;
    const char* script = "TargetUnit('player')";
    // Interface code runs it.
    CHECK(run(f.L, "TargetUnit('player') return tostring(calls)") == "1");
    CHECK(run(f.L, "return tostring(issecure())") == "1");
    // A /script line does not.
    wowee::addons::markInsecureChunk(f.L, script);
    REQUIRE(luaL_loadbuffer(f.L, script, std::strlen(script), script) == 0);
    REQUIRE(lua_pcall(f.L, 0, 0, 0) == 0);
    CHECK(run(f.L, "return tostring(calls)") == "1");
    CHECK(run(f.L, "return fired") == "MACRO_ACTION_FORBIDDEN TargetUnit()");
}

TEST_CASE("TBC refuses protected functions from an AddOn", "[addons][api_surface]") {
    ConventionFixture f;
    wowee::addons::markInsecureRoot(f.L, "/x/Interface/AddOns/Probe", "Probe");
    const char* code = "function ProbeCall() TargetUnit('player') end\n"
                       "function ProbeSecure() return issecure() end\n"
                       "function ProbePcall() pcall(TargetUnit, 'player') end";
    const char* chunk = "@/x/Interface/AddOns/Probe/probe.lua";
    REQUIRE(luaL_loadbuffer(f.L, code, std::strlen(code), chunk) == 0);
    REQUIRE(lua_pcall(f.L, 0, 0, 0) == 0);
    CHECK(run(f.L, "ProbeCall() return tostring(calls)") == "nil");
    CHECK(run(f.L, "return fired") == "ADDON_ACTION_FORBIDDEN Probe TargetUnit()");
    CHECK(run(f.L, "ProbePcall() return fired") == "ADDON_ACTION_FORBIDDEN Probe UNKNOWN()");
    CHECK(run(f.L, "return tostring(ProbeSecure())") == "nil");
    // Blizzard's code from a sibling directory stays secure.
    const char* blizzard = "function BlizzCall() TargetUnit('player') end";
    REQUIRE(luaL_loadbuffer(f.L, blizzard, std::strlen(blizzard),
                            "@/x/Interface/AddOns/Blizzard_RaidUI/x.lua") == 0);
    REQUIRE(lua_pcall(f.L, 0, 0, 0) == 0);
    CHECK(run(f.L, "BlizzCall() return tostring(calls)") == "1");
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

// The client's own Lua (bootstrap and injected snippets, written as C++
// string literals) runs under the TBC shapes too. A `local a, b, ... = F(`
// that names more values than applyApiConventions leaves F read nil there:
// SetBagItem took the link as GetContainerItemInfo's seventh value and every
// bag tooltip was empty on TBC.
TEST_CASE("Client Lua reads no more values than TBC answers", "[addons][api_surface]") {
    const std::regex call(R"re(local\s+([A-Za-z_][\w\s,]*?)\s*=\s*([A-Za-z_]\w*)\()re");
    // WatchFrame exists from 3.0 only, so this replacement never runs on TBC.
    const std::string watchFrameOnly = "addon_manager.cpp:GetQuestLogTitle";
    for (const char* file : {"lua_engine.cpp", "addon_manager.cpp", "lua_action_api.cpp",
                             "lua_system_api.cpp", "lua_social_api.cpp", "lua_unit_api.cpp",
                             "lua_spell_api.cpp", "lua_quest_api.cpp", "lua_inventory_api.cpp"}) {
        std::ifstream in(std::string(WOWEE_SOURCE_DIR) + "/src/addons/" + file);
        REQUIRE(in.is_open());
        std::string line;
        int number = 0;
        while (std::getline(in, line)) {
            ++number;
            if (line.find('"') == std::string::npos) continue;  // Lua lives in string literals
            for (std::sregex_iterator it(line.begin(), line.end(), call), end; it != end; ++it) {
                const std::string name = (*it)[2];
                const int most = wowee::addons::apiResultCount("tbc", name);
                if (most < 0 || std::string(file) + ":" + name == watchFrameOnly) continue;
                const std::string vars = (*it)[1];
                const int count = 1 + static_cast<int>(std::count(vars.begin(), vars.end(), ','));
                INFO(file << ":" << number << " " << line);
                CHECK(count <= most);
            }
        }
    }
}
