// TBC 2.4.3 taxi mount fallback and replacement lifecycle tests.
//
// Root cause of the "tilted floating elf with white textureless cloak" at
// The Crossroads:
// 1. TaxiNodes.dbc stores MountCreatureID[0/1] (creature_template entry IDs:
//    2224 = Horde Wyvern, 541 = Alliance Gryphon), NOT CreatureDisplayInfo IDs.
//    CreatureDisplayInfo 2224 is Character\NightElf\Female\NightElfFemale.m2.
// 2. Passing 2224 into applyTaxiMountForCurrentNode() spawned NightElfFemale.m2
//    as a flying mount during the ~200ms before SMSG_UPDATE_OBJECT delivered
//    UNIT_FIELD_MOUNTDISPLAYID = 6851 (Wyvern).
// 3. EntitySpawner::processPendingMount() then overwrote mountInstanceId_
//    without removing the previous mount instance, permanently leaking the
//    tilted, mid-air NightElfFemale (with untextured cloak geosets) at liftoff.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/movement_handler.hpp"

#include <cstdint>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Race;
using wowee::game::UF;

namespace {

uint32_t unitBytes0(Race race, uint8_t cls = 1, uint8_t gender = 0, uint8_t power = 1) {
    return static_cast<uint32_t>(race) |
           (static_cast<uint32_t>(cls) << 8) |
           (static_cast<uint32_t>(gender) << 16) |
           (static_cast<uint32_t>(power) << 24);
}

}  // namespace

TEST_CASE("TBC Horde taxi mount uses DBC wyvern displayId, never raw TaxiNodes creature ID 2224",
          "[taxi][mount]") {
    Session s("tbc");
    auto& gh = s.handler();
    s.services().gryphonDisplayId = 7270;
    s.services().wyvernDisplayId = 11012;
    gh.playerRaceRef() = Race::UNDEAD;

    std::vector<uint32_t> mountCalls;
    gh.setMountCallback([&](uint32_t displayId) { mountCalls.push_back(displayId); });

    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, unitBytes0(Race::UNDEAD)}}, true));

    REQUIRE(gh.getMovementHandler() != nullptr);
    gh.getMovementHandler()->applyTaxiMountForCurrentNode();

    REQUIRE(mountCalls.size() == 1);
    CHECK(mountCalls[0] == 11012u);
    CHECK(mountCalls[0] != 2224u);
    CHECK(mountCalls[0] != 1310u);

    // Server's authoritative UNIT_FIELD_MOUNTDISPLAYID arrives ~200ms later.
    s.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, 6851u}}));
    REQUIRE(mountCalls.size() == 2);
    CHECK(mountCalls[1] == 6851u);

    // Landing clears the mount cleanly.
    s.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, 0u}}));
    REQUIRE(mountCalls.size() == 3);
    CHECK(mountCalls[2] == 0u);
}

TEST_CASE("TBC Alliance taxi mount uses DBC gryphon displayId, never raw creature ID 541",
          "[taxi][mount]") {
    Session s("tbc");
    auto& gh = s.handler();
    s.services().gryphonDisplayId = 7270;
    s.services().wyvernDisplayId = 11012;
    gh.playerRaceRef() = Race::HUMAN;

    std::vector<uint32_t> mountCalls;
    gh.setMountCallback([&](uint32_t displayId) { mountCalls.push_back(displayId); });

    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, unitBytes0(Race::HUMAN)}}, true));

    gh.getMovementHandler()->applyTaxiMountForCurrentNode();

    REQUIRE(mountCalls.size() == 1);
    CHECK(mountCalls[0] == 7270u);
    CHECK(mountCalls[0] != 541u);
    CHECK(mountCalls[0] != 1210u);
}

TEST_CASE("Taxi mount prediction does not clobber an already-authoritative server mount",
          "[taxi][mount]") {
    Session s("tbc");
    auto& gh = s.handler();
    s.services().gryphonDisplayId = 7270;
    s.services().wyvernDisplayId = 11012;
    gh.playerRaceRef() = Race::UNDEAD;

    std::vector<uint32_t> mountCalls;
    gh.setMountCallback([&](uint32_t displayId) { mountCalls.push_back(displayId); });

    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, unitBytes0(Race::UNDEAD)},
         {UF::UNIT_FIELD_MOUNTDISPLAYID, 6851u}},
        true));

    REQUIRE(mountCalls.size() == 1);
    CHECK(mountCalls[0] == 6851u);

    gh.getMovementHandler()->applyTaxiMountForCurrentNode();
    CHECK(mountCalls.size() == 1);
    CHECK(gh.getMovementHandler()->isTaxiMountActive());
}

TEST_CASE("Taxi mount prediction emits nothing when DBC fallback displayIds are unavailable",
          "[taxi][mount]") {
    Session s("tbc");
    auto& gh = s.handler();
    s.services().gryphonDisplayId = 0;
    s.services().wyvernDisplayId = 0;
    gh.playerRaceRef() = Race::ORC;

    std::vector<uint32_t> mountCalls;
    gh.setMountCallback([&](uint32_t displayId) { mountCalls.push_back(displayId); });

    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_BYTES_0, unitBytes0(Race::ORC)}}, true));

    gh.getMovementHandler()->applyTaxiMountForCurrentNode();
    CHECK(mountCalls.empty());
}
