#include <catch_amalgamated.hpp>

#include "game/pet_action.hpp"

using namespace wowee::game::pet;

// The bug these cover: dismiss packed action 0 under the command type, which is
// COMMAND_STAY, so the pet planted itself instead of leaving. The ids overlap
// between the two built-in types, so a test that only checks the low bits would
// have passed on the broken encoding too - each case pins both halves.

TEST_CASE("pet action packs type into the high byte", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) == 0x07000003u);
    REQUIRE(packPetAction(ActionType::Command, kStay)    == 0x07000000u);
    REQUIRE(packPetAction(ActionType::Command, kFollow)  == 0x07000001u);
    REQUIRE(packPetAction(ActionType::Command, kAttack)  == 0x07000002u);

    REQUIRE(packPetAction(ActionType::Reaction, kPassive)    == 0x06000000u);
    REQUIRE(packPetAction(ActionType::Reaction, kDefensive)  == 0x06000001u);
    REQUIRE(packPetAction(ActionType::Reaction, kAggressive) == 0x06000002u);
}

TEST_CASE("dismiss is not stay", "[pet]") {
    REQUIRE(packPetAction(ActionType::Command, kAbandon) !=
            packPetAction(ActionType::Command, kStay));
}

TEST_CASE("built-in ids collide across types and are told apart by type", "[pet]") {
    const uint32_t follow    = packPetAction(ActionType::Command,  kFollow);
    const uint32_t defensive = packPetAction(ActionType::Reaction, kDefensive);

    // Same low bits - the id alone cannot distinguish them.
    REQUIRE(petActionId(follow) == petActionId(defensive));
    REQUIRE(follow != defensive);
    REQUIRE(petActionType(follow) == ActionType::Command);
    REQUIRE(petActionType(defensive) == ActionType::Reaction);
}

TEST_CASE("round trip through unpack", "[pet]") {
    for (uint32_t id : {kStay, kFollow, kAttack, kAbandon, kMoveTo}) {
        const uint32_t packed = packPetAction(ActionType::Command, id);
        REQUIRE(petActionId(packed) == id);
        REQUIRE(petActionType(packed) == ActionType::Command);
    }
}

TEST_CASE("only castable slots count as spells", "[pet]") {
    // Commands and stances are not spells, whatever their id - the old test for
    // this was "id > 6", which called stay/follow/attack spells the moment the
    // server sent them with their real ids of 0/1/2.
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kStay)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Command, kAttack)));
    REQUIRE_FALSE(isPetSpellAction(packPetAction(ActionType::Reaction, kPassive)));

    REQUIRE(isPetSpellAction(packPetAction(ActionType::Disabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Enabled, 2649u)));
    REQUIRE(isPetSpellAction(packPetAction(ActionType::Passive, 3110u)));
}

TEST_CASE("a spell id larger than 24 bits cannot corrupt the type", "[pet]") {
    const uint32_t packed = packPetAction(ActionType::Enabled, 0xFFFFFFFFu);
    REQUIRE(petActionType(packed) == ActionType::Enabled);
    REQUIRE(petActionId(packed) == 0x00FFFFFFu);
}

TEST_CASE("stay and passive have zero low bits but non-zero packed values", "[pet]") {
    // Regression: CastPetAction and GetPetActionSlotUsable previously checked
    // (packed & 0x00FFFFFF) != 0, silently dropping Stay (0x07000000) and
    // Passive (0x06000000).
    const uint32_t stay    = packPetAction(ActionType::Command,  kStay);
    const uint32_t passive = packPetAction(ActionType::Reaction, kPassive);
    REQUIRE(stay != 0u);
    REQUIRE(passive != 0u);
    REQUIRE(petActionId(stay) == 0u);
    REQUIRE(petActionId(passive) == 0u);
    REQUIRE_FALSE(isPetSpellAction(stay));
    REQUIRE_FALSE(isPetSpellAction(passive));
}

TEST_CASE("possessed unit action routing sends spells via CMSG_PET_CAST_SPELL", "[pet][possess]") {
    // Power of the Blue Flight (entry 25653) abilities arrive packed with ACT_PASSIVE (0x01):
    // 45848 Shield of the Blue, 45856 Breath: Revitalize, 45860 Breath: Haste, 45862 Blink.
    for (uint32_t spellId : {45848u, 45856u, 45860u, 45862u}) {
        const uint32_t packed = packPetAction(ActionType::Passive, spellId);
        REQUIRE(possessActionRoute(packed, false) == PossessActionRoute::PetCastSpell);
    }

    // Enabled/Disabled pet spells also route via CMSG_PET_CAST_SPELL.
    REQUIRE(possessActionRoute(packPetAction(ActionType::Enabled, 45848u), false) ==
            PossessActionRoute::PetCastSpell);
    REQUIRE(possessActionRoute(packPetAction(ActionType::Disabled, 45848u), false) ==
            PossessActionRoute::PetCastSpell);

    // Ground-targeted possessed spells (e.g. Karazhan Chess Move) arm the ground-target cursor.
    REQUIRE(possessActionRoute(packPetAction(ActionType::Passive, 37146u), true) ==
            PossessActionRoute::GroundTarget);

    // Built-in commands and stances stay on CMSG_PET_ACTION.
    REQUIRE(possessActionRoute(packPetAction(ActionType::Command, kAttack), false) ==
            PossessActionRoute::PetAction);
    REQUIRE(possessActionRoute(packPetAction(ActionType::Command, kStay), false) ==
            PossessActionRoute::PetAction);
    REQUIRE(possessActionRoute(packPetAction(ActionType::Reaction, kPassive), false) ==
            PossessActionRoute::PetAction);

    // Empty slot does nothing.
    REQUIRE(possessActionRoute(0u, false) == PossessActionRoute::None);
}
