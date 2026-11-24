// Another corpse while one corpse's loot is open, as original 2.4.3 does it
// (wire captures of build 8606 against CMaNGOS TBC, ledger lootswitch-1):
//   right-click B with A open: LOOT_RELEASE A, SET_SELECTION 0, SET_SELECTION B, LOOT B
//   close button / Escape on A: LOOT_RELEASE A, SET_SELECTION 0
//   click on empty ground:      LOOT_RELEASE A, SET_SELECTION 0, SET_SELECTION 0
//   right-click A again:        nothing
//   a clam opened, self targeted, closed: LOOT_RELEASE only
// CMaNGOS answers a client release of a corpse with nothing (Loot::Release
// drops the player before SendReleaseForAll), so the client closes its window
// itself. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using wowee::game::UF;

namespace {

constexpr uint64_t kCorpseA = 0xF1300000745945BDull;
constexpr uint64_t kCorpseB = 0xF1300000745945BEull;
constexpr uint64_t kClam = 0x400000000000047Eull;

// Loot::ShowContentTo: guid, client loot type 1 (corpse), gold, item count 0.
wowee::network::Packet lootResponse(uint64_t guid, uint32_t gold) {
    wowee::network::Packet p(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    p.writeUInt64(guid);
    p.writeUInt8(1);
    p.writeUInt32(gold);
    p.writeUInt8(0);
    return p;
}

struct Sent {
    uint16_t opcode;
    uint64_t guid;
    bool operator==(const Sent&) const = default;
};

Sent sent(Opcode opcode, uint64_t guid) { return {wowee::game::wireOpcode(opcode), guid}; }

// What went out from index `from` on, loot and selection packets only.
std::vector<Sent> since(const Session& s, size_t from) {
    std::vector<Sent> out;
    const auto& all = s.sent().all();
    for (size_t i = from; i < all.size(); ++i) {
        auto p = all[i];
        const uint16_t op = p.getOpcode();
        if (op == wowee::game::wireOpcode(Opcode::CMSG_LOOT) ||
            op == wowee::game::wireOpcode(Opcode::CMSG_LOOT_RELEASE) ||
            op == wowee::game::wireOpcode(Opcode::CMSG_SET_SELECTION)) {
            out.push_back({op, p.readUInt64()});
        }
    }
    return out;
}

void spawnCorpses(Session& s) {
    s.receive(UpdateObject().tbc().create(kCorpseA, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
    s.receive(UpdateObject().tbc().create(kCorpseB, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 0}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));
}

// Right-click on a corpse: game_screen selects it, then loots it.
void rightClick(Session& s, uint64_t corpse) {
    s.handler().setTarget(corpse);
    s.handler().prepareLoot(corpse, false);
    s.handler().lootTarget(corpse);
}

}  // namespace

TEST_CASE("looting corpse B releases corpse A first", "[replay][loot]") {
    Session s("tbc");
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { events.push_back(e); });
    spawnCorpses(s);

    rightClick(s, kCorpseA);
    s.receive(lootResponse(kCorpseA, 6));
    REQUIRE(s.handler().isLootWindowOpen());

    const size_t mark = s.sent().all().size();
    events.clear();
    rightClick(s, kCorpseB);
    CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kCorpseA),
                                              sent(Opcode::CMSG_SET_SELECTION, 0),
                                              sent(Opcode::CMSG_SET_SELECTION, kCorpseB),
                                              sent(Opcode::CMSG_LOOT, kCorpseB)});
    CHECK_FALSE(s.handler().isLootWindowOpen());
    REQUIRE(std::find(events.begin(), events.end(), "LOOT_CLOSED") != events.end());

    s.receive(lootResponse(kCorpseB, 10));
    CHECK(s.handler().isLootWindowOpen());
    CHECK(s.handler().getCurrentLoot().lootGuid == kCorpseB);
    CHECK(s.handler().getTargetGuid() == kCorpseB);
    // LootFrame rebuilds in OnShow: B's LOOT_OPENED must follow A's LOOT_CLOSED.
    const auto closed = std::find(events.begin(), events.end(), "LOOT_CLOSED");
    const auto opened = std::find(events.begin(), events.end(), "LOOT_OPENED");
    CHECK(closed < opened);
}

TEST_CASE("right-clicking the open corpse again sends nothing", "[replay][loot]") {
    Session s("tbc");
    spawnCorpses(s);
    rightClick(s, kCorpseA);
    s.receive(lootResponse(kCorpseA, 6));
    const size_t mark = s.sent().all().size();
    rightClick(s, kCorpseA);
    CHECK(since(s, mark).empty());
    CHECK(s.handler().isLootWindowOpen());
}

TEST_CASE("closing a corpse's loot drops it as target", "[replay][loot]") {
    Session s("tbc");
    spawnCorpses(s);
    rightClick(s, kCorpseA);
    s.receive(lootResponse(kCorpseA, 6));
    const size_t mark = s.sent().all().size();
    s.handler().closeLoot();
    CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kCorpseA),
                                              sent(Opcode::CMSG_SET_SELECTION, 0)});
    CHECK(s.handler().getTargetGuid() == 0);
}

TEST_CASE("deselecting the looted corpse closes its loot", "[replay][loot]") {
    Session s("tbc");
    spawnCorpses(s);
    rightClick(s, kCorpseA);
    s.receive(lootResponse(kCorpseA, 6));
    const size_t mark = s.sent().all().size();
    s.handler().clearTarget();
    CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kCorpseA),
                                              sent(Opcode::CMSG_SET_SELECTION, 0),
                                              sent(Opcode::CMSG_SET_SELECTION, 0)});
    CHECK_FALSE(s.handler().isLootWindowOpen());
}

TEST_CASE("loot of something other than the target leaves the target alone", "[replay][loot]") {
    Session s("tbc");
    spawnCorpses(s);
    s.handler().setTarget(kCorpseA);
    // CMSG_OPEN_ITEM answered for the clam (item loot, type 2, one Clam
    // Meat). Selecting another unit keeps it open; closing it keeps the target.
    wowee::network::Packet clam(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    clam.writeUInt64(kClam);
    clam.writeUInt8(2);
    clam.writeUInt32(0);
    clam.writeUInt8(1);
    clam.writeUInt8(0);          // slot
    clam.writeUInt32(5503);      // Clam Meat
    clam.writeUInt32(1);
    clam.writeUInt32(7376);
    clam.writeUInt32(0);
    clam.writeUInt32(0);
    clam.writeUInt8(0);
    s.receive(clam);
    REQUIRE(s.handler().isLootWindowOpen());
    REQUIRE(s.handler().getCurrentLoot().lootGuid == kClam);

    s.handler().setTarget(kCorpseB);
    CHECK(s.handler().isLootWindowOpen());
    const size_t mark = s.sent().all().size();
    s.handler().closeLoot();
    CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kClam)});
    CHECK(s.handler().getTargetGuid() == kCorpseB);
}

TEST_CASE("a release response for another loot keeps the open window", "[replay][loot]") {
    Session s("tbc");
    spawnCorpses(s);
    rightClick(s, kCorpseB);
    s.receive(lootResponse(kCorpseB, 10));
    // Loot::SendReleaseFor: guid, uint8 1.
    wowee::network::Packet release(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RELEASE_RESPONSE));
    release.writeUInt64(kCorpseA);
    release.writeUInt8(1);
    s.receive(release);
    CHECK(s.handler().isLootWindowOpen());

    wowee::network::Packet own(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RELEASE_RESPONSE));
    own.writeUInt64(kCorpseB);
    own.writeUInt8(1);
    s.receive(own);
    CHECK_FALSE(s.handler().isLootWindowOpen());
}

TEST_CASE("using an object releases the open corpse loot", "[replay][loot]") {
    constexpr uint64_t kChest = 0xF110000000000099ull;
    Session s("tbc");
    spawnCorpses(s);
    rightClick(s, kCorpseA);
    s.receive(lootResponse(kCorpseA, 6));
    const size_t mark = s.sent().all().size();
    s.handler().interactWithGameObject(kChest);
    const auto out = since(s, mark);
    REQUIRE(out.size() >= 2);
    CHECK(out[0] == sent(Opcode::CMSG_LOOT_RELEASE, kCorpseA));
    CHECK(out[1] == sent(Opcode::CMSG_SET_SELECTION, 0));
    CHECK_FALSE(s.handler().isLootWindowOpen());
}

TEST_CASE("kneel-loot animation callback fires for corpse loot and UNIT_FLAG_LOOTING, not pickpocket or fishing",
          "[replay][loot][pickpocket]") {
    Session s("tbc");
    spawnCorpses(s);

    std::vector<bool> lootAnimEvents;
    s.handler().setLootWindowCallback([&](bool open) { lootAnimEvents.push_back(open); });

    // Pick Pocket (lootType = 2): opens loot window, does NOT trigger kneel animation.
    {
        wowee::network::Packet pp(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        pp.writeUInt64(kCorpseA);
        pp.writeUInt8(static_cast<uint8_t>(wowee::game::ClientLootType::PICKPOCKETING));
        pp.writeUInt32(290); // copper
        pp.writeUInt8(0);    // 0 items
        s.receive(pp);
        REQUIRE(s.handler().isLootWindowOpen());
        CHECK(lootAnimEvents.empty());
        s.handler().closeLoot();
        CHECK(lootAnimEvents == std::vector<bool>{false});
        lootAnimEvents.clear();
    }

    // Fishing (lootType = 3): opens loot window, does NOT trigger kneel animation.
    {
        wowee::network::Packet fish(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        fish.writeUInt64(kCorpseA);
        fish.writeUInt8(static_cast<uint8_t>(wowee::game::ClientLootType::FISHING));
        fish.writeUInt32(0);
        fish.writeUInt8(1);
        fish.writeUInt8(0);
        fish.writeUInt32(6291); // Raw Brilliant Smallfish
        fish.writeUInt32(1);
        fish.writeUInt32(1);
        fish.writeUInt32(0);
        fish.writeUInt32(0);
        fish.writeUInt8(0);
        s.receive(fish);
        REQUIRE(s.handler().isLootWindowOpen());
        CHECK(lootAnimEvents.empty());
        s.handler().closeLoot();
        lootAnimEvents.clear();
    }

    // Corpse loot (lootType = 1): triggers kneel animation on open and release on close.
    {
        s.receive(lootResponse(kCorpseB, 42));
        REQUIRE(s.handler().isLootWindowOpen());
        CHECK(lootAnimEvents == std::vector<bool>{true});
        s.handler().closeLoot();
        CHECK(lootAnimEvents == std::vector<bool>{true, false});
        lootAnimEvents.clear();
    }

    // Chest / gather GO (UNIT_FLAG_LOOTING set on local player via UNIT_FIELD_FLAGS):
    // triggers kneel animation on set and release on clear.
    {
        s.receive(UpdateObject().tbc().create(
            Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
            {{UF::UNIT_FIELD_FLAGS, 0u}}, /*self=*/true));
        s.receive(UpdateObject().tbc().values(
            Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, wowee::game::UNIT_FLAG_LOOTING}}));
        CHECK(lootAnimEvents == std::vector<bool>{true});
        s.receive(UpdateObject().tbc().values(
            Session::kPlayerGuid, {{UF::UNIT_FIELD_FLAGS, 0u}}));
        CHECK(lootAnimEvents == std::vector<bool>{true, false});
    }
}

TEST_CASE("moving or turning closes world corpse and chest loot immediately, but keeps bag container loot open", "[replay][loot]") {
    Session s("tbc");
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { events.push_back(e); });
    spawnCorpses(s);
    constexpr uint64_t kChest = 0xF110000123456789ull;
    s.receive(UpdateObject().tbc().create(kChest, ObjectType::GAMEOBJECT, {}));

    // Corpse loot closes immediately on translation, jump, and keyboard turn opcodes,
    // sending CMSG_LOOT_RELEASE, clearing selection, and firing LOOT_CLOSED.
    for (Opcode moveOp : {Opcode::MSG_MOVE_START_FORWARD,
                          Opcode::MSG_MOVE_START_BACKWARD,
                          Opcode::MSG_MOVE_START_STRAFE_LEFT,
                          Opcode::MSG_MOVE_START_STRAFE_RIGHT,
                          Opcode::MSG_MOVE_START_TURN_LEFT,
                          Opcode::MSG_MOVE_START_TURN_RIGHT,
                          Opcode::MSG_MOVE_JUMP}) {
        rightClick(s, kCorpseA);
        s.receive(lootResponse(kCorpseA, 12));
        REQUIRE(s.handler().isLootWindowOpen());

        // Stop/heartbeat packets sent while standing still do not close loot.
        s.handler().sendMovement(Opcode::MSG_MOVE_STOP);
        s.handler().sendMovement(Opcode::MSG_MOVE_HEARTBEAT);
        REQUIRE(s.handler().isLootWindowOpen());

        const size_t mark = s.sent().all().size();
        events.clear();
        s.handler().sendMovement(moveOp);
        CHECK_FALSE(s.handler().isLootWindowOpen());
        CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kCorpseA),
                                                  sent(Opcode::CMSG_SET_SELECTION, 0)});
        CHECK(std::find(events.begin(), events.end(), "LOOT_CLOSED") != events.end());
    }

    // World GameObject chest loot also closes immediately on movement.
    {
        s.receive(lootResponse(kChest, 25));
        REQUIRE(s.handler().isLootWindowOpen());
        const size_t mark = s.sent().all().size();
        events.clear();
        s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
        CHECK_FALSE(s.handler().isLootWindowOpen());
        CHECK(since(s, mark) == std::vector<Sent>{sent(Opcode::CMSG_LOOT_RELEASE, kChest)});
        CHECK(std::find(events.begin(), events.end(), "LOOT_CLOSED") != events.end());
    }

    // Inventory container loot (e.g. Small Barnacled Clam, HIGHGUID_ITEM) stays open
    // across turns, forward movement, and jumps, matching Native 2.4.3.
    {
        s.receive(lootResponse(kClam, 5));
        REQUIRE(s.handler().isLootWindowOpen());
        events.clear();
        s.handler().sendMovement(Opcode::MSG_MOVE_START_TURN_LEFT);
        s.handler().sendMovement(Opcode::MSG_MOVE_START_FORWARD);
        s.handler().sendMovement(Opcode::MSG_MOVE_JUMP);
        CHECK(s.handler().isLootWindowOpen());
        CHECK(std::find(events.begin(), events.end(), "LOOT_CLOSED") == events.end());
        s.handler().closeLoot();
    }
}
