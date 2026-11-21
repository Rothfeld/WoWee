// TBC 2.4.3 mirror timer parity through GameHandler's packet dispatch and Lua API:
// - SMSG_START_MIRROR_TIMER / SMSG_PAUSE_MIRROR_TIMER / SMSG_STOP_MIRROR_TIMER
// - Timer names (EXHAUSTION, BREATH, FEIGNDEATH) and localized labels
//   (Fatigue, Breath, Feign Death — matching GlobalStrings.lua EXHAUSTION_LABEL)
// - GetMirrorTimerInfo(index) returning numeric paused (MirrorTimer.lua compares
//   `paused > 0` on PLAYER_ENTERING_WORLD)
// - GetMirrorTimerProgress(timer) supporting EXHAUSTION, BREATH, FEIGNDEATH, DEATH
// - Simultaneous Fatigue + Breath countdown, pause, surfacing refill, and stop.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

using replay::Session;
using wowee::game::Opcode;

namespace {

using Packet = wowee::network::Packet;

Packet packetFor(Opcode op) { return Packet(wowee::game::wireOpcode(op)); }

Packet startMirrorTimer(uint32_t type, int32_t value, int32_t maxValue,
                        int32_t scale, uint8_t paused, uint32_t spellId = 0) {
    Packet p = packetFor(Opcode::SMSG_START_MIRROR_TIMER);
    p.writeUInt32(type);
    p.writeUInt32(static_cast<uint32_t>(value));
    p.writeUInt32(static_cast<uint32_t>(maxValue));
    p.writeUInt32(static_cast<uint32_t>(scale));
    p.writeUInt8(paused);
    p.writeUInt32(spellId);
    return p;
}

Packet pauseMirrorTimer(uint32_t type, uint8_t paused) {
    Packet p = packetFor(Opcode::SMSG_PAUSE_MIRROR_TIMER);
    p.writeUInt32(type);
    p.writeUInt8(paused);
    return p;
}

Packet stopMirrorTimer(uint32_t type) {
    Packet p = packetFor(Opcode::SMSG_STOP_MIRROR_TIMER);
    p.writeUInt32(type);
    return p;
}

struct MirrorWatch {
    std::vector<std::string> log;
    explicit MirrorWatch(Session& s) {
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            if (name.rfind("MIRROR_TIMER_", 0) != 0) return;
            std::string line = name;
            for (const auto& a : args) line += " " + a;
            log.push_back(line);
        });
    }
    std::vector<std::string> take() { return std::exchange(log, {}); }
};

struct MirrorInfo {
    std::string timer;
    double value = 0;
    double maxValue = 0;
    double scale = 0;
    bool pausedIsNumber = false;
    double paused = 0;
    std::string label;
};

class MirrorLua {
public:
    explicit MirrorLua(Session& s) : L(luaL_newstate()) {
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        luaopen_math(L);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, &s.handler());
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
        wowee::addons::registerUnitLuaAPI(L);
        wowee::addons::registerActionLuaAPI(L);
    }
    ~MirrorLua() { lua_close(L); }

    MirrorInfo info(int index) {
        lua_getglobal(L, "GetMirrorTimerInfo");
        lua_pushnumber(L, index);
        REQUIRE(lua_pcall(L, 1, 6, 0) == 0);
        MirrorInfo out;
        out.timer = lua_tostring(L, -6) ? lua_tostring(L, -6) : "";
        out.value = lua_tonumber(L, -5);
        out.maxValue = lua_tonumber(L, -4);
        out.scale = lua_tonumber(L, -3);
        out.pausedIsNumber = (lua_type(L, -2) == LUA_TNUMBER);
        out.paused = lua_tonumber(L, -2);
        out.label = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 6);
        return out;
    }

    double progress(const char* timerName) {
        lua_getglobal(L, "GetMirrorTimerProgress");
        lua_pushstring(L, timerName);
        REQUIRE(lua_pcall(L, 1, 1, 0) == 0);
        const double out = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return out;
    }

    void run(const char* chunk) {
        if (luaL_dostring(L, chunk) != 0) {
            std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "(lua error)";
            lua_pop(L, 1);
            FAIL(err);
        }
    }

private:
    lua_State* L;
};

}  // namespace

TEST_CASE("TBC MirrorTimer types, localized labels, and numeric paused contract",
          "[replay][mirrortimer]") {
    Session s("tbc");
    MirrorWatch watch(s);
    MirrorLua lua(s);

    // Inactive slots return "UNKNOWN" and numeric paused = 0.
    const auto empty = lua.info(1);
    CHECK(empty.timer == "UNKNOWN");
    CHECK(empty.pausedIsNumber);
    CHECK(empty.paused == 0.0);
    CHECK(empty.label.empty());

    // Start all three TBC mirror timer types.
    s.receive(startMirrorTimer(0, 60000, 60000, -1, 0));
    s.receive(startMirrorTimer(1, 54000, 60000, -1, 0));
    s.receive(startMirrorTimer(2, 360000, 360000, -1, 1, 5384));

    const auto events = watch.take();
    REQUIRE(events.size() == 3);
    CHECK(events[0] == "MIRROR_TIMER_START EXHAUSTION 60000 60000 -1 0 Fatigue");
    CHECK(events[1] == "MIRROR_TIMER_START BREATH 54000 60000 -1 0 Breath");
    CHECK(events[2] == "MIRROR_TIMER_START FEIGNDEATH 360000 360000 -1 1 Feign Death");

    const auto fatigue = lua.info(1);
    CHECK(fatigue.timer == "EXHAUSTION");
    CHECK(fatigue.value == 60000.0);
    CHECK(fatigue.maxValue == 60000.0);
    CHECK(fatigue.scale == -1.0);
    CHECK(fatigue.pausedIsNumber);
    CHECK(fatigue.paused == 0.0);
    CHECK(fatigue.label == "Fatigue");

    const auto breath = lua.info(2);
    CHECK(breath.timer == "BREATH");
    CHECK(breath.value == 54000.0);
    CHECK(breath.maxValue == 60000.0);
    CHECK(breath.scale == -1.0);
    CHECK(breath.pausedIsNumber);
    CHECK(breath.paused == 0.0);
    CHECK(breath.label == "Breath");

    const auto feign = lua.info(3);
    CHECK(feign.timer == "FEIGNDEATH");
    CHECK(feign.value == 360000.0);
    CHECK(feign.maxValue == 360000.0);
    CHECK(feign.scale == -1.0);
    CHECK(feign.pausedIsNumber);
    CHECK(feign.paused == 1.0);
    CHECK(feign.label == "Feign Death");

    // GetMirrorTimerProgress resolves all three timer names (and legacy DEATH alias).
    CHECK(lua.progress("EXHAUSTION") == 60000.0);
    CHECK(lua.progress("BREATH") == 54000.0);
    CHECK(lua.progress("FEIGNDEATH") == 360000.0);
    CHECK(lua.progress("DEATH") == 360000.0);

    // TBC MirrorTimer.lua:49 executes `if ( paused > 0 ) then` on values from
    // GetMirrorTimerInfo(index) during PLAYER_ENTERING_WORLD; verify Lua does
    // not raise "attempt to compare boolean with number".
    lua.run(R"(
        for i = 1, 3 do
            local timer, value, maxvalue, scale, paused, label = GetMirrorTimerInfo(i)
            assert(type(paused) == "number", "paused must be number")
            if paused > 0 then
                assert(i == 3)
            end
        end
    )");
}

TEST_CASE("TBC MirrorTimer countdown, pause, surfacing refill, and stop lifecycle",
          "[replay][mirrortimer]") {
    Session s("tbc");
    MirrorWatch watch(s);
    MirrorLua lua(s);

    // Submerged in deep ocean: both Fatigue and Breath count down simultaneously.
    s.receive(startMirrorTimer(0, 60000, 60000, -1, 0));
    s.receive(startMirrorTimer(1, 60000, 60000, -1, 0));
    watch.take();

    s.handler().tickMirrorTimers(5.0f);
    CHECK(lua.progress("EXHAUSTION") == 55000.0);
    CHECK(lua.progress("BREATH") == 55000.0);

    // Pause Breath: Fatigue continues ticking while Breath stays frozen.
    s.receive(pauseMirrorTimer(1, 1));
    CHECK(watch.take() == std::vector<std::string>{"MIRROR_TIMER_PAUSE 1"});
    s.handler().tickMirrorTimers(3.0f);
    CHECK(lua.progress("EXHAUSTION") == 52000.0);
    CHECK(lua.progress("BREATH") == 55000.0);

    // Surfacing: CMaNGOS resends BREATH with positive scale (+10) and paused=0.
    s.receive(startMirrorTimer(1, 55000, 60000, 10, 0));
    CHECK(watch.take() == std::vector<std::string>{
        "MIRROR_TIMER_START BREATH 55000 60000 10 0 Breath"});

    // Refilling 5000 ms at scale +10 takes 0.5 seconds and auto-stops Breath.
    s.handler().tickMirrorTimers(0.6f);
    CHECK(lua.info(2).timer == "UNKNOWN");
    CHECK(watch.take() == std::vector<std::string>{"MIRROR_TIMER_STOP BREATH"});

    // Explicit server stop dismisses Fatigue and clears its value.
    s.receive(stopMirrorTimer(0));
    CHECK(lua.info(1).timer == "UNKNOWN");
    CHECK(lua.progress("EXHAUSTION") == 0.0);
    CHECK(watch.take() == std::vector<std::string>{"MIRROR_TIMER_STOP EXHAUSTION"});
}
