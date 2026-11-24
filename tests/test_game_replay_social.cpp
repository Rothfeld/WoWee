// The friends, ignore and who lists as the social panel reads them, through
// GameHandler's own dispatch. See game_replay.hpp.
//
// Measured on 2.4.3 (build 8606) against CMaNGOS-TBC on pair 11:
// - friends list online first, each half by name; an away friend (FriendStatus
//   2, not online|away) is connected with "<AFK>"; an offline one is name, 0,
//   "Unknown", "Unknown", nil, ""; the note is a seventh value only when set.
// - ignore list by name; names come from name queries, the packets carry guids.
// - adding to the ignore list fires IGNORELIST_UPDATE before the chat line.
// - SortWho keeps a chain of keys, most recent first: the same field again
//   reverses it, another moves to the front keeping its direction; zone,
//   level, name before any.
// Evidence: _experiments/20261118T200713Z.social_audit/ in the workspace root.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"
#include "addons/lua_engine.hpp"

#include <algorithm>
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

struct Contact {
    uint64_t guid;
    uint32_t flags;
    std::string note;
    uint8_t status;
    uint32_t area;
};

// PlayerSocial::SendSocialList.
Packet contactList(const std::vector<Contact>& contacts) {
    Packet p = packetFor(Opcode::SMSG_CONTACT_LIST);
    p.writeUInt32(0x7);
    p.writeUInt32(static_cast<uint32_t>(contacts.size()));
    for (const auto& c : contacts) {
        p.writeUInt64(c.guid);
        p.writeUInt32(c.flags);
        p.writeString(c.note);
        if (!(c.flags & 0x1)) continue;
        p.writeUInt8(c.status);
        if (!c.status) continue;
        p.writeUInt32(c.area);
        p.writeUInt32(40);
        p.writeUInt32(4);
    }
    return p;
}

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
        wowee::addons::registerSocialLuaAPI(L);
        wowee::addons::registerUnitLuaAPI(L);
        REQUIRE(luaL_dostring(L, R"(
            function show(...)
                local out = {}
                for i = 1, select("#", ...) do out[i] = tostring((select(i, ...))) end
                return table.concat(out, ",")
            end)") == 0);
    }
    ~Lua() { lua_close(L); }
    std::string run(const std::string& chunk) {
        const int rc = luaL_dostring(L, ("return show(" + chunk + ")").c_str());
        INFO(chunk << ": " << (rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
        std::string out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return out;
    }
    void exec(const std::string& chunk) {
        const int rc = luaL_dostring(L, chunk.c_str());
        INFO(chunk << ": " << (rc ? lua_tostring(L, -1) : ""));
        REQUIRE(rc == 0);
    }

private:
    lua_State* L;
};

constexpr uint64_t kEdricj = 36, kEdrikb = 19, kEdricc = 20, kGearbefore = 10;
constexpr uint64_t kGearugsy = 11, kGearoqqtr = 14;

}  // namespace

TEST_CASE("Friends and ignores are named, ordered and shaped as 2.4.3 lists them",
          "[replay][social]") {
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 12, "Elwynn Forest");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 40, "Westfall");
    std::vector<std::string> events;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        events.push_back(e);
    });

    s.receive(contactList({{kGearbefore, 0x1, "", 0, 0},
                           {kGearugsy, 0x2, "", 0, 0},
                           {kGearoqqtr, 0x2, "", 0, 0},
                           {kEdrikb, 0x1, "", 2, 40},
                           {kEdricc, 0x1, "", 0, 0},
                           {kEdricj, 0x1, "note two", 1, 12}}));
    // Every name is asked for, the ignored ones too.
    CHECK(s.sent().of(Opcode::CMSG_NAME_QUERY).size() == 6);

    events.clear();
    s.receive(replay::nameResponse(kGearugsy, "Gearugsy"));
    CHECK(events == std::vector<std::string>{"IGNORELIST_UPDATE"});
    for (const auto& [guid, name] : std::vector<std::pair<uint64_t, std::string>>{
             {kGearbefore, "Gearbefore"}, {kGearoqqtr, "Gearoqqtr"}, {kEdrikb, "Edrikb"},
             {kEdricc, "Edricc"}, {kEdricj, "Edricj"}})
        s.receive(replay::nameResponse(guid, name));

    Lua lua(s);
    CHECK(lua.run("GetNumFriends()") == "4,2");
    CHECK(lua.run("GetFriendInfo(1)") == "Edricj,40,Rogue,Elwynn Forest,true,,note two");
    CHECK(lua.run("GetFriendInfo(2)") == "Edrikb,40,Rogue,Westfall,true,<AFK>");
    CHECK(lua.run("GetFriendInfo(3)") == "Edricc,0,Unknown,Unknown,nil,");
    CHECK(lua.run("GetFriendInfo(4)") == "Gearbefore,0,Unknown,Unknown,nil,");
    CHECK(lua.run("GetFriendInfo(5)") == "nil,0,Unknown,Unknown,nil,");
    CHECK(lua.run("GetFriendInfo('Edrikb')") == "Edrikb,40,Rogue,Westfall,true,<AFK>");
    CHECK(lua.run("GetNumIgnores(), GetIgnoreName(1), GetIgnoreName(2), GetIgnoreName(3)") ==
          "2,Gearoqqtr,Gearugsy,Unknown");

    SECTION("the selection stays on the friend, not the row") {
        lua.run("SetSelectedFriend(2)");
        // Edricc comes online and moves above Edrikb.
        Packet online = packetFor(Opcode::SMSG_FRIEND_STATUS);
        online.writeUInt8(2);
        online.writeUInt64(kEdricc);
        online.writeUInt8(1);
        online.writeUInt32(12);
        online.writeUInt32(40);
        online.writeUInt32(4);
        s.receive(online);
        CHECK(lua.run("GetFriendInfo(1)") == "Edricc,40,Rogue,Elwynn Forest,true,");
        CHECK(lua.run("GetSelectedFriend()") == "3");
    }

    SECTION("an ignore added is on the list, announced as an ignore list change") {
        events.clear();
        s.handler().addIgnore("Edrikl");
        Packet added = packetFor(Opcode::SMSG_FRIEND_STATUS);
        added.writeUInt8(0x0F);
        added.writeUInt64(77);
        s.receive(added);
        // First, before the chat line (which needs GlobalStrings, absent here).
        REQUIRE_FALSE(events.empty());
        CHECK(events[0] == "IGNORELIST_UPDATE");
        CHECK(std::count(events.begin(), events.end(), "FRIENDLIST_UPDATE") == 0);
        CHECK(lua.run("GetNumIgnores(), GetIgnoreName(1)") == "3,Edrikl");
    }
}

TEST_CASE("Who rows carry their zone and sort by a chain of keys", "[replay][social][who]") {
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 1537, "Ironforge");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 876, "GM Island");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 40, "Westfall");
    s.handler().setWhoToUI(true);

    // WorldSession::HandleWhoOpcode: name, guild, level, class, race, gender,
    // zone - in the realm's own (hash) order.
    struct Row { const char* name; uint32_t level; uint32_t zone; };
    const std::vector<Row> rows = {{"Edrice", 70, 1537}, {"Edrikg", 40, 876}, {"Edricb", 70, 40},
                                   {"Edrikn", 40, 1537}, {"Edrikb", 40, 40}, {"Edricg", 40, 876},
                                   {"Edricn", 40, 1537}};
    Packet who = packetFor(Opcode::SMSG_WHO);
    who.writeUInt32(static_cast<uint32_t>(rows.size()));
    who.writeUInt32(static_cast<uint32_t>(rows.size()));
    for (const auto& r : rows) {
        who.writeString(r.name);
        who.writeString("");
        who.writeUInt32(r.level);
        who.writeUInt32(4);
        who.writeUInt32(1);
        who.writeUInt8(0);
        who.writeUInt32(r.zone);
    }
    s.receive(who);

    Lua lua(s);
    const std::string names =
        "(GetWhoInfo(1))..' '..(GetWhoInfo(2))..' '..(GetWhoInfo(3))..' '..(GetWhoInfo(4))..' '.."
        "(GetWhoInfo(5))..' '..(GetWhoInfo(6))..' '..(GetWhoInfo(7))";
    CHECK(lua.run("GetWhoInfo(1)") == "Edricg,,40,Human,Rogue,GM Island,ROGUE");
    // Zone, level, name: Ironforge 40, 40, 70 and Westfall 40, 70.
    CHECK(lua.run(names) == "Edricg Edrikg Edricn Edrikn Edrice Edrikb Edricb");
    CHECK(lua.run("GetWhoInfo(8)") == ",,0,Unknown,Unknown,Unknown");

    lua.run("SortWho('name')");
    lua.run("SortWho('name')");
    CHECK(lua.run(names) == "Edrikn Edrikg Edrikb Edricn Edricg Edrice Edricb");
    // Level to the front; ties stay by name, descending.
    lua.run("SortWho('level')");
    CHECK(lua.run(names) == "Edrikn Edrikg Edrikb Edricn Edricg Edrice Edricb");
    lua.run("SortWho('zone')");
    CHECK(lua.run(names) == "Edrikg Edricg Edrikn Edricn Edrice Edrikb Edricb");
    // Name back to the front keeps its descending direction.
    lua.run("SortWho('name')");
    CHECK(lua.run(names) == "Edrikn Edrikg Edrikb Edricn Edricg Edrice Edricb");
}

TEST_CASE("First-time faction discovery via 4-byte SMSG_SET_FACTION_VISIBLE announces standing and adds Other group",
          "[replay][social][reputation]") {
    Session s("tbc");
    auto& gh = s.handler();

    // Wire TBC Faction.dbc entries: Horde + 5 cities, plus standalone Argent Dawn & Cenarion Circle.
    wowee::game::GameHandlerTestAccess::setFaction(gh, 67, 12, "Horde", 0);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 530, 15, "Darkspear Trolls", 67);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 76, 14, "Orgrimmar", 67);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 911, 55, "Silvermoon City", 67);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 81, 16, "Thunder Bluff", 67);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 68, 17, "Undercity", 67);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 529, 13, "Argent Dawn", 0);
    wowee::game::GameHandlerTestAccess::setFaction(gh, 609, 36, "Cenarion Circle", 0);

    gh.setGlobalStringLookup([](const char* key) -> std::string {
        const std::string k(key);
        if (k == "FACTION_STANDING_CHANGED") return "You are now %s with %s.";
        if (k == "FACTION_STANDING_LABEL4") return "Neutral";
        if (k == "FACTION_STANDING_LABEL5") return "Friendly";
        return {};
    });

    std::vector<std::string> events;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        events.push_back(e);
    });

    // ReputationMgr::SendInitialReputations: 128 slots of (uint8 flags, int32 standing).
    Packet init = packetFor(Opcode::SMSG_INITIALIZE_FACTIONS);
    init.writeUInt32(128);
    for (uint32_t i = 0; i < 128; ++i) {
        uint8_t flags = 0;
        int32_t standing = 0;
        if (i == 14 || i == 15 || i == 16) {
            flags = wowee::game::GameHandler::FACTION_FLAG_VISIBLE;
            standing = 3000;  // Friendly
        } else if (i == 17 || i == 55) {
            flags = wowee::game::GameHandler::FACTION_FLAG_VISIBLE;
            standing = 0;  // Neutral
        }
        init.writeUInt8(flags);
        init.writeUInt32(static_cast<uint32_t>(standing));
    }
    s.receive(init);

    Lua lua(s);
    lua.exec("FACTION_OTHER = 'Other'; FACTION_INACTIVE = 'Inactive'");

    // Before discovery: only Horde header + 5 Horde cities, no Other header.
    CHECK(lua.run("GetNumFactions()") == "6");
    CHECK(lua.run("(select(1, GetFactionInfo(1))), (select(9, GetFactionInfo(1)))") == "Horde,true");
    CHECK(lua.run("(select(1, GetFactionInfo(2)))") == "Darkspear Trolls");
    CHECK(lua.run("(select(1, GetFactionInfo(6)))") == "Undercity");

    // ReputationMgr::SendVisible: 4-byte uint32 repListId only.
    events.clear();
    const size_t chatBefore = gh.getChatHistory().size();
    Packet visArgent = packetFor(Opcode::SMSG_SET_FACTION_VISIBLE);
    visArgent.writeUInt32(13);  // Argent Dawn
    s.receive(visArgent);

    CHECK(events == std::vector<std::string>{"CHAT_MSG_SYSTEM", "UPDATE_FACTION"});
    REQUIRE(gh.getChatHistory().size() == chatBefore + 1);
    CHECK(gh.getChatHistory().back().type == wowee::game::ChatType::SYSTEM);
    CHECK(gh.getChatHistory().back().message == "You are now Neutral with Argent Dawn.");

    // ReputationFrame rows now include "- Other" header followed by Argent Dawn (Neutral).
    CHECK(lua.run("GetNumFactions()") == "8");
    CHECK(lua.run("(select(1, GetFactionInfo(7))), (select(9, GetFactionInfo(7))), (select(10, GetFactionInfo(7)))") ==
          "Other,true,false");
    CHECK(lua.run("(select(1, GetFactionInfo(8))), (select(3, GetFactionInfo(8))), (select(9, GetFactionInfo(8)))") ==
          "Argent Dawn,4,false");

    // Re-receiving SMSG_SET_FACTION_VISIBLE for an already-visible faction is a no-op.
    events.clear();
    const size_t chatAfterFirst = gh.getChatHistory().size();
    Packet visArgentAgain = packetFor(Opcode::SMSG_SET_FACTION_VISIBLE);
    visArgentAgain.writeUInt32(13);
    s.receive(visArgentAgain);
    CHECK(events.empty());
    CHECK(gh.getChatHistory().size() == chatAfterFirst);

    // Discovering a second standalone faction appends under Other in alphabetical order.
    events.clear();
    Packet visCenarion = packetFor(Opcode::SMSG_SET_FACTION_VISIBLE);
    visCenarion.writeUInt32(36);  // Cenarion Circle
    s.receive(visCenarion);
    CHECK(events == std::vector<std::string>{"CHAT_MSG_SYSTEM", "UPDATE_FACTION"});
    CHECK(gh.getChatHistory().back().message == "You are now Neutral with Cenarion Circle.");
    CHECK(lua.run("GetNumFactions()") == "9");
    CHECK(lua.run("(select(1, GetFactionInfo(7))), (select(1, GetFactionInfo(8))), (select(1, GetFactionInfo(9)))") ==
          "Other,Argent Dawn,Cenarion Circle");

    // Collapsing and expanding the Other header toggles visibility of its children.
    events.clear();
    lua.run("CollapseFactionHeader(7)");
    CHECK(events == std::vector<std::string>{"UPDATE_FACTION"});
    CHECK(lua.run("GetNumFactions()") == "7");
    CHECK(lua.run("(select(1, GetFactionInfo(7))), (select(9, GetFactionInfo(7))), (select(10, GetFactionInfo(7)))") ==
          "Other,true,true");

    events.clear();
    lua.run("ExpandFactionHeader(7)");
    CHECK(events == std::vector<std::string>{"UPDATE_FACTION"});
    CHECK(lua.run("GetNumFactions()") == "9");
    CHECK(lua.run("(select(1, GetFactionInfo(7))), (select(10, GetFactionInfo(7)))") == "Other,false");

    // Moving all Other factions to Inactive removes the Other header and reveals Inactive.
    lua.run("SetFactionInactive(9)");
    lua.run("SetFactionInactive(8)");
    CHECK(lua.run("GetNumFactions()") == "7");
    CHECK(lua.run("(select(1, GetFactionInfo(7))), (select(9, GetFactionInfo(7))), (select(10, GetFactionInfo(7)))") ==
          "Inactive,true,true");
}

TEST_CASE("Reputation standing gain does not auto-watch; watched faction follows PLAYER_FIELD_WATCHED_FACTION_INDEX",
          "[replay][social][reputation][watched-faction]") {
    Session s("tbc");
    auto& gh = s.handler();
    wowee::game::GameHandlerTestAccess::setFaction(gh, 942, 48, "Cenarion Expedition");
    wowee::game::GameHandlerTestAccess::setFaction(gh, 970, 56, "Sporeggar");

    Packet init = packetFor(Opcode::SMSG_INITIALIZE_FACTIONS);
    init.writeUInt32(64);
    for (uint32_t i = 0; i < 64; ++i) {
        const uint8_t flags = (i == 48 || i == 56) ? wowee::game::GameHandler::FACTION_FLAG_VISIBLE : 0;
        init.writeUInt8(flags);
        init.writeUInt32(0);
    }
    s.receive(init);

    Lua lua(s);
    lua.exec("FACTION_OTHER = 'Other'; FACTION_INACTIVE = 'Inactive'");

    // Initially no faction is watched.
    CHECK(gh.getWatchedFactionId() == 0);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "nil");

    // Gaining reputation via SMSG_SET_FACTION_STANDING must NOT auto-watch the faction.
    Packet standing = packetFor(Opcode::SMSG_SET_FACTION_STANDING);
    standing.writeFloat(0.0f);  // TBC RAF bonus
    standing.writeUInt32(1);    // count = 1
    standing.writeUInt32(56);   // Sporeggar repListId
    standing.writeUInt32(21);   // new standing
    s.receive(standing);

    CHECK(gh.getWatchedFactionId() == 0);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "nil");

    // Server PLAYER_FIELD_WATCHED_FACTION_INDEX (= PLAYER_FIELD_BYTES2 + 1) syncs watched faction.
    const uint16_t watchedField = static_cast<uint16_t>(
        wowee::game::fieldIndex(wowee::game::UF::PLAYER_FIELD_BYTES2) + 1);
    REQUIRE(watchedField == 1519);

    std::vector<std::string> events;
    gh.setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>&) {
        events.push_back(ev);
    });

    auto sendPlayerField = [&](uint16_t fieldIdx, uint32_t value) {
        Packet pkt = packetFor(Opcode::SMSG_UPDATE_OBJECT);
        pkt.writeUInt32(1);  // block count
        pkt.writeUInt8(0);   // hasTransport
        pkt.writeUInt8(0);   // UPDATETYPE_VALUES
        // Packed GUID for playerGuid (1)
        pkt.writeUInt8(0x01);
        pkt.writeUInt8(static_cast<uint8_t>(gh.getPlayerGuid() & 0xFF));
        const uint8_t blockCount = static_cast<uint8_t>(fieldIdx / 32 + 1);
        pkt.writeUInt8(blockCount);
        for (uint8_t b = 0; b < blockCount; ++b) {
            uint32_t mask = (b == fieldIdx / 32) ? (1u << (fieldIdx % 32)) : 0u;
            pkt.writeUInt32(mask);
        }
        pkt.writeUInt32(value);
        s.receive(pkt);
    };

    // Spawn player entity so UPDATETYPE_VALUES applies to it.
    auto playerUnit = std::make_shared<wowee::game::Player>(gh.getPlayerGuid());
    gh.getEntityManager().addEntity(gh.getPlayerGuid(), playerUnit);

    events.clear();
    sendPlayerField(watchedField, 56u);  // Sporeggar
    CHECK(gh.getWatchedFactionId() == 970);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "Sporeggar");
    CHECK(std::find(events.begin(), events.end(), "UPDATE_FACTION") != events.end());

    // Server setting -1 (0xFFFFFFFF) unwatches the bar.
    events.clear();
    sendPlayerField(watchedField, 0xFFFFFFFFu);
    CHECK(gh.getWatchedFactionId() == 0);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "nil");
    CHECK(std::find(events.begin(), events.end(), "UPDATE_FACTION") != events.end());

    // Explicit SetWatchedFactionIndex sends CMSG_SET_WATCHED_FACTION and updates local watched faction.
    // Rows: 1 = Other (header), 2 = Cenarion Expedition, 3 = Sporeggar
    lua.run("SetWatchedFactionIndex(3)");
    CHECK(gh.getWatchedFactionId() == 970);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "Sporeggar");
    REQUIRE(s.sent().of(Opcode::CMSG_SET_WATCHED_FACTION).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_SET_WATCHED_FACTION).front().readUInt32() == 56u);

    lua.run("SetWatchedFactionIndex(0)");
    CHECK(gh.getWatchedFactionId() == 0);
    CHECK(lua.run("(select(1, GetWatchedFactionInfo()))") == "nil");
    REQUIRE(s.sent().of(Opcode::CMSG_SET_WATCHED_FACTION).size() == 2);
    CHECK(s.sent().of(Opcode::CMSG_SET_WATCHED_FACTION).back().readUInt32() == 0xFFFFFFFFu);
}

TEST_CASE("Guild roster defaults to rank descending then name ascending, and sorts offline zones",
          "[replay][social][guild]") {
    Session s("tbc");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 1497, "Undercity");
    wowee::game::GameHandlerTestAccess::setAreaName(s.handler(), 1519, "Stormwind City");

    // Wire layout matching TBC 2.4.3 SMSG_GUILD_ROSTER (ranks with bank tabs + gender byte).
    Packet roster = packetFor(Opcode::SMSG_GUILD_ROSTER);
    roster.writeUInt32(6);
    roster.writeString("Test MOTD");
    roster.writeString("Test Info");
    roster.writeUInt32(5);
    for (uint32_t r = 0; r < 5; ++r) {
        roster.writeUInt32(0);  // rights
        roster.writeUInt32(0);  // goldLimit
        for (int t = 0; t < 6; ++t) {
            roster.writeUInt32(0);
            roster.writeUInt32(0);
        }
    }
    struct MemberSpec {
        uint64_t guid;
        bool online;
        const char* name;
        uint32_t rank;
        uint32_t zone;
    };
    const MemberSpec specs[] = {
        {1, true,  "Pertybert",  0, 1497},
        {2, false, "Edrick",     1, 1519},
        {3, false, "Edrikk",     1, 1497},
        {4, true,  "Pertybarry", 1, 1497},
        {5, false, "Gearoqqt",   4, 1519},
        {6, false, "Gearbefore", 3, 1497},
    };
    for (const auto& m : specs) {
        roster.writeUInt64(m.guid);
        roster.writeUInt8(m.online ? 1 : 0);
        roster.writeString(m.name);
        roster.writeUInt32(m.rank);
        roster.writeUInt8(70);  // level
        roster.writeUInt8(1);   // classId
        roster.writeUInt8(0);   // gender (TBC)
        roster.writeUInt32(m.zone);
        if (!m.online) roster.writeFloat(1.0f);
        roster.writeString("");
        roster.writeString("");
    }
    s.receive(roster);

    Lua lua(s);
    lua.exec("SetGuildRosterShowOffline(true)");
    // Default order matches 2.4.3: lowest rank (highest rankIndex) first, tie-breaking by name ascending.
    CHECK(lua.run("(select(1, GetGuildRosterInfo(1))),(select(1, GetGuildRosterInfo(2))),"
                  "(select(1, GetGuildRosterInfo(3))),(select(1, GetGuildRosterInfo(4))),"
                  "(select(1, GetGuildRosterInfo(5))),(select(1, GetGuildRosterInfo(6)))") ==
          "Gearoqqt,Gearbefore,Edrick,Edrikk,Pertybarry,Pertybert");

    // Clicking the rank column reverses the default descending rank order to ascending (GM first).
    lua.exec("SortGuildRoster('rank')");
    CHECK(lua.run("(select(1, GetGuildRosterInfo(1))),(select(1, GetGuildRosterInfo(6)))") ==
          "Pertybert,Gearoqqt");

    // Sorting by zone includes offline members' resolved area names (Stormwind City before Undercity).
    lua.exec("SortGuildRoster('zone')");
    CHECK(lua.run("(select(1, GetGuildRosterInfo(1))),(select(1, GetGuildRosterInfo(2)))") ==
          "Edrick,Gearoqqt");
}

TEST_CASE("Guild bank parity: GetCoinText uses coin words, toplevel Show raises, and bank purchase refreshes tab",
          "[replay][social][guild]") {
    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());

    REQUIRE(lua.executeString(R"lua(
        __coin_full = GetCoinText(23456, " ")
        __coin_gold_only = GetCoinText(120000, " ")
        __coin_zero = GetCoinText(0, " ")
        __coin_default_sep = GetCoinText(10203)
        __coin_tex = GetCoinTextureString(23456)

        local bank = CreateFrame("Frame", "TestGuildBankFrame", UIParent)
        bank:SetToplevel(true)
        local tab1 = CreateFrame("Frame", "TestGuildBankTab1", bank)
        local popup = CreateFrame("Frame", "TestGuildBankPopupFrame", UIParent)
        popup:SetToplevel(true)
        popup:Hide()

        -- Clicking a tab on GuildBankFrame raises GuildBankFrame (and its children).
        bank:Raise()
        -- Opening GuildBankPopupFrame via Show() must raise it above GuildBankFrame and its tab children.
        popup:Show()
        __bank_lvl = bank:GetFrameLevel()
        __tab_lvl = tab1:GetFrameLevel()
        __popup_lvl = popup:GetFrameLevel()
    )lua"));

    lua_State* L = lua.getState();
    auto getStr = [L](const char* name) -> std::string {
        lua_getglobal(L, name);
        const char* s = lua_tostring(L, -1);
        std::string out = s ? s : "";
        lua_pop(L, 1);
        return out;
    };
    auto getInt = [L](const char* name) -> int {
        lua_getglobal(L, name);
        int v = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        return v;
    };

    CHECK(getStr("__coin_full") == "2 Gold 34 Silver 56 Copper");
    CHECK(getStr("__coin_gold_only") == "12 Gold");
    CHECK(getStr("__coin_zero") == "0 Copper");
    CHECK(getStr("__coin_default_sep") == "1 Gold, 2 Silver, 3 Copper");
    CHECK(getStr("__coin_tex").find("|TInterface\\MoneyFrame\\UI-GoldIcon") != std::string::npos);

    CHECK(getInt("__popup_lvl") > getInt("__bank_lvl"));
    CHECK(getInt("__popup_lvl") > getInt("__tab_lvl"));

    // Receiving BANK_TAB_PURCHASED while the guild bank is open re-queries the active tab.
    Session s("tbc");
    Packet minRoster = packetFor(Opcode::SMSG_GUILD_ROSTER);
    minRoster.writeUInt32(0);  // 0 members is enough to set guildRoster_ / isInGuild via guildName
    minRoster.writeString("");
    minRoster.writeString("");
    minRoster.writeUInt32(0);
    s.receive(minRoster);
    // Mark player as in guild 1 via SMSG_GUILD_QUERY_RESPONSE so openGuildBank accepts the vault.
    Packet gq = packetFor(Opcode::SMSG_GUILD_QUERY_RESPONSE);
    gq.writeUInt32(1);
    gq.writeString("Test Guild");
    for (int r = 0; r < 10; ++r) gq.writeString("Rank");
    gq.writeUInt32(0); gq.writeUInt32(0); gq.writeUInt32(0); gq.writeUInt32(0); gq.writeUInt32(0);
    s.receive(gq);
    s.handler().openGuildBank(0xF11000A3B7000001ULL);
    Packet bankList = packetFor(Opcode::SMSG_GUILD_BANK_LIST);
    bankList.writeUInt64(0);   // money
    bankList.writeUInt8(0);    // tabId
    bankList.writeUInt32(0);   // remainingWithdrawals
    bankList.writeUInt8(1);    // fullUpdate
    bankList.writeUInt8(0);    // numTabs
    bankList.writeUInt8(0);    // numItems
    s.receive(bankList);
    REQUIRE(s.handler().isGuildBankOpen());

    const size_t queriesBefore = s.sent().of(Opcode::CMSG_GUILD_BANK_QUERY_TAB).size();
    Packet evt = packetFor(Opcode::SMSG_GUILD_EVENT);
    evt.writeUInt8(wowee::game::GuildEvent::BANK_TAB_PURCHASED);
    evt.writeUInt8(0);
    s.receive(evt);
    CHECK(s.sent().of(Opcode::CMSG_GUILD_BANK_QUERY_TAB).size() == queriesBefore + 1);
}

TEST_CASE("SMSG_SET_FACTION_STANDING fires reputation level-up callback only on upward standing tier transitions",
          "[replay][social][reputation][levelup]") {
    auto initFactions = [](Session& s, int32_t standing14, int32_t standing15) {
        Packet init = packetFor(Opcode::SMSG_INITIALIZE_FACTIONS);
        init.writeUInt32(128);
        for (uint32_t i = 0; i < 128; ++i) {
            uint8_t flags = (i == 14 || i == 15) ? wowee::game::GameHandler::FACTION_FLAG_VISIBLE : 0;
            int32_t standing = (i == 14) ? standing14 : (i == 15 ? standing15 : 0);
            init.writeUInt8(flags);
            init.writeUInt32(static_cast<uint32_t>(standing));
        }
        s.receive(init);
    };

    SECTION("TBC rank-up, spillover deduplication, rank-down, and same-rank gain") {
        Session s("tbc");
        auto& gh = s.handler();
        wowee::game::GameHandlerTestAccess::setFaction(gh, 970, 14, "Sporeggar", 0);
        wowee::game::GameHandlerTestAccess::setFaction(gh, 942, 15, "Cenarion Expedition", 0);
        initFactions(s, -1500, 2900);  // Unfriendly, Neutral

        int repLevelUpCount = 0;
        gh.setReputationLevelUpCallback([&]() { ++repLevelUpCount; });

        auto sendTbcStanding = [&](const std::vector<std::pair<uint32_t, int32_t>>& entries) {
            Packet pkt = packetFor(Opcode::SMSG_SET_FACTION_STANDING);
            pkt.writeFloat(0.0f);  // TBC RAF bonus
            pkt.writeUInt32(static_cast<uint32_t>(entries.size()));
            for (const auto& [repListId, standing] : entries) {
                pkt.writeUInt32(repListId);
                pkt.writeUInt32(static_cast<uint32_t>(standing));
            }
            s.receive(pkt);
        };

        // Unfriendly (-1500) -> Neutral (0): rank UP -> fires once.
        sendTbcStanding({{14, 0}});
        CHECK(repLevelUpCount == 1);

        // Same-rank gain Neutral (0) -> Neutral (500): no tier cross -> does not fire.
        sendTbcStanding({{14, 500}});
        CHECK(repLevelUpCount == 1);

        // Spillover packet where BOTH factions cross upward (Neutral -> Friendly): fires only once per packet.
        sendTbcStanding({{14, 3000}, {15, 3100}});
        CHECK(repLevelUpCount == 2);

        // Rank DOWN Friendly (3000) -> Neutral (0): does not fire.
        sendTbcStanding({{14, 0}});
        CHECK(repLevelUpCount == 2);
    }

    SECTION("WotLK showVisual byte gates the reputation level-up callback") {
        Session s("wotlk");
        auto& gh = s.handler();
        wowee::game::GameHandlerTestAccess::setFaction(gh, 970, 14, "Sporeggar", 0);
        initFactions(s, 0, 0);

        int repLevelUpCount = 0;
        gh.setReputationLevelUpCallback([&]() { ++repLevelUpCount; });

        // showVisual = 0 suppresses visual/sound callback even on Neutral -> Friendly.
        Packet hidden = packetFor(Opcode::SMSG_SET_FACTION_STANDING);
        hidden.writeFloat(0.0f);
        hidden.writeUInt8(0);  // showVisual = false
        hidden.writeUInt32(1);
        hidden.writeUInt32(14);
        hidden.writeUInt32(3000);
        s.receive(hidden);
        CHECK(repLevelUpCount == 0);

        // showVisual = 1 fires callback on Friendly -> Honored.
        Packet shown = packetFor(Opcode::SMSG_SET_FACTION_STANDING);
        shown.writeFloat(0.0f);
        shown.writeUInt8(1);  // showVisual = true
        shown.writeUInt32(1);
        shown.writeUInt32(14);
        shown.writeUInt32(9000);
        s.receive(shown);
        CHECK(repLevelUpCount == 1);
    }
}
