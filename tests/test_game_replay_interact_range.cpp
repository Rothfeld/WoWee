// Right-clicking a service NPC sends CMSG_GOSSIP_HELLO only within the 3D
// centre distance the original 2.4.3 client measured (between 5.536 and
// 5.562 yd for Marshal Dughan and Vaelastrasz, unchanged by either unit's
// combat reach, bounding radius or scale). CMaNGOS answers out to 5 yd plus
// both combat reaches, so the client is the one that refuses. See
// game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <bit>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using UF = wowee::game::UF;

TEST_CASE("gossip hello only within native interaction reach", "[replay][gossip]") {
    // Vaelastrasz the Corrupt: npc flags gossip|questgiver, combat reach 13.
    constexpr uint64_t kVael = 0xF13000032C0012A1ull;
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}},
                                          true));
    s.receive(UpdateObject().tbc().create(kVael, ObjectType::UNIT,
                                          {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
                                           {UF::UNIT_NPC_FLAGS, 3},
                                           {UF::UNIT_FIELD_COMBATREACH, std::bit_cast<uint32_t>(13.0f)},
                                           {UF::UNIT_FIELD_BOUNDINGRADIUS, std::bit_cast<uint32_t>(12.0f)}}));
    auto& gh = s.handler();
    // The creature stands at the origin (stationary create block).
    gh.setPosition(9.0f, 0.0f, 0.0f);
    gh.interactWithNpc(kVael);
    CHECK(s.sent().of(Opcode::CMSG_GOSSIP_HELLO).empty());
    gh.setPosition(5.4f, 0.0f, -1.18f); // native: 5.5275 yd 3D opened
    gh.interactWithNpc(kVael);
    CHECK(s.sent().of(Opcode::CMSG_GOSSIP_HELLO).size() == 1);
    gh.setPosition(4.95f, 0.0f, -2.87f); // native: 4.95 yd 2D, 5.72 yd 3D did not
    gh.interactWithNpc(kVael);
    CHECK(s.sent().of(Opcode::CMSG_GOSSIP_HELLO).size() == 1);
}
