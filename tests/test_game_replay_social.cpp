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

// CMaNGOS-TBC CharacterNameQueryResponse: guid, name, realm byte, race,
// gender, class as uint32s, no declined names.
Packet nameResponse(uint64_t guid, const std::string& name) {
    Packet p = packetFor(Opcode::SMSG_NAME_QUERY_RESPONSE);
    p.writeUInt64(guid);
    p.writeString(name);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt32(0);
    p.writeUInt32(4);
    p.writeUInt8(0);
    return p;
}

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
    s.receive(nameResponse(kGearugsy, "Gearugsy"));
    CHECK(events == std::vector<std::string>{"IGNORELIST_UPDATE"});
    for (const auto& [guid, name] : std::vector<std::pair<uint64_t, std::string>>{
             {kGearbefore, "Gearbefore"}, {kGearoqqtr, "Gearoqqtr"}, {kEdrikb, "Edrikb"},
             {kEdricc, "Edricc"}, {kEdricj, "Edricj"}})
        s.receive(nameResponse(guid, name));

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
