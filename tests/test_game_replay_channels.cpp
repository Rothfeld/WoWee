// Chat channel numbers and the channel arguments of chat events, through
// GameHandler's own dispatch. See game_replay.hpp.
//
// Measured on 2.4.3 (build 8606) against CMaNGOS-TBC on pair 15:
// - a channel left gives up its number and the others keep theirs: after
//   "4. GuildRecruitment - Tanaris" is left, LookingForGroup stays 5, and the
//   next channel joined ("zzb", or GuildRecruitment again) takes 4.
// - GetChannelList skips a free number; GetChannelName answers 0, nil for it.
// - CHAT_MSG_CHANNEL: arg4 "1. General - Tanaris", arg7 the ChatChannels id
//   (0 for a custom channel), arg8 the number, arg9 the full name.
// Evidence: _experiments/20261119T093133Z.chat_channels/ in the workspace root.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "addons/lua_api_registrations.hpp"

#include <cstdint>
#include <string>
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

// CMaNGOS Channel::MakeYouJoined / MakeYouLeft.
Packet youJoined(const std::string& name) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
    p.writeUInt8(0x02);
    p.writeString(name);
    p.writeUInt8(0);   // channel flags
    p.writeUInt32(0);  // ChatChannels id: a custom channel
    return p;
}

Packet youLeft(const std::string& name) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_CHANNEL_NOTIFY));
    p.writeUInt8(0x03);
    p.writeString(name);
    p.writeUInt32(0);
    return p;
}

// CMaNGOS ChatHandler::BuildChatPacket for CHAT_MSG_CHANNEL.
Packet channelLine(const std::string& channel, uint64_t sender, const std::string& message) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_MESSAGECHAT));
    p.writeUInt8(static_cast<uint8_t>(wowee::game::ChatType::CHANNEL));
    p.writeUInt32(7);  // LANG_COMMON
    p.writeUInt64(sender);
    p.writeUInt32(0);
    p.writeString(channel);
    p.writeUInt64(0);
    p.writeUInt32(static_cast<uint32_t>(message.size() + 1));
    p.writeString(message);
    p.writeUInt8(0);
    return p;
}

// CMaNGOS-TBC CharacterNameQueryResponse: guid, name, realm byte, race,
// gender, class as uint32s, no declined names.
Packet nameResponse(uint64_t guid, const std::string& name) {
    Packet p(wowee::game::wireOpcode(Opcode::SMSG_NAME_QUERY_RESPONSE));
    p.writeUInt64(guid);
    p.writeString(name);
    p.writeUInt8(0);
    p.writeUInt32(1);
    p.writeUInt32(0);
    p.writeUInt32(4);
    p.writeUInt8(0);
    return p;
}

std::string luaAnswer(lua_State* L, const std::string& chunk) {
    const std::string code = "local out = {} local r = {" + chunk +
        "} for i = 1, table.maxn(r) do out[i] = tostring(r[i]) end return table.concat(out, ',')";
    const int rc = luaL_dostring(L, code.c_str());
    INFO(chunk << ": " << (rc ? lua_tostring(L, -1) : ""));
    REQUIRE(rc == 0);
    std::string out = lua_tostring(L, -1);
    lua_pop(L, 1);
    return out;
}

}  // namespace

TEST_CASE("Channel numbers stay put when a channel is left", "[replay][social][channels]") {
    Session s("tbc");
    std::vector<std::vector<std::string>> lines;
    s.handler().setAddonEventCallback([&](const std::string& e, const std::vector<std::string>& args) {
        if (e == "CHAT_MSG_CHANNEL") lines.push_back(args);
    });
    lua_State* L = luaL_newstate();
    luaopen_base(L);
    luaopen_table(L);
    lua_pushlightuserdata(L, &s.handler());
    lua_setfield(L, LUA_REGISTRYINDEX, "wowee_game_handler");
    wowee::addons::registerSocialLuaAPI(L);

    for (const char* name : {"aa", "bb", "cc"}) s.receive(youJoined(name));
    CHECK(luaAnswer(L, "GetChannelList()") == "1,aa,2,bb,3,cc");

    s.receive(youLeft("bb"));
    CHECK(luaAnswer(L, "GetChannelList()") == "1,aa,3,cc");
    CHECK(luaAnswer(L, "GetChannelName(2)") == "0,nil,0");
    CHECK(luaAnswer(L, "GetChannelName(3)") == "3,cc,0");

    s.receive(youJoined("dd"));
    CHECK(luaAnswer(L, "GetChannelList()") == "1,aa,2,dd,3,cc");

    // The last one left leaves no free number behind it.
    s.receive(youLeft("cc"));
    s.receive(youJoined("ee"));
    CHECK(luaAnswer(L, "GetChannelList()") == "1,aa,2,dd,3,ee");

    s.receive(nameResponse(77, "Edriko"));
    s.receive(channelLine("dd", 77, "hello"));
    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].size() >= 9);
    CHECK(lines[0][0] == "hello");
    CHECK(lines[0][3] == "2. dd");
    CHECK(lines[0][6] == "0");
    CHECK(lines[0][7] == "2");
    CHECK(lines[0][8] == "dd");
    lua_close(L);
}
