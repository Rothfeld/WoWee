// TBC 2.4.3 Shard 1 end-to-end GameHandler replay tests:
// - Multi-hop CMSG_ACTIVATETAXIEXPRESS includes uint32 totalCost on TBC
// - SMSG_LEVELUP_INFO reads 5 TBC power deltas before 5 stat deltas
// - SMSG_LOG_XPGAIN kill XP formats rested/group/RaF bonuses accurately
// - CMSG_SELL_ITEM writes uint8 count on TBC (17 bytes) vs uint32 on WotLK (20 bytes)
// - CMSG_PLAYED_TIME (0 bytes on TBC) & SMSG_PLAYED_TIME (8 bytes on TBC) silent vs /played
// - TBC handleOtherPlayerMovement pitch check & MOVEFLAG_SPLINE_ELEVATION before knockback
// - MSG_MOVE_SET_*_SPEED & SMSG_SPLINE_SET_{SWIM,FLIGHT}_*_SPEED remote speed/pos updates
// - SMSG_SPLINE_MOVE_{FEATHER,NORMAL}_FALL & {WATER,LAND}_WALK state updates
// - SMSG_FLIGHT_SPLINE_SYNC (12-byte TBC float + uint64 guid) cyclic spline sync
// - SMSG_UPDATE_INSTANCE_OWNERSHIP state & UPDATE_INSTANCE_INFO event
// - CMSG_ZONEUPDATE deduplication & reset on SMSG_NEW_WORLD
// - CMSG_MOVE_SPLINE_DONE on player server spline / taxi landing
// - CMSG_MOVE_SET_FLY on takeoff and landing when CAN_FLY is active
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "core/coordinates.hpp"
#include "game/world_packets.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::GameHandlerTestAccess;
using wowee::game::MovementFlags;
using wowee::game::MovementHandler;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::SellItemPacket;
using wowee::game::UF;
using wowee::network::Packet;

namespace {

uint32_t u32At(const Packet& p, size_t offset) {
    const auto& d = p.getData();
    return static_cast<uint32_t>(d[offset]) |
           (static_cast<uint32_t>(d[offset + 1]) << 8) |
           (static_cast<uint32_t>(d[offset + 2]) << 16) |
           (static_cast<uint32_t>(d[offset + 3]) << 24);
}

void spawnPlayer(Session& s) {
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}},
        true));
}

} // namespace

TEST_CASE("TBC multi-hop activateTaxi sends CMSG_ACTIVATETAXIEXPRESS with uint32 totalCost",
          "[game_replay][taxi][tbc][shard1]") {
    Session s("tbc");
    spawnPlayer(s);

    std::unordered_map<uint32_t, MovementHandler::TaxiNode> nodes;
    nodes[2]  = {.id = 2,  .mapId = 0, .x = -8830.0f, .y = 622.0f,   .z = 94.0f, .name = "Stormwind", .mountDisplayIdAlliance = 541};
    nodes[4]  = {.id = 4,  .mapId = 0, .x = -10628.0f,.y = 1037.0f,  .z = 34.0f, .name = "Sentinel Hill", .mountDisplayIdAlliance = 541};
    nodes[19] = {.id = 19, .mapId = 0, .x = -14477.0f,.y = 464.0f,   .z = 36.0f, .name = "Booty Bay", .mountDisplayIdAlliance = 541};
    std::vector<MovementHandler::TaxiPathEdge> edges = {
        {10, 2, 4, 50},
        {11, 4, 19, 110},
    };
    GameHandlerTestAccess::setTaxiGraph(s.handler(), std::move(nodes), std::move(edges));

    constexpr uint64_t kFlightMaster = 0xF130000160000001ull;
    const uint32_t mask = (1u << (2 - 1)) | (1u << (4 - 1)) | (1u << (19 - 1));
    Packet show(wowee::game::wireOpcode(Opcode::SMSG_SHOWTAXINODES));
    show.writeUInt32(1);
    show.writeUInt64(kFlightMaster);
    show.writeUInt32(2);
    show.writeUInt32(mask);
    for (int i = 1; i < 8; ++i) show.writeUInt32(0);
    s.receive(show);

    s.handler().activateTaxi(19);
    const auto express = s.sent().of(Opcode::CMSG_ACTIVATETAXIEXPRESS);
    REQUIRE(express.size() == 1);
    // guid(8) + totalCost(4=160) + nodeCount(4=3) + 3 * uint32 nodes = 28 bytes
    REQUIRE(express.front().getSize() == 28);
    CHECK(u32At(express.front(), 8) == 160u);
    CHECK(u32At(express.front(), 12) == 3u);
    CHECK(u32At(express.front(), 16) == 2u);
    CHECK(u32At(express.front(), 20) == 4u);
    CHECK(u32At(express.front(), 24) == 19u);
}

TEST_CASE("TBC SMSG_LEVELUP_INFO reads 5 power deltas before 5 stat deltas",
          "[game_replay][levelup][tbc][shard1]") {
    Session s("tbc");
    spawnPlayer(s);

    std::vector<std::string> levelUpArgs;
    s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>& args) {
        if (ev == "PLAYER_LEVEL_UP") levelUpArgs = args;
    });

    // CMaNGOS-TBC Player::GiveLevel: level + hp + 5 powers (mana, rage, focus, energy, happiness) + 5 stats
    Packet pkt(wowee::game::wireOpcode(Opcode::SMSG_LEVELUP_INFO));
    pkt.writeUInt32(15);  // newLevel
    pkt.writeUInt32(28);  // hpDelta
    pkt.writeUInt32(45);  // manaDelta
    pkt.writeUInt32(0);   // rage
    pkt.writeUInt32(0);   // focus
    pkt.writeUInt32(0);   // energy
    pkt.writeUInt32(0);   // happiness
    pkt.writeUInt32(1);   // str
    pkt.writeUInt32(2);   // agi
    pkt.writeUInt32(3);   // sta
    pkt.writeUInt32(4);   // intel
    pkt.writeUInt32(5);   // spi
    s.receive(pkt);

    const auto& d = s.handler().getLastLevelUpDeltas();
    CHECK(d.hp == 28u);
    CHECK(d.mana == 45u);
    CHECK(d.str == 1u);
    CHECK(d.agi == 2u);
    CHECK(d.sta == 3u);
    CHECK(d.intel == 4u);
    CHECK(d.spi == 5u);

    REQUIRE(levelUpArgs.size() == 9);
    CHECK(levelUpArgs[0] == "15");
    CHECK(levelUpArgs[1] == "28");
    CHECK(levelUpArgs[2] == "45");
    CHECK(levelUpArgs[4] == "1");
    CHECK(levelUpArgs[5] == "2");
    CHECK(levelUpArgs[6] == "3");
    CHECK(levelUpArgs[7] == "4");
    CHECK(levelUpArgs[8] == "5");
}

TEST_CASE("TBC CMSG_SELL_ITEM, CMSG_PLAYED_TIME/SMSG_PLAYED_TIME, and SMSG_LOG_XPGAIN match TBC wire layouts",
          "[game_replay][economy][social][tbc][shard1]") {
    Session s("tbc");
    spawnPlayer(s);

    // 1) CMSG_SELL_ITEM on TBC is 17 bytes (uint8 count)
    auto sellTbc = SellItemPacket::build(0x11ull, 0x22ull, 5u);
    CHECK(sellTbc.getSize() == 17);
    CHECK(sellTbc.getData()[16] == 5u);

    // 2) Silent CMSG_PLAYED_TIME vs explicit /played on TBC (0-byte request, 8-byte response)
    int timePlayedEvents = 0;
    s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>&) {
        if (ev == "TIME_PLAYED_MSG") ++timePlayedEvents;
    });

    auto silentReq = wowee::game::RequestPlayedTimePacket::build(false);
    CHECK(silentReq.getSize() == 0);
    Packet reply1(wowee::game::wireOpcode(Opcode::SMSG_PLAYED_TIME));
    reply1.writeUInt32(7200);
    reply1.writeUInt32(1800);
    s.receive(reply1);
    CHECK(timePlayedEvents == 0);

    s.handler().getSocialHandler()->requestPlayedTime();
    const auto playedSent = s.sent().of(Opcode::CMSG_PLAYED_TIME);
    REQUIRE(playedSent.size() == 1);
    CHECK(playedSent.front().getSize() == 0);

    Packet reply2(wowee::game::wireOpcode(Opcode::SMSG_PLAYED_TIME));
    reply2.writeUInt32(7260);
    reply2.writeUInt32(1860);
    s.receive(reply2);
    CHECK(timePlayedEvents == 1);

    // 3) SMSG_LOG_XPGAIN kill XP with rested bonus on TBC
    Packet xp(wowee::game::wireOpcode(Opcode::SMSG_LOG_XPGAIN));
    xp.writeUInt64(0xF13000002F000001ull);
    xp.writeUInt32(200); // GivenXP + RestXP
    xp.writeUInt8(0);    // kill_xp
    xp.writeUInt32(100); // GivenXP
    xp.writeFloat(1.0f); // solo groupRate
    xp.writeUInt8(0);    // recruitAFriend
    s.receive(xp);
    REQUIRE_FALSE(s.handler().getChatHistory().empty());
    CHECK(s.handler().getChatHistory().back().message.find("Rested bonus") != std::string::npos);

    // WotLK CMSG_SELL_ITEM remains 20 bytes (uint32 count)
    Session sWotlk("wotlk");
    auto sellWotlk = SellItemPacket::build(0x11ull, 0x22ull, 5u);
    CHECK(sellWotlk.getSize() == 20);
}

TEST_CASE("TBC remote movement, speed broadcasts, spline flags, and SMSG_FLIGHT_SPLINE_SYNC",
          "[game_replay][movement][spline][tbc][shard1]") {
    Session s("tbc");
    spawnPlayer(s);

    constexpr uint64_t kOtherPlayer = 0x0000000000000099ull;
    constexpr uint64_t kFlyerNpc    = 0xF130005000000001ull;
    s.receive(UpdateObject().tbc().create(kOtherPlayer, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100}}, false));
    s.receive(UpdateObject().tbc().create(kFlyerNpc, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 100}}, false));

    uint32_t lastMoveFlags = 0;
    uint32_t lastMoveMask  = 0;
    uint64_t lastMoveGuid  = 0;
    s.handler().setUnitMoveFlagsCallback([&](uint64_t g, uint32_t f, uint32_t m) {
        lastMoveGuid  = g;
        lastMoveFlags = f;
        lastMoveMask  = m;
    });

    // 1) TBC MSG_MOVE_KNOCK_BACK with flags2=0x20 (not pitch in TBC!) + SPLINE_ELEVATION (0x04000000)
    {
        Packet kb(wowee::game::wireOpcode(Opcode::MSG_MOVE_KNOCK_BACK));
        kb.writePackedGuid(kOtherPlayer);
        kb.writeUInt32(0x00001000u | 0x04000000u); // FALLING | SPLINE_ELEVATION (no SWIMMING/FLYING)
        kb.writeUInt8(0x20u); // TBC flags2 byte (must NOT consume 4 bytes as WotLK pitch!)
        kb.writeUInt32(2000u);
        kb.writeFloat(50.0f);
        kb.writeFloat(60.0f);
        kb.writeFloat(70.0f);
        kb.writeFloat(0.5f);
        kb.writeUInt32(10u);   // fallTime
        kb.writeFloat(-5.0f);  // jumpVelocity
        kb.writeFloat(1.0f);   // cos
        kb.writeFloat(0.0f);   // sin
        kb.writeFloat(12.0f);  // xySpeed
        kb.writeFloat(1.75f);  // splineElevation
        // Trailing knockback fields (cos, sin, xySpeed, zSpeed)
        kb.writeFloat(1.0f);
        kb.writeFloat(0.0f);
        kb.writeFloat(15.0f);
        kb.writeFloat(-8.0f);
        s.receive(kb);

        auto other = s.handler().getEntityManager().getEntity(kOtherPlayer);
        REQUIRE(other != nullptr);
        CHECK(other->isRemoteAirborne());
    }

    // 2) MSG_MOVE_SET_FLIGHT_SPEED parses full MovementInfo + updates remote entity position & flightSpeed
    {
        Packet sp(wowee::game::wireOpcode(Opcode::MSG_MOVE_SET_FLIGHT_SPEED));
        sp.writePackedGuid(kOtherPlayer);
        sp.writeUInt32(static_cast<uint32_t>(MovementFlags::CAN_FLY) |
                       static_cast<uint32_t>(MovementFlags::FLYING));
        sp.writeUInt8(0);
        sp.writeUInt32(3000u);
        sp.writeFloat(120.0f); // server X
        sp.writeFloat(240.0f); // server Y
        sp.writeFloat(80.0f);  // server Z
        sp.writeFloat(1.0f);   // server O
        sp.writeFloat(0.25f);  // pitch (FLYING is set)
        sp.writeUInt32(0u);    // fallTime
        sp.writeFloat(28.0f);  // newspeed (280% epic flying = 7 * 4)
        s.receive(sp);

        auto other = s.handler().getEntityManager().getEntity(kOtherPlayer);
        REQUIRE(other != nullptr);
        CHECK(other->flightSpeed == Catch::Approx(28.0f));
        const glm::vec3 expectedCan = wowee::core::coords::serverToCanonical(glm::vec3(120.0f, 240.0f, 80.0f));
        CHECK(other->getX() == Catch::Approx(expectedCan.x));
        CHECK(other->getY() == Catch::Approx(expectedCan.y));
        CHECK(other->getZ() == Catch::Approx(expectedCan.z));
    }

    // 3) SMSG_SPLINE_SET_SWIM_SPEED & SMSG_SPLINE_SET_FLIGHT_BACK_SPEED update remote NPC speeds
    {
        Packet sw(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_SET_SWIM_SPEED));
        sw.writePackedGuid(kFlyerNpc);
        sw.writeFloat(9.5f);
        s.receive(sw);

        Packet fb(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_SET_FLIGHT_BACK_SPEED));
        fb.writePackedGuid(kFlyerNpc);
        fb.writeFloat(6.25f);
        s.receive(fb);

        auto npc = s.handler().getEntityManager().getEntity(kFlyerNpc);
        REQUIRE(npc != nullptr);
        CHECK(npc->swimSpeed == Catch::Approx(9.5f));
        CHECK(npc->flightBackSpeed == Catch::Approx(6.25f));
    }

    // 4) SMSG_SPLINE_MOVE_FEATHER_FALL & SMSG_SPLINE_MOVE_WATER_WALK propagate flags
    {
        Packet ff(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_MOVE_FEATHER_FALL));
        ff.writePackedGuid(kFlyerNpc);
        s.receive(ff);
        CHECK(lastMoveGuid == kFlyerNpc);
        CHECK(lastMoveFlags == static_cast<uint32_t>(MovementFlags::FEATHER_FALL));
        CHECK(lastMoveMask  == static_cast<uint32_t>(MovementFlags::FEATHER_FALL));

        Packet ww(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_MOVE_WATER_WALK));
        ww.writePackedGuid(kFlyerNpc);
        s.receive(ww);
        CHECK(lastMoveFlags == static_cast<uint32_t>(MovementFlags::WATER_WALK));

        Packet lw(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_MOVE_LAND_WALK));
        lw.writePackedGuid(kFlyerNpc);
        s.receive(lw);
        CHECK(lastMoveFlags == 0u);
        CHECK(lastMoveMask  == static_cast<uint32_t>(MovementFlags::WATER_WALK));
    }

    // 5) SMSG_FLIGHT_SPLINE_SYNC (12-byte TBC float + uint64 guid) advances cyclic spline position
    {
        // Start a cyclic TBC flying spline (SplineFlag::FLYING_TBC=0x200 | SplineFlag::CYCLIC=0x100000)
        Packet mm(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
        mm.writePackedGuid(kFlyerNpc);
        mm.writeFloat(0.0f); mm.writeFloat(0.0f); mm.writeFloat(50.0f);
        mm.writeUInt32(555u);
        mm.writeUInt8(0); // Normal
        mm.writeUInt32(0x00000200u | 0x00100000u);
        mm.writeUInt32(10000u); // 10s loop
        mm.writeUInt32(4u);     // 4 uncompressed points
        mm.writeFloat(0.0f);   mm.writeFloat(0.0f);   mm.writeFloat(50.0f);
        mm.writeFloat(100.0f); mm.writeFloat(0.0f);   mm.writeFloat(50.0f);
        mm.writeFloat(100.0f); mm.writeFloat(100.0f); mm.writeFloat(50.0f);
        mm.writeFloat(0.0f);   mm.writeFloat(100.0f); mm.writeFloat(50.0f);
        s.receive(mm);

        auto npc = s.handler().getEntityManager().getEntity(kFlyerNpc);
        REQUIRE(npc != nullptr);
        const float startX = npc->getX();
        const float startY = npc->getY();

        Packet sync(wowee::game::wireOpcode(Opcode::SMSG_FLIGHT_SPLINE_SYNC));
        sync.writeFloat(0.5f);       // halfway around the cyclic spline
        sync.writeUInt64(kFlyerNpc); // TBC writes full 8-byte ObjectGuid
        s.receive(sync);

        const float distMoved = std::hypot(npc->getX() - startX, npc->getY() - startY);
        CHECK(distMoved > 50.0f);
    }
}

TEST_CASE("TBC SMSG_UPDATE_INSTANCE_OWNERSHIP, CMSG_ZONEUPDATE, CMSG_MOVE_SPLINE_DONE, and CMSG_MOVE_SET_FLY",
          "[game_replay][movement][instance][zone][tbc][shard1]") {
    Session s("tbc");
    spawnPlayer(s);

    int instanceInfoEvents = 0;
    s.handler().setAddonEventCallback([&](const std::string& ev, const std::vector<std::string>&) {
        if (ev == "UPDATE_INSTANCE_INFO") ++instanceInfoEvents;
    });

    // 1) SMSG_UPDATE_INSTANCE_OWNERSHIP
    CHECK_FALSE(s.handler().hasInstanceOwnership());
    Packet own(wowee::game::wireOpcode(Opcode::SMSG_UPDATE_INSTANCE_OWNERSHIP));
    own.writeUInt32(1u);
    s.receive(own);
    CHECK(s.handler().hasInstanceOwnership());
    CHECK(instanceInfoEvents == 1);

    // 2) CMSG_ZONEUPDATE deduplicates and resets on SMSG_NEW_WORLD
    s.handler().sendZoneUpdate(1519u); // Stormwind City
    s.handler().sendZoneUpdate(1519u); // duplicate -> ignored
    s.handler().sendZoneUpdate(12u);   // Elwynn Forest
    auto zonePkts = s.sent().of(Opcode::CMSG_ZONEUPDATE);
    REQUIRE(zonePkts.size() == 2);
    CHECK(u32At(zonePkts[0], 0) == 1519u);
    CHECK(u32At(zonePkts[1], 0) == 12u);

    Packet nw(wowee::game::wireOpcode(Opcode::SMSG_NEW_WORLD));
    nw.writeUInt32(530u);
    nw.writeFloat(-248.0f);
    nw.writeFloat(946.0f);
    nw.writeFloat(84.0f);
    nw.writeFloat(0.0f);
    s.receive(nw);
    GameHandlerTestAccess::enterWorld(s.handler());
    spawnPlayer(s);
    s.handler().sendZoneUpdate(12u); // allowed again after map transfer
    CHECK(s.sent().of(Opcode::CMSG_ZONEUPDATE).size() == 3);

    // 3) CMSG_MOVE_SPLINE_DONE sent with MovementInfo + splineId when player server spline settles
    {
        Packet mm(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
        mm.writePackedGuid(Session::kPlayerGuid);
        mm.writeFloat(0.0f); mm.writeFloat(0.0f); mm.writeFloat(0.0f);
        mm.writeUInt32(777u); // splineId
        mm.writeUInt8(0);     // Normal
        mm.writeUInt32(0x00000100u); // Runmode
        mm.writeUInt32(500u); // 0.5s duration
        mm.writeUInt32(1u);
        mm.writeFloat(10.0f); mm.writeFloat(0.0f); mm.writeFloat(0.0f);
        s.receive(mm);

        // Advance entity interpolation past the 0.5s spline duration
        s.handler().updateEntityInterpolation(0.6f);

        const auto donePkts = s.sent().of(Opcode::CMSG_MOVE_SPLINE_DONE);
        REQUIRE(donePkts.size() == 1);
        // TBC MovementInfo (29 bytes) + uint32 splineId (4 bytes) = 33 bytes
        REQUIRE(donePkts.front().getSize() == 33);
        CHECK(u32At(donePkts.front(), 29) == 777u);
    }

    // 4) CMSG_MOVE_SET_FLY sent on takeoff and landing when CAN_FLY is active
    {
        Packet canFly(wowee::game::wireOpcode(Opcode::SMSG_MOVE_SET_CAN_FLY));
        canFly.writePackedGuid(Session::kPlayerGuid);
        canFly.writeUInt32(1u);
        s.receive(canFly);

        auto* mh = s.handler().getMovementHandler();
        REQUIRE(mh != nullptr);
        mh->setFlightAirborne(true);
        mh->setFlightAirborne(false);

        const auto flyPkts = s.sent().of(Opcode::CMSG_MOVE_SET_FLY);
        REQUIRE(flyPkts.size() == 2);
        const uint32_t takeoffFlags = u32At(flyPkts[0], 0);
        const uint32_t landFlags    = u32At(flyPkts[1], 0);
        CHECK((takeoffFlags & static_cast<uint32_t>(MovementFlags::FLYING)) != 0);
        CHECK((landFlags    & static_cast<uint32_t>(MovementFlags::FLYING)) == 0);
    }
}
