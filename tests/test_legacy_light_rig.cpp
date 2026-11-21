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

TEST_CASE("softenMcshShadowMap preserves uniform maps and smoothly softens 1-bit step edges", "[adt][lighting][mcsh]") {
    std::array<uint8_t, 4096> allLit{};
    CHECK(wowee::pipeline::softenMcshShadowMap(allLit) == allLit);

    std::array<uint8_t, 4096> allShadow{};
    allShadow.fill(255u);
    CHECK(wowee::pipeline::softenMcshShadowMap(allShadow) == allShadow);

    // Vertical half-plane step: cols [0..31] lit (0), cols [32..63] shadowed (255).
    std::array<uint8_t, 4096> halfPlane{};
    for (size_t row = 0; row < 64; ++row) {
        for (size_t col = 32; col < 64; ++col) {
            halfPlane[row * 64 + col] = 255u;
        }
    }
    const auto soft = wowee::pipeline::softenMcshShadowMap(halfPlane);
    const size_t midRow = 32 * 64;
    // Far from edge remains exact 0 and 255.
    CHECK(soft[midRow + 20] == 0u);
    CHECK(soft[midRow + 44] == 255u);
    // 5-tap binomial ramp across cols 30..33: strictly monotonic and symmetric (sum to 255).
    CHECK(soft[midRow + 29] == 0u);
    CHECK(soft[midRow + 30] > 0u);
    CHECK(soft[midRow + 31] > soft[midRow + 30]);
    CHECK(soft[midRow + 32] > soft[midRow + 31]);
    CHECK(soft[midRow + 33] > soft[midRow + 32]);
    CHECK(soft[midRow + 34] == 255u);
    CHECK(soft[midRow + 30] + soft[midRow + 33] == 255u);
    CHECK(soft[midRow + 31] + soft[midRow + 32] == 255u);
}

TEST_CASE("computeTerrainMcshFactor applies 0.7x TBC attenuation across night and day LightIntBand values", "[adt][lighting][mcsh]") {
    // Dynamic shadows off: 1.0 lit, 0.7 full MCSH shadow, 0.85 half-penumbra.
    CHECK(wowee::pipeline::computeTerrainMcshFactor(0.0f, false, 1.0f) == Catch::Approx(1.0f));
    CHECK(wowee::pipeline::computeTerrainMcshFactor(0.5f, false, 1.0f) == Catch::Approx(0.85f));
    CHECK(wowee::pipeline::computeTerrainMcshFactor(1.0f, false, 1.0f) == Catch::Approx(0.7f));

    // Dynamic shadows on: full dynamic shadow (shadow=0) avoids double-darkening;
    // sunlit fragments (shadow=1) still receive full 0.7x MCSH shadow.
    CHECK(wowee::pipeline::computeTerrainMcshFactor(1.0f, true, 0.0f) == Catch::Approx(1.0f));
    CHECK(wowee::pipeline::computeTerrainMcshFactor(1.0f, true, 1.0f) == Catch::Approx(0.7f));
    CHECK(wowee::pipeline::computeTerrainMcshFactor(0.0f, true, 0.0f) == Catch::Approx(1.0f));

    // Verify terrain lighting equation preserves exact 0.7x achromatic ratio for
    // both Ratchet night (23:07) and Ratchet/Everlook daytime (12:00) LightIntBand curves.
    struct LightCase { glm::vec3 amb; glm::vec3 diff; float ndl; };
    const LightCase cases[] = {
        // Ratchet 23:07 (blended Light 205/203/191)
        {{0.1841f, 0.4323f, 0.5074f}, {0.5361f, 0.7667f, 0.8288f}, 0.547f},
        // Ratchet 12:00 noon
        {{0.4237f, 0.3823f, 0.3506f}, {1.0000f, 0.6094f, 0.2861f}, 0.582f},
        // Everlook 22:42 night
        {{0.3920f, 0.2890f, 0.5870f}, {0.4140f, 0.3280f, 0.6190f}, 0.530f},
        // Everlook 12:00 noon
        {{0.4900f, 0.2820f, 0.5100f}, {0.6860f, 0.6350f, 0.8080f}, 0.582f},
    };
    const glm::vec3 albedo(0.45f, 0.60f, 0.35f);
    for (const auto& c : cases) {
        const glm::vec3 lit = (c.amb + c.ndl * c.diff) * albedo;
        const glm::vec3 shd = lit * wowee::pipeline::computeTerrainMcshFactor(1.0f, false, 1.0f);
        CHECK(shd.r / lit.r == Catch::Approx(0.7f));
        CHECK(shd.g / lit.g == Catch::Approx(0.7f));
        CHECK(shd.b / lit.b == Catch::Approx(0.7f));
    }
}
