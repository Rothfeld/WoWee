// TBC 2.4.3 Network Wire-Payload Sweep — Shard 4 (Social, Groups, Chat, LFG, GMTickets)
// Deterministic packet round-trip, dispatch, and Lua API parity tests.
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
using wowee::game::GameHandlerTestAccess;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

namespace {

struct RecordedEvent {
    std::string name;
    std::vector<std::string> args;
};

std::string shard4GlobalString(const char* name) {
    const std::string key = name ? name : "";
    if (key == "ERR_CHAT_RESTRICTED") return "Trial accounts cannot send unlimited tells.";
    if (key == "ERR_CHAT_THROTTLED") return "The number of messages that can be sent is limited.";
    if (key == "ERR_USER_SQUELCHED") return "Your chat and mail privileges have been temporarily suspended.";
    if (key == "ERR_MEETING_STONE_MUST_BE_LEADER") return "You must be the party leader to interact with the meeting stone";
    if (key == "ERR_MEETING_STONE_GROUP_FULL") return "You are already in a full group";
    if (key == "ERR_MEETING_STONE_NO_RAID_GROUP") return "You cannot use a meeting stone while in a raid group";
    if (key == "ERR_MEETING_STONE_IN_QUEUE_S") return "You are now in the queue to join a party for %s.";
    if (key == "ERR_MEETING_STONE_LEFT_QUEUE_S") return "You have left the queue to join a party for %s.";
    if (key == "ERR_MEETING_STONE_OTHER_MEMBER_LEFT") return "Party member has left.  Looking for a new party in the meeting stone queue.";
    if (key == "ERR_MEETING_STONE_SUCCESS") return "Your group is complete, you have left the meeting stone queue.";
    if (key == "ERR_FRIEND_DB_ERROR") return "Friend database error.";
    if (key == "ERR_FRIEND_LIST_FULL") return "Your friend list is full.";
    if (key == "ERR_FRIEND_ADDED_S") return "%s added to friends.";
    if (key == "ERR_FRIEND_REMOVED_S") return "%s removed from friends.";
    if (key == "ERR_VOICE_IGNORE_ADDED_S") return "%s is now being voice muted.";
    if (key == "ERR_VOICE_IGNORE_REMOVED_S") return "%s is no longer being voice muted.";
    if (key == "ERR_VOICE_IGNORE_FULL") return "You can't voice mute any more players.";
    if (key == "ERR_VOICE_IGNORE_SELF") return "You can't voice mute yourself.";
    if (key == "ERR_VOICE_IGNORE_NOT_FOUND") return "Player not found.";
    if (key == "ERR_VOICE_IGNORE_ALREADY_S") return "%s is already being voice muted.";
    if (key == "ERR_VOICE_IGNORE_AMBIGUOUS") return "That name is ambiguous.";
    return {};
}

lua_State* makeLua(wowee::game::GameHandler& gh) {
    lua_State* L = luaL_newstate();
    luaopen_base(L);
    luaopen_table(L);
    luaopen_string(L);
    lua_pushlightuserdata(L, &gh);
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerUnitLuaAPI(L);
    wowee::addons::registerSocialLuaAPI(L);
    wowee::addons::registerSystemLuaAPI(L);
    wowee::addons::registerLfgLuaAPI(L);
    return L;
}

std::string evalLua(lua_State* L, const std::string& expr) {
    const std::string code = "local out = {} local r = {" + expr +
        "} for i = 1, table.maxn(r) do out[i] = tostring(r[i]) end return table.concat(out, ',')";
    const int rc = luaL_dostring(L, code.c_str());
    INFO(expr << ": " << (rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
    std::string out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return out;
}

void writePackedGuid(Packet& p, uint64_t guid) {
    uint8_t mask = 0;
    uint8_t bytes[8];
    int count = 0;
    for (int i = 0; i < 8; ++i) {
        uint8_t b = static_cast<uint8_t>((guid >> (i * 8)) & 0xFFu);
        if (b != 0) {
            mask |= static_cast<uint8_t>(1u << i);
            bytes[count++] = b;
        }
    }
    p.writeUInt8(mask);
    for (int i = 0; i < count; ++i) p.writeUInt8(bytes[i]);
}

}  // namespace

TEST_CASE("TBC GM tickets: SMSG_GMTICKET_GETTICKET, CMSG_GMTICKET_CREATE, SMSG_GM_TICKET_STATUS_UPDATE, CMSG_GMSURVEY_SUBMIT",
          "[replay][shard4][gmticket]") {
    Session s("tbc");
    auto& gh = s.handler();
    GameHandlerTestAccess::setNoticesInterfaceReady(gh, true);

    std::vector<RecordedEvent> events;
    gh.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        events.push_back({name, args});
    });

    // 1. CMSG_GMTICKET_CREATE on TBC omits WotLK uint8 needMoreHelp (exact 29 + text+1 bytes)
    gh.setPosition(100.0f, 200.0f, 50.0f);
    gh.submitGmTicket("Stuck in geometry");
    const auto creates = s.sent().of(Opcode::CMSG_GMTICKET_CREATE);
    REQUIRE(creates.size() == 1);
    {
        Packet body = creates[0];
        /*mapId=*/ body.readUInt32();
        CHECK(body.readFloat() == Catch::Approx(100.0f));
        CHECK(body.readFloat() == Catch::Approx(200.0f));
        CHECK(body.readFloat() == Catch::Approx(50.0f));
        CHECK(body.readString() == "Stuck in geometry");
        CHECK(body.readUInt32() == 1u); // needResponse
        CHECK(body.readUInt32() == 0u); // chatDataLineCount
        CHECK(body.readUInt32() == 0u); // chatDataSizeInflated
        CHECK(body.getRemainingSize() == 0u); // NO extra WotLK needMoreHelp byte!
    }

    // 2. SMSG_GMTICKET_GETTICKET (TBC layout: uint32 status=6, cstring text, uint8 category,
    //    3x float days, uint8 escalatedStatus, uint8 isSeen)
    events.clear();
    {
        Packet pkt(wireOpcode(Opcode::SMSG_GMTICKET_GETTICKET));
        pkt.writeUInt32(6); // GMTICKET_STATUS_HASTEXT (uint32 on TBC, NOT uint8!)
        pkt.writeString("Need GM assistance in Shattrath");
        pkt.writeUInt8(2);      // category
        pkt.writeFloat(0.25f);  // daysTicketAge (6h)
        pkt.writeFloat(0.50f);  // daysOldestTicketAge (12h -> 6h remaining wait)
        pkt.writeFloat(0.01f);  // daysLastUpdated
        pkt.writeUInt8(2);      // escalatedStatus = GMTICKET_ASSIGNEDTOGM_STATUS_ESCALATED
        pkt.writeUInt8(1);      // isSeen = GMTICKET_OPENEDBYGM_STATUS_OPENED
        s.receive(pkt);
    }
    CHECK(gh.hasActiveGmTicket());
    CHECK(gh.getGmTicketText() == "Need GM assistance in Shattrath");
    CHECK(gh.getGmTicketWaitHours() == Catch::Approx(6.0f));
    REQUIRE(events.size() == 1);
    CHECK(events[0].name == "UPDATE_TICKET");
    REQUIRE(events[0].args.size() == 7);
    CHECK(events[0].args[0] == "2");
    CHECK(events[0].args[1] == "Need GM assistance in Shattrath");
    CHECK(events[0].args[5] == "2");
    CHECK(events[0].args[6] == "1");

    // 3. SMSG_GM_TICKET_STATUS_UPDATE: 1=UPDATED triggers CMSG_GMTICKET_GETTICKET, 3=SURVEY fires GMSURVEY_DISPLAY
    {
        Packet upd(wireOpcode(Opcode::SMSG_GM_TICKET_STATUS_UPDATE));
        upd.writeUInt32(1); // GMTICKET_STATUS_UPDATED
        s.receive(upd);
    }
    CHECK(s.sent().of(Opcode::CMSG_GMTICKET_GETTICKET).size() == 1);

    events.clear();
    {
        Packet survey(wireOpcode(Opcode::SMSG_GM_TICKET_STATUS_UPDATE));
        survey.writeUInt32(3); // GMTICKET_STATUS_SURVEY
        s.receive(survey);
    }
    CHECK_FALSE(gh.hasActiveGmTicket());
    REQUIRE(events.size() == 2);
    CHECK(events[0].name == "UPDATE_TICKET");
    CHECK(events[0].args.empty());
    CHECK(events[1].name == "GMSURVEY_DISPLAY");

    // 4. GMSurveySubmit Lua API sends CMSG_GMSURVEY_SUBMIT matching GMTicketHandler.cpp:198-226
    lua_State* L = makeLua(gh);
    REQUIRE(luaL_dostring(L,
        "GMSurveyAnswerSubmit(1, 5, 'Great')\n"
        "GMSurveyAnswerSubmit(2, 4, '')\n"
        "GMSurveyCommentSubmit('Fast response!')\n"
        "GMSurveySubmit()") == 0);
    const auto surveys = s.sent().of(Opcode::CMSG_GMSURVEY_SUBMIT);
    REQUIRE(surveys.size() == 1);
    {
        Packet body = surveys[0];
        /*surveyId=*/ body.readUInt32();
        CHECK(body.readUInt32() == 1u);
        CHECK(body.readUInt8() == 5u);
        CHECK(body.readString() == "Great");
        CHECK(body.readUInt32() == 2u);
        CHECK(body.readUInt8() == 4u);
        CHECK(body.readString().empty());
        CHECK(body.readUInt32() == 0u); // sentinel
        CHECK(body.readString() == "Fast response!");
        CHECK(body.getRemainingSize() == 0u);
    }
    lua_close(L);
}

TEST_CASE("TBC MeetingStone & LFG pending match/invite wire contracts", "[replay][shard4][lfg]") {
    Session s("tbc");
    auto& gh = s.handler();
    gh.setGlobalStringLookup(shard4GlobalString);
    GameHandlerTestAccess::setNoticesInterfaceReady(gh, true);
    GameHandlerTestAccess::setAreaName(gh, 1581, "The Deadmines");

    std::vector<RecordedEvent> events;
    gh.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        events.push_back({name, args});
    });

    auto lastSystemMsg = [&]() -> std::string {
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->name == "CHAT_MSG_SYSTEM" && !it->args.empty()) return it->args[0];
        }
        return {};
    };

    // 1. 5-byte SMSG_MEETINGSTONE_SETQUEUE IN_QUEUE (status=1) and LEFT_QUEUE (status=0), plus 6-byte JOINED_GROUP (status=5)
    {
        Packet inQ(wireOpcode(Opcode::SMSG_MEETINGSTONE_SETQUEUE));
        inQ.writeUInt32(1581);
        inQ.writeUInt8(1); // IN_QUEUE (5 bytes total!)
        s.receive(inQ);
    }
    CHECK(lastSystemMsg() == "You are now in the queue to join a party for The Deadmines.");

    {
        Packet leftQ(wireOpcode(Opcode::SMSG_MEETINGSTONE_SETQUEUE));
        leftQ.writeUInt32(1581);
        leftQ.writeUInt8(0); // LEFT_QUEUE (5 bytes total!)
        s.receive(leftQ);
    }
    CHECK(lastSystemMsg() == "You have left the queue to join a party for The Deadmines.");

    {
        Packet joined(wireOpcode(Opcode::SMSG_MEETINGSTONE_SETQUEUE));
        joined.writeUInt32(1581);
        joined.writeUInt8(5); // JOINED_GROUP
        joined.writeUInt8(1); // asLeader (6 bytes total)
        s.receive(joined);
    }
    CHECK(lastSystemMsg() == "Your group is complete, you have left the meeting stone queue.");

    // 2. SMSG_MEETINGSTONE_JOINFAILED reasons 0..3
    events.clear();
    for (uint8_t reason : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(3)}) {
        Packet fail(wireOpcode(Opcode::SMSG_MEETINGSTONE_JOINFAILED));
        fail.writeUInt8(reason);
        s.receive(fail);
    }
    std::vector<std::string> sysLines;
    for (const auto& ev : events) {
        if (ev.name == "CHAT_MSG_SYSTEM" && !ev.args.empty()) sysLines.push_back(ev.args[0]);
    }
    REQUIRE(sysLines.size() == 3); // reason=0 is silent
    CHECK(sysLines[0] == "You must be the party leader to interact with the meeting stone");
    CHECK(sysLines[1] == "You are already in a full group");
    CHECK(sysLines[2] == "You cannot use a meeting stone while in a raid group");

    // 3. SMSG_LFG_PENDING_INVITE / SMSG_LFG_PENDING_MATCH + CMSG_ACCEPT/DECLINE_LFG_MATCH
    events.clear();
    {
        Packet inv(wireOpcode(Opcode::SMSG_LFG_PENDING_INVITE));
        inv.writeUInt32(1581);
        s.receive(inv);
    }
    REQUIRE(events.size() == 1);
    CHECK(events[0].name == "LFG_PENDING_REQUEST");
    CHECK(gh.tbcLfg().pendingInviteEntry == 1581u);

    // 0-byte SMSG_LFG_PENDING_INVITE (CMaNGOS PendingMatchDone) cancels pending popup
    events.clear();
    {
        Packet done(wireOpcode(Opcode::SMSG_LFG_PENDING_INVITE));
        s.receive(done);
    }
    REQUIRE(events.size() == 1);
    CHECK(events[0].name == "LFG_PENDING_CANCEL");
    CHECK(gh.tbcLfg().pendingInviteEntry == 0u);

    events.clear();
    {
        Packet match(wireOpcode(Opcode::SMSG_LFG_PENDING_MATCH));
        match.writeUInt32(1581);
        s.receive(match);
    }
    REQUIRE(events.size() == 1);
    CHECK(events[0].name == "LFG_MATCH_REQUEST");
    REQUIRE(events[0].args.size() == 1);
    CHECK(events[0].args[0] == "The Deadmines");

    lua_State* L = makeLua(gh);
    REQUIRE(luaL_dostring(L, "AcceptLFGMatch()") == 0);
    CHECK(s.sent().of(Opcode::CMSG_ACCEPT_LFG_MATCH).size() == 1);
    REQUIRE(luaL_dostring(L, "DeclineLFGMatch()") == 0);
    CHECK(s.sent().of(Opcode::CMSG_DECLINE_LFG_MATCH).size() == 1);
    REQUIRE(luaL_dostring(L, "CancelPendingLFG()") == 0);
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_PENDING_LFG).size() == 1);
    lua_close(L);
}

TEST_CASE("TBC Groups: MSG_PARTY_ASSIGNMENT, SMSG_GROUP_LIST battleGroup/guid, SMSG_PARTY_MEMBER_STATS pet stats & auras",
          "[replay][shard4][groups]") {
    Session s("tbc");
    auto& gh = s.handler();
    GameHandlerTestAccess::setNoticesInterfaceReady(gh, true);

    constexpr uint64_t kMateGuid = 0x0000000000000222ull;
    constexpr uint64_t kPetGuid  = 0xF140001042000777ull;
    constexpr uint64_t kGroupGuid = 0x1F50000000000099ull;

    // 1. SMSG_GROUP_LIST stores isBattleGroup and groupGuid
    {
        Packet gl(wireOpcode(Opcode::SMSG_GROUP_LIST));
        gl.writeUInt8(0);          // party
        gl.writeUInt8(1);          // isBattleGroup = true
        gl.writeUInt8(0);          // subgroup
        gl.writeUInt8(0);          // flags
        gl.writeUInt64(kGroupGuid);
        gl.writeUInt32(1);         // 1 other member
        gl.writeString("Warlockmate");
        gl.writeUInt64(kMateGuid);
        gl.writeUInt8(1);          // online
        gl.writeUInt8(0);          // subgroup
        gl.writeUInt8(0x02);       // MEMBER_FLAG_MAINTANK
        gl.writeUInt64(Session::kPlayerGuid);
        gl.writeUInt8(0);          // lootMethod
        gl.writeUInt64(0);         // looterGuid
        gl.writeUInt8(2);          // lootThreshold
        gl.writeUInt8(0);          // difficulty
        s.receive(gl);
    }
    CHECK(gh.getPartyData().isBattleGroup);
    CHECK(gh.getPartyData().groupGuid == kGroupGuid);

    // 2. setMainTank / setMainAssist / clearMainTank send MSG_PARTY_ASSIGNMENT (NOT MSG_RAID_TARGET_UPDATE!)
    const size_t raidTargetCountBefore = s.sent().of(Opcode::MSG_RAID_TARGET_UPDATE).size();
    gh.setMainTank(kMateGuid);
    gh.setMainAssist(kMateGuid);
    gh.clearMainTank(); // resolves kMateGuid from flags & 0x02 when no target is selected!
    CHECK(s.sent().of(Opcode::MSG_RAID_TARGET_UPDATE).size() == raidTargetCountBefore);
    const auto assigns = s.sent().of(Opcode::MSG_PARTY_ASSIGNMENT);
    REQUIRE(assigns.size() == 3);
    {
        Packet p0 = assigns[0];
        CHECK(p0.readUInt8() == 0u); // Main Tank
        CHECK(p0.readUInt8() == 1u); // apply = true
        CHECK(p0.readUInt64() == kMateGuid);

        Packet p1 = assigns[1];
        CHECK(p1.readUInt8() == 1u); // Main Assist
        CHECK(p1.readUInt8() == 1u); // apply = true
        CHECK(p1.readUInt64() == kMateGuid);

        Packet p2 = assigns[2];
        CHECK(p2.readUInt8() == 0u); // Main Tank
        CHECK(p2.readUInt8() == 0u); // apply = false
        CHECK(p2.readUInt64() == kMateGuid);
    }

    // 3. SMSG_PARTY_MEMBER_STATS_FULL with all 9 pet flags (0x0400..0x40000)
    std::vector<RecordedEvent> events;
    gh.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        events.push_back({name, args});
    });
    {
        Packet stats(wireOpcode(Opcode::SMSG_PARTY_MEMBER_STATS_FULL));
        writePackedGuid(stats, kMateGuid);
        constexpr uint32_t kPetFlags = 0x0001u | 0x0400u | 0x0800u | 0x1000u | 0x2000u |
                                       0x4000u | 0x8000u | 0x10000u | 0x20000u | 0x40000u;
        stats.writeUInt32(kPetFlags);
        stats.writeUInt16(0x0001); // online
        stats.writeUInt64(kPetGuid);
        stats.writeString("Jubjub");
        stats.writeUInt16(4449);   // petDisplayId
        stats.writeUInt16(1850);   // petCurHp (uint16 on TBC)
        stats.writeUInt16(2400);   // petMaxHp (uint16 on TBC)
        stats.writeUInt8(0);       // petPowerType = Mana
        stats.writeUInt16(900);    // petCurPower
        stats.writeUInt16(1200);   // petMaxPower
        stats.writeUInt64((1ull << 0) | (1ull << 40)); // slot 0 (buff) + slot 40 (TBC debuff)
        stats.writeUInt16(6307);   // Blood Pact
        stats.writeUInt8(0x09);    // positive flags
        stats.writeUInt16(172);    // Corruption
        stats.writeUInt8(0x00);    // negative slot >= 40
        s.receive(stats);
    }

    REQUIRE(gh.getPartyData().members.size() == 1);
    const auto& m = gh.getPartyData().members[0];
    CHECK(m.petGuid == kPetGuid);
    CHECK(m.petName == "Jubjub");
    CHECK(m.petDisplayId == 4449u);
    CHECK(m.petCurHealth == 1850u);
    CHECK(m.petMaxHealth == 2400u);
    CHECK(m.petPowerType == 0u);
    CHECK(m.petCurPower == 900u);
    CHECK(m.petMaxPower == 1200u);
    const auto* petAuras = gh.getUnitAuras(kPetGuid);
    REQUIRE(petAuras != nullptr);
    REQUIRE(petAuras->size() >= 41);
    CHECK((*petAuras)[0].spellId == 6307u);
    CHECK((*petAuras)[40].spellId == 172u);
    CHECK(((*petAuras)[40].flags & 0x80u) != 0);

    lua_State* L = makeLua(gh);
    CHECK(evalLua(L, "UnitExists('partypet1')") == "true");
    CHECK(evalLua(L, "UnitExists('party1pet')") == "true");
    CHECK(evalLua(L, "UnitName('partypet1')") == "Jubjub");
    CHECK(evalLua(L, "UnitHealth('partypet1')") == "1850");
    CHECK(evalLua(L, "UnitHealthMax('partypet1')") == "2400");
    CHECK(evalLua(L, "UnitMana('partypet1')") == "900");
    CHECK(evalLua(L, "UnitManaMax('partypet1')") == "1200");
    CHECK(evalLua(L, "UnitPowerType('partypet1')") == "0,MANA");
    CHECK(evalLua(L, "UnitIsDead('partypet1')") == "false");
    CHECK(evalLua(L, "UnitInBattleground('player')") == "1");
    lua_close(L);
}

TEST_CASE("TBC Social & Chat: SMSG_CONTACT_LIST mutes, SMSG_FRIEND_STATUS cache/mutes, SMSG_CHAT_RESTRICTED, SMSG_CHANNEL_NOTIFY",
          "[replay][shard4][social][chat]") {
    Session s("tbc");
    auto& gh = s.handler();
    gh.setGlobalStringLookup(shard4GlobalString);
    GameHandlerTestAccess::setNoticesInterfaceReady(gh, true);

    std::vector<RecordedEvent> events;
    gh.setAddonEventCallback([&](const std::string& name, const std::vector<std::string>& args) {
        events.push_back({name, args});
    });

    constexpr uint64_t kFriendGuid = 0x0000000000000301ull;
    constexpr uint64_t kMuteGuid   = 0x0000000000000302ull;
    s.receive(replay::nameResponse(kFriendGuid, "Palbuddy"));
    s.receive(replay::nameResponse(kMuteGuid, "Loudmouth"));

    // 1. SMSG_CONTACT_LIST with mask 0x7 (friends + ignores + mutes) fires MUTELIST_UPDATE
    {
        Packet cl(wireOpcode(Opcode::SMSG_CONTACT_LIST));
        cl.writeUInt32(0x7); // mask includes SOCIAL_FLAG_MUTED (0x4)
        cl.writeUInt32(1);   // 1 entry
        cl.writeUInt64(kMuteGuid);
        cl.writeUInt32(0x4); // SOCIAL_FLAG_MUTED
        cl.writeString("");
        s.receive(cl);
    }
    bool sawMuteListUpdate = false;
    for (const auto& ev : events) {
        if (ev.name == "MUTELIST_UPDATE") sawMuteListUpdate = true;
    }
    CHECK(sawMuteListUpdate);

    lua_State* L = makeLua(gh);
    CHECK(evalLua(L, "GetNumMutes()") == "1");
    CHECK(evalLua(L, "GetMuteName(1)") == "Loudmouth");
    CHECK(evalLua(L, "IsMuted('Loudmouth')") == "true");
    REQUIRE(luaL_dostring(L, "SetSelectedMute(1)") == 0);
    CHECK(evalLua(L, "GetSelectedMute()") == "1");

    // 2. SMSG_FRIEND_STATUS: FRIEND_ADDED_OFFLINE (7) inserts friend; FRIEND_LIST_FULL (1) & FRIEND_DB_ERROR (0)
    //    do NOT corrupt friendsCache; FRIEND_REMOVED (5) removes friend; FRIEND_MUTE_REMOVED (0x17) removes mute.
    {
        Packet addF(wireOpcode(Opcode::SMSG_FRIEND_STATUS));
        addF.writeUInt8(7); // FRIEND_ADDED_OFFLINE
        addF.writeUInt64(kFriendGuid);
        addF.writeString("raid healer");
        s.receive(addF);
    }
    CHECK(evalLua(L, "GetNumFriends()") == "1,0");

    {
        Packet dbErr(wireOpcode(Opcode::SMSG_FRIEND_STATUS));
        dbErr.writeUInt8(0); // FRIEND_DB_ERROR must NOT erase Palbuddy!
        dbErr.writeUInt64(kFriendGuid);
        s.receive(dbErr);
    }
    CHECK(evalLua(L, "GetNumFriends()") == "1,0");

    {
        Packet remF(wireOpcode(Opcode::SMSG_FRIEND_STATUS));
        remF.writeUInt8(5); // FRIEND_REMOVED
        remF.writeUInt64(kFriendGuid);
        s.receive(remF);
    }
    CHECK(evalLua(L, "GetNumFriends()") == "0,0");

    {
        Packet unmute(wireOpcode(Opcode::SMSG_FRIEND_STATUS));
        unmute.writeUInt8(0x17); // FRIEND_MUTE_REMOVED
        unmute.writeUInt64(kMuteGuid);
        s.receive(unmute);
    }
    CHECK(evalLua(L, "GetNumMutes()") == "0");
    CHECK(evalLua(L, "IsMuted('Loudmouth')") == "false");

    // 3. SMSG_CHAT_RESTRICTED dispatches ChatRestrictionType 0, 1, 2
    events.clear();
    for (uint8_t r : {uint8_t(0), uint8_t(1), uint8_t(2)}) {
        Packet p(wireOpcode(Opcode::SMSG_CHAT_RESTRICTED));
        p.writeUInt8(r);
        s.receive(p);
    }
    std::vector<std::string> restrictedLines;
    for (const auto& ev : events) {
        if (ev.name == "CHAT_MSG_SYSTEM" && !ev.args.empty()) restrictedLines.push_back(ev.args[0]);
    }
    REQUIRE(restrictedLines.size() == 3);
    CHECK(restrictedLines[0] == "Trial accounts cannot send unlimited tells.");
    CHECK(restrictedLines[1] == "The number of messages that can be sent is limited.");
    CHECK(restrictedLines[2] == "Your chat and mail privileges have been temporarily suspended.");

    // 4. SMSG_CHANNEL_NOTIFY: YOU_JOINED -> PLAYER_JOINED -> MODE_CHANGE (+mod) -> YOU_LEFT (suspended=1)
    {
        Packet yj(wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
        yj.writeUInt8(0x02); // YOU_JOINED
        yj.writeString("RaidPrep");
        yj.writeUInt8(0);
        yj.writeUInt32(0);
        s.receive(yj);
    }
    events.clear();
    {
        Packet pj(wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
        pj.writeUInt8(0x00); // CHAT_JOINED_NOTICE
        pj.writeString("RaidPrep");
        pj.writeUInt64(kFriendGuid);
        s.receive(pj);
    }
    REQUIRE(events.size() == 1);
    CHECK(events[0].name == "CHAT_MSG_CHANNEL_JOIN");
    CHECK(events[0].args[1] == "Palbuddy");
    CHECK(events[0].args[3] == "1. RaidPrep");

    events.clear();
    {
        Packet mc(wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
        mc.writeUInt8(0x0C); // CHAT_MODE_CHANGE_NOTICE
        mc.writeString("RaidPrep");
        mc.writeUInt64(kFriendGuid);
        mc.writeUInt8(0x00); // oldFlags
        mc.writeUInt8(0x02); // newFlags = MEMBER_FLAG_MODERATOR
        s.receive(mc);
    }
    bool sawSetMod = false;
    for (const auto& ev : events) {
        if (ev.name == "CHAT_MSG_CHANNEL_NOTICE_USER" && !ev.args.empty() && ev.args[0] == "SET_MODERATOR") {
            CHECK(ev.args[1] == "Palbuddy");
            sawSetMod = true;
        }
    }
    CHECK(sawSetMod);

    events.clear();
    {
        Packet yl(wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
        yl.writeUInt8(0x03); // CHAT_YOU_LEFT_NOTICE
        yl.writeString("RaidPrep");
        yl.writeUInt32(0);   // channelId
        yl.writeUInt8(1);    // suspended = 1 -> CHAT_SUSPENDED_NOTICE
        s.receive(yl);
    }
    bool sawSuspended = false;
    for (const auto& ev : events) {
        if (ev.name == "CHAT_MSG_CHANNEL_NOTICE" && !ev.args.empty() && ev.args[0] == "SUSPENDED") {
            sawSuspended = true;
        }
    }
    CHECK(sawSuspended);

    lua_close(L);
}

TEST_CASE("WotLK GM ticket backward compatibility preserved", "[replay][shard4][gmticket][wotlk]") {
    Session s("wotlk");
    auto& gh = s.handler();
    gh.setPosition(10.0f, 20.0f, 30.0f);
    gh.submitGmTicket("WotLK ticket");
    const auto creates = s.sent().of(Opcode::CMSG_GMTICKET_CREATE);
    REQUIRE(creates.size() == 1);
    {
        Packet body = creates[0];
        body.readUInt32();
        body.readFloat(); body.readFloat(); body.readFloat();
        CHECK(body.readString() == "WotLK ticket");
        CHECK(body.readUInt32() == 1u); // needResponse
        CHECK(body.readUInt8() == 0u);  // WotLK needMoreHelp byte preserved
        CHECK(body.readUInt32() == 0u);
        CHECK(body.readUInt32() == 0u);
        CHECK(body.getRemainingSize() == 0u);
    }

    Packet getPkt(wireOpcode(Opcode::SMSG_GMTICKET_GETTICKET));
    getPkt.writeUInt8(6); // WotLK uint8 status
    getPkt.writeString("WotLK open ticket");
    getPkt.writeUInt32(120); // ageSec
    getPkt.writeUInt32(2);   // daysLeft
    getPkt.writeFloat(1.5f); // waitTimeHours
    s.receive(getPkt);
    CHECK(gh.hasActiveGmTicket());
    CHECK(gh.getGmTicketText() == "WotLK open ticket");
    CHECK(gh.getGmTicketWaitHours() == Catch::Approx(1.5f));
}

