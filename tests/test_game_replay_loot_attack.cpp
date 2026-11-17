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
