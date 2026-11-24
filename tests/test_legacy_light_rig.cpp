// Build 8606 indoor/outdoor unit and doodad directional light rig math
// (rendering/legacy_light_direction.hpp) and ADT MCSH 64x64 shadow bit lookup.
#include <catch_amalgamated.hpp>

#include "pipeline/adt_loader.hpp"
#include "pipeline/adt_mcsh.hpp"
#include "rendering/legacy_light_direction.hpp"
#include "rendering/lighting_manager.hpp"

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

TEST_CASE("tbcModBlendSourceColor attenuates lit Mod2x M2 batches at night while preserving unlit Mod2x", "[lighting][unit][m2]") {
    // Mirage Raceway (LightParams 239) night (23:12): Ambient_DBC = (0, 30, 60)/255 -> R = 0.
    // On a shaded FelBeast.m2 carapace normal (N.L == 0), litFactor = (0, 0.118, 0.235).
    const glm::vec3 orangeTex(0.75f, 0.42f, 0.18f);
    const glm::vec3 white(1.0f);
    const glm::vec3 zero(0.0f);
    const glm::vec3 nightShadowLit(0.0f, 30.0f / 255.0f, 60.0f / 255.0f);

    // Lit Mod2x batch (rf_flags = 0x10 -> unlit = false): red channel attenuates to 0 in shadow
    // so framebuffer 2*src*dst darkens the shaded orange ridge instead of glowing unlit.
    const glm::vec3 litSrc = wowee::rendering::tbcModBlendSourceColor(
        orangeTex, white, nightShadowLit, zero, white, false, true, 1.0f, 1.0f);
    CHECK(litSrc.r == Catch::Approx(0.0f));
    CHECK(litSrc.g == Catch::Approx(orangeTex.g * (30.0f / 255.0f)));
    CHECK(litSrc.b == Catch::Approx(orangeTex.b * (60.0f / 255.0f)));

    // Unlit Mod2x batch (rf_flags = 0x11 -> unlit = true): retains full unlit texture RGB.
    const glm::vec3 unlitSrc = wowee::rendering::tbcModBlendSourceColor(
        orangeTex, white, nightShadowLit, zero, white, true, true, 1.0f, 1.0f);
    CHECK(unlitSrc.r == Catch::Approx(orangeTex.r));
    CHECK(unlitSrc.g == Catch::Approx(orangeTex.g));
    CHECK(unlitSrc.b == Catch::Approx(orangeTex.b));

    // Full fog (fogFactor = 0) converges to Mod2x neutral (0.5) and Mod neutral (1.0).
    const glm::vec3 foggedMod2x = wowee::rendering::tbcModBlendSourceColor(
        orangeTex, white, nightShadowLit, zero, white, false, true, 0.0f, 1.0f);
    const glm::vec3 foggedMod = wowee::rendering::tbcModBlendSourceColor(
        orangeTex, white, nightShadowLit, zero, white, false, false, 0.0f, 1.0f);
    CHECK(foggedMod2x.r == Catch::Approx(0.5f));
    CHECK(foggedMod.r == Catch::Approx(1.0f));
}

TEST_CASE("tbcModBlendSourceColor applies CharProc 1 kitColor before hoverAdd inside [0, 1] clamp", "[lighting][unit][m2]") {
    // Shadowform (Spell 16592 / 15473, SVK 3129): dark purple CharProc 1 kitColor.
    // Adding hoverAdd after kitColor lets hovered/targeted Shadowform units brighten
    // by rawTexRgb * hoverAdd instead of crushing hoverAdd near zero.
    const glm::vec3 robeTex(0.45f, 0.30f, 0.60f);
    const glm::vec3 white(1.0f);
    const glm::vec3 lit(0.80f, 0.75f, 0.70f);
    const glm::vec3 ambientHover(0.35f, 0.32f, 0.30f);
    const glm::vec3 shadowformKit(0.11f, 0.01f, 0.18f);

    const glm::vec3 unhovered = wowee::rendering::tbcModBlendSourceColor(
        robeTex, white, lit, glm::vec3(0.0f), shadowformKit, false, false, 1.0f, 1.0f);
    const glm::vec3 hovered = wowee::rendering::tbcModBlendSourceColor(
        robeTex, white, lit, ambientHover, shadowformKit, false, false, 1.0f, 1.0f);

    CHECK(unhovered.r == Catch::Approx(robeTex.r * lit.r * shadowformKit.r));
    CHECK(unhovered.g == Catch::Approx(robeTex.g * lit.g * shadowformKit.g));
    CHECK(unhovered.b == Catch::Approx(robeTex.b * lit.b * shadowformKit.b));
    CHECK(hovered.r == Catch::Approx(robeTex.r * (lit.r * shadowformKit.r + ambientHover.r)));
    CHECK(hovered.g == Catch::Approx(robeTex.g * (lit.g * shadowformKit.g + ambientHover.g)));
    CHECK(hovered.b == Catch::Approx(robeTex.b * (lit.b * shadowformKit.b + ambientHover.b)));

    // Red CharProc 1 (e.g. Spell 37112): R saturates at 1.0 while G/B brighten by ambientHover.
    const glm::vec3 redKit(1.0f, 0.13f, 0.18f);
    const glm::vec3 redHovered = wowee::rendering::tbcModBlendSourceColor(
        robeTex, white, lit, ambientHover, redKit, false, false, 1.0f, 1.0f);
    CHECK(redHovered.r == Catch::Approx(robeTex.r * 1.0f));
    CHECK(redHovered.g == Catch::Approx(robeTex.g * (lit.g * redKit.g + ambientHover.g)));
    CHECK(redHovered.b == Catch::Approx(robeTex.b * (lit.b * redKit.b + ambientHover.b)));

    // Normal un-tinted unit (kitColor == white) is unaffected.
    const glm::vec3 normalHovered = wowee::rendering::tbcModBlendSourceColor(
        robeTex, white, lit, ambientHover, white, false, false, 1.0f, 1.0f);
    CHECK(normalHovered.r == Catch::Approx(robeTex.r * 1.0f));
    CHECK(normalHovered.g == Catch::Approx(robeTex.g * 1.0f));
    CHECK(normalHovered.b == Catch::Approx(robeTex.b * 1.0f));
}

TEST_CASE("tbcTerrainLightFactor clamps primary vertex lighting to [0, 1] matching TBC terrain.bls", "[lighting][terrain][tbc]") {
    // Sunstrider Isle (LightParams 379) night (23:36): amb=(0.259, 0.333, 0.596), diff=(0.314, 0.427, 0.541).
    // At N.L = 0.85, unclamped blue is 0.596 + 0.85*0.541 = 1.05585; TBC terrain.bls clamps to 1.0.
    const glm::vec3 amb(0.259f, 0.333f, 0.596f);
    const glm::vec3 diff(0.314f, 0.427f, 0.541f);
    const glm::vec3 clamped = wowee::rendering::tbcTerrainLightFactor(amb, diff, 0.85f, 1.0f, true);
    const glm::vec3 unclamped = wowee::rendering::tbcTerrainLightFactor(amb, diff, 0.85f, 1.0f, false);
    CHECK(clamped.r == Catch::Approx(0.259f + 0.85f * 0.314f));
    CHECK(clamped.g == Catch::Approx(0.333f + 0.85f * 0.427f));
    CHECK(clamped.b == Catch::Approx(1.0f));
    CHECK(unclamped.b > 1.05f);
}
