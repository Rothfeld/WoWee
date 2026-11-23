#include <algorithm>
#include <catch_amalgamated.hpp>
#include <numeric>
#include <vector>
#include "rendering/character_material_policy.hpp"

using wowee::rendering::characterBatchDrawsBefore;
using wowee::rendering::characterBatchIsHair;
using wowee::rendering::characterBatchNeedsCutout;
using wowee::rendering::characterMaterialDomain;
using wowee::rendering::CharacterMaterialDomain;

constexpr auto character = CharacterMaterialDomain::Character;
constexpr auto creature = CharacterMaterialDomain::Creature;
constexpr auto scene = CharacterMaterialDomain::Scene;
constexpr auto attachment = CharacterMaterialDomain::Attachment;

TEST_CASE("attached item materials are distinct from characters and enchant effects", "[character][m2]") {
    CHECK(characterMaterialDomain(false, false, false, false) == character);
    CHECK(characterMaterialDomain(false, true, false, false) == creature);
    CHECK(characterMaterialDomain(true, true, true, false) == scene);
    CHECK(characterMaterialDomain(false, false, true, false) == attachment);
    CHECK(characterMaterialDomain(false, true, true, false) == attachment);
    // Existing enchant handling remains unchanged; it is not an item surface.
    CHECK(characterMaterialDomain(false, false, true, true) == character);
}

TEST_CASE("opaque creature and vehicle surfaces ignore an atlas alpha mask", "[character][m2]") {
    // SiegeVehicle's two bodywork batches both say opaque, but both atlases
    // contain low-alpha texels inside the UVs those batches actually sample.
    CHECK_FALSE(characterBatchIsHair(creature, false, 0, 0, 1));
    CHECK_FALSE(characterBatchNeedsCutout(creature, 0, true, false));
    CHECK_FALSE(characterBatchNeedsCutout(creature, 0, false, false));
    // The existing character and scene paths are not silently changed.
    CHECK(characterBatchNeedsCutout(character, 0, true, false));
    CHECK_FALSE(characterBatchNeedsCutout(scene, 0, true, false));
}

TEST_CASE("creature alpha-key and real hair remain cut out", "[character][m2]") {
    CHECK(characterBatchNeedsCutout(creature, 1, true, false));
    CHECK(characterBatchNeedsCutout(creature, 1, false, false));
    CHECK(characterBatchIsHair(creature, true, 201, 0, 1));
    CHECK(characterBatchNeedsCutout(creature, 0, true, true));
    CHECK_FALSE(characterBatchIsHair(scene, true, 201, 0, 1));
}

TEST_CASE("creature geosets are not mistaken for character hair", "[character][m2]") {
    CHECK_FALSE(characterBatchIsHair(creature, false, 201, 1, 1));
    CHECK_FALSE(characterBatchIsHair(creature, false, 42, 0, 2));
    CHECK(characterBatchIsHair(character, false, 201, 1, 1));
    CHECK(characterBatchIsHair(character, false, 42, 0, 2));
    CHECK_FALSE(characterBatchIsHair(character, false, 0, 0, 1));
}

TEST_CASE("creature blended and additive batches are not forced to cut out", "[character][m2]") {
    for (uint16_t mode : {uint16_t{2}, uint16_t{3}, uint16_t{4}, uint16_t{5}, uint16_t{6}}) {
        CHECK_FALSE(characterBatchNeedsCutout(creature, mode, false, false));
        CHECK_FALSE(characterBatchNeedsCutout(creature, mode, true, false));
    }
    // Preserve the existing character fallback and scene semantics.
    CHECK(characterBatchNeedsCutout(character, 2, false, false));
    CHECK_FALSE(characterBatchNeedsCutout(scene, 2, false, false));
}

TEST_CASE("attached equipment uses authored material alpha and not character geosets", "[character][m2]") {
    // The Bloodsurge blade is opaque despite an atlas alpha mask. Alpha-key
    // attachments must still cut out, and blended items must not be forced to.
    for (bool hasAlpha : {false, true}) {
        CHECK_FALSE(characterBatchNeedsCutout(attachment, 0, hasAlpha, false));
        CHECK(characterBatchNeedsCutout(attachment, 1, hasAlpha, false));
        for (uint16_t mode = 2; mode <= 7; ++mode)
            CHECK_FALSE(characterBatchNeedsCutout(attachment, mode, hasAlpha, false));
    }
    CHECK_FALSE(characterBatchIsHair(attachment, false, 201, 1, 1));
    CHECK_FALSE(characterBatchIsHair(attachment, false, 42, 0, 2));
    CHECK_FALSE(characterBatchIsHair(attachment, true, 201, 0, 1));
}

TEST_CASE("character batch ordering scopes materialLayer within each submesh", "[character][m2]") {
    // Glave_1H_DualBlade_D_02.m2 (Warglaive of Azzinoth):
    //   batch 0: prio=0, submesh=0, layer=0 (Opaque blade)
    //   batch 1: prio=0, submesh=0, layer=1 (Mod2x ArmorReflect4)
    //   batch 2: prio=0, submesh=0, layer=2 (Alpha hilt mask)
    //   batch 3: prio=0, submesh=1, layer=0 (AddAlpha scrolling green glow shell)
    // Submesh 1's layer-0 glow shell must draw AFTER submesh 0's layer-1/2 passes
    // so the blade's Mod2x reflection and Alpha mask do not wipe out the glow.
    struct BatchKey { int8_t prio; uint16_t submesh; uint16_t layer; };
    const std::vector<BatchKey> batches = {
        {0, 0, 0},
        {0, 0, 1},
        {0, 0, 2},
        {0, 1, 0},
    };
    std::vector<size_t> order(batches.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return characterBatchDrawsBefore(
            batches[a].prio, batches[a].submesh, batches[a].layer,
            batches[b].prio, batches[b].submesh, batches[b].layer);
    });
    CHECK(order == std::vector<size_t>{0, 1, 2, 3});

    // Within one submesh, materialLayer still orders passes 0 -> 1 -> 2.
    CHECK(characterBatchDrawsBefore(0, 0, 0, 0, 0, 1));
    CHECK(characterBatchDrawsBefore(0, 0, 1, 0, 0, 2));
    CHECK_FALSE(characterBatchDrawsBefore(0, 0, 2, 0, 0, 1));

    // priorityPlane still takes top precedence across submeshes.
    CHECK(characterBatchDrawsBefore(-1, 3, 2, 0, 0, 0));
    CHECK(characterBatchDrawsBefore(0, 1, 0, 1, 0, 0));
}
