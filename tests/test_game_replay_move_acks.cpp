// TBC 2.4.3 server->client forced movement/speed/state orders and client->server
// CMSG_*_ACK / MSG_MOVE_TELEPORT_ACK wire-format and GUID-filtering replay tests.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "core/coordinates.hpp"
#include "game/movement_handler.hpp"
#include "game/spell_handler.hpp"
#include "rendering/camera_controller.hpp"

#include <cmath>
#include <limits>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::MovementFlags;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using UF = wowee::game::UF;

namespace {

struct TbcMovementPayload {
    uint32_t flags = 0;
    uint8_t flags2 = 0;
    uint32_t time = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float orientation = 0.0f;
    float pitch = 0.0f;
    bool hasPitch = false;
    uint32_t fallTime = 0;
    float zspeed = 0.0f;
    float cosAngle = 0.0f;
    float sinAngle = 0.0f;
    float xyspeed = 0.0f;
    bool hasJump = false;
};

TbcMovementPayload readTbcAckMovementInfo(wowee::network::Packet& pkt) {
    TbcMovementPayload m;
    m.flags = pkt.readUInt32();
    m.flags2 = pkt.readUInt8();
    m.time = pkt.readUInt32();
    m.x = pkt.readFloat();
    m.y = pkt.readFloat();
    m.z = pkt.readFloat();
    m.orientation = pkt.readFloat();

    constexpr uint32_t kSwimOrFly =
        static_cast<uint32_t>(MovementFlags::SWIMMING) |
        static_cast<uint32_t>(MovementFlags::FLYING);
    if ((m.flags & kSwimOrFly) != 0) {
        m.pitch = pkt.readFloat();
        m.hasPitch = true;
    }

    m.fallTime = pkt.readUInt32();
    if ((m.flags & static_cast<uint32_t>(MovementFlags::FALLING)) != 0) {
        m.zspeed = pkt.readFloat();
        m.cosAngle = pkt.readFloat();
        m.sinAngle = pkt.readFloat();
        m.xyspeed = pkt.readFloat();
        m.hasJump = true;
    }
    return m;
}

void spawnTbcPlayerAt(Session& s, float canonicalX, float canonicalY, float canonicalZ, float canonicalYaw) {
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    s.handler().setPosition(canonicalX, canonicalY, canonicalZ);
    s.handler().setOrientation(canonicalYaw);
}

}  // namespace

TEST_CASE("TBC 2.4.3 movement order and ACK wire opcodes match build 8606 table",
          "[replay][movement][tbc][opcodes]") {
    Session s("tbc");
    using wowee::game::wireOpcode;

    CHECK(wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE) == 0x0E2);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK) == 0x0E3);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE) == 0x0E4);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK) == 0x0E5);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE) == 0x0E6);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK) == 0x0E7);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_WALK_SPEED_CHANGE) == 0x2DA);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK) == 0x2DB);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE) == 0x2DC);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK) == 0x2DD);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_TURN_RATE_CHANGE) == 0x2DE);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK) == 0x2DF);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE) == 0x381);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK) == 0x382);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE) == 0x383);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK) == 0x384);

    CHECK(wireOpcode(Opcode::SMSG_FORCE_MOVE_ROOT) == 0x0E8);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_MOVE_ROOT_ACK) == 0x0E9);
    CHECK(wireOpcode(Opcode::SMSG_FORCE_MOVE_UNROOT) == 0x0EA);
    CHECK(wireOpcode(Opcode::CMSG_FORCE_MOVE_UNROOT_ACK) == 0x0EB);

    CHECK(wireOpcode(Opcode::SMSG_MOVE_WATER_WALK) == 0x0DE);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_LAND_WALK) == 0x0DF);
    CHECK(wireOpcode(Opcode::CMSG_MOVE_WATER_WALK_ACK) == 0x2D0);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_FEATHER_FALL) == 0x0F2);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_NORMAL_FALL) == 0x0F3);
    CHECK(wireOpcode(Opcode::CMSG_MOVE_FEATHER_FALL_ACK) == 0x2CF);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_SET_HOVER) == 0x0F4);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_UNSET_HOVER) == 0x0F5);
    CHECK(wireOpcode(Opcode::CMSG_MOVE_HOVER_ACK) == 0x0F6);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_SET_CAN_FLY) == 0x343);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_UNSET_CAN_FLY) == 0x344);
    CHECK(wireOpcode(Opcode::CMSG_MOVE_SET_CAN_FLY_ACK) == 0x345);
    CHECK(wireOpcode(Opcode::SMSG_MOVE_KNOCK_BACK) == 0x0EF);
    CHECK(wireOpcode(Opcode::CMSG_MOVE_KNOCK_BACK_ACK) == 0x0F0);
    CHECK(wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK) == 0x0C7);
}

TEST_CASE("TBC SMSG_FORCE_*_SPEED_CHANGE echoes exact 41-byte CMSG_FORCE_*_SPEED_CHANGE_ACK",
          "[replay][movement][tbc][forceack]") {
    Session s("tbc");
    constexpr float kCanX = -8913.25f;
    constexpr float kCanY = 554.75f;
    constexpr float kCanZ = 93.5f;
    constexpr float kCanYaw = 0.62f;
    spawnTbcPlayerAt(s, kCanX, kCanY, kCanZ, kCanYaw);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    const glm::vec3 expectedServerPos =
        wowee::core::coords::canonicalToServer(glm::vec3(kCanX, kCanY, kCanZ));
    const float expectedServerYaw = wowee::core::coords::canonicalToServerYaw(kCanYaw);

    struct SpeedCase {
        Opcode serverOp;
        Opcode ackOp;
        bool hasRunTrackingByte;
        float speed;
        uint32_t counter;
        float (wowee::game::MovementHandler::*getter)() const;
    };

    const std::vector<SpeedCase> cases = {
        {Opcode::SMSG_FORCE_WALK_SPEED_CHANGE, Opcode::CMSG_FORCE_WALK_SPEED_CHANGE_ACK,
         false, 0.025f, 41u, &wowee::game::MovementHandler::getServerWalkSpeed},
        {Opcode::SMSG_FORCE_RUN_SPEED_CHANGE, Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK,
         true, 35.0f, 42u, &wowee::game::MovementHandler::getServerRunSpeed},
        {Opcode::SMSG_FORCE_RUN_BACK_SPEED_CHANGE, Opcode::CMSG_FORCE_RUN_BACK_SPEED_CHANGE_ACK,
         false, 4.5f, 43u, &wowee::game::MovementHandler::getServerRunBackSpeed},
        {Opcode::SMSG_FORCE_SWIM_SPEED_CHANGE, Opcode::CMSG_FORCE_SWIM_SPEED_CHANGE_ACK,
         false, 4.722222f, 44u, &wowee::game::MovementHandler::getServerSwimSpeed},
        {Opcode::SMSG_FORCE_SWIM_BACK_SPEED_CHANGE, Opcode::CMSG_FORCE_SWIM_BACK_SPEED_CHANGE_ACK,
         false, 2.5f, 45u, &wowee::game::MovementHandler::getServerSwimBackSpeed},
        {Opcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE, Opcode::CMSG_FORCE_FLIGHT_SPEED_CHANGE_ACK,
         false, 350.0f, 51u, &wowee::game::MovementHandler::getServerFlightSpeed},
        {Opcode::SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE, Opcode::CMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE_ACK,
         false, 22.5f, 52u, &wowee::game::MovementHandler::getServerFlightBackSpeed},
        {Opcode::SMSG_FORCE_TURN_RATE_CHANGE, Opcode::CMSG_FORCE_TURN_RATE_CHANGE_ACK,
         false, 3.1415927f, 53u, &wowee::game::MovementHandler::getServerTurnRate},
    };

    uint32_t sampleTs = mh->currentMovementClockMs() + 100u;
    for (const auto& tc : cases) {
        mh->recordPhysicsSample(sampleTs);
        wowee::network::Packet order(wowee::game::wireOpcode(tc.serverOp));
        order.writePackedGuid(Session::kPlayerGuid);
        order.writeUInt32(tc.counter);
        if (tc.hasRunTrackingByte) {
            order.writeUInt8(1u);
        }
        order.writeFloat(tc.speed);
        s.receive(order);

        const auto acks = s.sent().of(tc.ackOp);
        REQUIRE(acks.size() == 1);
        auto ack = acks[0];
        // uint64 guid(8) + uint32 counter(4) + TBC MovementInfo(29) + float speed(4) = 45 bytes
        REQUIRE(ack.getSize() == 45u);
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == tc.counter);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK(move.time == sampleTs);
        CHECK(move.x == Catch::Approx(expectedServerPos.x).margin(1e-3f));
        CHECK(move.y == Catch::Approx(expectedServerPos.y).margin(1e-3f));
        CHECK(move.z == Catch::Approx(expectedServerPos.z).margin(1e-3f));
        CHECK(move.orientation == Catch::Approx(expectedServerYaw).margin(1e-4f));
        CHECK(ack.readFloat() == Catch::Approx(tc.speed));
        CHECK(ack.getRemainingSize() == 0u);
        CHECK((mh->*(tc.getter))() == Catch::Approx(tc.speed));
        sampleTs += 20u;
    }

    // Invalid NaN / negative speeds must be rejected without sending a corrupt ACK.
    for (float badSpeed : {std::numeric_limits<float>::quiet_NaN(), -1.0f}) {
        wowee::network::Packet bad(wowee::game::wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE));
        bad.writePackedGuid(Session::kPlayerGuid);
        bad.writeUInt32(99u);
        bad.writeUInt8(0u);
        bad.writeFloat(badSpeed);
        s.receive(bad);
    }
    CHECK(s.sent().of(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK).size() == 1u);
}

TEST_CASE("TBC SMSG_MOVE_* flag toggles append uint32 isApplied and sanitize airborne flight flags",
          "[replay][movement][tbc][flagack]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, 1200.0f, -4400.0f, 35.0f, -0.4f);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    struct FlagPairCase {
        Opcode setOp;
        Opcode unsetOp;
        Opcode ackOp;
        MovementFlags flag;
        uint32_t setCounter;
        uint32_t unsetCounter;
    };

    const std::vector<FlagPairCase> pairs = {
        {Opcode::SMSG_MOVE_FEATHER_FALL, Opcode::SMSG_MOVE_NORMAL_FALL,
         Opcode::CMSG_MOVE_FEATHER_FALL_ACK, MovementFlags::FEATHER_FALL, 10u, 11u},
        {Opcode::SMSG_MOVE_WATER_WALK, Opcode::SMSG_MOVE_LAND_WALK,
         Opcode::CMSG_MOVE_WATER_WALK_ACK, MovementFlags::WATER_WALK, 12u, 13u},
        {Opcode::SMSG_MOVE_SET_HOVER, Opcode::SMSG_MOVE_UNSET_HOVER,
         Opcode::CMSG_MOVE_HOVER_ACK, MovementFlags::HOVER, 14u, 15u},
        {Opcode::SMSG_MOVE_SET_CAN_FLY, Opcode::SMSG_MOVE_UNSET_CAN_FLY,
         Opcode::CMSG_MOVE_SET_CAN_FLY_ACK, MovementFlags::CAN_FLY, 16u, 17u},
    };

    for (const auto& pc : pairs) {
        const uint32_t flagBit = static_cast<uint32_t>(pc.flag);

        wowee::network::Packet setPkt(wowee::game::wireOpcode(pc.setOp));
        setPkt.writePackedGuid(Session::kPlayerGuid);
        setPkt.writeUInt32(pc.setCounter);
        s.receive(setPkt);
        CHECK((mh->getMovementInfo().flags & flagBit) == flagBit);

        if (pc.flag == MovementFlags::CAN_FLY) {
            // Simulate taking off while CAN_FLY is granted, then receiving SMSG_MOVE_UNSET_CAN_FLY
            // mid-air: FLYING and ASCENDING must be cleared before CMSG_MOVE_SET_CAN_FLY_ACK is serialized.
            mh->getMovementInfoMut().flags |=
                static_cast<uint32_t>(MovementFlags::FLYING) |
                static_cast<uint32_t>(MovementFlags::ASCENDING);
        }

        wowee::network::Packet unsetPkt(wowee::game::wireOpcode(pc.unsetOp));
        unsetPkt.writePackedGuid(Session::kPlayerGuid);
        unsetPkt.writeUInt32(pc.unsetCounter);
        s.receive(unsetPkt);
        CHECK((mh->getMovementInfo().flags & flagBit) == 0u);

        const auto acks = s.sent().of(pc.ackOp);
        REQUIRE(acks.size() == 2u);

        // Verify SET ACK: uint64 guid(8) + uint32 counter(4) + MovementInfo(29) + uint32 isApplied(4) = 45
        {
            auto ackSet = acks[0];
            REQUIRE(ackSet.getSize() == 45u);
            CHECK(ackSet.readUInt64() == Session::kPlayerGuid);
            CHECK(ackSet.readUInt32() == pc.setCounter);
            const auto moveSet = readTbcAckMovementInfo(ackSet);
            CHECK((moveSet.flags & flagBit) == flagBit);
            CHECK_FALSE(moveSet.hasPitch);
            CHECK(ackSet.readUInt32() == 1u);
            CHECK(ackSet.getRemainingSize() == 0u);
        }

        // Verify UNSET ACK: uint32 isApplied == 0, flag cleared, and for UNSET_CAN_FLY no FLYING bit/pitch
        {
            auto ackUnset = acks[1];
            REQUIRE(ackUnset.getSize() == 45u);
            CHECK(ackUnset.readUInt64() == Session::kPlayerGuid);
            CHECK(ackUnset.readUInt32() == pc.unsetCounter);
            const auto moveUnset = readTbcAckMovementInfo(ackUnset);
            CHECK((moveUnset.flags & flagBit) == 0u);
            CHECK((moveUnset.flags & static_cast<uint32_t>(MovementFlags::FLYING)) == 0u);
            CHECK((moveUnset.flags & static_cast<uint32_t>(MovementFlags::ASCENDING)) == 0u);
            CHECK_FALSE(moveUnset.hasPitch);
            CHECK(ackUnset.readUInt32() == 0u);
            CHECK(ackUnset.getRemainingSize() == 0u);
        }
    }
}

TEST_CASE("TBC SMSG_FORCE_MOVE_ROOT clears moving flags before CMSG_FORCE_MOVE_ROOT_ACK",
          "[replay][movement][tbc][rootack]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -1500.0f, -2600.0f, 95.0f, 1.1f);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    gh.sendMovement(Opcode::MSG_MOVE_START_STRAFE_LEFT);
    REQUIRE(mh->getMovementInfo().hasFlag(MovementFlags::FORWARD));
    REQUIRE(mh->getMovementInfo().hasFlag(MovementFlags::STRAFE_LEFT));

    wowee::network::Packet rootPkt(wowee::game::wireOpcode(Opcode::SMSG_FORCE_MOVE_ROOT));
    rootPkt.writePackedGuid(Session::kPlayerGuid);
    rootPkt.writeUInt32(21u);
    s.receive(rootPkt);

    CHECK(mh->isPlayerRooted());
    const auto rootAcks = s.sent().of(Opcode::CMSG_FORCE_MOVE_ROOT_ACK);
    REQUIRE(rootAcks.size() == 1u);
    {
        auto ack = rootAcks[0];
        // uint64 guid(8) + uint32 counter(4) + MovementInfo(29) = 41 bytes
        REQUIRE(ack.getSize() == 41u);
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == 21u);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK((move.flags & static_cast<uint32_t>(MovementFlags::ROOT)) != 0u);
        constexpr uint32_t kMovingMask =
            static_cast<uint32_t>(MovementFlags::FORWARD) |
            static_cast<uint32_t>(MovementFlags::BACKWARD) |
            static_cast<uint32_t>(MovementFlags::STRAFE_LEFT) |
            static_cast<uint32_t>(MovementFlags::STRAFE_RIGHT) |
            static_cast<uint32_t>(MovementFlags::PITCH_UP) |
            static_cast<uint32_t>(MovementFlags::PITCH_DOWN);
        CHECK((move.flags & kMovingMask) == 0u);
        CHECK(ack.getRemainingSize() == 0u);
    }

    wowee::network::Packet unrootPkt(wowee::game::wireOpcode(Opcode::SMSG_FORCE_MOVE_UNROOT));
    unrootPkt.writePackedGuid(Session::kPlayerGuid);
    unrootPkt.writeUInt32(22u);
    s.receive(unrootPkt);

    CHECK_FALSE(mh->isPlayerRooted());
    const auto unrootAcks = s.sent().of(Opcode::CMSG_FORCE_MOVE_UNROOT_ACK);
    REQUIRE(unrootAcks.size() == 1u);
    {
        auto ack = unrootAcks[0];
        REQUIRE(ack.getSize() == 41u);
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == 22u);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK((move.flags & static_cast<uint32_t>(MovementFlags::ROOT)) == 0u);
        CHECK(ack.getRemainingSize() == 0u);
    }
}

TEST_CASE("TBC SMSG_MOVE_KNOCK_BACK echoes exact knockback velocity in ACK without fake MSG_MOVE_JUMP",
          "[replay][movement][tbc][knockback]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -9120.0f, 1980.0f, 75.0f, 0.25f);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    // Wire a real CameraController the same way Application wires knockBackCallback and movementCallback.
    wowee::rendering::CameraController camCtrl(nullptr);
    camCtrl.setMovementCallback([&gh](uint32_t op) {
        gh.sendMovement(static_cast<Opcode>(op));
    });
    gh.setKnockBackCallback([&camCtrl](float vcos, float vsin, float hspeed, float vspeed) {
        camCtrl.applyKnockBack(vcos, vsin, hspeed, vspeed);
    });

    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    REQUIRE(s.sent().of(Opcode::MSG_MOVE_JUMP).empty());

    // Exact Stinger Charge knockback parameters from world-control.log / client.log (counter=47).
    constexpr float kVcos = -0.719246f;
    constexpr float kVsin = 0.694756f;
    constexpr float kHspeed = 1.0f;
    constexpr float kVspeed = -10.2f;

    const uint32_t tKnock1 = mh->currentMovementClockMs() + 150u;
    mh->recordPhysicsSample(tKnock1);
    wowee::network::Packet kb1(wowee::game::wireOpcode(Opcode::SMSG_MOVE_KNOCK_BACK));
    kb1.writePackedGuid(Session::kPlayerGuid);
    kb1.writeUInt32(47u);
    kb1.writeFloat(kVcos);
    kb1.writeFloat(kVsin);
    kb1.writeFloat(kHspeed);
    kb1.writeFloat(kVspeed);
    s.receive(kb1);

    // Consecutive knockback while already airborne must also never emit MSG_MOVE_JUMP (which tripped MultiJump).
    const uint32_t tKnock2 = tKnock1 + 300u;
    mh->recordPhysicsSample(tKnock2);
    wowee::network::Packet kb2(wowee::game::wireOpcode(Opcode::SMSG_MOVE_KNOCK_BACK));
    kb2.writePackedGuid(Session::kPlayerGuid);
    kb2.writeUInt32(48u);
    kb2.writeFloat(0.5f);
    kb2.writeFloat(-0.8660254f);
    kb2.writeFloat(12.5f);
    kb2.writeFloat(-7.5f);
    s.receive(kb2);

    CHECK(s.sent().of(Opcode::MSG_MOVE_JUMP).empty());

    const auto acks = s.sent().of(Opcode::CMSG_MOVE_KNOCK_BACK_ACK);
    REQUIRE(acks.size() == 2u);

    {
        auto ack = acks[0];
        // uint64 guid(8) + uint32 counter(4) + base MovementInfo(29) + jump block(16) = 57 bytes
        REQUIRE(ack.getSize() == 57u);
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == 47u);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK(move.time == tKnock1);
        // Native: 0x5001 - falling, and the level travel as forward with a
        // pending stop.
        CHECK(move.flags == (static_cast<uint32_t>(MovementFlags::FALLING) |
                             static_cast<uint32_t>(MovementFlags::FORWARD) |
                             static_cast<uint32_t>(MovementFlags::PENDING_STOP)));
        CHECK(move.fallTime == 0u);
        REQUIRE(move.hasJump);
        CHECK(move.zspeed == Catch::Approx(kVspeed).margin(1e-4f));
        CHECK(move.cosAngle == Catch::Approx(kVcos).margin(1e-5f));
        CHECK(move.sinAngle == Catch::Approx(kVsin).margin(1e-5f));
        CHECK(move.xyspeed == Catch::Approx(kHspeed).margin(1e-5f));
        CHECK(ack.getRemainingSize() == 0u);
    }

    {
        auto ack = acks[1];
        REQUIRE(ack.getSize() == 57u);
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == 48u);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK(move.time == tKnock2);
        REQUIRE(move.hasJump);
        CHECK(move.zspeed == Catch::Approx(-7.5f).margin(1e-4f));
        CHECK(move.cosAngle == Catch::Approx(0.5f).margin(1e-5f));
        CHECK(move.sinAngle == Catch::Approx(-0.8660254f).margin(1e-5f));
        CHECK(move.xyspeed == Catch::Approx(12.5f).margin(1e-5f));
        CHECK(ack.getRemainingSize() == 0u);
    }

    // The acknowledgement starts the heartbeat period: none one frame later,
    // one 500 ms after it.
    mh->recordPhysicsSample(tKnock2 + 33u);
    mh->sendDueHeartbeats();
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());
    mh->recordPhysicsSample(tKnock2 + 520u);
    mh->sendDueHeartbeats();
    {
        const auto beats = s.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
        REQUIRE(beats.size() == 1u);
        auto beat = beats[0];
        const auto move = readTbcAckMovementInfo(beat);
        CHECK(move.time == tKnock2 + 500u);
        CHECK(move.fallTime == 500u);
    }

    // Subsequent landing packet reports exact fallTime elapsed from the second knockback ACK timestamp.
    mh->recordPhysicsSample(tKnock2 + 640u);
    gh.sendMovement(Opcode::MSG_MOVE_FALL_LAND);
    const auto lands = s.sent().of(Opcode::MSG_MOVE_FALL_LAND);
    REQUIRE(lands.size() == 1u);
    auto land = lands[0];
    const auto landMove = readTbcAckMovementInfo(land);
    CHECK(landMove.fallTime == 640u);
}

// Build 8606, _experiments/20261124T202000Z.movement_collision_parity_round24
// (kb.py): a knockback that arrives with the loss of control is acknowledged
// with the flags the body had and is not flown; when control comes back the
// body falls and lands - a heartbeat that says falling, then the landing.
TEST_CASE("TBC knockback arriving with loss of control is acknowledged, not flown, and control regained lands the body",
          "[replay][movement][tbc][knockback]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -265.645f, -19.7387f, 8.09701f, 1.59686f);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    wowee::rendering::CameraController camCtrl(nullptr);
    camCtrl.setMovementCallback([&gh](uint32_t op) {
        gh.sendMovement(static_cast<Opcode>(op));
    });
    int launches = 0;
    gh.setKnockBackCallback([&](float vcos, float vsin, float hspeed, float vspeed) {
        ++launches;
        camCtrl.applyKnockBack(vcos, vsin, hspeed, vspeed);
    });
    int releases = 0;
    gh.setReleasedFallCallback([&] {
        ++releases;
        return true;  // the body stands on ground
    });

    mh->recordPhysicsSample(mh->currentMovementClockMs() + 150u);
    wowee::network::Packet kb(wowee::game::wireOpcode(Opcode::SMSG_MOVE_KNOCK_BACK));
    kb.writePackedGuid(Session::kPlayerGuid);
    kb.writeUInt32(77u);
    kb.writeFloat(0.38691f);
    kb.writeFloat(0.922117f);
    kb.writeFloat(15.0f);
    kb.writeFloat(-21.3f);
    wowee::network::Packet lost(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
    lost.writePackedGuid(Session::kPlayerGuid);
    lost.writeUInt8(0u);
    s.receiveTogether({kb, lost});

    CHECK(launches == 0);
    const auto acks = s.sent().of(Opcode::CMSG_MOVE_KNOCK_BACK_ACK);
    REQUIRE(acks.size() == 1u);
    {
        auto ack = acks[0];
        CHECK(ack.readUInt64() == Session::kPlayerGuid);
        CHECK(ack.readUInt32() == 77u);
        const auto move = readTbcAckMovementInfo(ack);
        CHECK(move.flags == 0u);
        CHECK_FALSE(move.hasJump);
    }
    CHECK(mh->getMovementInfo().flags == 0u);
    // A body that stood sends no stop when control is taken (round 25: the
    // server relayed native's acknowledgement and nothing else).
    for (const Opcode stop : {Opcode::MSG_MOVE_STOP, Opcode::MSG_MOVE_STOP_STRAFE, Opcode::MSG_MOVE_STOP_TURN,
                              Opcode::MSG_MOVE_STOP_SWIM}) {
        CHECK(s.sent().of(stop).empty());
    }

    const size_t sentWhileHeld = s.sent().all().size();
    wowee::network::Packet back(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
    back.writePackedGuid(Session::kPlayerGuid);
    back.writeUInt8(1u);
    s.receive(back);
    CHECK(s.sent().all().size() == sentWhileHeld);

    mh->recordPhysicsSample(mh->currentMovementClockMs() + 15000u);
    mh->releaseHeldBody();
    CHECK(releases == 1);
    const auto beats = s.sent().of(Opcode::MSG_MOVE_HEARTBEAT);
    REQUIRE(beats.size() == 1u);
    uint32_t beatTime = 0;
    {
        auto beat = beats[0];
        const auto move = readTbcAckMovementInfo(beat);
        CHECK(move.flags == static_cast<uint32_t>(MovementFlags::FALLING));
        CHECK(move.fallTime == 0u);
        beatTime = move.time;
    }
    const auto lands = s.sent().of(Opcode::MSG_MOVE_FALL_LAND);
    REQUIRE(lands.size() == 1u);
    {
        // The landing of the same instant: one timestamp, fall time 0.
        auto land = lands[0];
        const auto move = readTbcAckMovementInfo(land);
        CHECK(move.flags == 0u);
        CHECK(move.time == beatTime);
        CHECK(move.fallTime == 0u);
    }
    mh->releaseHeldBody();
    CHECK(releases == 1);
}

// Build 8606 running when control is taken: what it sends is not visible from
// outside (round 25). The movement handler sends nothing of its own; the
// controller, losing the keys, stops what they were doing.
TEST_CASE("TBC loss of control clears the movement in progress without a packet of its own",
          "[replay][movement][tbc][knockback]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -7035.27f, -3539.36f, 10.6f, 0.05f);
    auto& gh = s.handler();
    gh.sendMovement(Opcode::MSG_MOVE_START_FORWARD);
    const size_t sentBefore = s.sent().all().size();
    wowee::network::Packet lost(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
    lost.writePackedGuid(Session::kPlayerGuid);
    lost.writeUInt8(0u);
    s.receive(lost);
    CHECK(s.sent().all().size() == sentBefore);
    CHECK(gh.getMovementHandler()->getMovementInfo().flags == 0u);
    gh.sendMovement(Opcode::MSG_MOVE_STOP);
    const auto stops = s.sent().of(Opcode::MSG_MOVE_STOP);
    REQUIRE(stops.size() == 1u);
    auto stop = stops[0];
    CHECK(readTbcAckMovementInfo(stop).flags == 0u);
}

TEST_CASE("TBC unroot is acknowledged as a body let go: falling, then the landing is owed",
          "[replay][movement][tbc][root]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -306.0f, -39.0f, 8.07279f, 3.14f);
    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);
    int releases = 0;
    bool onGround = true;
    gh.setReleasedFallCallback([&] { ++releases; return onGround; });

    wowee::network::Packet root(wowee::game::wireOpcode(Opcode::SMSG_FORCE_MOVE_ROOT));
    root.writePackedGuid(Session::kPlayerGuid);
    root.writeUInt32(5u);
    s.receive(root);
    const auto rootAcks = s.sent().of(Opcode::CMSG_FORCE_MOVE_ROOT_ACK);
    REQUIRE(rootAcks.size() == 1u);
    {
        auto ack = rootAcks[0];
        ack.readUInt64();
        ack.readUInt32();
        CHECK(readTbcAckMovementInfo(ack).flags == static_cast<uint32_t>(MovementFlags::ROOT));
    }
    CHECK(releases == 0);

    wowee::network::Packet unroot(wowee::game::wireOpcode(Opcode::SMSG_FORCE_MOVE_UNROOT));
    unroot.writePackedGuid(Session::kPlayerGuid);
    unroot.writeUInt32(6u);
    s.receive(unroot);
    const auto unrootAcks = s.sent().of(Opcode::CMSG_FORCE_MOVE_UNROOT_ACK);
    REQUIRE(unrootAcks.size() == 1u);
    uint32_t unrootTime = 0;
    {
        auto ack = unrootAcks[0];
        ack.readUInt64();
        ack.readUInt32();
        const auto move = readTbcAckMovementInfo(ack);
        CHECK(move.flags == static_cast<uint32_t>(MovementFlags::FALLING));
        CHECK(move.fallTime == 0u);
        unrootTime = move.time;
    }
    CHECK(releases == 1);
    const auto lands = s.sent().of(Opcode::MSG_MOVE_FALL_LAND);
    REQUIRE(lands.size() == 1u);
    {
        auto land = lands[0];
        const auto move = readTbcAckMovementInfo(land);
        CHECK(move.flags == 0u);
        CHECK(move.time == unrootTime);
        CHECK(move.fallTime == 0u);
    }
    CHECK(mh->getMovementInfo().flags == 0u);
    CHECK(s.sent().of(Opcode::MSG_MOVE_HEARTBEAT).empty());

    // In the air when the root is lifted: the fall goes on, and the landing
    // is the camera controller's to report.
    onGround = false;
    s.receive(root);
    s.receive(unroot);
    CHECK(releases == 2);
    CHECK(s.sent().of(Opcode::MSG_MOVE_FALL_LAND).size() == 1u);
    CHECK(mh->getMovementInfo().flags == static_cast<uint32_t>(MovementFlags::FALLING));
}

TEST_CASE("TBC MSG_MOVE_TELEPORT_ACK echoes 16-byte response and preserves server-granted flags",
          "[replay][movement][tbc][teleportack]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -9000.0f, 200.0f, 60.0f, 0.0f);

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    // Grant CAN_FLY and WATER_WALK first, and start running forward.
    mh->getMovementInfoMut().flags |=
        static_cast<uint32_t>(MovementFlags::CAN_FLY) |
        static_cast<uint32_t>(MovementFlags::WATER_WALK) |
        static_cast<uint32_t>(MovementFlags::FORWARD);

    // TBC server MSG_MOVE_TELEPORT_ACK: PackedGuid + uint32 counter + TBC MovementInfo
    wowee::network::Packet tp(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
    tp.writePackedGuid(Session::kPlayerGuid);
    tp.writeUInt32(70u);
    tp.writeUInt32(0u);        // server moveFlags
    tp.writeUInt8(0u);         // TBC moveFlags2 (uint8)
    tp.writeUInt32(123456u);   // moveTime
    tp.writeFloat(6640.5f);    // serverX
    tp.writeFloat(-4670.25f);  // serverY
    tp.writeFloat(721.75f);    // serverZ
    tp.writeFloat(1.57f);      // server orientation
    tp.writeUInt32(0u);        // fallTime
    s.receive(tp);

    const auto acks = s.sent().of(Opcode::MSG_MOVE_TELEPORT_ACK);
    REQUIRE(acks.size() == 1u);
    auto ack = acks[0];
    // CMaNGOS WorldSession::HandleMoveTeleportAckOpcode: ObjectGuid(8) + uint32 counter(4) + uint32 time(4) = 16
    REQUIRE(ack.getSize() == 16u);
    CHECK(ack.readUInt64() == Session::kPlayerGuid);
    CHECK(ack.readUInt32() == 70u);
    CHECK(ack.readUInt32() == 123456u);
    CHECK(ack.getRemainingSize() == 0u);

    const glm::vec3 expectedCanonical =
        wowee::core::coords::serverToCanonical(glm::vec3(6640.5f, -4670.25f, 721.75f));
    CHECK(mh->getMovementInfo().x == Catch::Approx(expectedCanonical.x).margin(1e-3f));
    CHECK(mh->getMovementInfo().y == Catch::Approx(expectedCanonical.y).margin(1e-3f));
    CHECK(mh->getMovementInfo().z == Catch::Approx(expectedCanonical.z).margin(1e-3f));
    CHECK(mh->getMovementInfo().hasFlag(MovementFlags::CAN_FLY));
    CHECK(mh->getMovementInfo().hasFlag(MovementFlags::WATER_WALK));
    CHECK_FALSE(mh->getMovementInfo().hasFlag(MovementFlags::FORWARD));
}

TEST_CASE("TBC forced movement/speed/teleport orders for foreign units never emit ACKs or mutate player state",
          "[replay][movement][tbc][guid-filter]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, -8900.0f, 500.0f, 90.0f, 0.2f);

    constexpr uint64_t kOtherUnitGuid = 0x0000000000000099ull;
    s.receive(UpdateObject().tbc().create(kOtherUnitGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100},
                                           {UF::UNIT_FIELD_MAXHEALTH, 100}}));

    auto& gh = s.handler();
    auto* mh = gh.getMovementHandler();
    REQUIRE(mh != nullptr);

    const float origRun = mh->getServerRunSpeed();
    const float origFlight = mh->getServerFlightSpeed();
    const uint32_t origFlags = mh->getMovementInfo().flags;
    const float origX = mh->getMovementInfo().x;
    const size_t sentBefore = s.sent().all().size();

    // Foreign SMSG_FORCE_RUN_SPEED_CHANGE & SMSG_FORCE_FLIGHT_SPEED_CHANGE
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeUInt32(1u);
        p.writeUInt8(0u);
        p.writeFloat(14.0f);
        s.receive(p);
    }
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_FORCE_FLIGHT_SPEED_CHANGE));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeUInt32(2u);
        p.writeFloat(28.0f);
        s.receive(p);
    }

    // Foreign SMSG_SPLINE_SET_RUN_SPEED & SMSG_SPLINE_SET_FLIGHT_SPEED
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_SET_RUN_SPEED));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeFloat(18.0f);
        s.receive(p);
    }
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_SPLINE_SET_FLIGHT_SPEED));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeFloat(32.0f);
        s.receive(p);
    }

    // Foreign flag/root orders
    for (Opcode op : {Opcode::SMSG_FORCE_MOVE_ROOT, Opcode::SMSG_FORCE_MOVE_UNROOT,
                      Opcode::SMSG_MOVE_FEATHER_FALL, Opcode::SMSG_MOVE_NORMAL_FALL,
                      Opcode::SMSG_MOVE_WATER_WALK, Opcode::SMSG_MOVE_LAND_WALK,
                      Opcode::SMSG_MOVE_SET_HOVER, Opcode::SMSG_MOVE_UNSET_HOVER,
                      Opcode::SMSG_MOVE_SET_CAN_FLY, Opcode::SMSG_MOVE_UNSET_CAN_FLY}) {
        wowee::network::Packet p(wowee::game::wireOpcode(op));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeUInt32(3u);
        s.receive(p);
    }

    // Foreign SMSG_MOVE_KNOCK_BACK
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_MOVE_KNOCK_BACK));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeUInt32(4u);
        p.writeFloat(1.0f);
        p.writeFloat(0.0f);
        p.writeFloat(10.0f);
        p.writeFloat(-8.0f);
        s.receive(p);
    }

    // Foreign MSG_MOVE_TELEPORT_ACK
    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::MSG_MOVE_TELEPORT_ACK));
        p.writePackedGuid(kOtherUnitGuid);
        p.writeUInt32(5u);
        p.writeUInt32(0u);
        p.writeUInt8(0u);
        p.writeUInt32(500u);
        p.writeFloat(100.0f);
        p.writeFloat(200.0f);
        p.writeFloat(300.0f);
        p.writeFloat(0.0f);
        p.writeUInt32(0u);
        s.receive(p);
    }

    CHECK(s.sent().all().size() == sentBefore);
    CHECK(mh->getServerRunSpeed() == Catch::Approx(origRun));
    CHECK(mh->getServerFlightSpeed() == Catch::Approx(origFlight));
    CHECK(mh->getMovementInfo().flags == origFlags);
    CHECK(mh->getMovementInfo().x == Catch::Approx(origX));

    // Possessed unit (SMSG_CLIENT_CONTROL_UPDATE allow=1) receives and echoes ACKs with possessed GUID.
    constexpr uint64_t kPossessedGuid = 0xF1300037EA000055ull;
    {
        wowee::network::Packet ctrl(wowee::game::wireOpcode(Opcode::SMSG_CLIENT_CONTROL_UPDATE));
        ctrl.writePackedGuid(kPossessedGuid);
        ctrl.writeUInt8(1u);
        s.receive(ctrl);
    }
    REQUIRE(mh->clientControlledUnit() == kPossessedGuid);

    {
        wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_FORCE_RUN_SPEED_CHANGE));
        p.writePackedGuid(kPossessedGuid);
        p.writeUInt32(77u);
        p.writeUInt8(0u);
        p.writeFloat(9.0f);
        s.receive(p);
    }
    const auto possessedAcks = s.sent().of(Opcode::CMSG_FORCE_RUN_SPEED_CHANGE_ACK);
    REQUIRE(possessedAcks.size() == 1u);
    auto ack = possessedAcks[0];
    CHECK(ack.readUInt64() == kPossessedGuid);
    CHECK(ack.readUInt32() == 77u);
    // Player's own cached run speed was not overwritten by possessed creature speed.
    CHECK(mh->getServerRunSpeed() == Catch::Approx(origRun));
}

TEST_CASE("SpellHandler refuses spellId 0 without sending CMSG_CAST_SPELL or CMSG_CANCEL_AURA",
          "[replay][spell][guard]") {
    Session s("tbc");
    spawnTbcPlayerAt(s, 0.0f, 0.0f, 0.0f, 0.0f);

    auto* sh = s.handler().getSpellHandler();
    REQUIRE(sh != nullptr);

    const size_t sentBefore = s.sent().all().size();
    sh->castSpell(0u, Session::kPlayerGuid);
    sh->cancelAura(0u);
    CHECK(s.sent().all().size() == sentBefore);
    CHECK(s.sent().of(Opcode::CMSG_CAST_SPELL).empty());
    CHECK(s.sent().of(Opcode::CMSG_CANCEL_AURA).empty());
}
