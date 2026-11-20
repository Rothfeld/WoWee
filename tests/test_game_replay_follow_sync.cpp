// TBC 2.4.3 movement packet clock monotonicity, opposing-flag mutual exclusion,
// force-speed ACK wire yaw conversion, and GameHandler /follow state invariants.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "core/coordinates.hpp"
#include "game/movement_handler.hpp"

#include <cmath>

using replay::Session;
using replay::UpdateObject;
using wowee::game::MovementFlags;
using wowee::game::MovementInfo;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using UF = wowee::game::UF;

namespace {

struct WireMovement {
    uint32_t flags = 0;
    uint8_t flags2 = 0;
    uint32_t time = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;
};

// Reads a TBC client MSG_MOVE_* payload (no leading packed GUID in 2.4.3).
WireMovement readTbcClientMove(wowee::network::Packet pkt) {
    WireMovement m;
    m.flags = pkt.readUInt32();
    m.flags2 = pkt.readUInt8();
    m.time = pkt.readUInt32();
    m.x = pkt.readFloat();
    m.y = pkt.readFloat();
    m.z = pkt.readFloat();
    m.orientation = pkt.readFloat();
    return m;
}

// Builds a TBC server MSG_MOVE_* broadcast packet (packed GUID + TBC MovementInfo).
wowee::network::Packet buildTbcServerMove(Opcode opcode, uint64_t guid, uint32_t flags,
                                          uint32_t timeMs, float serverX, float serverY,
                                          float serverZ, float serverYaw) {
    wowee::network::Packet pkt(wowee::game::wireOpcode(opcode));
    pkt.writePackedGuid(guid);
    pkt.writeUInt32(flags);
    pkt.writeUInt8(0);  // flags2 (1 byte in TBC)
    pkt.writeUInt32(timeMs);
    pkt.writeFloat(serverX);
    pkt.writeFloat(serverY);
    pkt.writeFloat(serverZ);
    pkt.writeFloat(serverYaw);
    pkt.writeUInt32(0); // fallTime
    return pkt;
}

}  // namespace

TEST_CASE("TBC SMSG_TIME_SYNC_REQ and suppressed sends do not advance movement packet clock",
          "[replay][movement][timesync]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    gh.setPosition(-9460.0f, 68.0f, 56.6f);
    gh.setOrientation(0.5f);

    const uint32_t t0 = mh->currentMovementClockMs() + 500u;
    mh->recordPhysicsSample(t0);
    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    const auto starts = s.sent().of(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(starts.size() == 1);
    const auto m0 = readTbcClientMove(starts[0]);
    CHECK(m0.time == t0);

    // Server sends SMSG_TIME_SYNC_REQ mid-run. Answering CMSG_TIME_SYNC_RESP
    // must report the live clock without pushing lastMovementTimestampMs_ ahead
    // of the next deterministic physics sample.
    wowee::network::Packet syncReq(wowee::game::wireOpcode(Opcode::SMSG_TIME_SYNC_REQ));
    syncReq.writeUInt32(7u);
    s.receive(syncReq);
    const auto syncResps = s.sent().of(Opcode::CMSG_TIME_SYNC_RESP);
    REQUIRE(syncResps.size() == 1);
    {
        auto resp = syncResps[0];
        CHECK(resp.readUInt32() == 7u);
    }

    // Duplicate MSG_MOVE_SET_FACING at the exact same heading is suppressed and
    // must roll back any timestamp reservation.
    mh->recordPhysicsSample(t0 + 16u);
    gh.sendMovement(Opcode::MSG_MOVE_SET_FACING);
    CHECK(s.sent().of(Opcode::MSG_MOVE_SET_FACING).empty());

    mh->recordPhysicsSample(t0 + 33u);
    gh.sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
    const auto heartbeats = s.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(heartbeats.size() == 1);
    const auto m1 = readTbcClientMove(heartbeats[0]);
    CHECK(m1.time == t0 + 33u);

    // Stop moving, then verify sendDueHeartbeats while stationary sends nothing
    // and leaves lastMovementTimestampMs_ untouched.
    mh->recordPhysicsSample(t0 + 66u);
    gh.sendMovement(Opcode::MSG_MOVE_STOP);
    REQUIRE(s.sent().of(Opcode::MSG_MOVE_STOP).size() == 1);

    mh->recordPhysicsSample(t0 + 600u);
    mh->sendDueHeartbeats();
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).size() == 1);

    mh->recordPhysicsSample(t0 + 620u);
    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    const auto startsAfter = s.sent().of(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(startsAfter.size() == 2);
    CHECK(readTbcClientMove(startsAfter[1]).time == t0 + 620u);
}

TEST_CASE("TBC MovementHandler clears opposing movement and turn flags on direct switch",
          "[replay][movement][flags]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    auto& gh = s.handler();
    gh.setPosition(-9460.0f, 68.0f, 56.6f);
    gh.setOrientation(0.0f);

    constexpr uint32_t kFwd = static_cast<uint32_t>(MovementFlags::FORWARD);
    constexpr uint32_t kBack = static_cast<uint32_t>(MovementFlags::BACKWARD);
    constexpr uint32_t kStrafeL = static_cast<uint32_t>(MovementFlags::STRAFE_LEFT);
    constexpr uint32_t kStrafeR = static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT);
    constexpr uint32_t kTurnL = static_cast<uint32_t>(MovementFlags::TURN_LEFT);
    constexpr uint32_t kTurnR = static_cast<uint32_t>(MovementFlags::TURN_RIGHT);

    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    gh.sendMovement(Opcode::MSG_MOVE_START_TURN_LEFT);
    gh.sendMovement(Opcode::MSG_MOVE_START_TURN_RIGHT);
    const auto turnRights = s.sent().of(Opcode::MSG_MOVE_START_TURN_RIGHT);
    REQUIRE(turnRights.size() == 1);
    const uint32_t turnFlags = readTbcClientMove(turnRights[0]).flags;
    CHECK((turnFlags & (kTurnL | kTurnR)) == kTurnR);

    gh.sendMovement(Opcode::MSG_MOVE_START_BACKWARD);
    const auto backs = s.sent().of(Opcode::MSG_MOVE_START_BACKWARD);
    REQUIRE(backs.size() == 1);
    const uint32_t moveFlags = readTbcClientMove(backs[0]).flags;
    CHECK((moveFlags & (kFwd | kBack)) == kBack);

    gh.sendMovement(Opcode::MSG_MOVE_START_STRAFE_LEFT);
    gh.sendMovement(Opcode::MSG_MOVE_START_STRAFE_RIGHT);
    const auto strafeRights = s.sent().of(Opcode::MSG_MOVE_START_STRAFE_RIGHT);
    REQUIRE(strafeRights.size() == 1);
    const uint32_t strafeFlags = readTbcClientMove(strafeRights[0]).flags;
    CHECK((strafeFlags & (kStrafeL | kStrafeR)) == kStrafeR);
}

TEST_CASE("TBC force run speed ACK converts canonical orientation to server yaw",
          "[replay][movement][forceack]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    constexpr float kCanonicalYaw = 0.35f;
    gh.setPosition(-9460.0f, 68.0f, 56.6f);
    gh.setOrientation(kCanonicalYaw);
    const uint32_t sampleTs = mh->currentMovementClockMs() + 250u;
    mh->recordPhysicsSample(sampleTs);

    // TBC SMSG_FORCE_RUN_SPEED_CHANGE: packedGuid + moveEvent(u32) + newSpeedByte(u8) + speed(float)
    wowee::network::Packet forceSpeed(wowee::game::wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE));
    forceSpeed.writePackedGuid(Session::kPlayerGuid);
    forceSpeed.writeUInt32(3u);
    forceSpeed.writeUInt8(1u);
    forceSpeed.writeFloat(8.5f);
    s.receive(forceSpeed);

    const auto acks = s.sent().of(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK);
    REQUIRE(acks.size() == 1);
    auto ack = acks[0];
    // CMaNGOS TBC reads full 8-byte ObjectGuid on CMSG_FORCE_*_ACK.
    CHECK(ack.readUInt64() == Session::kPlayerGuid);
    CHECK(ack.readUInt32() == 3u);
    const auto wireMove = readTbcClientMove(ack);
    const float expectedServerYaw = wowee::core::coords::canonicalToServerYaw(kCanonicalYaw);
    CHECK(wireMove.orientation == Catch::Approx(expectedServerYaw).margin(1e-4f));
    CHECK(wireMove.time == sampleTs);
}

TEST_CASE("GameHandler /follow tracks target walk mode and hysteresis without start-stop chatter",
          "[replay][follow]") {
    class ConnectedSentPackets : public replay::SentPackets {
    public:
        [[nodiscard]] bool isConnected() const override { return true; }
        void update() override {}
    };

    constexpr uint64_t kLeaderGuid = 0x0000000000000099ull;
    Session s("tbc");
    auto& gh = s.handler();
    auto connectedSocket = std::make_unique<ConnectedSentPackets>();
    auto* sent = connectedSocket.get();
    wowee::game::GameHandlerTestAccess::setSocket(gh, std::move(connectedSocket));

    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    s.receive(UpdateObject().tbc().create(kLeaderGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}}));

    constexpr float kBaseX = -9460.0f;
    constexpr float kBaseY = 68.0f;
    constexpr float kBaseZ = 56.6f;
    gh.setPosition(kBaseX, kBaseY, kBaseZ);
    gh.setOrientation(0.0f);

    const auto leader = gh.getEntityManager().getEntity(kLeaderGuid);
    REQUIRE(leader != nullptr);
    // UpdateObject().create() uses STATIONARY_POSITION without UPDATEFLAG_LIVING speeds.
    leader->groundWalkSpeed = 2.5f;
    leader->groundRunSpeed = 7.0f;
    leader->groundBackSpeed = 4.5f;
    leader->turnRate = 3.14159265f;
    leader->setPosition(kBaseX + 3.0f, kBaseY, kBaseZ, 0.0f);

    auto sendLeaderCanonical = [&](Opcode op, uint32_t flags, uint32_t timeMs,
                                   float cx, float cy, float cz, float canonYaw) {
        const glm::vec3 srv = wowee::core::coords::canonicalToServer(glm::vec3(cx, cy, cz));
        const float srvYaw = wowee::core::coords::canonicalToServerYaw(canonYaw);
        s.receive(buildTbcServerMove(op, kLeaderGuid, flags, timeMs, srv.x, srv.y, srv.z, srvYaw));
    };

    // Place stationary leader 3.0 yd away along canonical +X:
    // inside [2.65, 3.75] hysteresis band while stationary, so /follow stays put initially.
    sendLeaderCanonical(Opcode::MSG_MOVE_STOP, static_cast<uint32_t>(MovementFlags::WALKING),
                        1100u, kBaseX + 3.0f, kBaseY, kBaseZ, 0.0f);
    for (int i = 0; i < 4; ++i) gh.update(0.02f);

    gh.followTarget(kLeaderGuid);
    CHECK_FALSE(gh.isFollowTargetMoving());
    CHECK_FALSE(gh.isFollowTargetWalking());
    gh.update(0.016f);
    CHECK(sent->of(Opcode::MSG_MOVE_START_FORWARD).empty());

    // Leader starts WALKING away diagonally to (+4.0, +2.5) (~4.72 yd > 3.75 yd):
    // GameHandler::update refreshes followTargetMoving_/Walking_ and emits START_FORWARD
    // carrying the updated heading toward the leader!
    constexpr uint32_t kWalkForward =
        static_cast<uint32_t>(MovementFlags::FORWARD) |
        static_cast<uint32_t>(MovementFlags::WALKING);
    sendLeaderCanonical(Opcode::MSG_MOVE_START_FORWARD, kWalkForward,
                        1500u, kBaseX + 4.0f, kBaseY + 2.5f, kBaseZ, -0.55f);
    for (int i = 0; i < 8; ++i) gh.update(0.025f);
    CHECK(gh.isFollowTargetMoving());
    CHECK(gh.isFollowTargetWalking());

    const auto starts = sent->of(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(starts.size() == 1);
    const float expectedCanonicalYaw =
        std::atan2(-(leader->getY() - kBaseY),
                   leader->getX() - kBaseX);
    const float expectedServerYaw = wowee::core::coords::canonicalToServerYaw(expectedCanonicalYaw);
    const auto startWire = readTbcClientMove(starts[0]);
    CHECK(std::abs(wowee::core::coords::normalizeAngleRad(startWire.orientation - expectedServerYaw)) < 0.25f);

    // Stop leader and step player to 3.1 yd away (inside [2.65, 3.75] band):
    // must NOT chatter MSG_MOVE_STOP while already moving!
    sendLeaderCanonical(Opcode::MSG_MOVE_STOP, 0u,
                        1700u, kBaseX + 4.5f, kBaseY + 2.8f, kBaseZ, -0.55f);
    for (int i = 0; i < 8; ++i) gh.update(0.025f);
    const glm::vec3 leaderCanon(leader->getX(), leader->getY(), leader->getZ());
    const glm::vec2 dir = glm::normalize(glm::vec2(leaderCanon.x - kBaseX, leaderCanon.y - kBaseY));
    gh.setPosition(leaderCanon.x - dir.x * 3.1f, leaderCanon.y - dir.y * 3.1f, kBaseZ);
    gh.update(0.016f);
    CHECK(sent->of(Opcode::MSG_MOVE_STOP).empty());

    // Step player inside stop threshold (2.3 yd < 2.65 yd): cleanly emits MSG_MOVE_STOP once.
    gh.setPosition(leaderCanon.x - dir.x * 2.3f, leaderCanon.y - dir.y * 2.3f, kBaseZ);
    gh.update(0.016f);
    CHECK(sent->of(Opcode::MSG_MOVE_STOP).size() == 1);
}

TEST_CASE("TBC remote player wall-blocked heartbeats clamp dead-reckoning instead of phasing through wall",
          "[replay][movement][follow]") {
    constexpr uint64_t kRemoteGuid = 0x0000000000000077ull;
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    s.receive(UpdateObject().tbc().create(kRemoteGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}}));

    auto& gh = s.handler();
    const auto remote = gh.getEntityManager().getEntity(kRemoteGuid);
    REQUIRE(remote != nullptr);
    remote->groundRunSpeed = 7.0f;
    remote->groundWalkSpeed = 2.5f;
    remote->turnRate = 3.14159265f;
    remote->setPosition(34.5f, -9460.2f, 56.95f, 3.14159265f);

    constexpr uint32_t kForward = static_cast<uint32_t>(MovementFlags::FORWARD);
    // Remote player presses W facing South (-X canonical => serverYaw = 4.71239 rad),
    // blocked by a wall along -X so they only slide +0.42 yd along -Y in 500 ms.
    s.receive(buildTbcServerMove(Opcode::MSG_MOVE_HEARTBEAT, kRemoteGuid, kForward,
                                 10000u, -9460.2f, 34.50f, 56.95f, 4.712389f));
    s.receive(buildTbcServerMove(Opcode::MSG_MOVE_HEARTBEAT, kRemoteGuid, kForward,
                                 10500u, -9459.78f, 34.45f, 56.95f, 4.712389f));
    for (int i = 0; i < 8; ++i) {
        gh.updateEntityInterpolation(0.05f);  // 400 ms of prediction
    }
    // Without wall-blocked clamping, canonical X (server Y = 34.45) would extrapolate
    // to 34.45 - 7.0 * 0.40 = 31.65 yd (2.8 yd inside the wall!). With measured-velocity
    // clamping it stays within ~0.15 yd of the wall at 34.41 yd.
    CHECK(remote->getX() > 34.20f);
    CHECK(remote->getX() < 34.60f);
}
