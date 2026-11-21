// TBC 2.4.3 SMSG_MONSTER_MOVE and MOVEMENTBLOCK spline final facing tests.
// Covers Final_Angle, Final_Point (FacingSpot), Final_Target, Normal, Stop,
// and create-block restored splines.
#include <catch_amalgamated.hpp>

#include "core/coordinates.hpp"
#include "game/entity.hpp"
#include "game/packet_parsers.hpp"
#include "game/spline_packet.hpp"
#include "network/packet.hpp"

#include <array>
#include <cmath>
#include <vector>

using wowee::core::coords::normalizeAngleRad;
using wowee::core::coords::serverToCanonical;
using wowee::core::coords::serverToCanonicalYaw;
using wowee::game::Entity;
using wowee::game::MonsterMoveData;
namespace SplineFlag = wowee::game::SplineFlag;
using wowee::game::TbcPacketParsers;
using wowee::game::Unit;
using wowee::game::UpdateBlock;
using wowee::network::Packet;

namespace {

void writePackedGuid(Packet& p, uint64_t guid) {
    uint8_t mask = 0;
    uint8_t bytes[8];
    int count = 0;
    for (int i = 0; i < 8; ++i) {
        const uint8_t b = static_cast<uint8_t>((guid >> (i * 8)) & 0xFFu);
        if (b != 0) {
            mask |= static_cast<uint8_t>(1u << i);
            bytes[count++] = b;
        }
    }
    p.writeUInt8(mask);
    for (int i = 0; i < count; ++i) p.writeUInt8(bytes[i]);
}

// Build a TBC 2.4.3 SMSG_MONSTER_MOVE packet (no WotLK unk byte after GUID).
Packet buildTbcMonsterMove(uint64_t guid, glm::vec3 start, uint32_t splineId,
                           uint8_t moveType, uint32_t splineFlags, uint32_t durationMs,
                           glm::vec3 dest, float facingAngle = 0.0f,
                           glm::vec3 facingSpot = glm::vec3(0.0f),
                           uint64_t facingTarget = 0) {
    Packet p(0xDD);
    writePackedGuid(p, guid);
    p.writeFloat(start.x);
    p.writeFloat(start.y);
    p.writeFloat(start.z);
    p.writeUInt32(splineId);
    p.writeUInt8(moveType);
    if (moveType == 1) return p;  // Stop ends here.
    if (moveType == 2) {
        p.writeFloat(facingSpot.x);
        p.writeFloat(facingSpot.y);
        p.writeFloat(facingSpot.z);
    } else if (moveType == 3) {
        p.writeUInt64(facingTarget);
    } else if (moveType == 4) {
        p.writeFloat(facingAngle);
    }
    p.writeUInt32(splineFlags);
    p.writeUInt32(durationMs);
    p.writeUInt32(1);  // pointCount = 1 (destination only)
    p.writeFloat(dest.x);
    p.writeFloat(dest.y);
    p.writeFloat(dest.z);
    return p;
}

// Build a TBC 2.4.3 LIVING MOVEMENTBLOCK with SPLINE_ENABLED (0x08000000).
Packet buildTbcMovementBlockWithSpline(glm::vec3 pos, float orientation,
                                       uint32_t splineFlags,
                                       uint32_t elapsedMs, uint32_t durationMs,
                                       const std::vector<glm::vec3>& nodes,
                                       float finalAngle = 0.0f,
                                       glm::vec3 finalPoint = glm::vec3(0.0f),
                                       uint64_t finalTarget = 0) {
    Packet p(0xA9);
    // updateFlags: UPDATEFLAG_LIVING (0x20)
    p.writeUInt8(0x20);
    // moveFlags: SPLINE_ENABLED (0x08000000)
    p.writeUInt32(0x08000000u);
    p.writeUInt8(0);        // moveFlags2 (TBC uint8)
    p.writeUInt32(12345);   // timestamp
    p.writeFloat(pos.x);
    p.writeFloat(pos.y);
    p.writeFloat(pos.z);
    p.writeFloat(orientation);
    p.writeUInt32(0);       // fallTime
    // 8 speeds
    for (int i = 0; i < 8; ++i) p.writeFloat(7.0f);

    // Spline block (CMaNGOS-TBC PacketBuilder::WriteCreate)
    p.writeUInt32(splineFlags);
    if (splineFlags & SplineFlag::FINAL_POINT) {
        p.writeFloat(finalPoint.x);
        p.writeFloat(finalPoint.y);
        p.writeFloat(finalPoint.z);
    } else if (splineFlags & SplineFlag::FINAL_TARGET) {
        p.writeUInt64(finalTarget);
    } else if (splineFlags & SplineFlag::FINAL_ANGLE) {
        p.writeFloat(finalAngle);
    }
    p.writeUInt32(elapsedMs);
    p.writeUInt32(durationMs);
    p.writeUInt32(42);  // splineId
    p.writeUInt32(static_cast<uint32_t>(nodes.size()));
    for (const auto& n : nodes) {
        p.writeFloat(n.x);
        p.writeFloat(n.y);
        p.writeFloat(n.z);
    }
    const glm::vec3 endPt = nodes.empty() ? pos : nodes.back();
    p.writeFloat(endPt.x);
    p.writeFloat(endPt.y);
    p.writeFloat(endPt.z);
    return p;
}

}  // namespace

TEST_CASE("TBC SMSG_MONSTER_MOVE Final_Angle turns around on arrival", "[tbc][spline][facing]") {
    // Reproduces Mizzle the Crafty running +X up to Gordok's Seat (dest x=816.53)
    // and turning 180 degrees around to server orientation 3.159 rad (-X).
    constexpr uint64_t kMizzleGuid = 0xf130003811596f58ull;
    const glm::vec3 startServer(784.581f, 481.701f, 37.198f);
    const glm::vec3 destServer(816.53f, 482.302f, 37.3182f);
    constexpr float kFinalServerAngle = 3.159f;

    Packet pkt = buildTbcMonsterMove(kMizzleGuid, startServer, 100,
                                     /*moveType=*/4, /*splineFlags=*/0x100u,
                                     /*durationMs=*/8425, destServer,
                                     kFinalServerAngle);
    TbcPacketParsers parsers;
    MonsterMoveData data;
    REQUIRE(parsers.parseMonsterMove(pkt, data));
    REQUIRE(data.moveType == 4);
    REQUIRE(data.hasDest);
    REQUIRE(data.facingAngle == Catch::Approx(kFinalServerAngle));

    const glm::vec3 startCan = serverToCanonical(startServer);
    const glm::vec3 destCan = serverToCanonical(destServer);
    const float travelYaw = std::atan2(-(destCan.y - startCan.y), destCan.x - startCan.x);
    const float finalYaw = serverToCanonicalYaw(data.facingAngle);

    Unit mizzle(kMizzleGuid);
    mizzle.setPosition(startCan.x, startCan.y, startCan.z, travelYaw);
    std::vector<std::array<float, 3>> path = {
        {startCan.x, startCan.y, startCan.z},
        {destCan.x, destCan.y, destCan.z},
    };
    mizzle.startServerSpline(path, travelYaw, data.duration / 1000.0f,
                             /*smooth=*/false, /*followPathFacing=*/true,
                             data.splineFlags, /*facingTargetGuid=*/0, finalYaw);

    // Mid-spline: faces travel direction (+X server -> canonical ~ -PI/2), not backwards.
    mizzle.updateMovement(4.0f);
    REQUIRE(mizzle.isServerSplineActive());
    CHECK(std::abs(normalizeAngleRad(mizzle.getOrientation() - travelYaw)) < 0.02f);

    // Upon arrival: turns to Final_Angle (3.159 server -> canonical ~ +PI/2).
    mizzle.updateMovement(5.0f);
    REQUIRE_FALSE(mizzle.isServerSplineActive());
    CHECK(std::abs(normalizeAngleRad(mizzle.getOrientation() - finalYaw)) < 0.001f);
    CHECK(std::abs(normalizeAngleRad(mizzle.getOrientation() - travelYaw)) > 2.9f);
}

TEST_CASE("TBC SMSG_MONSTER_MOVE Final_Point (FacingSpot) parses and faces spot on arrival", "[tbc][spline][facing]") {
    constexpr uint64_t kNpcGuid = 0xf130001234000001ull;
    const glm::vec3 startServer(100.0f, 50.0f, 10.0f);
    const glm::vec3 destServer(120.0f, 50.0f, 10.0f);
    const glm::vec3 spotServer(120.0f, 80.0f, 10.0f);  // +Y server = +X canonical (North, yaw=0)

    Packet pkt = buildTbcMonsterMove(kNpcGuid, startServer, 101,
                                     /*moveType=*/2, /*splineFlags=*/0x100u,
                                     /*durationMs=*/2000, destServer,
                                     0.0f, spotServer);
    TbcPacketParsers parsers;
    MonsterMoveData data;
    REQUIRE(parsers.parseMonsterMove(pkt, data));
    REQUIRE(data.moveType == 2);
    CHECK(data.facingSpotX == Catch::Approx(spotServer.x));
    CHECK(data.facingSpotY == Catch::Approx(spotServer.y));
    CHECK(data.facingSpotZ == Catch::Approx(spotServer.z));

    const glm::vec3 startCan = serverToCanonical(startServer);
    const glm::vec3 destCan = serverToCanonical(destServer);
    const glm::vec3 spotCan = serverToCanonical(spotServer);
    const float travelYaw = std::atan2(-(destCan.y - startCan.y), destCan.x - startCan.x);
    const float spotYaw = std::atan2(-(spotCan.y - destCan.y), spotCan.x - destCan.x);

    Unit npc(kNpcGuid);
    npc.startServerSpline({{startCan.x, startCan.y, startCan.z},
                           {destCan.x, destCan.y, destCan.z}},
                          travelYaw, 2.0f, false, true, data.splineFlags, 0, spotYaw);

    npc.updateMovement(1.0f);
    CHECK(std::abs(normalizeAngleRad(npc.getOrientation() - travelYaw)) < 0.01f);

    npc.updateMovement(1.5f);
    CHECK(std::abs(normalizeAngleRad(npc.getOrientation() - spotYaw)) < 0.001f);
}

TEST_CASE("TBC SMSG_MONSTER_MOVE Final_Target flags arrival target resolution", "[tbc][spline][facing]") {
    constexpr uint64_t kNpcGuid = 0xf130001234000002ull;
    constexpr uint64_t kTargetGuid = 0x0000000000000096ull;
    const glm::vec3 startServer(0.0f, 0.0f, 0.0f);
    const glm::vec3 destServer(10.0f, 0.0f, 0.0f);

    Packet pkt = buildTbcMonsterMove(kNpcGuid, startServer, 102,
                                     /*moveType=*/3, /*splineFlags=*/0x100u,
                                     /*durationMs=*/1000, destServer,
                                     0.0f, glm::vec3(0.0f), kTargetGuid);
    TbcPacketParsers parsers;
    MonsterMoveData data;
    REQUIRE(parsers.parseMonsterMove(pkt, data));
    REQUIRE(data.moveType == 3);
    REQUIRE(data.facingTarget == kTargetGuid);

    Unit npc(kNpcGuid);
    npc.startServerSpline({{0.0f, 0.0f, 0.0f}, {0.0f, 10.0f, 0.0f}},
                          0.0f, 1.0f, false, false, data.splineFlags, kTargetGuid, 1.25f);
    npc.updateMovement(0.5f);
    CHECK(npc.isServerSplineActive());
    CHECK(npc.consumeSplineArrivalTargetGuid() == 0);

    npc.updateMovement(0.6f);
    CHECK_FALSE(npc.isServerSplineActive());
    CHECK(npc.getOrientation() == Catch::Approx(1.25f));
    CHECK(npc.consumeSplineArrivalTargetGuid() == kTargetGuid);
    CHECK(npc.consumeSplineArrivalTargetGuid() == 0);  // Consumed once.
}

TEST_CASE("TBC SMSG_MONSTER_MOVE Normal keeps travel facing and zero-distance Final_Angle snaps", "[tbc][spline][facing]") {
    Unit npc(0xf130001234000003ull);
    // Normal spline without finalOrientation retains travel heading.
    npc.startServerSpline({{0.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}},
                          0.0f, 1.0f, false, true, 0x100u, 0, std::nullopt);
    npc.updateMovement(1.2f);
    CHECK_FALSE(npc.isServerSplineActive());
    CHECK(std::abs(normalizeAngleRad(npc.getOrientation() - 0.0f)) < 0.001f);

    // Degenerate 0-distance spline with Final_Angle applies finalOrientation immediately.
    const float turnYaw = serverToCanonicalYaw(3.159f);
    npc.startServerSpline({{10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f}},
                          0.0f, 0.5f, false, true, 0x100u, 0, turnYaw);
    CHECK_FALSE(npc.isServerSplineActive());
    CHECK(std::abs(normalizeAngleRad(npc.getOrientation() - turnYaw)) < 0.001f);
}

TEST_CASE("TBC MOVEMENTBLOCK create-block spline parses FINAL_ANGLE, FINAL_POINT, FINAL_TARGET", "[tbc][spline][facing]") {
    TbcPacketParsers parsers;
    // CMaNGOS WriteCreate sends leading + interior + trailing control points.
    const std::vector<glm::vec3> nodes = {
        {784.0f, 482.0f, 37.0f},
        {784.0f, 482.0f, 37.0f},
        {816.53f, 482.302f, 37.3182f},
        {816.53f, 482.302f, 37.3182f},
    };

    SECTION("FINAL_ANGLE (0x40000)") {
        Packet pkt = buildTbcMovementBlockWithSpline(
            nodes[1], 0.0f, SplineFlag::FINAL_ANGLE | 0x100u,
            1000, 4000, nodes, /*finalAngle=*/3.159f);
        UpdateBlock block;
        REQUIRE(parsers.parseMovementBlock(pkt, block));
        REQUIRE(block.splineHasFinalAngle);
        CHECK_FALSE(block.splineHasFinalPoint);
        CHECK_FALSE(block.splineHasFinalTarget);
        CHECK(block.splineFinalAngle == Catch::Approx(3.159f));
        CHECK(block.splinePath.size() == 2);
        CHECK_FALSE(pkt.hasData());
    }

    SECTION("FINAL_POINT (0x10000)") {
        const glm::vec3 pt(700.0f, 482.0f, 37.0f);
        Packet pkt = buildTbcMovementBlockWithSpline(
            nodes[1], 0.0f, SplineFlag::FINAL_POINT | 0x100u,
            500, 4000, nodes, 0.0f, pt);
        UpdateBlock block;
        REQUIRE(parsers.parseMovementBlock(pkt, block));
        REQUIRE(block.splineHasFinalPoint);
        CHECK_FALSE(block.splineHasFinalAngle);
        CHECK_FALSE(block.splineHasFinalTarget);
        CHECK(block.splineFinalPoint[0] == Catch::Approx(pt.x));
        CHECK(block.splineFinalPoint[1] == Catch::Approx(pt.y));
        CHECK(block.splineFinalPoint[2] == Catch::Approx(pt.z));
        CHECK_FALSE(pkt.hasData());
    }

    SECTION("FINAL_TARGET (0x20000)") {
        constexpr uint64_t kTarget = 0x0000000000000096ull;
        Packet pkt = buildTbcMovementBlockWithSpline(
            nodes[1], 0.0f, SplineFlag::FINAL_TARGET | 0x100u,
            500, 4000, nodes, 0.0f, glm::vec3(0.0f), kTarget);
        UpdateBlock block;
        REQUIRE(parsers.parseMovementBlock(pkt, block));
        REQUIRE(block.splineHasFinalTarget);
        CHECK_FALSE(block.splineHasFinalAngle);
        CHECK_FALSE(block.splineHasFinalPoint);
        CHECK(block.splineFinalTarget == kTarget);
        CHECK_FALSE(pkt.hasData());
    }
}
