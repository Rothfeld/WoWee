// Starting an attack closes an open loot window, as the original client does
// (ledger #85). CMaNGOS HandleAttackSwingOpcode leaves loot open, so the
// client releases it itself. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;
using wowee::game::Opcode;

TEST_CASE("starting an attack releases open loot", "[replay][loot][combat]") {
    constexpr uint64_t kCorpse = 0xF130000000001001ull;
    constexpr uint64_t kEnemy = 0xF130000000002002ull;
    Session s("tbc");
    std::vector<std::string> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { events.push_back(e); });

    // Loot::BuildLootPacket: guid, type LOOT_CORPSE, gold, no items.
    wowee::network::Packet loot(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    loot.writeUInt64(kCorpse);
    loot.writeUInt8(1);
    loot.writeUInt32(25);
    loot.writeUInt8(0);
    s.receive(loot);
    REQUIRE(s.handler().isLootWindowOpen());

    s.handler().startAutoAttack(kEnemy);
    CHECK_FALSE(s.handler().isLootWindowOpen());
    const auto released = s.sent().of(Opcode::CMSG_LOOT_RELEASE);
    REQUIRE(released.size() == 1);
    auto release = released.front();
    CHECK(release.readUInt64() == kCorpse);
    CHECK(s.sent().of(Opcode::CMSG_ATTACKSWING).size() == 1);
    CHECK(std::find(events.begin(), events.end(), "LOOT_CLOSED") != events.end());
}

TEST_CASE("corpse loot reach honours combatReach for normal and giant bosses", "[replay][loot][reach]") {
    constexpr uint64_t kNormalCorpse = 0xF1300000000002FFull; // Swamp Jaguar (767)
    constexpr uint64_t kRajaxxCorpse = 0xF130003BED596DD2ull; // General Rajaxx (15341)
    Session s("tbc");
    auto& handler = s.handler();

    auto player = std::make_shared<wowee::game::Unit>(Session::kPlayerGuid);
    player->setCombatReach(1.5f);
    handler.getEntityManager().addEntity(Session::kPlayerGuid, player);
    handler.setPosition(0.0f, 0.0f, 0.0f);

    // Normal corpse: combatReach=1.725 -> max(5.0, 1.5 + 1.725 + 4/3) = 5.0 yd
    auto normal = std::make_shared<wowee::game::Unit>(kNormalCorpse);
    normal->setCombatReach(1.725f);
    normal->setPosition(5.20f, 0.0f, 0.0f, 0.0f);
    handler.getEntityManager().addEntity(kNormalCorpse, normal);

    CHECK_FALSE(handler.unitWithinLootReach(*normal));
    handler.lootTarget(kNormalCorpse);
    CHECK(s.sent().of(Opcode::CMSG_LOOT).empty());

    normal->setPosition(4.90f, 0.0f, 0.0f, 0.0f);
    CHECK(handler.unitWithinLootReach(*normal));
    handler.lootTarget(kNormalCorpse);
    REQUIRE(s.sent().of(Opcode::CMSG_LOOT).size() == 1);

    // Giant boss (General Rajaxx): combatReach=6.0 -> 1.5 + 6.0 + 4/3 = 8.8333 yd
    auto rajaxx = std::make_shared<wowee::game::Unit>(kRajaxxCorpse);
    rajaxx->setCombatReach(6.0f);
    rajaxx->setPosition(8.86f, 0.0f, 0.0f, 0.0f);
    handler.getEntityManager().addEntity(kRajaxxCorpse, rajaxx);

    CHECK_FALSE(handler.unitWithinLootReach(*rajaxx));
    handler.lootTarget(kRajaxxCorpse);
    CHECK(s.sent().of(Opcode::CMSG_LOOT).size() == 1); // unchanged

    rajaxx->setPosition(8.80f, 0.0f, 0.0f, 0.0f);
    CHECK(handler.unitWithinLootReach(*rajaxx));
    handler.lootTarget(kRajaxxCorpse);
    REQUIRE(s.sent().of(Opcode::CMSG_LOOT).size() == 2);
}

TEST_CASE("server SMSG_LOOT_RESPONSE LOOT_ERROR_TOO_FAR raises ERR_LOOT_TOO_FAR", "[replay][loot][error]") {
    constexpr uint64_t kCorpse = 0xF130003BED596DD2ull;
    Session s("tbc");
    auto& handler = s.handler();
    handler.setGlobalStringLookup([](const char* name) -> std::string {
        if (name && std::string(name) == "ERR_LOOT_TOO_FAR")
            return "You are too far away to loot that corpse.";
        return {};
    });

    std::vector<std::string> uiErrors;
    handler.setUIErrorCallback([&](const std::string& msg) { uiErrors.push_back(msg); });

    wowee::network::Packet denied(wowee::game::wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
    denied.writeUInt64(kCorpse);
    denied.writeUInt8(0);  // LOOT_NONE
    denied.writeUInt8(4);  // LOOT_ERROR_TOO_FAR
    s.receive(denied);

    CHECK_FALSE(handler.isLootWindowOpen());
    REQUIRE(uiErrors.size() == 1);
    CHECK(uiErrors.front() == "You are too far away to loot that corpse.");
}
