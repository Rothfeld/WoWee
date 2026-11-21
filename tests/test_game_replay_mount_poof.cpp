// TBC 2.4.3 mount-up transformation smoke parity (SpellVisualKit 2110 /
// Mount Impact -> Spells\DruidMorph_Impact_Base.m2).
//
// Live-verified on Native TBC 2.4.3 (build 8606, pair 3, Stormwind Flight Master):
// - On live UPDATETYPE_VALUES transition UNIT_FIELD_MOUNTDISPLAYID 0 -> non-zero
//   (taxi takeoff, regular mount, .modify mount, remote unit mounting in view),
//   the client plays the mount transformation smoke attached to the rider's Base.
// - On dismount (non-zero -> 0, both regular .dismount and taxi arrival), NO smoke plays.
// - On initial CREATE_OBJECT / CREATE_OBJECT2 while already mounted (e.g. logging in
//   mid-taxi flight or entering AOI of an already-mounted unit), NO smoke plays.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <cstdint>
#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using UF = wowee::game::UF;

namespace {

constexpr uint32_t kGryphonMountDisplay = 541;
constexpr uint32_t kHorseMountDisplay = 2404;
constexpr uint64_t kRemotePlayerGuid = 0x0000000000000456ull;
constexpr uint64_t kRemoteCreatureGuid = 0xF130000000000789ull;

}  // namespace

TEST_CASE("Mount transformation smoke triggers on live 0 -> non-zero mount transition (taxi takeoff & regular mount)",
          "[replay][mount-poof]") {
    Session session{"tbc"};
    std::vector<uint64_t> poofs;
    session.handler().setMountPoofCallback([&](uint64_t guid) { poofs.push_back(guid); });

    // Initial spawn unmounted.
    session.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100},
         {UF::UNIT_FIELD_MAXHEALTH, 100},
         {UF::UNIT_FIELD_MOUNTDISPLAYID, 0}},
        true));
    REQUIRE(poofs.empty());

    // Live taxi takeoff / mount-up: UNIT_FIELD_MOUNTDISPLAYID 0 -> 541.
    session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, kGryphonMountDisplay}}));
    REQUIRE(poofs.size() == 1);
    REQUIRE(poofs[0] == Session::kPlayerGuid);

    // Redundant values update while already mounted must NOT re-trigger smoke.
    session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, kGryphonMountDisplay}}));
    REQUIRE(poofs.size() == 1);

    // Dismount (non-zero -> 0) must NOT trigger smoke.
    session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    REQUIRE(poofs.size() == 1);

    // Subsequent mount-up (0 -> horse) triggers smoke again.
    session.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, kHorseMountDisplay}}));
    REQUIRE(poofs.size() == 2);
    REQUIRE(poofs[1] == Session::kPlayerGuid);
}

TEST_CASE("Mount transformation smoke does NOT trigger on initial login while already mounted mid-taxi",
          "[replay][mount-poof]") {
    Session session{"tbc"};
    std::vector<uint64_t> poofs;
    session.handler().setMountPoofCallback([&](uint64_t guid) { poofs.push_back(guid); });

    // Initial CREATE_OBJECT2 with UNIT_FIELD_MOUNTDISPLAYID already set (relog mid-flight).
    session.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100},
         {UF::UNIT_FIELD_MAXHEALTH, 100},
         {UF::UNIT_FIELD_MOUNTDISPLAYID, kGryphonMountDisplay}},
        true));
    REQUIRE(poofs.empty());
    REQUIRE(session.handler().isMounted());
}

TEST_CASE("Mount transformation smoke triggers on remote unit live mount-up but not initial AOI spawn or dismount",
          "[replay][mount-poof]") {
    Session session{"tbc"};
    std::vector<uint64_t> poofs;
    session.handler().setMountPoofCallback([&](uint64_t guid) { poofs.push_back(guid); });

    session.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}},
        true));

    // Remote player enters AOI unmounted.
    session.receive(UpdateObject().tbc().create(
        kRemotePlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100}}));

    // Remote mounted creature enters AOI already mounted -> NO smoke.
    session.receive(UpdateObject().tbc().create(
        kRemoteCreatureGuid, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 100},
         {UF::UNIT_FIELD_MAXHEALTH, 100},
         {UF::UNIT_FIELD_MOUNTDISPLAYID, kHorseMountDisplay}}));
    REQUIRE(poofs.empty());

    // Remote player mounts up in view -> triggers smoke.
    session.receive(UpdateObject().tbc().values(
        kRemotePlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, kGryphonMountDisplay}}));
    REQUIRE(poofs.size() == 1);
    REQUIRE(poofs[0] == kRemotePlayerGuid);

    // Remote player dismounts -> NO smoke.
    session.receive(UpdateObject().tbc().values(
        kRemotePlayerGuid, {{UF::UNIT_FIELD_MOUNTDISPLAYID, 0}}));
    REQUIRE(poofs.size() == 1);
}
