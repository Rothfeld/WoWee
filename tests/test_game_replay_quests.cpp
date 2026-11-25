// Quest progress through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"
#include "game/quest_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint32_t kQuest = 33;
constexpr uint32_t kYoungWolf = 299;
constexpr uint32_t kAltar = 1234;

struct InfoMessages {
    std::vector<std::string> lines;
    explicit InfoMessages(Session& s) {
        s.handler().setAddonEventCallback(
            [this](const std::string& name, const std::vector<std::string>& args) {
                if (name == "UI_INFO_MESSAGE" && !args.empty()) lines.push_back(args[0]);
            });
    }
};

void acceptQuest(Session& s) {
    s.handler().getQuestHandler()->addQuestToLocalLogIfMissing(kQuest, "Wolves Across the Border", "");
}

// As AzerothCore's QueryHandler writes it.
Packet creatureName(uint32_t entry, const std::string& name) {
    Packet p(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeString(name);
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (int i = 0; i < 4 + 2 + 4; ++i) p.writeUInt32(0);  // type to rank, kill credits, models
    p.writeFloat(1.0f);
    p.writeFloat(1.0f);
    p.writeUInt8(0);
    for (int i = 0; i < 6 + 1; ++i) p.writeUInt32(0);  // quest items, movement id
    return p;
}

Packet gameObjectName(uint32_t entry, const std::string& name) {
    Packet p(wireOpcode(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeUInt32(10);  // GAMEOBJECT_TYPE_GOOBER
    p.writeUInt32(1);
    p.writeString(name);
    for (int i = 0; i < 6; ++i) p.writeString("");
    for (int i = 0; i < 24; ++i) p.writeUInt32(0);
    p.writeFloat(1.0f);
    for (int i = 0; i < 6; ++i) p.writeUInt32(0);
    return p;
}

// Player::SendQuestUpdateAddCreatureOrGo: a game object's entry has the high
// bit set.
Packet addKill(uint32_t wireEntry, uint32_t count, uint32_t required) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
    p.writeUInt32(kQuest);
    p.writeUInt32(wireEntry);
    p.writeUInt32(count);
    p.writeUInt32(required);
    p.writeUInt64(0xF130000000000123ull);
    return p;
}

bool chatMentions(Session& s, const std::string& text) {
    const auto& history = s.handler().getChatHistory();
    return std::any_of(history.begin(), history.end(), [&](const auto& m) {
        return m.message.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("a quest kill is shown as ERR_QUEST_ADD_KILL_SII", "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    s.receive(creatureName(kYoungWolf, "Young Wolf"));
    s.receive(addKill(kYoungWolf, 3, 10));
    CHECK(info.lines == std::vector<std::string>{"Young Wolf slain: 3/10"});
    CHECK_FALSE(chatMentions(s, "3/10"));
}

TEST_CASE("a quest object is named from the object cache, as ERR_QUEST_ADD_FOUND_SII",
          "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    s.receive(creatureName(kAltar, "Kobold Vermin"));
    s.receive(gameObjectName(kAltar, "Altar of Zul"));
    s.receive(addKill(kAltar | 0x80000000u, 1, 1));
    CHECK(info.lines == std::vector<std::string>{"Altar of Zul: 1/1"});
}

TEST_CASE("a quest player kill is shown as ERR_QUEST_ADD_PLAYER_KILL_SII", "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    Packet p(wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_PVP_KILL));
    p.writeUInt32(kQuest);
    p.writeUInt32(2);
    p.writeUInt32(5);
    s.receive(p);
    CHECK(info.lines == std::vector<std::string>{"Players slain: 2/5"});
    CHECK_FALSE(chatMentions(s, "2/5"));
}

namespace {

using replay::UpdateObject;
using wowee::game::QuestGiverStatus;
using wowee::game::UF;

constexpr uint64_t kGornek = 0xF130000C47000D73ull;
constexpr uint32_t kYourPlace = 4641;

size_t multipleQueries(const Session& s) {
    return s.sent().of(Opcode::CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY).size();
}

// Player::SendQuestGiverStatusMultiple: a count, then guid and one status byte
// per giver; 8 is TBC DIALOG_STATUS_REWARD.
Packet statusMultiple(uint64_t guid, uint8_t status) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTGIVER_STATUS_MULTIPLE));
    p.writeUInt32(1);
    p.writeUInt64(guid);
    p.writeUInt8(status);
    return p;
}

// A quest log slot as Player::SetQuestSlot / SetQuestSlotCounter write it in
// TBC: id, state, packed counters, timer - four fields from
// PLAYER_QUEST_LOG_1_1.
std::map<uint16_t, uint32_t> slot1(uint32_t id, uint32_t counters) {
    const uint16_t start = wowee::game::fieldIndex(UF::PLAYER_QUEST_LOG_START);
    REQUIRE(start != 0xFFFF);
    return {{start, id}, {static_cast<uint16_t>(start + 2), counters}};
}

}  // namespace

// Build 8606 asks for every giver's status whenever its quest log fields
// change, and only then (pair 19: accepting Your Place In The World from
// Kaltunk, ".quest add/complete/remove", one Mottled Boar killed). Gornek,
// who takes the quest, kept the ! of Cutting Teeth when only Kaltunk was asked.
TEST_CASE("a quest log change asks every quest giver's status once", "[replay][quest]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 1}}, true));
    CHECK(multipleQueries(s) == 0);  // an empty log at login asks nothing

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(kYourPlace, 0)));
    CHECK(multipleQueries(s) == 1);
    s.receive(statusMultiple(kGornek, 8));
    CHECK(s.handler().getQuestGiverStatus(kGornek) == QuestGiverStatus::REWARD);

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(kYourPlace, 1)));
    CHECK(multipleQueries(s) == 2);  // a counter

    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_LEVEL, 2}}));
    CHECK(multipleQueries(s) == 2);  // not the log

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(0, 0)));
    CHECK(multipleQueries(s) == 3);  // removed or handed in
    CHECK(s.sent().of(Opcode::CMSG_QUESTGIVER_STATUS_QUERY).empty());
}

TEST_CASE("entering the world with quests in the log asks every status once", "[replay][quest]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 1},
                                           {UF::PLAYER_QUEST_LOG_START, kYourPlace}},
                                          true));
    CHECK(multipleQueries(s) == 1);
}

namespace {

struct ScopedLua {
    lua_State* L = nullptr;
    explicit ScopedLua(Session& s) : L(luaL_newstate()) {
        luaopen_base(L);
        luaopen_table(L);
        luaopen_string(L);
        luaopen_math(L);
        lua_settop(L, 0);
        lua_pushlightuserdata(L, &s.handler());
        lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
        wowee::addons::registerSystemLuaAPI(L);
        wowee::addons::registerQuestLuaAPI(L);
    }
    ~ScopedLua() {
        if (L) lua_close(L);
    }
    ScopedLua(const ScopedLua&) = delete;
    ScopedLua& operator=(const ScopedLua&) = delete;
};

void installTbcShard5GlobalStrings(Session& s) {
    s.handler().setGlobalStringLookup([](const char* key) -> std::string {
        const std::string k(key ? key : "");
        if (k == "ERR_ARENA_TEAM_JOIN_SS") return "%s has joined %s.";
        if (k == "ERR_ARENA_TEAM_LEAVE_SS") return "%s has left %s.";
        if (k == "ERR_ARENA_NO_TEAM_II") return "You are not in a %dv%d arena team.";
        if (k == "ERR_ARENA_EXPIRED_CAIS")
            return "You may not queue while one or more of your team members is under the effect of restricted play.";
        if (k == "ERR_QUEST_FAILED_MAX_COUNT_S") return "%s failed: Duplicate item found.";
        if (k == "ERR_QUEST_COMPLETE_S") return "%s completed.";
        if (k == "ERR_QUEST_REWARD_EXP_I") return "Experience gained: %d.";
        if (k == "ERR_QUEST_REWARD_MONEY_S") return "Received %s.";
        return {};
    });
}

}  // namespace

TEST_CASE("TBC Arena team auto-query, emblem response, roster without unk308, events, and error",
          "[replay][arena][tbc]") {
    Session s("tbc");
    installTbcShard5GlobalStrings(s);
    const uint16_t arenaBase = wowee::game::fieldIndex(UF::PLAYER_FIELD_ARENA_TEAM_INFO_1_1);
    REQUIRE(arenaBase != 0xFFFF);

    constexpr uint32_t kTeam2v2 = 77;
    constexpr uint64_t kMateGuid = 0x0000000000000099ull;

    // Login with 2v2 team id=77, ARENA_TEAM_MEMBER=0 (captain), week=6, season=20, personalRating=1840
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_LEVEL, 70}}, true));
    s.receive(UpdateObject().tbc().valuesAt(
        Session::kPlayerGuid,
        {{arenaBase, kTeam2v2},
         {static_cast<uint16_t>(arenaBase + 1), 0},
         {static_cast<uint16_t>(arenaBase + 2), 6},
         {static_cast<uint16_t>(arenaBase + 3), 20},
         {static_cast<uint16_t>(arenaBase + 4), 14},
         {static_cast<uint16_t>(arenaBase + 5), 1840}}));

    // S5-15: CMSG_ARENA_TEAM_QUERY automatically sent once for team 77
    const auto queries = s.sent().of(Opcode::CMSG_ARENA_TEAM_QUERY);
    REQUIRE(queries.size() == 1);
    {
        Packet q = queries[0];
        CHECK(q.readUInt32() == kTeam2v2);
    }

    // S5-04: SMSG_ARENA_TEAM_QUERY_RESPONSE with 5 emblem uint32s
    {
        Packet qr(wireOpcode(Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE));
        qr.writeUInt32(kTeam2v2);
        qr.writeString("Blades Edge Brawlers");
        qr.writeUInt32(2);          // 2v2
        qr.writeUInt32(0xFFCC3366); // backgroundColor (R=204, G=51, B=102)
        qr.writeUInt32(12);         // emblemStyle (1-based)
        qr.writeUInt32(0xFF112233); // emblemColor
        qr.writeUInt32(3);          // borderStyle (1-based)
        qr.writeUInt32(0xFF445566); // borderColor
        s.receive(qr);
    }

    // SMSG_ARENA_TEAM_STATS preserves teamName + 5 emblem fields
    {
        Packet st(wireOpcode(Opcode::SMSG_ARENA_TEAM_STATS));
        st.writeUInt32(kTeam2v2);
        st.writeUInt32(1875); // rating
        st.writeUInt32(8);    // weekGames
        st.writeUInt32(6);    // weekWins
        st.writeUInt32(24);   // seasonGames
        st.writeUInt32(17);   // seasonWins
        st.writeUInt32(4);    // rank
        s.receive(st);
    }

    // S5-01: TBC SMSG_ARENA_TEAM_ROSTER (no unk308 byte, no trailing floats; captain=1)
    {
        Packet ros(wireOpcode(Opcode::SMSG_ARENA_TEAM_ROSTER));
        ros.writeUInt32(kTeam2v2);
        ros.writeUInt32(2); // membersCount
        ros.writeUInt32(2); // teamType (2v2)

        // Member 1: local player (captain = 1, Lv70 Warrior = 1)
        ros.writeUInt64(Session::kPlayerGuid);
        ros.writeUInt8(1); // online
        ros.writeString("Hero");
        ros.writeUInt32(1); // captain (1 = captain on wire)
        ros.writeUInt8(70);
        ros.writeUInt8(1);  // Warrior
        ros.writeUInt32(7);
        ros.writeUInt32(5);
        ros.writeUInt32(21);
        ros.writeUInt32(15);
        ros.writeUInt32(1855);

        // Member 2: teammate (captain = 0, Lv70 Druid = 11)
        ros.writeUInt64(kMateGuid);
        ros.writeUInt8(0); // offline
        ros.writeString("Swiftmend");
        ros.writeUInt32(0); // non-captain
        ros.writeUInt8(70);
        ros.writeUInt8(11); // Druid
        ros.writeUInt32(8);
        ros.writeUInt32(6);
        ros.writeUInt32(24);
        ros.writeUInt32(17);
        ros.writeUInt32(1890);
        s.receive(ros);
    }

    const auto* roster = s.handler().getArenaTeamRoster(kTeam2v2);
    REQUIRE(roster != nullptr);
    REQUIRE(roster->members.size() == 2);
    CHECK(roster->members[0].name == "Hero");
    CHECK(roster->members[0].isCaptain);
    CHECK(roster->members[0].level == 70);
    CHECK(roster->members[0].classId == 1);
    CHECK(roster->members[0].personalRating == 1855);
    CHECK(roster->members[1].name == "Swiftmend");
    CHECK_FALSE(roster->members[1].isCaptain);
    CHECK(roster->members[1].classId == 11);
    CHECK(roster->members[1].personalRating == 1890);

    ScopedLua lua(s);
    REQUIRE(luaL_dostring(lua.L, "return IsArenaTeamCaptain(1)") == 0);
    CHECK(lua_toboolean(lua.L, -1) == 1);
    lua_settop(lua.L, 0);

    // GetArenaTeamRosterInfo: name, rank(0=captain,1=member), level, class, online, played, win, seasonPlayed, seasonWin, rating
    REQUIRE(luaL_dostring(lua.L, "return GetArenaTeamRosterInfo(1, 1)") == 0);
    REQUIRE(lua_gettop(lua.L) == 10);
    CHECK(std::string(lua_tostring(lua.L, 1)) == "Hero");
    CHECK(lua_tonumber(lua.L, 2) == 0); // captain rank = 0 (gold)
    CHECK(lua_tonumber(lua.L, 3) == 70);
    CHECK(std::string(lua_tostring(lua.L, 4)) == "Warrior");
    CHECK(lua_toboolean(lua.L, 5) == 1);
    CHECK(lua_tonumber(lua.L, 6) == 7);
    CHECK(lua_tonumber(lua.L, 7) == 5);
    CHECK(lua_tonumber(lua.L, 8) == 21);
    CHECK(lua_tonumber(lua.L, 9) == 15);
    CHECK(lua_tonumber(lua.L, 10) == 1855);
    lua_settop(lua.L, 0);

    REQUIRE(luaL_dostring(lua.L, "return GetArenaTeamRosterInfo(1, 2)") == 0);
    REQUIRE(lua_gettop(lua.L) == 10);
    CHECK(std::string(lua_tostring(lua.L, 1)) == "Swiftmend");
    CHECK(lua_tonumber(lua.L, 2) == 1); // member rank = 1 (white)
    CHECK(std::string(lua_tostring(lua.L, 4)) == "Druid");
    CHECK(lua_toboolean(lua.L, 5) == 0);
    CHECK(lua_tonumber(lua.L, 10) == 1890);
    lua_settop(lua.L, 0);

    // GetArenaTeam: 22 return values including playerPlayed(8), seasonPlayerPlayed(9), playerRating(11), and banner(12..22)
    REQUIRE(luaL_dostring(lua.L, "return GetArenaTeam(1)") == 0);
    REQUIRE(lua_gettop(lua.L) == 22);
    CHECK(std::string(lua_tostring(lua.L, 1)) == "Blades Edge Brawlers");
    CHECK(lua_tonumber(lua.L, 2) == 2);
    CHECK(lua_tonumber(lua.L, 3) == 1875);
    CHECK(lua_tonumber(lua.L, 8) == 7);
    CHECK(lua_tonumber(lua.L, 9) == 21);
    CHECK(lua_tonumber(lua.L, 10) == 4);
    CHECK(lua_tonumber(lua.L, 11) == 1855);
    CHECK(lua_tonumber(lua.L, 12) == Catch::Approx(204.0 / 255.0));
    CHECK(lua_tonumber(lua.L, 15) == 12); // emblemStyle
    CHECK(lua_tonumber(lua.L, 19) == 3);  // borderStyle
    lua_settop(lua.L, 0);

    // S5-05: SMSG_ARENA_TEAM_EVENT consumes trailing 8-byte ObjectGuid
    {
        Packet ev(wireOpcode(Opcode::SMSG_ARENA_TEAM_EVENT));
        ev.writeUInt8(3); // ERR_ARENA_TEAM_JOIN_SS
        ev.writeUInt8(2);
        ev.writeString("Swiftmend");
        ev.writeString("Blades Edge Brawlers");
        ev.writeUInt64(kMateGuid);
        s.receive(ev);
    }
    CHECK(s.handler().getSocialHandler()->getLastArenaTeamEventGuid() == kMateGuid);
    CHECK(chatMentions(s, "Swiftmend"));

    // S5-03: SMSG_ARENA_ERROR with unk == 0 and uint8 arenaType = 5
    {
        Packet err(wireOpcode(Opcode::SMSG_ARENA_ERROR));
        err.writeUInt32(0);
        err.writeUInt8(5);
        s.receive(err);
    }
    CHECK(chatMentions(s, "5v5"));
}

TEST_CASE("WotLK SMSG_ARENA_TEAM_ROSTER with unk308=1 trailing floats still parses cleanly",
          "[replay][arena][wotlk]") {
    Session s("wotlk");
    s.receive(UpdateObject().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                    {{UF::UNIT_FIELD_LEVEL, 80}}, true));
    Packet ros(wireOpcode(Opcode::SMSG_ARENA_TEAM_ROSTER));
    ros.writeUInt32(99);
    ros.writeUInt8(1);  // unk308 = hasTrailingFloats
    ros.writeUInt32(2); // memberCount
    ros.writeUInt32(3); // 3v3
    for (int i = 0; i < 2; ++i) {
        ros.writeUInt64(0x100ull + static_cast<uint64_t>(i));
        ros.writeUInt8(1);
        ros.writeString(i == 0 ? "Alpha" : "Beta");
        ros.writeUInt32(i == 0 ? 1u : 0u);
        ros.writeUInt8(80);
        ros.writeUInt8(2);
        ros.writeUInt32(10); ros.writeUInt32(7);
        ros.writeUInt32(30); ros.writeUInt32(20);
        ros.writeUInt32(2100 + i * 50);
        ros.writeFloat(1500.0f); ros.writeFloat(1500.0f);
    }
    s.receive(ros);
    const auto* r = s.handler().getArenaTeamRoster(99);
    REQUIRE(r != nullptr);
    REQUIRE(r->members.size() == 2);
    CHECK(r->members[0].name == "Alpha");
    CHECK(r->members[0].isCaptain);
    CHECK(r->members[1].name == "Beta");
    CHECK(r->members[1].personalRating == 2150);
}

TEST_CASE("TBC MSG_INSPECT_ARENA_TEAMS sends C->S, accumulates per-slot 33-byte S->C, and exposes 18-value tuple",
          "[replay][arena][inspect][tbc]") {
    Session s("tbc");
    constexpr uint64_t kTargetGuid = 0x0000000000000420ull;
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 70}}, true));
    s.receive(UpdateObject().tbc().create(kTargetGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 70}}, false));

    int honorUpdates = 0;
    s.handler().setAddonEventCallback(
        [&](const std::string& ev, const std::vector<std::string>&) {
            if (ev == "INSPECT_HONOR_UPDATE") ++honorUpdates;
        });

    // S5-16: inspectUnit sends both CMSG_INSPECT and MSG_INSPECT_ARENA_TEAMS
    s.handler().inspectUnit(kTargetGuid);
    CHECK(s.sent().of(Opcode::CMSG_INSPECT).size() == 1);
    const auto arenaReqs = s.sent().of(Opcode::MSG_INSPECT_ARENA_TEAMS);
    REQUIRE(arenaReqs.size() == 1);
    {
        Packet req = arenaReqs[0];
        CHECK(req.readUInt64() == kTargetGuid);
    }

    // S5-02: Server sends two 33-byte per-slot packets (slot 0 = 2v2, slot 2 = 5v5)
    auto makeInspectSlot = [&](uint8_t slot, uint32_t teamId, uint32_t rating,
                               uint32_t teamPlayed, uint32_t teamWins,
                               uint32_t memberPlayed, uint32_t personalRating) {
        Packet p(wireOpcode(Opcode::MSG_INSPECT_ARENA_TEAMS));
        p.writeUInt64(kTargetGuid);
        p.writeUInt8(slot);
        p.writeUInt32(teamId);
        p.writeUInt32(rating);
        p.writeUInt32(teamPlayed);
        p.writeUInt32(teamWins);
        p.writeUInt32(memberPlayed);
        p.writeUInt32(personalRating);
        REQUIRE(p.getSize() == 33u);
        return p;
    };

    s.receive(makeInspectSlot(0, 501, 2050, 40, 29, 38, 2035));
    s.receive(makeInspectSlot(2, 505, 1910, 18, 12, 18, 1925));

    // Auto-queried both teams via CMSG_ARENA_TEAM_QUERY
    CHECK(s.sent().of(Opcode::CMSG_ARENA_TEAM_QUERY).size() == 2);
    CHECK(honorUpdates == 2);

    // Reply with SMSG_ARENA_TEAM_QUERY_RESPONSE for slot 0's team 501
    {
        Packet qr(wireOpcode(Opcode::SMSG_ARENA_TEAM_QUERY_RESPONSE));
        qr.writeUInt32(501);
        qr.writeString("Nagrand Gladiators");
        qr.writeUInt32(2);
        qr.writeUInt32(0xFFAA5500);
        qr.writeUInt32(7);
        qr.writeUInt32(0xFFFFFFFF);
        qr.writeUInt32(2);
        qr.writeUInt32(0xFF000000);
        s.receive(qr);
    }
    CHECK(honorUpdates == 3);

    const auto* ir = s.handler().getInspectResult();
    REQUIRE(ir != nullptr);
    REQUIRE(ir->arenaTeams.size() == 2);
    CHECK(ir->arenaTeams[0].slot == 0);
    CHECK(ir->arenaTeams[0].type == 2);
    CHECK(ir->arenaTeams[0].name == "Nagrand Gladiators");
    CHECK(ir->arenaTeams[1].slot == 2);
    CHECK(ir->arenaTeams[1].type == 5);
    CHECK(ir->arenaTeams[1].teamRating == 1910);

    ScopedLua lua(s);
    REQUIRE(luaL_dostring(lua.L, "return GetInspectArenaTeamData(1)") == 0);
    REQUIRE(lua_gettop(lua.L) == 18);
    CHECK(std::string(lua_tostring(lua.L, 1)) == "Nagrand Gladiators");
    CHECK(lua_tonumber(lua.L, 2) == 2);    // teamSize
    CHECK(lua_tonumber(lua.L, 3) == 2050); // teamRating
    CHECK(lua_tonumber(lua.L, 4) == 40);   // teamPlayed
    CHECK(lua_tonumber(lua.L, 5) == 29);   // teamWins
    CHECK(lua_tonumber(lua.L, 6) == 38);   // playerPlayed
    CHECK(lua_tonumber(lua.L, 7) == 2035); // playerRating
    CHECK(lua_tonumber(lua.L, 11) == 7);   // emblem
    CHECK(lua_tonumber(lua.L, 15) == 2);   // border
}

TEST_CASE("TBC SMSG_PVP_CREDIT formats victim name and PvP rank title", "[replay][pvp][tbc]") {
    Session s("tbc");
    constexpr uint64_t kEnemyOrc = 0x0000000000000777ull;
    // Alliance Human player (race=1)
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, 1u | (1u << 8)}}, true));
    // Spawn enemy Orc Warrior (race=2, class=1, gender=0) and name via SMSG_NAME_QUERY_RESPONSE
    s.receive(UpdateObject().tbc().create(
        kEnemyOrc, wowee::game::ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, 2u | (1u << 8)}}, false));
    {
        Packet nq(wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
        nq.writeUInt64(kEnemyOrc);
        nq.writeString("Thrallmar");
        nq.writeString("");
        nq.writeUInt32(2); // Orc (Horde)
        nq.writeUInt32(0); // Male
        nq.writeUInt32(1); // Warrior
        nq.writeUInt8(0);  // no declined names
        s.receive(nq);
    }

    // Rank 18 Horde = High Warlord
    {
        Packet hk(wireOpcode(Opcode::SMSG_PVP_CREDIT));
        hk.writeUInt32(21);
        hk.writeUInt64(kEnemyOrc);
        hk.writeUInt32(18);
        s.receive(hk);
    }
    CHECK(chatMentions(s, "Thrallmar"));
    CHECK(chatMentions(s, "High Warlord"));
    CHECK(chatMentions(s, "21"));

    // victimGuid == 0 -> COMBATLOG_HONORAWARD
    {
        Packet award(wireOpcode(Opcode::SMSG_PVP_CREDIT));
        award.writeUInt32(42);
        award.writeUInt64(0);
        award.writeUInt32(0);
        s.receive(award);
    }
    CHECK(chatMentions(s, "42"));
}

TEST_CASE("TBC Quest push decline, complete rewards, query metadata, kill guid, invalid 17, and dialog emotes",
          "[replay][quest][tbc]") {
    Session s("tbc");
    installTbcShard5GlobalStrings(s);
    constexpr uint64_t kSharerGuid = 0x0000000000000888ull;
    constexpr uint64_t kNpcGuid    = 0xF130000000001234ull;
    constexpr uint32_t kQuestId    = 10750;

    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 68}}, true));

    std::vector<std::pair<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>>> playedEmotes;
    s.handler().setNpcPanelEmotesCallback(
        [&](uint64_t guid, const std::vector<std::pair<uint32_t, uint32_t>>& emotes) {
            playedEmotes.emplace_back(guid, emotes);
        });

    // S5-17: Declining SMSG_QUESTGIVER_QUEST_DETAILS from a party sharer (player GUID) sends MSG_QUEST_PUSH_RESULT(selfGuid, 3)
    {
        Packet det(wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_DETAILS));
        det.writeUInt64(kSharerGuid);
        det.writeUInt32(kQuestId);
        det.writeString("The Path of Conquest");
        det.writeString("Details");
        det.writeString("Objectives");
        det.writeUInt32(1); // activateAccept
        det.writeUInt32(3); // suggestedPlayers
        det.writeUInt32(0); // choiceCount
        det.writeUInt32(0); // rewardCount
        det.writeUInt32(45000); // money
        det.writeUInt32(2500);  // honor (x10 -> 250)
        det.writeUInt32(0);     // rewSpell
        det.writeUInt32(0);     // rewSpellCast
        det.writeUInt32(0);     // charTitleBitIndex
        det.writeUInt32(1);     // emoteCount
        det.writeUInt32(1);     // emote
        det.writeUInt32(150);   // delay
        s.receive(det);
    }
    REQUIRE(playedEmotes.size() == 1);
    CHECK(playedEmotes[0].first == kSharerGuid);
    REQUIRE(playedEmotes[0].second.size() == 1);
    CHECK(playedEmotes[0].second[0] == std::make_pair(150u, 1u));

    {
        ScopedLua lua(s);
        REQUIRE(luaL_dostring(lua.L, "return GetRewardHonor(), GetSuggestedGroupNum()") == 0);
        CHECK(lua_tonumber(lua.L, 1) == 250);
        CHECK(lua_tonumber(lua.L, 2) == 3);
    }

    s.handler().declineQuest(true);
    const auto pushReplies = s.sent().of(Opcode::MSG_QUEST_PUSH_RESULT);
    REQUIRE(pushReplies.size() == 1);
    {
        Packet p = pushReplies[0];
        CHECK(p.readUInt64() == Session::kPlayerGuid);
        CHECK(p.readUInt8() == 3); // QUEST_PARTY_MSG_DECLINE_QUEST
    }

    // S5-14: SMSG_QUESTGIVER_QUEST_INVALID reason 17 -> ERR_QUEST_FAILED_MAX_COUNT_S
    s.handler().getQuestHandler()->addQuestToLocalLogIfMissing(kQuestId, "The Path of Conquest", "");
    {
        Packet req(wireOpcode(Opcode::SMSG_QUESTGIVER_REQUEST_ITEMS));
        req.writeUInt64(kNpcGuid);
        req.writeUInt32(kQuestId);
        req.writeString("The Path of Conquest");
        req.writeString("Have you brought them?");
        req.writeUInt32(200); // emoteDelay
        req.writeUInt32(6);   // emote (QUESTION)
        req.writeUInt32(1);   // CloseOnCancel
        req.writeUInt32(5);   // SuggestedPlayers
        req.writeUInt32(0);   // RequiredMoney
        req.writeUInt32(0);   // ReqItemsCount
        req.writeUInt32(0x03);
        req.writeUInt32(0x04); req.writeUInt32(0x08); req.writeUInt32(0x10);
        s.receive(req);
    }
    REQUIRE(playedEmotes.size() == 2);
    CHECK(playedEmotes[1].second[0] == std::make_pair(200u, 6u));

    {
        Packet inv(wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_INVALID));
        inv.writeUInt32(17); // INVALIDREASON_QUEST_FAILED_DUPLICATE_ITEM
        s.receive(inv);
    }
    CHECK(chatMentions(s, "Duplicate item found"));

    // S5-12: TBC SMSG_QUEST_QUERY_RESPONSE transfers repObjective, chain, spellCast, and POI onto QuestLogEntry
    s.handler().currentMapIdRef() = 530;
    {
        Packet qqr(wireOpcode(Opcode::SMSG_QUEST_QUERY_RESPONSE));
        qqr.writeUInt32(kQuestId);
        qqr.writeUInt32(2);    // method
        qqr.writeUInt32(70);   // level
        qqr.writeUInt32(3520); // Shadowmoon Valley
        qqr.writeUInt32(81);   // Dungeon
        qqr.writeUInt32(5);    // suggestedPlayers
        qqr.writeUInt32(932);  // repObjectiveFaction (The Aldor)
        qqr.writeUInt32(9000); // repObjectiveValue
        qqr.writeUInt32(0);    // oppositeFaction
        qqr.writeUInt32(0);    // oppositeValue
        qqr.writeUInt32(10751);// nextQuestInChain
        qqr.writeUInt32(75000);// rewardMoney
        qqr.writeUInt32(119400);// rewardMoneyMaxLevel
        qqr.writeUInt32(34090);// rewardSpell
        qqr.writeUInt32(34091);// rewardSpellCast
        qqr.writeUInt32(495);  // rewardHonor
        qqr.writeUInt32(31120);// sourceItemId
        qqr.writeUInt32(8);    // flags
        qqr.writeUInt32(53);   // charTitleId
        for (int i = 0; i < 4; ++i) { qqr.writeUInt32(0); qqr.writeUInt32(0); }
        for (int i = 0; i < 6; ++i) { qqr.writeUInt32(0); qqr.writeUInt32(0); }
        qqr.writeUInt32(530);  // pointMapId (Outland)
        qqr.writeFloat(-3500.0f);
        qqr.writeFloat(2100.0f);
        qqr.writeUInt32(4);    // pointOpt
        qqr.writeString("The Path of Conquest");
        qqr.writeString("Defeat the fel commander.");
        qqr.writeString("Full details text.");
        qqr.writeString("Return to Shadowmoon.");
        qqr.writeUInt32(21900); qqr.writeUInt32(1); qqr.writeUInt32(0); qqr.writeUInt32(0);
        for (int i = 1; i < 4; ++i) {
            qqr.writeUInt32(0); qqr.writeUInt32(0); qqr.writeUInt32(0); qqr.writeUInt32(0);
        }
        for (int i = 0; i < 4; ++i) qqr.writeString("");
        s.receive(qqr);
    }

    const auto& log = s.handler().getQuestLog();
    REQUIRE(log.size() == 1);
    CHECK(log[0].method == 2);
    CHECK(log[0].repObjectiveFaction == 932);
    CHECK(log[0].repObjectiveValue == 9000);
    CHECK(log[0].nextQuestInChain == 10751);
    CHECK(log[0].rewardSpellCastId == 34091);
    CHECK(log[0].pointMapId == 530);
    CHECK(log[0].pointX == Catch::Approx(-3500.0f));
    CHECK(log[0].pointY == Catch::Approx(2100.0f));
    CHECK(log[0].pointOpt == 4);
    CHECK_FALSE(s.handler().getQuestHandler()->getGossipPois().empty());

    // S5-13: SMSG_QUESTUPDATE_ADD_KILL stores trailing ObjectGuid
    constexpr uint64_t kKilledMobGuid = 0xF13000558C000042ull;
    {
        Packet ak(wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
        ak.writeUInt32(kQuestId);
        ak.writeUInt32(21900);
        ak.writeUInt32(1);
        ak.writeUInt32(1);
        ak.writeUInt64(kKilledMobGuid);
        s.receive(ak);
    }
    CHECK(s.handler().getQuestLog()[0].lastKillCreditGuid == kKilledMobGuid);

    // S5-09: SMSG_QUESTGIVER_OFFER_REWARD plays NPC emotes
    {
        Packet off(wireOpcode(Opcode::SMSG_QUESTGIVER_OFFER_REWARD));
        off.writeUInt64(kNpcGuid);
        off.writeUInt32(kQuestId);
        off.writeString("The Path of Conquest");
        off.writeString("Victory is ours!");
        off.writeUInt32(1); // EnableNext
        off.writeUInt32(5); // SuggestedPlayers
        off.writeUInt32(1); // EmoteCount
        off.writeUInt32(300); // delay
        off.writeUInt32(4);   // CHEER
        off.writeUInt32(0);   // choices
        off.writeUInt32(0);   // fixed
        off.writeUInt32(75000);
        off.writeUInt32(4950);
        off.writeUInt32(0x08);
        off.writeUInt32(34090);
        off.writeUInt32(34091);
        off.writeUInt32(36);
        s.receive(off);
    }
    REQUIRE(playedEmotes.size() == 3);
    CHECK(playedEmotes[2].second[0] == std::make_pair(300u, 4u));

    // S5-11: SMSG_QUESTGIVER_QUEST_COMPLETE parses XP, money, honor, and reward items
    {
        Packet qc(wireOpcode(Opcode::SMSG_QUESTGIVER_QUEST_COMPLETE));
        qc.writeUInt32(kQuestId);
        qc.writeUInt32(0x03);
        qc.writeUInt32(12650); // XP
        qc.writeUInt32(75000); // 7g 50s
        qc.writeUInt32(495);   // honor
        qc.writeUInt32(1);     // rewItemsCount
        qc.writeUInt32(29434); // Badge of Justice
        qc.writeUInt32(2);
        s.receive(qc);
    }
    const auto& done = s.handler().getQuestHandler()->getLastCompletedQuestReward();
    CHECK(done.questId == kQuestId);
    CHECK(done.xp == 12650);
    CHECK(done.money == 75000);
    CHECK(done.honor == 495);
    REQUIRE(done.items.size() == 1);
    CHECK(done.items[0].itemId == 29434);
    CHECK(done.items[0].count == 2);
    CHECK(chatMentions(s, "12650"));
    CHECK(chatMentions(s, "7 Gold"));
}
