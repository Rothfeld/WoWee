// Build 8606 indoor/outdoor unit and doodad directional light rig math
// (rendering/legacy_light_direction.hpp) and ADT MCSH 64x64 shadow bit lookup.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "pipeline/adt_mcsh.hpp"
#include "rendering/legacy_light_direction.hpp"

#include <glm/glm.hpp>

using wowee::pipeline::MapChunk;
using wowee::rendering::DirectionalLightRig;
using wowee::rendering::blendIndoorWithOutdoorRig;
using wowee::rendering::capChannelMax;
using wowee::rendering::floorChannelMax;
using wowee::rendering::indoorDoodadRig;
using wowee::rendering::indoorUnitRig;
using wowee::rendering::kLegacyIndoorLightDir;
using wowee::rendering::mergeDirectionalRig;

TEST_CASE("indoorUnitRig caps ambient at 96/255 and floors diffuse at 168/255", "[lighting][unit]") {
    // Bright MOCV: ambient capped preserving hue, diffuse untouched.
    const glm::vec3 bright(0.8f, 0.4f, 0.2f);
    const DirectionalLightRig rBright = indoorUnitRig(bright);
    CHECK(rBright.ambient.r == Catch::Approx(96.0f / 255.0f));
    CHECK(rBright.ambient.g == Catch::Approx(48.0f / 255.0f));
    CHECK(rBright.ambient.b == Catch::Approx(24.0f / 255.0f));
    CHECK(rBright.diffuse.r == Catch::Approx(0.8f));
    CHECK(rBright.diffuse.g == Catch::Approx(0.4f));
    CHECK(rBright.diffuse.b == Catch::Approx(0.2f));
    CHECK(rBright.dir == kLegacyIndoorLightDir);

    // Dark MOCV (e.g. Scholomance Ossuary stairs ~0.25): ambient kept, diffuse floored to 168/255.
    const glm::vec3 dark(0.20f, 0.25f, 0.30f);
    const DirectionalLightRig rDark = indoorUnitRig(dark);
    CHECK(rDark.ambient.r == Catch::Approx(0.20f));
    CHECK(rDark.ambient.g == Catch::Approx(0.25f));
    CHECK(rDark.ambient.b == Catch::Approx(0.30f));
    const float scale = (168.0f / 255.0f) / 0.30f;
    CHECK(rDark.diffuse.r == Catch::Approx(0.20f * scale));
    CHECK(rDark.diffuse.g == Catch::Approx(0.25f * scale));
    CHECK(rDark.diffuse.b == Catch::Approx(168.0f / 255.0f));
}

TEST_CASE("indoorDoodadRig floors diffuse at 112/255", "[lighting][unit]") {
    const DirectionalLightRig rig = indoorDoodadRig(glm::vec3(0.10f, 0.20f, 0.10f));
    CHECK(rig.ambient.g == Catch::Approx(0.20f));
    CHECK(rig.diffuse.g == Catch::Approx(112.0f / 255.0f));
    CHECK(rig.diffuse.r == Catch::Approx(56.0f / 255.0f));
}

TEST_CASE("mergeDirectionalRig adds parallel lights and transfers opposing dipole to ambient", "[lighting][unit]") {
    const DirectionalLightRig a{.ambient = glm::vec3(0.1f), .diffuse = glm::vec3(0.4f), .dir = glm::vec3(0, 0, -1)};
    const DirectionalLightRig b{.ambient = glm::vec3(0.2f), .diffuse = glm::vec3(0.3f), .dir = glm::vec3(0, 0, -1)};
    const DirectionalLightRig parallel = mergeDirectionalRig(a, b);
    CHECK(parallel.ambient.r == Catch::Approx(0.3f));
    CHECK(parallel.diffuse.r == Catch::Approx(0.7f));
    CHECK(parallel.dir.z == Catch::Approx(-1.0f));

    // Equal-luma opposing lights: dot(dir_b, dir) == -1 -> clamped to 0, 1/4 cancelled dipole -> ambient.
    const DirectionalLightRig c{.ambient = glm::vec3(0.0f), .diffuse = glm::vec3(0.4f), .dir = glm::vec3(0, 0, 1)};
    const DirectionalLightRig opposed = mergeDirectionalRig(a, c);
    CHECK(opposed.ambient.r == Catch::Approx(0.1f + 0.25f * 0.4f));
    CHECK(opposed.diffuse.r == Catch::Approx(0.4f - 0.25f * 0.4f));
}

TEST_CASE("blendIndoorWithOutdoorRig applies 2.5x diffuse for units at exteriorBlend=1", "[lighting][unit]") {
    const DirectionalLightRig indoor = indoorUnitRig(glm::vec3(0.3f));
    const glm::vec3 outAmb(0.2f, 0.25f, 0.3f);
    const glm::vec3 outDiff(0.4f, 0.3f, 0.2f);
    const glm::vec3 outDir(0.0f, 0.0f, -1.0f);

    const DirectionalLightRig fullyIn = blendIndoorWithOutdoorRig(indoor, outAmb, outDiff, outDir, 0.0f, true);
    CHECK(fullyIn.ambient == indoor.ambient);
    CHECK(fullyIn.diffuse == indoor.diffuse);

    const DirectionalLightRig fullyOutUnit = blendIndoorWithOutdoorRig(indoor, outAmb, outDiff, outDir, 1.0f, true);
    CHECK(fullyOutUnit.ambient == outAmb);
    CHECK(fullyOutUnit.diffuse.r == Catch::Approx(outDiff.r * 2.5f));

    const DirectionalLightRig fullyOutDoodad = blendIndoorWithOutdoorRig(indoor, outAmb, outDiff, outDir, 1.0f, false);
    CHECK(fullyOutDoodad.diffuse.r == Catch::Approx(outDiff.r));
}

TEST_CASE("MapChunk::isShadowed reads 64x64 1-bit MCSH bit order", "[adt][lighting]") {
    MapChunk chunk{};
    CHECK_FALSE(chunk.isShadowed(0.0f, 0.0f));

    chunk.hasShadowMap = true;
    // Row 3, col 10 -> byte 3*8 + 1 = 25, bit 2
    chunk.shadowMap[25] = static_cast<uint8_t>(1u << 2);
    CHECK(chunk.isShadowed(10.2f / 8.0f, 3.2f / 8.0f));
    CHECK_FALSE(chunk.isShadowed(9.2f / 8.0f, 3.2f / 8.0f));
    CHECK_FALSE(chunk.isShadowed(10.2f / 8.0f, 4.2f / 8.0f));
}

TEST_CASE("unpackMcshShadowMap expands 512-byte 64x64 1-bit MCSH matching MapChunk::isShadowed", "[adt][lighting][mcsh]") {
    MapChunk chunk{};
    chunk.hasShadowMap = true;
    // Set corners and interior bits: (row=0,col=0), (row=0,col=63), (row=63,col=0), (row=63,col=63), (row=3,col=10)
    chunk.shadowMap[0] |= static_cast<uint8_t>(1u << 0);
    chunk.shadowMap[7] |= static_cast<uint8_t>(1u << 7);
    chunk.shadowMap[63 * 8 + 0] |= static_cast<uint8_t>(1u << 0);
    chunk.shadowMap[63 * 8 + 7] |= static_cast<uint8_t>(1u << 7);
    chunk.shadowMap[3 * 8 + 1] |= static_cast<uint8_t>(1u << 2);

    const auto unpacked = wowee::pipeline::unpackMcshShadowMap(chunk.shadowMap);
    REQUIRE(unpacked.size() == 64u * 64u);

    int shadowedCount = 0;
    for (int row = 0; row < 64; ++row) {
        for (int col = 0; col < 64; ++col) {
            const uint8_t texel = unpacked[static_cast<size_t>(row * 64 + col)];
            const bool expected = chunk.isShadowed((static_cast<float>(col) + 0.5f) / 8.0f,
                                                   (static_cast<float>(row) + 0.5f) / 8.0f);
            REQUIRE(texel == (expected ? 255u : 0u));
            if (expected) ++shadowedCount;
        }
    }
    CHECK(shadowedCount == 5);
}
