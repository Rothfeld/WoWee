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

TEST_CASE("TBC SMSG_MONSTER_MOVE Flying+Cyclic+Enter_Cycle (0x300300) parses uncompressed waypoints", "[tbc][spline][cyclic]") {
    // Mirrors Felmyst (entry 25038) circular flight in Sunwell Plateau:
    // CMaNGOS-TBC MoveSplineFlag = Runmode(0x100) | Flying(0x200) | Cyclic(0x100000) | Enter_Cycle(0x200000).
    constexpr uint64_t kFelmystGuid = 0xf1300061ce596e03ull;
    constexpr uint32_t kFelmystFlags = 0x00300300u;
    const glm::vec3 startServer(1489.44f, 651.615f, 44.651f);
    const std::vector<glm::vec3> ringServer = {
        {1469.84f, 617.638f, 43.109f},
        {1440.00f, 630.000f, 44.000f},
        {1450.00f, 670.000f, 45.000f},
        {1490.00f, 665.000f, 44.500f},
    };

    Packet p(0xDD);
    writePackedGuid(p, kFelmystGuid);
    p.writeFloat(startServer.x);
    p.writeFloat(startServer.y);
    p.writeFloat(startServer.z);
    p.writeUInt32(77);              // splineId
    p.writeUInt8(0);                // MonsterMoveNormal
    p.writeUInt32(kFelmystFlags);
    p.writeUInt32(31172);           // durationMs
    p.writeUInt32(static_cast<uint32_t>(ringServer.size()));
    for (const auto& wp : ringServer) {
        p.writeFloat(wp.x);
        p.writeFloat(wp.y);
        p.writeFloat(wp.z);
    }

    TbcPacketParsers parsers;
    MonsterMoveData data;
    REQUIRE(parsers.parseMonsterMove(p, data));
    CHECK_FALSE(p.hasData());
    CHECK(data.splineFlags == kFelmystFlags);
    CHECK(data.duration == 31172u);
    REQUIRE(data.hasDest);
    REQUIRE(data.waypoints.size() == ringServer.size() - 1);
    for (size_t i = 0; i + 1 < ringServer.size(); ++i) {
        CHECK(data.waypoints[i].x == Catch::Approx(ringServer[i].x));
        CHECK(data.waypoints[i].y == Catch::Approx(ringServer[i].y));
        CHECK(data.waypoints[i].z == Catch::Approx(ringServer[i].z));
    }
    CHECK(data.destX == Catch::Approx(ringServer.back().x));
    CHECK(data.destY == Catch::Approx(ringServer.back().y));
    CHECK(data.destZ == Catch::Approx(ringServer.back().z));
}

TEST_CASE("TBC SMSG_MONSTER_MOVE linear non-flying path still decodes packed deltas", "[tbc][spline][cyclic]") {
    constexpr uint64_t kNpcGuid = 0xf130001234000010ull;
    const glm::vec3 startServer(100.0f, 100.0f, 10.0f);
    const glm::vec3 destServer(120.0f, 100.0f, 10.0f);
    // Midpoint is (110, 100, 10). Packed delta 0 decodes to midpoint itself.
    Packet p(0xDD);
    writePackedGuid(p, kNpcGuid);
    p.writeFloat(startServer.x);
    p.writeFloat(startServer.y);
    p.writeFloat(startServer.z);
    p.writeUInt32(88);
    p.writeUInt8(0);
    p.writeUInt32(0x00000100u);     // Runmode only (no Flying -> compressed)
    p.writeUInt32(4000);
    p.writeUInt32(2);               // dest + 1 packed delta
    p.writeFloat(destServer.x);
    p.writeFloat(destServer.y);
    p.writeFloat(destServer.z);
    p.writeUInt32(0u);              // delta (0,0,0) -> midpoint (110,100,10)

    TbcPacketParsers parsers;
    MonsterMoveData data;
    REQUIRE(parsers.parseMonsterMove(p, data));
    CHECK_FALSE(p.hasData());
    REQUIRE(data.hasDest);
    REQUIRE(data.waypoints.size() == 1);
    CHECK(data.waypoints[0].x == Catch::Approx(110.0f));
    CHECK(data.waypoints[0].y == Catch::Approx(100.0f));
    CHECK(data.waypoints[0].z == Catch::Approx(10.0f));
    CHECK(data.destX == Catch::Approx(120.0f));
}

TEST_CASE("Cyclic Enter_Cycle server spline drops entry point and loops indefinitely", "[tbc][spline][cyclic]") {
    // Entry point P0=(0, -50, 44) far outside the circular patrol ring:
    // P1=(20, 0, 44), P2=(0, 20, 44), P3=(-20, 0, 44), P4=(0, -20, 44), closing to P1.
    const std::vector<std::array<float, 3>> path = {
        {  0.0f, -50.0f, 44.0f},  // P0 (pre-cycle liftoff position)
        { 20.0f,   0.0f, 44.0f},  // P1 (cycle start)
        {  0.0f,  20.0f, 44.0f},  // P2
        {-20.0f,   0.0f, 44.0f},  // P3
        {  0.0f, -20.0f, 44.0f},  // P4
        { 20.0f,   0.0f, 44.0f},  // closing vertex = P1
    };

    Unit felmyst(0xf1300061ce596e03ull);
    felmyst.startServerSpline(path, 0.0f, /*totalDuration=*/10.0f,
                              /*smooth=*/true, /*followPathFacing=*/true,
                              /*splineFlags=*/0x00300300u, /*facingTargetGuid=*/0,
                              std::nullopt, /*cyclic=*/true, /*enterCycle=*/true);

    // Mid lap 1: actively moving at airborne height.
    felmyst.updateMovement(2.5f);
    CHECK(felmyst.isServerSplineActive());
    CHECK(felmyst.isActivelyMoving());
    CHECK(felmyst.getZ() == Catch::Approx(44.0f).margin(0.5f));

    // Finish lap 1 (10s total) -> enters lap 2 at P1, drops P0.
    felmyst.updateMovement(7.5f);
    CHECK(felmyst.isServerSplineActive());
    CHECK(felmyst.isActivelyMoving());
    CHECK(felmyst.getX() == Catch::Approx(20.0f).margin(0.5f));
    CHECK(felmyst.getY() == Catch::Approx(0.0f).margin(0.5f));

    // Advance through 3 full subsequent laps in 0.5s steps:
    // must keep moving, stay near the ring (never revisit P0 y=-50), and cover all quadrants.
    bool sawPosX = false, sawNegX = false, sawPosY = false, sawNegY = false;
    for (int step = 0; step < 60; ++step) {
        felmyst.updateMovement(0.5f);
        REQUIRE(felmyst.isServerSplineActive());
        REQUIRE(felmyst.isActivelyMoving());
        CHECK(felmyst.getZ() == Catch::Approx(44.0f).margin(0.5f));
        CHECK(felmyst.getY() > -30.0f);  // Never revisits pre-cycle entry P0 (y=-50)
        if (felmyst.getX() > 10.0f) sawPosX = true;
        if (felmyst.getX() < -10.0f) sawNegX = true;
        if (felmyst.getY() > 10.0f) sawPosY = true;
        if (felmyst.getY() < -10.0f) sawNegY = true;
    }
    CHECK(sawPosX);
    CHECK(sawNegX);
    CHECK(sawPosY);
    CHECK(sawNegY);
}

TEST_CASE("TBC MOVEMENTBLOCK cyclic spline with Flying+Cyclic+Enter_Cycle parses cleanly", "[tbc][spline][cyclic]") {
    TbcPacketParsers parsers;
    // CMaNGOS WriteCreate for cyclic spline: [virtual_start, P0, P1, P2, P3, P1, virtual_end]
    const std::vector<glm::vec3> nodes = {
        {1500.0f, 670.0f, 44.0f},  // virtual start
        {1489.44f, 651.615f, 44.651f},  // P0
        {1469.84f, 617.638f, 43.109f},  // P1
        {1440.0f, 630.0f, 44.0f},       // P2
        {1450.0f, 670.0f, 45.0f},       // P3
        {1469.84f, 617.638f, 43.109f},  // closing P1
        {1440.0f, 630.0f, 44.0f},       // virtual end
    };
    Packet pkt = buildTbcMovementBlockWithSpline(
        nodes[1], 0.0f, 0x00300300u, 5000, 31172, nodes);
    UpdateBlock block;
    REQUIRE(parsers.parseMovementBlock(pkt, block));
    CHECK_FALSE(pkt.hasData());
    CHECK(block.splineFlags == 0x00300300u);
    CHECK(block.splineElapsedMs == 5000u);
    CHECK(block.splineDurationMs == 31172u);
    REQUIRE(block.splinePath.size() == 5);
    CHECK(block.splinePath.front()[0] == Catch::Approx(nodes[1].x));
    CHECK(block.splinePath.back()[0] == Catch::Approx(nodes[5].x));
}

TEST_CASE("TBC 10 Hz 2-point flying SMSG_MONSTER_MOVE advances linearly without forward skips", "[tbc][spline][flying]") {
    // Replays captured Black Temple Dragonmaw Wind Reaver (entry 23330) 10 Hz
    // SMSG_MONSTER_MOVE packets (flags=0x300 FLYING_TBC|WALKMODE, pointCount=1,
    // constant 12.75 yd/s). Previously, 2-point Catmull-Rom clamped endpoints
    // ran at 0.5x speed and snapped forward ~0.5 yd on every 100 ms packet.
    struct PacketStep {
        float dtAfterPrevSec;
        glm::vec3 start;
        glm::vec3 dest;
        uint32_t durationMs;
    };
    const PacketStep stream[] = {
        {0.000f, {595.93f, 745.84f, 102.04f}, {610.05f, 734.80f, 103.77f}, 1412},
        {0.072f, {596.96f, 745.03f, 102.17f}, {610.05f, 734.80f, 103.77f}, 1309},
        {0.100f, {597.99f, 744.22f, 102.29f}, {610.05f, 734.80f, 103.77f}, 1205},
        {0.124f, {599.03f, 743.41f, 102.42f}, {607.80f, 731.09f, 103.77f}, 1191},
        {0.095f, {599.80f, 742.34f, 102.54f}, {607.80f, 731.09f, 103.77f}, 1087},
        {0.100f, {600.56f, 741.26f, 102.66f}, {607.80f, 731.09f, 103.77f}, 983},
    };

    Unit drake(0xf130005b22596daeull);
    const glm::vec3 s0 = serverToCanonical(stream[0].start);
    drake.setPosition(s0.x, s0.y, s0.z, 0.0f);

    float maxPacketSnap = 0.0f;
    float minFrameSpeed = 1e9f;
    float maxFrameSpeed = 0.0f;

    for (size_t i = 0; i < std::size(stream); ++i) {
        if (stream[i].dtAfterPrevSec > 0.0f) {
            constexpr int kSubFrames = 6;
            const float subDt = stream[i].dtAfterPrevSec / static_cast<float>(kSubFrames);
            for (int f = 0; f < kSubFrames; ++f) {
                const glm::vec3 before(drake.getX(), drake.getY(), drake.getZ());
                drake.updateMovement(subDt);
                const glm::vec3 after(drake.getX(), drake.getY(), drake.getZ());
                const float spd = glm::length(after - before) / subDt;
                minFrameSpeed = std::min(minFrameSpeed, spd);
                maxFrameSpeed = std::max(maxFrameSpeed, spd);
            }
        }
        const glm::vec3 beforePkt(drake.getX(), drake.getY(), drake.getZ());
        const glm::vec3 sc = serverToCanonical(stream[i].start);
        const glm::vec3 dc = serverToCanonical(stream[i].dest);
        const float yaw = std::atan2(-(dc.y - sc.y), dc.x - sc.x);
        drake.startServerSpline({{sc.x, sc.y, sc.z}, {dc.x, dc.y, dc.z}},
                                yaw, stream[i].durationMs / 1000.0f,
                                /*smooth=*/true, /*followPathFacing=*/true,
                                /*splineFlags=*/0x300u);
        const glm::vec3 afterPkt(drake.getX(), drake.getY(), drake.getZ());
        if (i > 0) {
            maxPacketSnap = std::max(maxPacketSnap, glm::length(afterPkt - beforePkt));
        }
    }

    // Zero discontinuity on 10 Hz refreshes and steady ~12.75 yd/s velocity throughout.
    CHECK(maxPacketSnap < 0.001f);
    CHECK(minFrameSpeed > 11.5f);
    CHECK(maxFrameSpeed < 14.5f);

    // Large server teleport (>2 yd error) must still snap authoritatively.
    const glm::vec3 teleStart = serverToCanonical({650.0f, 700.0f, 120.0f});
    const glm::vec3 teleDest = serverToCanonical({660.0f, 700.0f, 120.0f});
    drake.startServerSpline({{teleStart.x, teleStart.y, teleStart.z},
                             {teleDest.x, teleDest.y, teleDest.z}},
                            0.0f, 1.0f, true, true, 0x300u);
    CHECK(drake.getX() == Catch::Approx(teleStart.x).margin(0.01f));
    CHECK(drake.getY() == Catch::Approx(teleStart.y).margin(0.01f));
    CHECK(drake.getZ() == Catch::Approx(teleStart.z).margin(0.01f));
}
