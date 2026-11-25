// TBC 2.4.3 Shard 1 (Entities, Movement, Maps) wire-payload unit tests.
// Covers SMSG_UPDATE_OBJECT (PackedGuid MOVEMENT + mid-packet OUT_OF_RANGE/NEAR_OBJECTS
// + parseMovementBlock fields), TBC writeMovementPayload SPLINE_ELEVATION,
// TbcPacketParsers::parseMonsterMove splineId, ActivateTaxiExpressPacket TBC cost,
// XpGainParser TBC GivenXP + rafBonus, and ZoneUpdatePacket.
#include <catch_amalgamated.hpp>

#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/world_packets.hpp"
#include "network/packet.hpp"

#include <cstdint>
#include <vector>

using wowee::game::ActivateTaxiExpressPacket;
using wowee::game::LogicalOpcode;
using wowee::game::MonsterMoveData;
using wowee::game::MovementInfo;
using wowee::game::MovementPacket;
using wowee::game::ObjectType;
using wowee::game::TbcPacketParsers;
using wowee::game::UpdateObjectData;
using wowee::game::UpdateType;
using wowee::game::XpGainData;
using wowee::game::XpGainParser;
using wowee::game::ZoneUpdatePacket;
using wowee::network::Packet;

namespace {

uint32_t readU32LE(const std::vector<uint8_t>& b, size_t at) {
    return static_cast<uint32_t>(b[at]) |
           (static_cast<uint32_t>(b[at + 1]) << 8) |
           (static_cast<uint32_t>(b[at + 2]) << 16) |
           (static_cast<uint32_t>(b[at + 3]) << 24);
}

uint64_t readU64LE(const std::vector<uint8_t>& b, size_t at) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | b[at + static_cast<size_t>(i)];
    }
    return v;
}

void writeTbcLivingMovementBlock(Packet& p, uint32_t moveFlags, float x, float y, float z, float o,
                                 float pitch = 0.0f, uint32_t fallTime = 0,
                                 float jVel = 0.0f, float jCos = 0.0f, float jSin = 0.0f, float jXY = 0.0f,
                                 float splineElev = 0.0f, uint64_t targetGuid = 0) {
    uint8_t updateFlags = 0x20; // UPDATEFLAG_LIVING
    if (targetGuid != 0) updateFlags |= 0x04; // UPDATEFLAG_HAS_TARGET
    p.writeUInt8(updateFlags);
    p.writeUInt32(moveFlags);
    p.writeUInt8(0);        // TBC moveFlags2
    p.writeUInt32(1000);    // time
    p.writeFloat(x);
    p.writeFloat(y);
    p.writeFloat(z);
    p.writeFloat(o);
    if (moveFlags & (0x00200000u | 0x02000000u)) {
        p.writeFloat(pitch);
    }
    p.writeUInt32(fallTime);
    if (moveFlags & 0x00001000u) {
        p.writeFloat(jVel);
        p.writeFloat(jCos);
        p.writeFloat(jSin);
        p.writeFloat(jXY);
    }
    if (moveFlags & 0x04000000u) {
        p.writeFloat(splineElev);
    }
    // 8 TBC speeds: walk, run, runBack, swim, swimBack, fly, flyBack, turn
    p.writeFloat(2.5f);
    p.writeFloat(7.0f);
    p.writeFloat(4.5f);
    p.writeFloat(4.7222f);
    p.writeFloat(2.5f);
    p.writeFloat(14.0f);
    p.writeFloat(4.5f);
    p.writeFloat(3.14159f);
    if (targetGuid != 0) {
        p.writePackedGuid(targetGuid);
    }
}

} // namespace

TEST_CASE("TBC parseUpdateObject reads PackedGuid on MOVEMENT and parses mid-packet OUT_OF_RANGE_OBJECTS",
          "[tbc][update_object][shard1]") {
    TbcPacketParsers parser;
    Packet p(0x0A9);

    constexpr uint64_t kUnitA = 0xF130001234000001ull;
    constexpr uint64_t kGone1 = 0xF1300012340000AAull;
    constexpr uint64_t kGone2 = 0x00000000000000BBull;
    constexpr uint64_t kNear1 = 0x00000000000000CCull;
    constexpr uint64_t kUnitB = 0x0000000000000077ull; // 2-byte PackedGuid (0x01, 0x77)
    constexpr uint64_t kTarget = 0x0000000000000099ull;

    // 4 blocks in header: CREATE_OBJECT -> OUT_OF_RANGE_OBJECTS -> NEAR_OBJECTS -> MOVEMENT
    p.writeUInt32(4);
    p.writeUInt8(0); // TBC hasTransport

    // Block 0: CREATE_OBJECT with empty values mask
    p.writeUInt8(static_cast<uint8_t>(UpdateType::CREATE_OBJECT));
    p.writePackedGuid(kUnitA);
    p.writeUInt8(static_cast<uint8_t>(ObjectType::UNIT));
    p.writeUInt8(0x40); // UPDATEFLAG_HAS_POSITION
    p.writeFloat(10.0f);
    p.writeFloat(20.0f);
    p.writeFloat(30.0f);
    p.writeFloat(1.0f);
    p.writeUInt8(0); // 0 mask blocks

    // Block 1 (non-first!): OUT_OF_RANGE_OBJECTS with 2 PackedGuids
    p.writeUInt8(static_cast<uint8_t>(UpdateType::OUT_OF_RANGE_OBJECTS));
    p.writeUInt32(2);
    p.writePackedGuid(kGone1);
    p.writePackedGuid(kGone2);

    // Block 2 (non-first!): NEAR_OBJECTS with 1 PackedGuid
    p.writeUInt8(static_cast<uint8_t>(UpdateType::NEAR_OBJECTS));
    p.writeUInt32(1);
    p.writePackedGuid(kNear1);

    // Block 3: UPDATETYPE_MOVEMENT with PackedGuid + pitch + fall + splineElevation + HAS_TARGET
    p.writeUInt8(static_cast<uint8_t>(UpdateType::MOVEMENT));
    p.writePackedGuid(kUnitB);
    const uint32_t moveFlags = 0x00200000u | 0x00001000u | 0x04000000u; // SWIMMING | FALLING | SPLINE_ELEVATION
    writeTbcLivingMovementBlock(p, moveFlags, 100.0f, 200.0f, 300.0f, 1.5f,
                                -0.35f, 420u, -7.9f, 0.8f, 0.6f, 7.0f, 3.25f, kTarget);

    UpdateObjectData out;
    REQUIRE(parser.parseUpdateObject(p, out));
    CHECK(p.getRemainingSize() == 0);
    REQUIRE(out.outOfRangeGuids.size() == 2);
    CHECK(out.outOfRangeGuids[0] == kGone1);
    CHECK(out.outOfRangeGuids[1] == kGone2);

    REQUIRE(out.blocks.size() == 2);
    CHECK(out.blocks[0].updateType == UpdateType::CREATE_OBJECT);
    CHECK(out.blocks[0].guid == kUnitA);

    const auto& mv = out.blocks[1];
    CHECK(mv.updateType == UpdateType::MOVEMENT);
    CHECK(mv.guid == kUnitB);
    CHECK(mv.x == Catch::Approx(100.0f));
    CHECK(mv.y == Catch::Approx(200.0f));
    CHECK(mv.z == Catch::Approx(300.0f));
    CHECK(mv.pitch == Catch::Approx(-0.35f));
    CHECK(mv.fallTime == 420u);
    CHECK(mv.jumpVelocity == Catch::Approx(-7.9f));
    CHECK(mv.jumpCosAngle == Catch::Approx(0.8f));
    CHECK(mv.jumpSinAngle == Catch::Approx(0.6f));
    CHECK(mv.jumpXYSpeed == Catch::Approx(7.0f));
    CHECK(mv.splineElevation == Catch::Approx(3.25f));
    CHECK(mv.swimSpeed == Catch::Approx(4.7222f));
    CHECK(mv.flightSpeed == Catch::Approx(14.0f));
    CHECK(mv.attackingTargetGuid == kTarget);
}

TEST_CASE("TBC and WotLK writeMovementPayload serialize float splineElevation when 0x04000000 is set",
          "[tbc][movement][shard1]") {
    TbcPacketParsers tbc;
    MovementInfo info{};
    info.flags = 0x04000001u; // FORWARD | SPLINE_ELEVATION
    info.flags2 = 0x04;
    info.time = 5000;
    info.x = 12.0f;
    info.y = 34.0f;
    info.z = 56.0f;
    info.orientation = 0.75f;
    info.fallTime = 0;
    info.splineElevation = -2.5f;

    auto tbcPkt = tbc.buildMovementPacket(LogicalOpcode::MSG_MOVE_HEARTBEAT, info, 0x1234ull);
    // Base TBC movement payload is 29 bytes + 4 bytes splineElevation = 33 bytes
    REQUIRE(tbcPkt.getSize() == 33);
    CHECK(tbcPkt.readUInt32() == 0x04000001u);
    CHECK(tbcPkt.readUInt8() == 0x04u);
    CHECK(tbcPkt.readUInt32() == 5000u);
    for (int i = 0; i < 4; ++i) (void)tbcPkt.readFloat();
    CHECK(tbcPkt.readUInt32() == 0u); // fallTime
    CHECK(tbcPkt.readFloat() == Catch::Approx(-2.5f));
    CHECK_FALSE(tbcPkt.hasData());

    Packet wotlkPayload(0x0EE);
    MovementPacket::writeMovementPayload(wotlkPayload, info);
    // Base WotLK payload is 30 bytes + 4 bytes splineElevation = 34 bytes
    REQUIRE(wotlkPayload.getSize() == 34);
}

TEST_CASE("TBC ActivateTaxiExpressPacket writes totalCost before nodeCount while WotLK omits it",
          "[tbc][taxi][shard1]") {
    constexpr uint64_t kNpc = 0xF130000624000002ull;
    const std::vector<uint32_t> route = {2, 12, 19};

    auto tbcPkt = ActivateTaxiExpressPacket::build(kNpc, 1850u, route, /*includeTotalCost=*/true);
    const auto& tbcBytes = tbcPkt.getData();
    REQUIRE(tbcBytes.size() == 8 + 4 + 4 + route.size() * 4);
    CHECK(readU64LE(tbcBytes, 0) == kNpc);
    CHECK(readU32LE(tbcBytes, 8) == 1850u);
    CHECK(readU32LE(tbcBytes, 12) == 3u);
    CHECK(readU32LE(tbcBytes, 16) == 2u);
    CHECK(readU32LE(tbcBytes, 20) == 12u);
    CHECK(readU32LE(tbcBytes, 24) == 19u);

    auto wotlkPkt = ActivateTaxiExpressPacket::build(kNpc, 1850u, route, /*includeTotalCost=*/false);
    const auto& wotlkBytes = wotlkPkt.getData();
    REQUIRE(wotlkBytes.size() == 8 + 4 + route.size() * 4);
    CHECK(readU64LE(wotlkBytes, 0) == kNpc);
    CHECK(readU32LE(wotlkBytes, 8) == 3u);
}

TEST_CASE("XpGainParser reads TBC GivenXP before groupRate on kill XP and preserves rested/RaF flags",
          "[tbc][xpgain][shard1]") {
    // CMaNGOS-TBC Player::SendLogXPGain (kill XP with rested bonus and 1.2x group rate):
    // guid(8) + totalXp(4=240) + type(1=0) + GivenXP(4=120) + groupRate(f32=1.2f) + raf(1=0)
    Packet killPkt(0x1D0);
    killPkt.writeUInt64(0xF130000ABC000001ull);
    killPkt.writeUInt32(240);
    killPkt.writeUInt8(0);
    killPkt.writeUInt32(120);
    killPkt.writeFloat(1.2f);
    killPkt.writeUInt8(0);

    XpGainData d;
    REQUIRE(XpGainParser::parse(killPkt, d));
    CHECK(killPkt.getRemainingSize() == 0);
    CHECK(d.totalXp == 240u);
    CHECK(d.baseXp == 120u);
    CHECK(d.restedBonus == 120u);
    CHECK(d.groupBonus == 20u); // 120 - (120 / 1.2) = 20
    CHECK_FALSE(d.rafBonus);

    // Non-kill XP with RaF flag
    Packet questPkt(0x1D0);
    questPkt.writeUInt64(0);
    questPkt.writeUInt32(1500);
    questPkt.writeUInt8(1);
    questPkt.writeUInt8(1);

    XpGainData q;
    REQUIRE(XpGainParser::parse(questPkt, q));
    CHECK(questPkt.getRemainingSize() == 0);
    CHECK(q.totalXp == 1500u);
    CHECK(q.rafBonus);
}

TEST_CASE("TbcPacketParsers::parseMonsterMove preserves splineId and ZoneUpdatePacket writes uint32 zoneId",
          "[tbc][spline][zone][shard1]") {
    Packet mm(0x0DD);
    mm.writePackedGuid(0xF130000ABC000001ull);
    mm.writeFloat(1.0f);
    mm.writeFloat(2.0f);
    mm.writeFloat(3.0f);
    mm.writeUInt32(98765u); // splineId
    mm.writeUInt8(1);       // Stop

    TbcPacketParsers parser;
    MonsterMoveData move;
    REQUIRE(parser.parseMonsterMove(mm, move));
    CHECK(move.splineId == 98765u);

    auto z = ZoneUpdatePacket::build(3483u); // Hellfire Peninsula
    REQUIRE(z.getSize() == 4);
    CHECK(z.readUInt32() == 3483u);
}
