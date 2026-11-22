#include <catch_amalgamated.hpp>

#include "core/creature_ground_alignment.hpp"
#include "rendering/character_depth_bias.hpp"
#include <cmath>
#include <optional>

using wowee::core::creatureGroundAlignment;
using wowee::rendering::computeCharacterDepthBias;

namespace {

wowee::pipeline::M2Model makeSlimeModel() {
    wowee::pipeline::M2Model m;
    m.globalFlags = 0x3u; // TiltX | TiltY
    m.boundMin = glm::vec3(-0.347f, -0.347f, 0.0f);
    m.boundMax = glm::vec3(0.347f, 0.347f, 2.083f);
    m.vertexBoxMin = glm::vec3(-3.798f, -1.816f, -0.120f);
    m.vertexBoxMax = glm::vec3(2.765f, 1.847f, 3.994f);
    return m;
}

} // namespace

TEST_CASE("Character depth bias pulls NO_ZWRITE blended and layered batches toward camera", "[character][depth_bias]") {
    // Opaque base layer (materialLayer=0, bm=0, flags=0): zero bias
    const auto opaqueBase = computeCharacterDepthBias(false, 0, 0, 0x00);
    REQUIRE(opaqueBase.constantFactor == 0.0f);
    REQUIRE(opaqueBase.slopeFactor == 0.0f);

    // Higher materialLayer pulls toward camera (negative under Vulkan LESS depth)
    const auto armorLayer = computeCharacterDepthBias(false, 2, 0, 0x00);
    REQUIRE(armorLayer.constantFactor < 0.0f);
    REQUIRE(armorLayer.constantFactor == Catch::Approx(-1.0f));
    REQUIRE(armorLayer.slopeFactor == 0.0f);

    // Slime.m2 Sub[4] corpse puddle: bm=2 (alpha), flags=0x10 (NO_ZWRITE), layer=0
    const auto slimePuddle = computeCharacterDepthBias(false, 0, 2, 0x10);
    REQUIRE(slimePuddle.constantFactor < 0.0f);
    REQUIRE(slimePuddle.slopeFactor < 0.0f);

    // Surface-fading instance must stay at 0 bias to match depth prepass EQUAL/LEQUAL
    const auto fading = computeCharacterDepthBias(true, 2, 2, 0x10);
    REQUIRE(fading.constantFactor == 0.0f);
    REQUIRE(fading.slopeFactor == 0.0f);
}

TEST_CASE("Corpse ground alignment has zero lift on flat floor and planar ramps", "[corpse][ground_alignment]") {
    const auto slime = makeSlimeModel();
    const glm::vec3 pos(100.0f, 200.0f, 50.0f);

    // Flat floor: zero pitch, zero roll, zero lift
    const auto flat = creatureGroundAlignment(slime, 1.5f, pos, 0.0f,
        [](const glm::vec3&) -> std::optional<float> { return 50.0f; }, true);
    REQUIRE(flat.rotation.x == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(flat.rotation.y == Catch::Approx(0.0f).margin(1e-5f));
    REQUIRE(flat.zLift == 0.0f);

    // Planar ramp z = 50 + 0.25*(x - 100) - 0.15*(y - 200): tilts accurately with zero lift
    const auto ramp = creatureGroundAlignment(slime, 1.5f, pos, 0.0f,
        [](const glm::vec3& p) -> std::optional<float> {
            return 50.0f + 0.25f * (p.x - 100.0f) - 0.15f * (p.y - 200.0f);
        }, true);
    REQUIRE(ramp.rotation.y == Catch::Approx(-std::atan(0.25f)).margin(1e-4f));
    REQUIRE(ramp.rotation.x == Catch::Approx(std::atan(-0.15f)).margin(1e-4f));
    REQUIRE(ramp.zLift == Catch::Approx(0.0f).margin(1e-5f));
}

TEST_CASE("Corpse ground alignment lifts wide puddle out of concave terrain hollow", "[corpse][ground_alignment]") {
    const auto slime = makeSlimeModel();
    const glm::vec3 pos(0.0f, 0.0f, 10.0f);

    // Concave bowl z = 10 + 0.02 * (x^2 + y^2): rims are higher than center.
    // Living slime uses tight boundingBox (~0.26 yd) and does not lift.
    // Dead slime uses wider clamped vertexBox footprint (~2.0 yd x 1.37 yd) and lifts
    // onto the chord so flat puddle edges clear the hollow sides.
    auto bowl = [](const glm::vec3& p) -> std::optional<float> {
        return 10.0f + 0.02f * (p.x * p.x + p.y * p.y);
    };
    const auto living = creatureGroundAlignment(slime, 1.5f, pos, 0.0f, bowl, false);
    REQUIRE(living.zLift == 0.0f);

    const auto dead = creatureGroundAlignment(slime, 1.5f, pos, 0.0f, bowl, true);
    REQUIRE(dead.zLift > 0.04f);
    REQUIRE(dead.zLift <= 0.15f);

    // Convex dome z = 10 - 0.02 * (x^2 + y^2): never lifts
    auto dome = [](const glm::vec3& p) -> std::optional<float> {
        return 10.0f - 0.02f * (p.x * p.x + p.y * p.y);
    };
    const auto deadDome = creatureGroundAlignment(slime, 1.5f, pos, 0.0f, dome, true);
    REQUIRE(deadDome.zLift == 0.0f);
}
