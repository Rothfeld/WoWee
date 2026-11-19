// The ready check as 2.4.3 (build 8606) runs it against CMaNGOS-TBC, through
// GameHandler's own dispatch. See game_replay.hpp.
//
// Measured on pair 5 with a probe addon logging every ready check event, its
// arguments, GetReadyCheckStatus/GetReadyCheckTimeLeft and the chat lines, in
// party and raid, with the native client asking, answering, as assistant and
// as plain member:
// - the realm broadcasts MSG_RAID_READY_CHECK (asker's guid) to the group,
//   relays answers (MSG_RAID_READY_CHECK_CONFIRM, guid + byte) to the leader
//   and assistants only, and relays MSG_RAID_READY_CHECK_FINISHED from the
//   asker (GroupHandler.cpp HandleRaidReadyCheckOpcode).
// - the asker prints "Starting ready check...", the others "<name> has
//   initiated a ready check."; READY_CHECK gets the asker's name and the
//   whole seconds left.
// - the leader and assistants keep a status per member: the asker "ready",
//   the rest "waiting". READY_CHECK_CONFIRM gets the member's raid (party)
//   number and 1 or 0. When nobody waits any more they forget the statuses
//   and the timer; the asker prints "Everyone is Ready" or, if someone said
//   no, "Ready check finished", and sends the finish. A "no" is printed by the
//   asker alone, at once, before the event.
// - thirty seconds on, the asker prints "The following players are AFK: ..."
//   and sends the finish; an assistant just forgets.
// - a plain member keeps no statuses and its timer runs on after the finish.
// - a new leader, or leaving the group, ends the check silently.
// Evidence: _experiments/20261119T094500Z.readycheck_audit/ in the workspace root.
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

constexpr uint64_t kSelf = Session::kPlayerGuid;
constexpr uint64_t kEdrike = 0x19;

struct Member {
    std::string name;
    uint64_t guid;
    uint8_t flags = 0;
};

// CMaNGOS-TBC Group::SendUpdate for the player: type, battleground byte,
// subgroup, own flags, group guid, the others, leader, loot rules.
Packet roster(bool raid, uint64_t leader, const std::vector<Member>& others, uint8_t ownFlags = 0) {
    Packet p = packetFor(Opcode::SMSG_GROUP_LIST);
    p.writeUInt8(raid ? 1 : 0);
    p.writeUInt8(0);
    p.writeUInt8(0);
    p.writeUInt8(ownFlags);
    p.writeUInt64(0x1F4);
    p.writeUInt32(static_cast<uint32_t>(others.size()));
    for (const auto& m : others) {
        p.writeString(m.name);
        p.writeUInt64(m.guid);
        p.writeUInt8(1);
        p.writeUInt8(0);
        p.writeUInt8(m.flags);
    }
    p.writeUInt64(leader);
    p.writeUInt8(3);
    p.writeUInt64(0);
    p.writeUInt8(2);
    p.writeUInt8(0);
    return p;
}

Packet readyCheck(uint64_t asker) {
    Packet p = packetFor(Opcode::MSG_RAID_READY_CHECK);
    p.writeUInt64(asker);
    return p;
}

Packet confirm(uint64_t guid, bool ready) {
    Packet p = packetFor(Opcode::MSG_RAID_READY_CHECK_CONFIRM);
    p.writeUInt64(guid);
    p.writeUInt8(ready ? 1 : 0);
    return p;
}

Packet setLeader(const std::string& name) {
    Packet p = packetFor(Opcode::SMSG_GROUP_SET_LEADER);
    p.writeString(name);
    return p;
}

// GlobalStrings.lua of 2.4.3, the lines the ready check prints.
std::string globalString(const char* name) {
    static const std::vector<std::pair<std::string, std::string>> strings = {
        {"READY_CHECK_START", "Starting ready check..."},
        {"ERR_RAID_LEADER_READY_CHECK_START_S", "%s has initiated a ready check."},
        {"READY_CHECK_ALL_READY", "Everyone is Ready"},
        {"READY_CHECK_FINISHED", "Ready check finished"},
        {"RAID_MEMBER_NOT_READY", "%s is not ready"},
        {"RAID_MEMBERS_AFK", "The following players are AFK: %s"},
        {"PLAYER_LIST_DELIMITER", ", "},
        {"ERR_READY_CHECK_IN_PROGRESS", "You are already running a ready check"},
        {"ERR_NEW_LEADER_S", "%s is now the group leader."},
        {"ERR_NEW_LEADER_YOU", "You are now the group leader."},
        {"ERR_LEFT_GROUP_S", "%s leaves the party."},
    };
    for (const auto& [key, text] : strings)
        if (key == name) return text;
    return {};
}

// Events and chat lines in the order the interface sees them.
struct Watch {
    std::vector<std::string> log;
    explicit Watch(Session& s) {
        s.handler().setGlobalStringLookup(globalString);
        s.handler().setAddonEventCallback([this](const std::string& name, const std::vector<std::string>& args) {
            if (name == "CHAT_MSG_SYSTEM") { log.push_back("SYS " + (args.empty() ? std::string() : args[0])); return; }
            if (name.rfind("READY_CHECK", 0) != 0) return;
            std::string line = name;
            for (const auto& a : args) line += " " + a;
            log.push_back(line);
        });
    }
    std::vector<std::string> take() { return std::exchange(log, {}); }
};

class Lua {
public:
    explicit Lua(Session& s) : L(luaL_newstate()) {
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        luaopen_math(L);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, &s.handler());
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
        wowee::addons::registerUnitLuaAPI(L);
        wowee::addons::registerSocialLuaAPI(L);
    }
    ~Lua() { lua_close(L); }
    std::string status(const char* unit) {
        lua_getglobal(L, "GetReadyCheckStatus");
        lua_pushstring(L, unit);
        REQUIRE(lua_pcall(L, 1, 1, 0) == 0);
        std::string out = lua_isnil(L, -1) ? "nil" : lua_tostring(L, -1);
        lua_pop(L, 1);
        return out;
    }
    double timeLeft() {
        lua_getglobal(L, "GetReadyCheckTimeLeft");
        REQUIRE(lua_pcall(L, 0, 1, 0) == 0);
        const double out = lua_tonumber(L, -1);
        lua_pop(L, 1);
        return out;
    }
    void run(const char* chunk) { REQUIRE(luaL_dostring(L, chunk) == 0); }

private:
    lua_State* L;
};

size_t finishesSent(Session& s) { return s.sent().of(Opcode::MSG_RAID_READY_CHECK_FINISHED).size(); }

}  // namespace

TEST_CASE("The leader's ready check: statuses, answers, lines and the finish", "[replay][readycheck]") {
    Session s("tbc");
    Watch watch(s);
    Lua lua(s);
    s.receive(roster(false, kSelf, {{"Edrike", kEdrike}}));
    watch.take();

    SECTION("everyone ready") {
        s.receive(readyCheck(kSelf));
        CHECK(lua.status("player") == "ready");
        CHECK(lua.status("party1") == "waiting");
        CHECK(lua.timeLeft() == 29);
        s.receive(confirm(kEdrike, true));
        CHECK(lua.status("player") == "nil");
        CHECK(lua.status("party1") == "nil");
        CHECK(lua.timeLeft() == 0);
        CHECK(finishesSent(s) == 1);
        s.receive(packetFor(Opcode::MSG_RAID_READY_CHECK_FINISHED));
        CHECK(watch.take() == std::vector<std::string>{"SYS Starting ready check...", "READY_CHECK  29",
                                                       "READY_CHECK_CONFIRM 1 1", "SYS Everyone is Ready",
                                                       "READY_CHECK_FINISHED"});
    }
    SECTION("someone says no") {
        s.receive(readyCheck(kSelf));
        s.receive(confirm(kEdrike, false));
        CHECK(finishesSent(s) == 1);
        CHECK(watch.take() == std::vector<std::string>{"SYS Starting ready check...", "READY_CHECK  29",
                                                       "SYS Edrike is not ready", "READY_CHECK_CONFIRM 1 0",
                                                       "SYS Ready check finished"});
    }
    SECTION("nobody answers") {
        s.receive(readyCheck(kSelf));
        s.handler().checkReadyCheckTime();
        CHECK(finishesSent(s) == 0);
        wowee::game::GameHandlerTestAccess::expireReadyCheck(s.handler());
        s.handler().checkReadyCheckTime();
        CHECK(finishesSent(s) == 1);
        CHECK(lua.status("party1") == "nil");
        s.handler().checkReadyCheckTime();
        CHECK(finishesSent(s) == 1);
        CHECK(watch.take() == std::vector<std::string>{"SYS Starting ready check...", "READY_CHECK  29",
                                                       "SYS The following players are AFK: Edrike"});
    }
    SECTION("asking again while it runs") {
        s.receive(readyCheck(kSelf));
        const auto asked = s.sent().of(Opcode::MSG_RAID_READY_CHECK).size();
        std::vector<std::string> errors;
        s.handler().setUIErrorCallback([&](const std::string& msg) { errors.push_back(msg); });
        lua.run("DoReadyCheck()");
        CHECK(s.sent().of(Opcode::MSG_RAID_READY_CHECK).size() == asked);
        CHECK(errors == std::vector<std::string>{"You are already running a ready check"});
    }
}

TEST_CASE("A member's ready check: no statuses, the answer, a timer that runs on", "[replay][readycheck]") {
    Session s("tbc");
    Watch watch(s);
    Lua lua(s);
    s.receive(roster(false, kEdrike, {{"Edrike", kEdrike}}));
    watch.take();
    s.receive(readyCheck(kEdrike));
    CHECK(lua.status("player") == "nil");
    CHECK(lua.status("party1") == "nil");
    lua.run("ConfirmReadyCheck()");
    const auto answers = s.sent().of(Opcode::MSG_RAID_READY_CHECK);
    REQUIRE(answers.size() == 1);
    auto answer = answers[0];
    REQUIRE(answer.getSize() == 1);
    CHECK(answer.readUInt8() == 0);
    s.receive(packetFor(Opcode::MSG_RAID_READY_CHECK_FINISHED));
    CHECK(lua.timeLeft() == 29);
    wowee::game::GameHandlerTestAccess::expireReadyCheck(s.handler());
    s.handler().checkReadyCheckTime();
    CHECK(lua.timeLeft() == 0);
    CHECK(finishesSent(s) == 0);
    CHECK(watch.take() == std::vector<std::string>{"SYS Edrike has initiated a ready check.", "READY_CHECK Edrike 29",
                                                   "READY_CHECK_FINISHED"});
}

TEST_CASE("A raid assistant keeps answers but neither prints nor finishes", "[replay][readycheck]") {
    Session s("tbc");
    Watch watch(s);
    Lua lua(s);
    constexpr uint8_t kAssistant = 0x01;
    s.receive(roster(true, kEdrike, {{"Edrike", kEdrike}}, kAssistant));
    watch.take();

    SECTION("the assistant answers") {
        s.receive(readyCheck(kEdrike));
        CHECK(lua.status("raid1") == "ready");
        CHECK(lua.status("raid2") == "waiting");
        CHECK(lua.status("player") == "waiting");
        s.receive(confirm(kSelf, false));
        CHECK(lua.status("player") == "nil");
        CHECK(lua.timeLeft() == 0);
        CHECK(finishesSent(s) == 0);
        CHECK(watch.take() == std::vector<std::string>{"SYS Edrike has initiated a ready check.",
                                                       "READY_CHECK Edrike 29", "READY_CHECK_CONFIRM 2 0"});
    }
    SECTION("the time runs out") {
        s.receive(readyCheck(kEdrike));
        wowee::game::GameHandlerTestAccess::expireReadyCheck(s.handler());
        s.handler().checkReadyCheckTime();
        CHECK(lua.status("player") == "nil");
        CHECK(finishesSent(s) == 0);
        CHECK(watch.take() == std::vector<std::string>{"SYS Edrike has initiated a ready check.",
                                                       "READY_CHECK Edrike 29"});
    }
    SECTION("the assistant asks, the leader answers") {
        s.receive(readyCheck(kSelf));
        CHECK(lua.status("raid1") == "waiting");
        CHECK(lua.status("player") == "ready");
        s.receive(confirm(kEdrike, true));
        CHECK(finishesSent(s) == 1);
        CHECK(watch.take() == std::vector<std::string>{"SYS Starting ready check...", "READY_CHECK  29",
                                                       "READY_CHECK_CONFIRM 1 1", "SYS Everyone is Ready"});
    }
}

TEST_CASE("A new leader or leaving the group ends the check without a word", "[replay][readycheck]") {
    Session s("tbc");
    Watch watch(s);
    Lua lua(s);
    s.receive(roster(false, kSelf, {{"Edrike", kEdrike}}));
    s.receive(readyCheck(kSelf));
    watch.take();

    SECTION("promotion") {
        s.receive(setLeader("Edrike"));
        CHECK(lua.timeLeft() == 0);
        CHECK(lua.status("player") == "nil");
        wowee::game::GameHandlerTestAccess::expireReadyCheck(s.handler());
        s.handler().checkReadyCheckTime();
        CHECK(finishesSent(s) == 0);
        CHECK(watch.take() == std::vector<std::string>{"SYS Edrike is now the group leader."});
    }
    SECTION("the group is gone") {
        s.receive(roster(false, 0, {}));
        CHECK(lua.timeLeft() == 0);
        CHECK(lua.status("player") == "nil");
        CHECK(watch.take() == std::vector<std::string>{"SYS Edrike leaves the party."});
    }
}
