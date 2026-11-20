// TBC 2.4.3 Meeting Stone (GAMEOBJECT_TYPE_MEETINGSTONE = 23) interaction and
// SMSG_SUMMON_REQUEST -> CMSG_SUMMON_RESPONSE flow.
//
// CMaNGOS GameObject::Use(GAMEOBJECT_TYPE_MEETINGSTONE) silently returns on
// every precondition failure; the original 2.4.3 client checks reach, party,
// target, party membership, summoner level, and target level locally before
// sending CMSG_GAMEOBJ_USE (verified on pair 17 at Deadmines Meeting Stone
// 178834, minLevel=16, maxLevel=24).
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <cstdint>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::GameHandlerTestAccess;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;
using UF = wowee::game::UF;

namespace {

constexpr uint32_t kMeetingStoneEntry = 178834;
constexpr uint32_t kMinLevel = 16;
constexpr uint32_t kMaxLevel = 24;
constexpr uint32_t kDeadminesArea = 1581;
constexpr uint64_t kPartyMateGuid = 0x0000000000000142ull;
constexpr uint64_t kNonPartyGuid = 0xF130000000000099ull;

std::string meetingStoneGlobalString(const char* name) {
    const std::string key = name ? name : "";
    if (key == "ERR_USE_TOO_FAR") return "You are too far away.";
    if (key == "ERR_MEETING_STONE_NEED_PARTY") return "You need to be in a party to use a meeting stone";
    if (key == "ERR_GENERIC_NO_TARGET") return "You have no target.";
    if (key == "ERR_MEETING_STONE_TARGET_NOT_IN_PARTY") return "Your target is not in the party";
    if (key == "ERR_MEETING_STONE_INVALID_LEVEL") {
        return "You do not meet the level requirements of this meeting stone.";
    }
    if (key == "ERR_MEETING_STONE_TARGET_INVALID_LEVEL") {
        return "Your target does not meet the level requirements of this meeting stone.";
    }
    return {};
}

Packet meetingStoneQueryResponse(uint32_t entry, uint32_t minLevel, uint32_t maxLevel, uint32_t areaId) {
    Packet p(wireOpcode(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeUInt32(23);  // GAMEOBJECT_TYPE_MEETINGSTONE
    p.writeUInt32(1);   // displayId
    p.writeString("Meeting Stone");
    for (int i = 0; i < 6; ++i) p.writeString("");
    p.writeUInt32(minLevel);
    p.writeUInt32(maxLevel);
    p.writeUInt32(areaId);
    for (int i = 3; i < 24; ++i) p.writeUInt32(0);
    p.writeFloat(1.0f);
    return p;
}

Packet tbcPartyList(uint64_t leaderGuid, uint64_t mateGuid, const std::string& mateName) {
    Packet p(wireOpcode(Opcode::SMSG_GROUP_LIST));
    p.writeUInt8(0);  // party
    p.writeUInt8(0);  // battleground
    p.writeUInt8(0);  // subgroup
    p.writeUInt8(0);  // flags
    p.writeUInt64(0x1F4);
    p.writeUInt32(1);  // 1 other member
    p.writeString(mateName);
    p.writeUInt64(mateGuid);
    p.writeUInt8(1);  // online
    p.writeUInt8(0);  // subgroup
    p.writeUInt8(0);  // flags
    p.writeUInt64(leaderGuid);
    p.writeUInt8(3);
    p.writeUInt64(0);
    p.writeUInt8(2);
    p.writeUInt8(0);
    return p;
}

struct MeetingStoneFixture {
    Session session{"tbc"};
    std::vector<std::string> errors;
    uint64_t stoneGuid = 0;

    MeetingStoneFixture(uint64_t guid, uint32_t playerLevel, uint32_t mateLevel, bool inParty)
        : stoneGuid(guid) {
        auto& gh = session.handler();
        gh.setGlobalStringLookup(meetingStoneGlobalString);
        gh.setUIErrorCallback([this](const std::string& msg) { errors.push_back(msg); });

        session.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, playerLevel}},
            true));
        session.receive(UpdateObject().tbc().create(
            kPartyMateGuid, ObjectType::PLAYER,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, mateLevel}}));
        session.receive(UpdateObject().tbc().create(
            kNonPartyGuid, ObjectType::UNIT,
            {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}, {UF::UNIT_FIELD_LEVEL, 15}}));
        session.receive(UpdateObject().tbc().create(
            stoneGuid, ObjectType::GAMEOBJECT,
            {{UF::OBJECT_FIELD_ENTRY, kMeetingStoneEntry},
             {UF::GAMEOBJECT_FLAGS, 0},
             {UF::GAMEOBJECT_DYNAMIC, 0}}));
        session.receive(meetingStoneQueryResponse(kMeetingStoneEntry, kMinLevel, kMaxLevel, kDeadminesArea));

        if (inParty) {
            session.receive(tbcPartyList(Session::kPlayerGuid, kPartyMateGuid, "Edricq"));
        }
        gh.setPosition(2.0f, 0.0f, 0.0f);
    }
};

}  // namespace

TEST_CASE("meeting stone enforces party, target, and level checks in native order", "[replay][meetingstone]") {
    SECTION("solo player in level range gets ERR_MEETING_STONE_NEED_PARTY") {
        MeetingStoneFixture f(0xF110002B7E000101ull, 20, 20, false);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "You need to be in a party to use a meeting stone");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("solo player out of level range still gets ERR_MEETING_STONE_NEED_PARTY first") {
        MeetingStoneFixture f(0xF110002B7E000102ull, 70, 20, false);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "You need to be in a party to use a meeting stone");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("out of reach fails with ERR_USE_TOO_FAR before party check") {
        MeetingStoneFixture f(0xF110002B7E000103ull, 20, 20, false);
        f.session.handler().setPosition(15.0f, 0.0f, 0.0f);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "You are too far away.");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("in party with no target gets ERR_GENERIC_NO_TARGET even when summoner level is invalid") {
        MeetingStoneFixture f(0xF110002B7E000104ull, 70, 20, true);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "You have no target.");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("in party targeting self produces no error and does not send CMSG_GAMEOBJ_USE") {
        MeetingStoneFixture f(0xF110002B7E000105ull, 20, 20, true);
        f.session.handler().setTarget(Session::kPlayerGuid);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        CHECK(f.errors.empty());
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("in party targeting non-party unit gets ERR_MEETING_STONE_TARGET_NOT_IN_PARTY") {
        MeetingStoneFixture f(0xF110002B7E000106ull, 20, 20, true);
        f.session.handler().setTarget(kNonPartyGuid);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "Your target is not in the party");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("in party with invalid summoner level gets ERR_MEETING_STONE_INVALID_LEVEL") {
        MeetingStoneFixture f(0xF110002B7E000107ull, 70, 20, true);
        f.session.handler().setTarget(kPartyMateGuid);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "You do not meet the level requirements of this meeting stone.");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("in party with invalid target level gets ERR_MEETING_STONE_TARGET_INVALID_LEVEL") {
        MeetingStoneFixture f(0xF110002B7E000108ull, 20, 70, true);
        f.session.handler().setTarget(kPartyMateGuid);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        REQUIRE(f.errors.size() == 1);
        CHECK(f.errors[0] == "Your target does not meet the level requirements of this meeting stone.");
        CHECK(f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE).empty());
    }

    SECTION("valid party summon sends CMSG_GAMEOBJ_USE for the meeting stone") {
        MeetingStoneFixture f(0xF110002B7E000109ull, 20, 20, true);
        f.session.handler().setTarget(kPartyMateGuid);
        f.session.handler().interactWithGameObject(f.stoneGuid);
        CHECK(f.errors.empty());
        const auto sent = f.session.sent().of(Opcode::CMSG_GAMEOBJ_USE);
        REQUIRE(sent.size() == 1);
        Packet body = sent[0];
        CHECK(body.readUInt64() == f.stoneGuid);
    }
}

TEST_CASE("SMSG_SUMMON_REQUEST stores area name and acceptSummon sends CMSG_SUMMON_RESPONSE", "[replay][meetingstone]") {
    Session s("tbc");
    GameHandlerTestAccess::setAreaName(s.handler(), 20, "Moonbrook");

    Packet req(wireOpcode(Opcode::SMSG_SUMMON_REQUEST));
    req.writeUInt64(kPartyMateGuid);
    req.writeUInt32(20);
    req.writeUInt32(120000);
    s.receive(req);

    CHECK(s.handler().hasPendingSummonRequest());
    CHECK(s.handler().getSummonAreaName() == "Moonbrook");
    CHECK(s.handler().getSummonTimeoutSec() == Catch::Approx(120.0f));

    s.handler().acceptSummon();
    CHECK_FALSE(s.handler().hasPendingSummonRequest());

    const auto responses = s.sent().of(Opcode::CMSG_SUMMON_RESPONSE);
    REQUIRE(responses.size() == 1);
    Packet body = responses[0];
    CHECK(body.readUInt64() == kPartyMateGuid);
    CHECK(body.readUInt8() == 1);
}
