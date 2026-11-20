// SMSG_BATTLEFIELD_STATUS's header, which was read two bytes short.
//
// The server writes a stretch of this packet as one uint64 and says so in a
// comment, which is what makes it easy to read as fewer, wider fields than it
// holds:
//
//     uint32 queueSlot, uint8 arenaType, uint8 isArena, uint32 bgTypeId,
//     uint16 0x1F90, uint8 minLevel, uint8 maxLevel, uint32 instanceId,
//     uint8 isRated, uint32 statusId
//
// Read as `uint32 instanceId, uint8 isRated` - five bytes where the server
// sends seven - the status arrives as the top of the instance id joined to the
// bottom of the status, and is never one of the values it is compared against.
// The packet is the same length either way, so nothing runs short and nothing
// reports anything: a queue that exists reads as no queue at all, which is a
// battleground queue with no tracker, no chat line and no invitation.
#include <catch_amalgamated.hpp>
#include "game/world_packets.hpp"
#include "game/bg_score_defs.hpp"
#include "game/handler_types.hpp"
#include "core/application.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

// The bytes AzerothCore's BattlegroundMgr::BuildBattlegroundStatusPacket puts
// on the wire, in its order. Built here rather than asserted against our own
// writer so the test fails if we drift from the server rather than from
// ourselves.
std::vector<uint8_t> serverStatusBytes(uint32_t queueSlot, uint8_t arenaType,
                                       uint32_t bgTypeId, uint8_t minLevel,
                                       uint8_t maxLevel, uint32_t instanceId,
                                       bool isRated, uint32_t statusId,
                                       const std::vector<uint32_t>& tail) {
    std::vector<uint8_t> b;
    auto u32 = [&](uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    };
    auto u16 = [&](uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); };

    u32(queueSlot);
    b.push_back(arenaType);
    b.push_back(arenaType ? 0x0E : 0x00);
    u32(bgTypeId);
    u16(0x1F90);
    b.push_back(minLevel);
    b.push_back(maxLevel);
    u32(instanceId);
    b.push_back(isRated ? 1 : 0);
    u32(statusId);
    for (uint32_t v : tail) u32(v);
    return b;
}

wowee::network::Packet packetOf(const std::vector<uint8_t>& body) {
    return wowee::network::Packet(0x2D4, body);  // SMSG_BATTLEFIELD_STATUS, 3.3.5a
}

}  // namespace

TEST_CASE("SMSG_BATTLEFIELD_STATUS reads the status the server sent",
          "[battleground][packet]") {
    SECTION("queued for Warsong Gulch") {
        // Time1 is the average wait, Time2 the time already spent in queue.
        auto p = packetOf(serverStatusBytes(0, 0, 2, 10, 19, 1, false, 1,
                                            {120000, 65000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.queueSlot == 0u);
        CHECK(d.bgTypeId == 2u);
        CHECK(d.statusId == 1u);          // read two bytes early this was 65536
        CHECK(d.minLevel == 10);
        CHECK(d.maxLevel == 19);
        CHECK(d.instanceId == 1u);
        CHECK(d.avgWaitMs == 120000u);
        CHECK(d.timeInQueueMs == 65000u);
    }

    SECTION("an instance id large enough to reach the status") {
        // The misread joined the instance id's top byte to the status's bottom
        // half, so an instance id in the millions moved the status as well.
        auto p = packetOf(serverStatusBytes(1, 0, 30, 71, 80, 0x01020304, false, 1,
                                            {1000, 2000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.statusId == 1u);
        CHECK(d.instanceId == 0x01020304u);
        CHECK(d.minLevel == 71);
        CHECK(d.maxLevel == 80);
    }

    SECTION("an invitation carries a map id before its timeout") {
        // status 2: uint32 mapId, uint64 unused, uint32 time to remove from
        // queue. The timeout used to be read from the map id.
        auto p = packetOf(serverStatusBytes(2, 0, 3, 10, 19, 5, false, 2,
                                            {529, 0, 0, 80000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.statusId == 2u);
        CHECK(d.mapId == 529u);
        CHECK(d.inviteTimeoutMs == 80000u);
    }

    SECTION("before Wrath the invitation has no eight unused bytes") {
        // CMaNGOS TBC header has no min/max level bytes.
        auto bytes = serverStatusBytes(0, 0, 3, 10, 19, 5, false, 2, {529, 80000});
        bytes.erase(bytes.begin() + 12, bytes.begin() + 14);
        auto p = packetOf(bytes);
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d, /*classicFormat=*/false,
                                               /*wotlkFormat=*/false));
        CHECK(d.mapId == 529u);
        CHECK(d.inviteTimeoutMs == 80000u);
    }

    SECTION("TBC live Arathi queue packet retains queued status") {
        auto p = packetOf({0,0,0,0, 0,13,3,0,0,0,144,31,
                           0,0,0,0, 0, 1,0,0,0, 0,0,0,0, 0,0,0,0});
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d, false, false));
        CHECK(d.bgTypeId == 3);
        CHECK(d.statusId == 1);
        CHECK(d.instanceId == 0);
    }

    SECTION("TBC in progress carries time to removal and time since start") {
        // CMaNGOS TBC STATUS_IN_PROGRESS: mapId, Time1 (auto leave), Time2
        // (since start), uint8 arena team.
        auto p = packetOf({0,0,0,0, 0,13,2,0,0,0,144,31,
                           7,0,0,0, 0, 3,0,0,0,
                           0xE9,0x01,0,0,  0xC0,0xD4,0x01,0,  0x10,0x27,0,0,  1});
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d, false, false));
        CHECK(d.statusId == 3u);
        CHECK(d.mapId == 489u);
        CHECK(d.autoLeaveMs == 120000u);
        CHECK(d.runTimeMs == 10000u);
    }

    SECTION("TBC zero identifier clears queue without status tail") {
        auto p = packetOf({1,0,0,0, 0,0,0,0,0,0,0,0});
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d, false, false));
        CHECK(d.queueSlot == 1);
        CHECK(d.statusId == 0);
    }

    SECTION("an arena carries its team size") {
        auto p = packetOf(serverStatusBytes(0, 3, 4, 70, 80, 2, true, 1,
                                            {30000, 1000}));
        BattlefieldStatusData d;
        REQUIRE(BattlefieldStatusPacket::parse(p, d));
        CHECK(d.arenaType == 3);
        CHECK(d.isRated);
        CHECK(d.statusId == 1u);
    }

    SECTION("a truncated packet is refused rather than half read") {
        std::vector<uint8_t> body = serverStatusBytes(0, 0, 2, 10, 19, 1, false, 1, {});
        body.resize(10);
        auto p = packetOf(body);
        BattlefieldStatusData d;
        CHECK_FALSE(BattlefieldStatusPacket::parse(p, d));
    }
}

TEST_CASE("MSG_PVP_LOG_DATA parses TBC 2.4.3 Arena and EotS layouts",
          "[battleground][arena][packet]") {
    auto pushU32 = [](std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24));
    };
    auto pushU64 = [&](std::vector<uint8_t>& b, uint64_t v) {
        pushU32(b, uint32_t(v)); pushU32(b, uint32_t(v >> 32));
    };
    auto pushStr = [](std::vector<uint8_t>& b, const char* s) {
        while (*s) b.push_back(uint8_t(*s++));
        b.push_back(0);
    };

    SECTION("TBC 2.4.3 Arena Skirmish parses 4 uint32s + 2 team names and player teams") {
        std::vector<uint8_t> body;
        body.push_back(1);            // isArena = 1
        pushU32(body, 1500);          // team1 (Gold) oldRating
        pushU32(body, 1516);          // team1 (Gold) newRating
        pushU32(body, 1500);          // team0 (Green) oldRating
        pushU32(body, 1484);          // team0 (Green) newRating
        pushStr(body, "GoldTeam");    // team1 name
        pushStr(body, "GreenTeam");   // team0 name
        body.push_back(1);            // hasWinner = 1
        body.push_back(1);            // winner = 1 (Gold)
        pushU32(body, 2);             // playerCount = 2
        // Player 1: Gold (team=1)
        pushU64(body, 0x33);
        pushU32(body, 1);             // killingBlows
        body.push_back(1);            // team = 1 (Gold)
        pushU32(body, 4200);          // damageDone
        pushU32(body, 500);           // healingDone
        pushU32(body, 0);             // statCount = 0
        // Player 2: Green (team=0)
        pushU64(body, 0x34);
        pushU32(body, 0);             // killingBlows
        body.push_back(0);            // team = 0 (Green)
        pushU32(body, 1800);          // damageDone
        pushU32(body, 0);             // healingDone
        pushU32(body, 0);             // statCount = 0

        wowee::network::Packet pkt(0x2E0, body);
        BgScoreboardData sb;
        REQUIRE(PvpLogDataPacket::parse(pkt, sb, /*wotlkFormat=*/false));
        CHECK(sb.isArena);
        CHECK(sb.hasWinner);
        CHECK(sb.winner == 1);
        CHECK(sb.arenaTeams[1].teamName == "GoldTeam");
        CHECK(sb.arenaTeams[1].oldRating == 1500);
        CHECK(sb.arenaTeams[1].newRating == 1516);
        CHECK(sb.arenaTeams[1].ratingChange == 16);
        CHECK(sb.arenaTeams[0].teamName == "GreenTeam");
        CHECK(sb.arenaTeams[0].oldRating == 1500);
        CHECK(sb.arenaTeams[0].newRating == 1484);
        CHECK(sb.arenaTeams[0].ratingChange == -16);
        REQUIRE(sb.players.size() == 2);
        CHECK(sb.players[0].guid == 0x33u);
        CHECK(sb.players[0].hasTeam);
        CHECK(sb.players[0].team == 1);
        CHECK(sb.players[0].killingBlows == 1u);
        CHECK(sb.players[0].damageDone == 4200u);
        CHECK(sb.players[0].healingDone == 500u);
        CHECK(sb.players[1].guid == 0x34u);
        CHECK(sb.players[1].team == 0);
        CHECK(sb.players[1].damageDone == 1800u);
    }

    SECTION("TBC EotS battleground row carries 1 flag capture objective stat") {
        std::vector<uint8_t> body;
        body.push_back(0);            // isArena = 0
        body.push_back(0);            // hasWinner = 0
        pushU32(body, 1);             // playerCount = 1
        pushU64(body, 0x33);
        pushU32(body, 2);             // killingBlows
        pushU32(body, 5);             // honorableKills
        pushU32(body, 1);             // deaths
        pushU32(body, 42);            // bonusHonor
        pushU32(body, 9000);          // damageDone
        pushU32(body, 1200);          // healingDone
        pushU32(body, 1);             // statCount = 1 (Flag Captures)
        pushU32(body, 3);             // flagCaptures = 3

        wowee::network::Packet pkt(0x2E0, body);
        BgScoreboardData sb;
        REQUIRE(PvpLogDataPacket::parse(pkt, sb, /*wotlkFormat=*/false));
        CHECK_FALSE(sb.isArena);
        CHECK_FALSE(sb.hasWinner);
        REQUIRE(sb.players.size() == 1);
        CHECK(sb.players[0].honorableKills == 5u);
        CHECK(sb.players[0].bonusHonor == 42u);
        REQUIRE(sb.players[0].bgStats.size() == 1);
        CHECK(sb.players[0].bgStats[0].second == 3u);
    }

    SECTION("TBC Arathi Basin victory row carries Bases Assaulted and Bases Defended") {
        std::vector<uint8_t> body;
        body.push_back(0);            // isArena = 0
        body.push_back(1);            // hasWinner = 1
        body.push_back(1);            // winner = 1 (Alliance)
        pushU32(body, 1);             // playerCount = 1
        pushU64(body, 0x2f);
        pushU32(body, 0);             // killingBlows
        pushU32(body, 0);             // honorableKills
        pushU32(body, 0);             // deaths
        pushU32(body, 189);           // bonusHonor
        pushU32(body, 0);             // damageDone
        pushU32(body, 0);             // healingDone
        pushU32(body, 2);             // statCount = 2 (Bases Assaulted, Bases Defended)
        pushU32(body, 4);             // basesAssaulted = 4
        pushU32(body, 1);             // basesDefended = 1

        wowee::network::Packet pkt(0x2E0, body);
        BgScoreboardData sb;
        REQUIRE(PvpLogDataPacket::parse(pkt, sb, /*wotlkFormat=*/false));
        CHECK_FALSE(sb.isArena);
        CHECK(sb.hasWinner);
        CHECK(sb.winner == 1);
        REQUIRE(sb.players.size() == 1);
        CHECK(sb.players[0].bonusHonor == 189u);
        REQUIRE(sb.players[0].bgStats.size() == 2);
        CHECK(sb.players[0].bgStats[0].second == 4u);
        CHECK(sb.players[0].bgStats[1].second == 1u);
    }

    SECTION("CMSG_BATTLEMASTER_JOIN_ARENA encodes guid, slot, asGroup, isRated") {
        auto pkt = BattlemasterJoinArenaPacket::build(0xF130004DC7596DC1ULL, 0, false, false);
        REQUIRE(pkt.getSize() == 11);
        CHECK(pkt.readUInt64() == 0xF130004DC7596DC1ULL);
        CHECK(pkt.readUInt8() == 0);
        CHECK(pkt.readUInt8() == 0);
        CHECK(pkt.readUInt8() == 0);
    }
}

TEST_CASE("SMSG_MESSAGECHAT parses BG_SYSTEM_NEUTRAL/ALLIANCE/HORDE with targetGuid",
          "[battleground][chat][packet]") {
    auto buildBgChat = [](ChatType type, const std::string& text) {
        wowee::network::Packet pkt(0x096);
        pkt.writeUInt8(static_cast<uint8_t>(type));
        pkt.writeUInt32(0); // LANG_UNIVERSAL
        pkt.writeUInt64(0); // senderGuid
        pkt.writeUInt32(0); // unknown/chatFlags (TBC/WotLK)
        pkt.writeUInt64(0); // targetGuid (mangos-tbc Chat.cpp:3723)
        pkt.writeUInt32(static_cast<uint32_t>(text.size() + 1));
        for (char c : text) pkt.writeUInt8(static_cast<uint8_t>(c));
        pkt.writeUInt8(0);
        pkt.writeUInt8(0); // chatTag
        return pkt;
    };

    for (ChatType t : {ChatType::BG_SYSTEM_NEUTRAL, ChatType::BG_SYSTEM_ALLIANCE, ChatType::BG_SYSTEM_HORDE}) {
        auto pkt = buildBgChat(t, "The battle for Alterac Valley has begun!");
        MessageChatData d;
        REQUIRE(MessageChatParser::parse(pkt, d));
        CHECK(d.type == t);
        CHECK(d.message == "The battle for Alterac Valley has begun!");
    }
}

TEST_CASE("bgFactionFromRace maps TBC Alliance and Horde races", "[battleground]") {
    for (uint8_t r : {1, 3, 4, 7, 11}) CHECK(bgFactionFromRace(r) == 1);
    for (uint8_t r : {2, 5, 6, 8, 10}) CHECK(bgFactionFromRace(r) == 0);
    CHECK(bgFactionFromRace(0) == -1);
}

TEST_CASE("substituteBgSystemPlayerToken expands $n and $N in BG system announcements",
          "[battleground][chat][ab]") {
    std::string msg = "$n claims the stables! If left unchallenged, the Alliance will control it in 1 minute!";
    substituteBgSystemPlayerToken(msg, "Edricp");
    CHECK(msg == "Edricp claims the stables! If left unchallenged, the Alliance will control it in 1 minute!");

    std::string upper = "$N has assaulted the blacksmith!";
    substituteBgSystemPlayerToken(upper, "Edrikp");
    CHECK(upper == "Edrikp has assaulted the blacksmith!");

    std::string unchanged = "The Battle for Arathi Basin has begun!";
    substituteBgSystemPlayerToken(unchanged, "Edricp");
    CHECK(unchanged == "The Battle for Arathi Basin has begun!");
}
