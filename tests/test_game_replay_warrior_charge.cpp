// TBC 2.4.3 Warrior Charge / Intercept bidirectional spline and movement sync parity.
// Covers:
//   1. Local Warrior Charge / Intercept: pre-cast wire silence, SMSG_CAST_FAILED
//      non-movement, multi-waypoint SMSG_MONSTER_MOVE linear spline follow while
//      control=1, mid-spline packet suppression, dynamic target facing, and silent
//      arrival without post-spline overrun or spurious heartbeats.
//   2. Remote Warrior Charge: exact Deathknell SMSG_MONSTER_MOVE packet decoding
//      (multi-waypoint and 1-segment), stale FALLING/STRAFE/WALKING flag clearing,
//      zero upward/forward dead-reckoning overrun past spline completion, and
//      persistent hasServerSplinePosition() for post-charge floor projection.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "core/coordinates.hpp"
#include "game/movement_handler.hpp"
#include "game/spell_classification.hpp"
#include "game/spline_packet.hpp"

#include <bit>
#include <cmath>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::MovementFlags;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using UF = wowee::game::UF;
namespace coords = wowee::core::coords;
namespace spellclass = wowee::game::spellclass;

namespace {

constexpr uint64_t kRemoteWarrior = 0x0000000000000060ull;
constexpr uint64_t kZombie        = 0xF1300005DD00AF01ull;
constexpr uint32_t kChargeR3      = 11578u;
constexpr uint32_t kInterceptR1   = 20252u;
constexpr uint32_t kEffectCharge  = 96u;
constexpr uint32_t kAttrInitiatesCombat = 0x00000200u;

// CMaNGOS PacketBuilder::WriteCompressed (11-bit X, 11-bit Y, 10-bit Z signed quarter-yard deltas from midpoint).
uint32_t encodePackedDelta(const glm::vec3& midpoint, const glm::vec3& waypoint) {
    const auto packSigned = [](float delta, int bits) -> uint32_t {
        const int32_t minVal = -(1 << (bits - 1));
        const int32_t maxVal = (1 << (bits - 1)) - 1;
        const int32_t q = std::clamp(static_cast<int32_t>(std::lround(delta * 4.0f)), minVal, maxVal);
        const uint32_t mask = (1u << bits) - 1u;
        return static_cast<uint32_t>(q) & mask;
    };
    const glm::vec3 d = midpoint - waypoint;
    return packSigned(d.x, 11) | (packSigned(d.y, 11) << 11) | (packSigned(d.z, 10) << 22);
}

// Builds a TBC 2.4.3 SMSG_MONSTER_MOVE (moveType=3 MonsterMoveFacingTarget, compressed waypoints).
wowee::network::Packet buildTbcFacingTargetMonsterMove(
    uint64_t moverGuid,
    const glm::vec3& startServer,
    uint32_t splineId,
    uint64_t facingTargetGuid,
    uint32_t splineFlags,
    uint32_t durationMs,
    const std::vector<glm::vec3>& intermediateServerWaypoints,
    const glm::vec3& destServer) {
    wowee::network::Packet pkt(wowee::game::wireOpcode(Opcode::SMSG_MONSTER_MOVE));
    pkt.writePackedGuid(moverGuid);
    pkt.writeFloat(startServer.x);
    pkt.writeFloat(startServer.y);
    pkt.writeFloat(startServer.z);
    pkt.writeUInt32(splineId);
    pkt.writeUInt8(3u);  // MonsterMoveFacingTarget
    pkt.writeUInt64(facingTargetGuid);
    pkt.writeUInt32(splineFlags);
    pkt.writeUInt32(durationMs);
    const uint32_t pointCount = static_cast<uint32_t>(intermediateServerWaypoints.size() + 1u);
    pkt.writeUInt32(pointCount);
    pkt.writeFloat(destServer.x);
    pkt.writeFloat(destServer.y);
    pkt.writeFloat(destServer.z);
    const glm::vec3 midpoint = 0.5f * (startServer + destServer);
    for (const auto& wp : intermediateServerWaypoints) {
        const uint32_t packed = encodePackedDelta(midpoint, wp);
        const glm::vec3 decoded = wowee::game::decodePackedDelta(packed, midpoint);
        REQUIRE(decoded.x == Catch::Approx(wp.x).margin(0.26f));
        REQUIRE(decoded.y == Catch::Approx(wp.y).margin(0.26f));
        REQUIRE(decoded.z == Catch::Approx(wp.z).margin(0.26f));
        pkt.writeUInt32(packed);
    }
    return pkt;
}

// Builds a TBC 2.4.3 SMSG_CAST_FAILED packet (spellId u32 + result u8 + castCount u8).
wowee::network::Packet buildTbcCastFailed(uint32_t spellId, uint8_t result, uint8_t castCount) {
    wowee::network::Packet pkt(wowee::game::wireOpcode(Opcode::SMSG_CAST_FAILED));
    pkt.writeUInt32(spellId);
    pkt.writeUInt8(result);
    pkt.writeUInt8(castCount);
    return pkt;
}

// Builds a TBC 2.4.3 SMSG_ATTACKSTART packet (attacker u64 + victim u64).
wowee::network::Packet buildAttackStart(uint64_t attacker, uint64_t victim) {
    wowee::network::Packet pkt(wowee::game::wireOpcode(Opcode::SMSG_ATTACKSTART));
    pkt.writeUInt64(attacker);
    pkt.writeUInt64(victim);
    return pkt;
}

// Builds a TBC server MSG_MOVE_* relay packet (packedGuid + flags + flags2 + time + xyz + o + fallTime).
wowee::network::Packet buildTbcServerMove(Opcode opcode, uint64_t guid, uint32_t flags,
                                          uint32_t timeMs, const glm::vec3& serverPos,
                                          float serverYaw, float jumpVel = 0.0f,
                                          float jumpCos = 1.0f, float jumpSin = 0.0f,
                                          float jumpXY = 0.0f) {
    wowee::network::Packet pkt(wowee::game::wireOpcode(opcode));
    pkt.writePackedGuid(guid);
    pkt.writeUInt32(flags);
    pkt.writeUInt8(0);
    pkt.writeUInt32(timeMs);
    pkt.writeFloat(serverPos.x);
    pkt.writeFloat(serverPos.y);
    pkt.writeFloat(serverPos.z);
    pkt.writeFloat(serverYaw);
    pkt.writeUInt32(0);
    if ((flags & 0x00001000u) != 0) {
        pkt.writeFloat(jumpVel);
        pkt.writeFloat(jumpCos);
        pkt.writeFloat(jumpSin);
        pkt.writeFloat(jumpXY);
    }
    return pkt;
}

size_t countSentClientMoves(const Session& s) {
    size_t count = 0;
    const Opcode moveOps[] = {
        Opcode::MSG_MOVE_START_FORWARD,
        Opcode::MSG_MOVE_START_BACKWARD,
        Opcode::MSG_MOVE_STOP,
        Opcode::MSG_MOVE_START_STRAFE_LEFT,
        Opcode::MSG_MOVE_START_STRAFE_RIGHT,
        Opcode::MSG_MOVE_STOP_STRAFE,
        Opcode::MSG_MOVE_JUMP,
        Opcode::MSG_MOVE_START_TURN_LEFT,
        Opcode::MSG_MOVE_START_TURN_RIGHT,
        Opcode::MSG_MOVE_STOP_TURN,
        Opcode::MSG_MOVE_SET_FACING,
        Opcode::MSG_MOVE_FALL_LAND,
        Opcode::MSG_MOVE_HEARTBEAT,
    };
    for (Opcode op : moveOps) {
        count += s.sent().of(op).size();
    }
    return count;
}

void setupWarriorChargeSession(Session& s) {
    auto& gh = s.handler();
    auto& cache = gh.spellNameCacheRef();
    for (const auto& [id, name] : {std::pair{kChargeR3, "Charge"},
                                   std::pair{kInterceptR1, "Intercept"}}) {
        auto& entry = cache[id];
        entry.name = name;
        entry.maxRange = 25.0f;
        entry.implicitTargetsA[0] = spellclass::kImplicitTargetEnemy;
        entry.effectIds[0] = kEffectCharge;
        entry.attrEx = kAttrInitiatesCombat;
    }
    gh.spellNameCacheLoadedRef() = true;
    gh.setSpellDataResolver([](uint32_t id) {
        wowee::game::GameHandler::SpellDataInfo info;
        if (id == kChargeR3 || id == kInterceptR1) {
            info.minRange = 8.0f;
            info.maxRange = 25.0f;
        }
        return info;
    });

    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 250},
         {UF::UNIT_FIELD_MAXHEALTH, 250},
         {UF::UNIT_FIELD_LEVEL, 30},
         {UF::UNIT_FIELD_COMBATREACH, std::bit_cast<uint32_t>(1.5f)}},
        true));

    s.receive(UpdateObject().tbc().create(
        kZombie, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 120},
         {UF::UNIT_FIELD_MAXHEALTH, 120},
         {UF::UNIT_FIELD_LEVEL, 2},
         {UF::UNIT_FIELD_FACTIONTEMPLATE, 21},
         {UF::UNIT_FIELD_COMBATREACH, std::bit_cast<uint32_t>(1.5f)}}));
    if (auto zombie = std::dynamic_pointer_cast<wowee::game::Unit>(
            gh.getEntityManager().getEntity(kZombie))) {
        zombie->setHostile(true);
    }
}

}  // namespace

TEST_CASE("TBC local Warrior Charge rejected cast sends CMSG_CAST_SPELL without local dash or movement spam",
          "[replay][warrior][charge]") {
    Session s("tbc");
    setupWarriorChargeSession(s);
    auto& gh = s.handler();

    const glm::vec3 startServer(1929.68f, 1609.05f, 83.24f);
    const glm::vec3 zombieServer(1950.47f, 1609.34f, 81.96f);
    const glm::vec3 startCan = coords::serverToCanonical(startServer);
    const glm::vec3 zombieCan = coords::serverToCanonical(zombieServer);

    gh.setPosition(startCan.x, startCan.y, startCan.z);
    gh.setOrientation(0.25f);
    auto zombie = gh.getEntityManager().getEntity(kZombie);
    REQUIRE(zombie != nullptr);
    zombie->setPosition(zombieCan.x, zombieCan.y, zombieCan.z, 0.0f);
    gh.setTarget(kZombie);

    const size_t movesBeforeCast = countSentClientMoves(s);
    gh.castSpell(kChargeR3, kZombie);

    const auto casts = s.sent().of(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);
    auto castPkt = casts[0];
    CHECK(castPkt.readUInt32() == kChargeR3);
    const uint8_t castCount = castPkt.readUInt8();
    CHECK(castCount != 0u);

    // Pre-cast must NOT emit MSG_MOVE_SET_FACING or MSG_MOVE_HEARTBEAT spam.
    CHECK(countSentClientMoves(s) == movesBeforeCast);

    // Server rejects with SMSG_CAST_FAILED (result 129 in TBC).
    // Note: Session's SentPackets socket is intentionally unconnected, so step
    // entity interpolation + timers directly rather than updateNetworking().
    s.receive(buildTbcCastFailed(kChargeR3, 129u, castCount));
    for (int i = 0; i < 20; ++i) {
        gh.updateTimers(0.05f);
        gh.updateEntityInterpolation(0.05f);
    }

    const auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self != nullptr);
    CHECK_FALSE(gh.isPlayerServerSplineActive());
    CHECK(self->getX() == Catch::Approx(startCan.x));
    CHECK(self->getY() == Catch::Approx(startCan.y));
    CHECK(self->getZ() == Catch::Approx(startCan.z));
    CHECK(gh.getMovementInfo().x == Catch::Approx(startCan.x));
    CHECK(gh.getMovementInfo().y == Catch::Approx(startCan.y));
    CHECK(gh.getMovementInfo().z == Catch::Approx(startCan.z));
    CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());
}

TEST_CASE("TBC local Warrior Charge and Intercept follow SMSG_MONSTER_MOVE spline silently and face target",
          "[replay][warrior][charge]") {
    Session s("tbc");
    setupWarriorChargeSession(s);
    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    // Coordinates from Deathknell live log (413939): 3-point path (start + 1 packed waypoint + dest), 764 ms.
    const glm::vec3 startServer(1918.92f, 1601.94f, 84.0966f);
    const glm::vec3 wpServer(1920.885f, 1612.065f, 84.333f);
    const glm::vec3 destServer(1922.85f, 1622.19f, 84.5699f);
    const glm::vec3 zombieServer(1923.01f, 1623.02f, 83.8884f);

    const glm::vec3 startCan = coords::serverToCanonical(startServer);
    const glm::vec3 destCan = coords::serverToCanonical(destServer);
    const glm::vec3 zombieCan = coords::serverToCanonical(zombieServer);

    gh.setPosition(startCan.x, startCan.y, startCan.z);
    gh.setOrientation(0.0f);
    auto zombie = gh.getEntityManager().getEntity(kZombie);
    REQUIRE(zombie != nullptr);
    zombie->setPosition(zombieCan.x, zombieCan.y, zombieCan.z, 0.0f);
    gh.setTarget(kZombie);

    // Player starts running forward right before pressing Charge.
    const uint32_t t0 = mh->currentMovementClockMs() + 200u;
    mh->recordPhysicsSample(t0);
    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE((gh.getMovementInfo().flags & static_cast<uint32_t>(MovementFlags::FORWARD)) != 0u);

    const size_t movesBeforeCharge = countSentClientMoves(s);
    gh.castSpell(kChargeR3, kZombie);
    REQUIRE(s.sent().of(Opcode::CMSG_CAST_SPELL).size() == 1);
    CHECK(countSentClientMoves(s) == movesBeforeCharge);

    // Server starts Charge spline (control remains allowed = 1) and combat swing.
    REQUIRE(gh.isServerMovementAllowed());
    s.receive(buildAttackStart(Session::kPlayerGuid, kZombie));
    s.receive(buildTbcFacingTargetMonsterMove(
        Session::kPlayerGuid, startServer, 101u, kZombie,
        0x00000100u /*Runmode*/, 764u, {wpServer}, destServer));

    auto self = gh.getEntityManager().getEntity(Session::kPlayerGuid);
    REQUIRE(self != nullptr);
    CHECK(gh.isPlayerServerSplineActive());
    CHECK((gh.getMovementInfo().flags & static_cast<uint32_t>(MovementFlags::FORWARD)) == 0u);

    // Mid-spline: constant linear spline travel, target shift updates facing, zero outgoing move packets.
    gh.updateEntityInterpolation(0.382f);  // 50% of 764 ms
    CHECK(gh.isPlayerServerSplineActive());
    CHECK(countSentClientMoves(s) == movesBeforeCharge);
    const glm::vec3 midExpectedCan = coords::serverToCanonical(wpServer);
    CHECK(self->getX() == Catch::Approx(midExpectedCan.x).margin(0.35f));
    CHECK(self->getY() == Catch::Approx(midExpectedCan.y).margin(0.35f));

    // Renderer setPosition calls during an active spline must NOT cancel the entity spline.
    gh.setPosition(self->getX(), self->getY(), self->getZ() - 0.8f);
    CHECK(gh.isPlayerServerSplineActive());

    // Move zombie sideways mid-charge to verify dynamic MonsterMoveFacingTarget tracking.
    const glm::vec3 movedZombieCan = zombieCan + glm::vec3(3.0f, -2.0f, 0.0f);
    zombie->setPosition(movedZombieCan.x, movedZombieCan.y, movedZombieCan.z, 0.0f);

    // Attempt manual heartbeat and sampled due-heartbeats mid-spline: held back silently.
    mh->recordPhysicsSample(t0 + 600u);
    gh.sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    mh->sendDueHeartbeats();
    CHECK(countSentClientMoves(s) == movesBeforeCharge);

    // Complete spline and continue stepping past 2x duration: zero overrun and zero post-arrival spam.
    gh.updateEntityInterpolation(0.400f);  // 782 ms >= 764 ms
    CHECK_FALSE(gh.isPlayerServerSplineActive());
    uint32_t landedFlags = 0;
    CHECK(mh->consumePlayerServerSplineLanding(landedFlags));
    CHECK(landedFlags == 0x00000100u);
    CHECK_FALSE(mh->consumePlayerServerSplineLanding(landedFlags));
    CHECK(self->getX() == Catch::Approx(destCan.x).margin(0.01f));
    CHECK(self->getY() == Catch::Approx(destCan.y).margin(0.01f));
    CHECK(self->getZ() == Catch::Approx(destCan.z).margin(0.01f));
    const float expectedFinalYaw = std::atan2(-(movedZombieCan.y - destCan.y),
                                              movedZombieCan.x - destCan.x);
    CHECK(gh.getMovementInfo().orientation == Catch::Approx(expectedFinalYaw).margin(0.01f));

    for (int i = 0; i < 10; ++i) {
        gh.updateTimers(0.20f);  // Clear 1.5s GCD as well
        gh.updateEntityInterpolation(0.05f);
        mh->recordPhysicsSample(t0 + 800u + static_cast<uint32_t>(i * 50));
        mh->sendDueHeartbeats();
    }
    CHECK(self->getX() == Catch::Approx(destCan.x).margin(0.01f));
    CHECK(self->getY() == Catch::Approx(destCan.y).margin(0.01f));
    CHECK(countSentClientMoves(s) == movesBeforeCharge);

    // Now cast Intercept (20252) WHILE IN COMBAT: must not be blocked by isInCombat(),
    // and must follow a 1-segment (0 intermediate waypoint) SMSG_MONSTER_MOVE spline.
    s.receive(buildAttackStart(kZombie, Session::kPlayerGuid));
    REQUIRE(gh.isInCombat());
    const glm::vec3 farZombieServer(1936.0f, 1632.0f, 83.5f);
    const glm::vec3 interceptDestServer(1934.8f, 1631.1f, 84.5f);
    const glm::vec3 farZombieCan = coords::serverToCanonical(farZombieServer);
    const glm::vec3 interceptDestCan = coords::serverToCanonical(interceptDestServer);
    zombie->setPosition(farZombieCan.x, farZombieCan.y, farZombieCan.z, 0.0f);

    gh.castSpell(kInterceptR1, kZombie);
    REQUIRE(s.sent().of(Opcode::CMSG_CAST_SPELL).size() == 2);
    CHECK(countSentClientMoves(s) == movesBeforeCharge);

    s.receive(buildTbcFacingTargetMonsterMove(
        Session::kPlayerGuid, destServer, 102u, kZombie,
        0x00000100u, 500u, {}, interceptDestServer));
    CHECK(gh.isPlayerServerSplineActive());
    gh.updateEntityInterpolation(0.25f);
    CHECK(gh.isPlayerServerSplineActive());
    gh.updateEntityInterpolation(0.30f);
    CHECK_FALSE(gh.isPlayerServerSplineActive());
    CHECK(self->getX() == Catch::Approx(interceptDestCan.x).margin(0.01f));
    CHECK(self->getY() == Catch::Approx(interceptDestCan.y).margin(0.01f));
    CHECK(countSentClientMoves(s) == movesBeforeCharge);

    // Post-charge manual movement resumes immediately.
    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    CHECK(s.sent().of(Opcode::MSG_MOVE_START_FORWARD).size() == 2);
}

TEST_CASE("TBC remote Warrior Charge SMSG_MONSTER_MOVE clears airborne/strafe flags and stops at endpoint without upward overrun",
          "[replay][warrior][charge][remote]") {
    Session s("tbc");
    setupWarriorChargeSession(s);
    auto& gh = s.handler();

    s.receive(UpdateObject().tbc().create(
        kRemoteWarrior, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 250},
         {UF::UNIT_FIELD_MAXHEALTH, 250},
         {UF::UNIT_FIELD_LEVEL, 30}}));

    auto remote = std::dynamic_pointer_cast<wowee::game::Player>(
        gh.getEntityManager().getEntity(kRemoteWarrior));
    REQUIRE(remote != nullptr);
    remote->groundRunSpeed = 7.0f;
    remote->groundWalkSpeed = 2.5f;
    remote->groundBackSpeed = 4.5f;
    remote->turnRate = 3.14159265f;

    uint32_t trackedRemoteFlags = 0;
    gh.setUnitMoveFlagsCallback([&](uint64_t guid, uint32_t flags, uint32_t mask) {
        if (guid == kRemoteWarrior) {
            trackedRemoteFlags = (trackedRemoteFlags & ~mask) | (flags & mask);
        }
    });

    // Exact coordinates from Deathknell live capture (client.log:430766 / 430829):
    // Legless (0x60) charges Mindless Zombie (0xf1300005dd00af01).
    const glm::vec3 startServer(1936.0811f, 1603.9934f, 82.7718f);
    const glm::vec3 wp0Server(1930.7927f, 1598.1473f, 84.1082f);
    const glm::vec3 destServer(1928.5044f, 1595.8013f, 84.9445f);
    const glm::vec3 zombieServer(1927.35f, 1594.60f, 83.85f);
    const glm::vec3 destCan = coords::serverToCanonical(destServer);
    const glm::vec3 zombieCan = coords::serverToCanonical(zombieServer);

    auto zombie = gh.getEntityManager().getEntity(kZombie);
    REQUIRE(zombie != nullptr);
    zombie->setPosition(zombieCan.x, zombieCan.y, zombieCan.z, 0.0f);

    // Pre-charge: remote player jumped while strafing left & walking (FALLING | STRAFE_LEFT | WALKING).
    constexpr uint32_t kPreChargeFlags =
        static_cast<uint32_t>(MovementFlags::FALLING) |
        static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
        static_cast<uint32_t>(MovementFlags::WALKING);
    s.receive(buildTbcServerMove(Opcode::MSG_MOVE_JUMP, kRemoteWarrior, kPreChargeFlags,
                                 1000u, startServer, 0.0f, -7.9555f, 1.0f, 0.0f, 7.0f));
    REQUIRE(remote->isRemoteAirborne());
    REQUIRE((trackedRemoteFlags & kPreChargeFlags) == kPreChargeFlags);

    // Remote Warrior casts Charge: server broadcasts SMSG_MONSTER_MOVE (dur=421ms, flags=0x100 Runmode).
    s.receive(buildTbcFacingTargetMonsterMove(
        kRemoteWarrior, startServer, 201u, kZombie,
        0x00000100u, 421u, {wp0Server}, destServer));

    // Stale FALLING, STRAFE_LEFT, and WALKING flags must be cleared immediately.
    CHECK_FALSE(remote->isRemoteAirborne());
    CHECK((trackedRemoteFlags & kPreChargeFlags) == 0u);
    CHECK(remote->isServerSplineActive());
    CHECK(remote->hasServerSplinePosition());

    // Step across the 421 ms spline: mid-point at 210 ms, then completion at 430 ms >= 421 ms.
    gh.updateEntityInterpolation(0.210f);
    CHECK(remote->isActivelyMoving());
    CHECK(remote->getZ() < destCan.z);
    gh.updateEntityInterpolation(0.220f);  // 430 ms >= 421 ms

    // Step across [430ms, 1030ms] (spanning the old [421ms, 842ms] overrun window
    // where impliedVZ = +5.16 yd/s previously launched Legless up to Z = 86.35!).
    for (int step = 0; step < 12; ++step) {
        CHECK_FALSE(remote->isActivelyMoving());
        CHECK_FALSE(remote->isEntityMoving());
        CHECK(remote->getX() == Catch::Approx(destCan.x).margin(0.001f));
        CHECK(remote->getY() == Catch::Approx(destCan.y).margin(0.001f));
        CHECK(remote->getZ() == Catch::Approx(destCan.z).margin(0.001f));
        // Must remain true after stopping so Application floor-projects dest.z (terrain + 1.0)
        // onto actual terrain height (83.85) while standing still auto-attacking!
        CHECK(remote->hasServerSplinePosition());
        gh.updateEntityInterpolation(0.050f);
    }

    const float expectedFacing = std::atan2(-(zombieCan.y - destCan.y), zombieCan.x - destCan.x);
    CHECK(remote->getOrientation() == Catch::Approx(expectedFacing).margin(0.01f));

    // Replay 1-segment (pcnt=1, 0 intermediate waypoints) Deathknell Charge (client.log:418553):
    // must ALSO clamp with zero horizontal/vertical overrun!
    const glm::vec3 start1Server(1935.9388f, 1614.4438f, 81.7724f);
    const glm::vec3 dest1Server(1928.1654f, 1603.2649f, 84.3613f);
    const glm::vec3 dest1Can = coords::serverToCanonical(dest1Server);
    s.receive(buildTbcFacingTargetMonsterMove(
        kRemoteWarrior, start1Server, 202u, kZombie,
        0x00000100u, 514u, {}, dest1Server));
    CHECK(remote->isServerSplineActive());
    gh.updateEntityInterpolation(0.550f);
    CHECK_FALSE(remote->isActivelyMoving());
    CHECK_FALSE(remote->isEntityMoving());
    CHECK(remote->getX() == Catch::Approx(dest1Can.x).margin(0.001f));
    CHECK(remote->getY() == Catch::Approx(dest1Can.y).margin(0.001f));
    CHECK(remote->getZ() == Catch::Approx(dest1Can.z).margin(0.001f));
    CHECK(remote->hasServerSplinePosition());

    // When the remote player finally moves manually (client.log:422449, real ground Z = 83.3611),
    // client-authored ground prediction takes over and clears hasServerSplinePosition().
    const glm::vec3 realGroundServer(1928.1654f, 1603.2649f, 83.3611f);
    s.receive(buildTbcServerMove(
        Opcode::MSG_MOVE_START_FORWARD, kRemoteWarrior,
        static_cast<uint32_t>(MovementFlags::FORWARD), 22000u, realGroundServer, 4.0f));
    CHECK_FALSE(remote->hasServerSplinePosition());
    CHECK(remote->isActivelyMoving());
}
