#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "pipeline/m2_loader.hpp"
#include "rendering/lighting_manager.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/particle_emission.hpp"
#include "rendering/sky_system.hpp"
#include "tbc_data_test_util.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace wowee::rendering;

namespace {

std::string readShaderFile(const char* relativePath) {
    const std::filesystem::path candidates[] = {
        std::filesystem::path(relativePath),
        std::filesystem::path("..") / relativePath,
        std::filesystem::path("../..") / relativePath,
        std::filesystem::path(__FILE__).parent_path().parent_path() / relativePath,
    };
    for (const auto& candidate : candidates) {
        std::ifstream in(candidate);
        if (in.is_open()) {
            std::ostringstream ss;
            ss << in.rdbuf();
            return ss.str();
        }
    }
    FAIL("Unable to locate shader file: " << relativePath);
    return {};
}

} // namespace

TEST_CASE("resolveEffectiveOutdoorFog preserves authored TBC DBC fogColor and farclip/storm fogEnd",
          "[sidedisc5][fog][lighting][tbc]") {
    // Elwynn Clear LP 12 at 16:32: fogColor = (84.1, 90.3, 108.7)/255,
    // skyMiddleColor = (82.7, 126.7, 167.2)/255, fogEnd = 486.0 yd, fogStart = 121.5 yd.
    const glm::vec3 elwynnAuthoredFog(84.1f / 255.0f, 90.3f / 255.0f, 108.7f / 255.0f);
    const glm::vec3 elwynnSkyMid(82.7f / 255.0f, 126.7f / 255.0f, 167.2f / 255.0f);

    SECTION("TBC farclip=177 clamps fogEnd to 177 yd and ignores non-TBC fogSkyBlend=0.7 and fogStrength=0.4") {
        glm::vec3 fogColor = elwynnAuthoredFog;
        float fogStart = 121.5f;
        float fogEnd = 486.0f;
        resolveEffectiveOutdoorFog(fogColor, elwynnSkyMid, fogStart, fogEnd,
                                   177.0f, 0.7f, 0.4f, SubmergedLiquid::None, true);
        CHECK(fogColor.r == Catch::Approx(elwynnAuthoredFog.r));
        CHECK(fogColor.g == Catch::Approx(elwynnAuthoredFog.g));
        CHECK(fogColor.b == Catch::Approx(elwynnAuthoredFog.b));
        CHECK(fogEnd == Catch::Approx(177.0f));
        CHECK(fogStart == Catch::Approx(177.0f * 0.25f));
    }

    SECTION("TBC Stormspire arcane-storm blend (LP 552 + LP 209 at storm=0.71, fogEnd=302.8 yd) is not inflated 2.5x") {
        // At storm=0.71, Light 669 blends LP 552 (fogEnd=500) and LP 209 (fogEnd=222.22)
        // to fogEnd=302.8 yd (< farclip=477). Inflating by 1/0.4 = 2.5x to 757 yd
        // previously exposed floating rocks at 429..481 yd past the storm fog wall.
        const glm::vec3 stormspireFog(16.0f / 255.0f, 93.2f / 255.0f, 85.1f / 255.0f);
        const glm::vec3 stormspireSkyMid(58.0f / 255.0f, 162.0f / 255.0f, 207.0f / 255.0f);
        glm::vec3 fogColor = stormspireFog;
        float fogStart = -157.8f;
        float fogEnd = 302.8f;
        resolveEffectiveOutdoorFog(fogColor, stormspireSkyMid, fogStart, fogEnd,
                                   477.0f, 0.7f, 0.4f, SubmergedLiquid::None, true);
        CHECK(fogColor.r == Catch::Approx(stormspireFog.r));
        CHECK(fogColor.g == Catch::Approx(stormspireFog.g));
        CHECK(fogColor.b == Catch::Approx(stormspireFog.b));
        CHECK(fogEnd == Catch::Approx(302.8f));
        CHECK(fogStart == Catch::Approx(-157.8f));
    }

    SECTION("TBC submerged underwater fog preserves underwater fogEnd without viewDistance clamp") {
        glm::vec3 uwFog(0.1f, 0.3f, 0.4f);
        float fogStart = -41.67f;
        float fogEnd = 83.33f;
        resolveEffectiveOutdoorFog(uwFog, elwynnSkyMid, fogStart, fogEnd,
                                   50.0f, 0.7f, 0.4f, SubmergedLiquid::Water, true);
        CHECK(uwFog.r == Catch::Approx(0.1f));
        CHECK(fogEnd == Catch::Approx(83.33f));
        CHECK(fogStart == Catch::Approx(-41.67f));
    }

    SECTION("Non-TBC mode applies fogSkyBlend and fogStrength before clamping to viewDistance") {
        glm::vec3 fogColor = elwynnAuthoredFog;
        float fogStart = 100.0f;
        float fogEnd = 200.0f;
        resolveEffectiveOutdoorFog(fogColor, elwynnSkyMid, fogStart, fogEnd,
                                   300.0f, 0.5f, 0.4f, SubmergedLiquid::None, false);
        CHECK(fogColor.b == Catch::Approx(0.5f * (elwynnAuthoredFog.b + elwynnSkyMid.b)));
        // 200 / 0.4 = 500, then clamped to viewDistance = 300 while keeping 0.5 ratio.
        CHECK(fogEnd == Catch::Approx(300.0f));
        CHECK(fogStart == Catch::Approx(150.0f));
    }
}

TEST_CASE("shouldRenderProceduralCloudDome suppresses non-TBC cumulus in clear/rain TBC skies while preserving authored overcast decks",
          "[sidedisc5][sky][clouds][tbc]") {
    // Northshire LP 12 (FloatBand[3] = 0.50, IntBand[12] = black) and Elwynn Rain LP 13 (FloatBand[3] = 0.00):
    const float elwynnDensity = effectiveSkyCloudDensity(false, 0.50f, 16.5f, 0.0f);
    const float elwynnRainDensity = effectiveSkyCloudDensity(false, 0.00f, 16.5f, 0.8f);
    // Deadwind Pass LP 19 / Karazhan LP 386 (FloatBand[3] = 0.70, IntBand[12] = (13, 16, 18)):
    const float deadwindDensity = effectiveSkyCloudDensity(false, 0.70f, 19.1f, 0.0f, glm::vec3(0.3f), 1.0f);
    // Hinterlands LP 51 / Jintha'Alor LP 72 (FloatBand[3] = 0.50, IntBand[12] non-black):
    const float hinterlandsDensity = effectiveSkyCloudDensity(false, 0.50f, 22.0f, 0.0f, glm::vec3(0.0f), 1.0f);

    CHECK(elwynnDensity == Catch::Approx(0.50f));
    CHECK(deadwindDensity == Catch::Approx(0.70f));
    CHECK(overcastCloudCover(0.50f, 0.0f) == Catch::Approx(0.0f));
    CHECK(overcastCloudCover(0.70f, 1.0f) == Catch::Approx(1.0f));

    // TBC suppresses procedural cumulus on clear/rain FloatBand[3] <= 0.50 zones with black IntBand[12]:
    CHECK_FALSE(shouldRenderProceduralCloudDome(true, false, false, elwynnDensity, 0.50f, 0.0f));
    CHECK_FALSE(shouldRenderProceduralCloudDome(true, false, false, elwynnRainDensity, 0.00f, 0.0f));
    // TBC preserves authored overcast cloud decks (Deadwind Pass / Karazhan, Duskwood, Hinterlands):
    CHECK(shouldRenderProceduralCloudDome(true, false, false, deadwindDensity, 0.70f, 1.0f));
    CHECK(shouldRenderProceduralCloudDome(true, false, false, 0.68f, 0.68f, 0.0f));
    CHECK(shouldRenderProceduralCloudDome(true, false, false, hinterlandsDensity, 0.50f, 1.0f));
    // Outland M2 skyboxes and submerged cameras never draw the procedural cloud dome:
    CHECK_FALSE(shouldRenderProceduralCloudDome(true, true, false, deadwindDensity, 0.70f, 1.0f));
    CHECK_FALSE(shouldRenderProceduralCloudDome(true, false, true, deadwindDensity, 0.70f, 1.0f));

    // Non-TBC mode still renders procedural clouds when not using an original M2 skybox or submerged.
    CHECK(shouldRenderProceduralCloudDome(false, false, false, elwynnDensity, 0.50f, 0.0f));
    CHECK_FALSE(shouldRenderProceduralCloudDome(false, true, false, elwynnDensity, 0.50f, 0.0f));
    CHECK_FALSE(shouldRenderProceduralCloudDome(false, false, true, elwynnDensity, 0.50f, 0.0f));
    CHECK_FALSE(shouldRenderProceduralCloudDome(false, false, false, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("tbcDoodadFarclipCeiling prevents far-plane-sliced floating rock slabs across Stormspire farclip 350..777",
          "[sidedisc5][m2][viewdist][stormspire][tbc]") {
    // Live WOWEE_PAIR=15 Native TBC 2.4.3 measurements at Stormspire (4135.2, 3007.6, 344.97):
    //   1) rock at d3 = 436.0 yd (vrad*sc = 24.7 yd): culled at fc=440, drawn (faded) at fc=477
    //   2) rock at d3 = 463.2 yd (vrad*sc = 46.8 yd): culled at fc=477 & fc=500, drawn at fc=550
    //   3) rock at d3 = 474.7 yd (vrad*sc = 37.3 yd, slab_mid_sky): culled at fc=477 & fc=500, drawn at fc=550
    //   4) rock at d3 = 476.7 yd (vrad*sc = 26.3 yd, slab_right_sky): culled at fc=477 & fc=500, drawn at fc=550
    //   5) rock at d3 = 515.8 yd (vrad*sc = 24.7 yd): culled at fc=500, ~38% faded at fc=550, opaque at fc=650
    CHECK(436.0f > tbcDoodadFarclipCeiling(440.0f, 24.7f));
    CHECK(436.0f <= tbcDoodadFarclipCeiling(477.0f, 24.7f));

    CHECK(463.2f > tbcDoodadFarclipCeiling(477.0f, 46.8f));
    CHECK(463.2f > tbcDoodadFarclipCeiling(500.0f, 46.8f));
    CHECK(463.2f <= tbcDoodadFarclipCeiling(550.0f, 46.8f));

    CHECK(474.7f > tbcDoodadFarclipCeiling(477.0f, 37.3f));
    CHECK(474.7f > tbcDoodadFarclipCeiling(500.0f, 37.3f));
    CHECK(474.7f <= tbcDoodadFarclipCeiling(550.0f, 37.3f));

    CHECK(476.7f > tbcDoodadFarclipCeiling(477.0f, 26.3f));
    CHECK(476.7f > tbcDoodadFarclipCeiling(500.0f, 26.3f));
    CHECK(476.7f <= tbcDoodadFarclipCeiling(550.0f, 26.3f));

    const float max550 = tbcDoodadFarclipCeiling(550.0f, 24.7f);
    CHECK(max550 == Catch::Approx(525.3f));
    const float alpha550 = tbcDoodadDistanceAlpha(515.8f * 515.8f, max550 - 25.0f, 25.0f);
    CHECK(alpha550 == Catch::Approx(0.38f).margin(0.02f));

    // Massive nearby biodomes (vrad ~220 yd, dist ~60 yd) remain visible even at minimal farclip = 177:
    CHECK(tbcDoodadFarclipCeiling(177.0f, 220.0f) == Catch::Approx(88.5f));
    // Indoor WMO MODD doodads are unaffected by outdoor farclip sphere inset:
    CHECK(tbcDoodadFarclipCeiling(477.0f, 30.0f, true) == Catch::Approx(477.0f));
}

TEST_CASE("M2Renderer::isExteriorCulled culls exterior and roof GameObjects from interior rooms while keeping indoor GameObjects",
          "[sidedisc5][m2][chapel][cull][tbc]") {
    M2ModelGPU regularModel{};
    M2ModelGPU spellModel{};
    spellModel.isSpellEffect = true;

    // 1) Indoor Chapel Fire (e.g. Small Chapel Fire GO 182071 at z=139.9 inside Alonsus Chapel)
    M2Instance indoorFire{};
    indoorFire.isGameObject = true;
    indoorFire.interiorLightQueried = true;
    indoorFire.wmoRoomInstance = 42u;
    indoorFire.interiorFog = true;
    indoorFire.position = glm::vec3(3671.7f, -3643.8f, 139.9f);
    indoorFire.cachedModel = &regularModel;
    CHECK_FALSE(M2Renderer::isExteriorCulled(true, indoorFire, true, glm::vec3(3685.5f, -3643.8f, 141.3f)));
    CHECK_FALSE(M2Renderer::isExteriorCulled(false, indoorFire, true, glm::vec3(3685.5f, -3643.8f, 141.3f)));

    // 2) Roof Chapel Fire (e.g. Large Chapel Fire GO 182072 at z=150.4 on exterior roof above Alonsus Chapel)
    M2Instance roofFire{};
    roofFire.isGameObject = true;
    roofFire.interiorLightQueried = true;
    roofFire.wmoRoomInstance = 0u;
    roofFire.interiorFog = false;
    roofFire.onExteriorRoofAboveInterior = true;
    roofFire.position = glm::vec3(3680.9f, -3644.4f, 150.4f);
    roofFire.cachedModel = &regularModel;
    // Culled whenever camera is inside the interior room below the roof (even facing an open doorway):
    CHECK(M2Renderer::isExteriorCulled(false, roofFire, true, glm::vec3(3686.9f, -3649.5f, 143.6f)));
    CHECK(M2Renderer::isExteriorCulled(true, roofFire, true, glm::vec3(3685.5f, -3643.8f, 141.3f)));
    // Visible as soon as camera exits into exterior Elders' Square (insideInterior == false):
    CHECK_FALSE(M2Renderer::isExteriorCulled(false, roofFire, false, glm::vec3(3655.0f, -3620.0f, 139.0f)));

    // 3) Street GameObject outside doorway (e.g. Supply Crate GO 176224 at z=138.4, not on a roof):
    M2Instance streetCrate{};
    streetCrate.isGameObject = true;
    streetCrate.interiorLightQueried = true;
    streetCrate.wmoRoomInstance = 0u;
    streetCrate.interiorFog = false;
    streetCrate.onExteriorRoofAboveInterior = false;
    streetCrate.position = glm::vec3(3665.5f, -3663.0f, 138.4f);
    streetCrate.cachedModel = &regularModel;
    // Visible through open doorway (exteriorHidden == false), culled when exteriorHidden == true:
    CHECK_FALSE(M2Renderer::isExteriorCulled(false, streetCrate, true, glm::vec3(3678.0f, -3643.8f, 141.3f)));
    CHECK(M2Renderer::isExteriorCulled(true, streetCrate, true, glm::vec3(3685.5f, -3643.8f, 141.3f)));

    // 4) Unqueried freshly spawned GameObject and externally posed unit/spell effects remain visible:
    M2Instance freshGo{};
    freshGo.isGameObject = true;
    freshGo.interiorLightQueried = false;
    freshGo.cachedModel = &regularModel;
    CHECK_FALSE(M2Renderer::isExteriorCulled(true, freshGo, true, glm::vec3(0.0f)));

    M2Instance unitEmitter{};
    unitEmitter.externalPose = true;
    unitEmitter.cachedModel = &regularModel;
    CHECK_FALSE(M2Renderer::isExteriorCulled(true, unitEmitter, true, glm::vec3(0.0f)));

    M2Instance spellFx{};
    spellFx.cachedModel = &spellModel;
    CHECK_FALSE(M2Renderer::isExteriorCulled(true, spellFx, true, glm::vec3(0.0f)));
}

TEST_CASE("skybox.frag.glsl, clouds.frag.glsl, and m2.frag.glsl skip non-TBC volumetric sky inscatter when TBC lightDir.w is active",
          "[sidedisc5][shader][sky][tbc]") {
    const std::string skyFrag = readShaderFile("assets/shaders/skybox.frag.glsl");
    const std::string cloudsFrag = readShaderFile("assets/shaders/clouds.frag.glsl");
    const std::string m2Frag = readShaderFile("assets/shaders/m2.frag.glsl");
    const std::string m2InstSrc = readShaderFile("src/rendering/m2_renderer_instance.cpp");

    CHECK(skyFrag.find("volumetricParams.x > 0.5 && lightDir.w == 0.0") != std::string::npos);
    CHECK(cloudsFrag.find("volumetricParams.x > 0.5 && lightDir.w == 0.0") != std::string::npos);
    CHECK(m2Frag.find("volumetricParams.x > 0.5 && lightDir.w == 0.0") != std::string::npos);
    CHECK(m2InstSrc.find("if (skyMode_) return;") != std::string::npos);
}

TEST_CASE("Shipped TBC Stormspire Light.dbc, NetherstormSkyBox.m2, and Stratholme fire/beam M2s satisfy parity invariants",
          "[sidedisc5][assets][tbc]") {
    // 1) Verify Stormspire Light 669 -> clear LP 552 (fogEnd=500 yd) / storm LP 209 (fogEnd=222.22 yd)
    const auto lightBytes = wowee::test::readTbcData("dbfilesclient/light.dbc");
    const auto fbBytes = wowee::test::readTbcData("dbfilesclient/lightfloatband.dbc");
    wowee::pipeline::DBCFile lightDbc;
    wowee::pipeline::DBCFile fbDbc;
    REQUIRE(lightDbc.load(lightBytes));
    REQUIRE(fbDbc.load(fbBytes));

    bool foundStormspireLight = false;
    for (uint32_t i = 0; i < lightDbc.getRecordCount(); ++i) {
        if (lightDbc.getUInt32(i, 0) == 669u) {
            CHECK(lightDbc.getUInt32(i, 1) == 530u);
            CHECK(lightDbc.getUInt32(i, 7) == 552u);
            CHECK(lightDbc.getUInt32(i, 8) == 209u);
            foundStormspireLight = true;
            break;
        }
    }
    REQUIRE(foundStormspireLight);

    const float lp552FogEnd = fbDbc.getFloat(552u * 6u - 6u, 18) / 36.0f;
    const float lp209FogEnd = fbDbc.getFloat(209u * 6u - 6u, 18) / 36.0f;
    CHECK(lp552FogEnd == Catch::Approx(500.0f).margin(0.1f));
    CHECK(lp209FogEnd == Catch::Approx(222.222f).margin(0.1f));

    // 2) Verify NetherstormSkyBox.m2 loads all 39 batches with valid textures and globalSequence alpha tracks
    const auto skyM2Bytes = wowee::test::readTbcData("environments/stars/netherstormskybox.m2");
    const wowee::pipeline::M2Model skyModel = wowee::pipeline::M2Loader::load(skyM2Bytes);
    REQUIRE(skyModel.isValid());
    CHECK(skyModel.version == 262u);
    CHECK(skyModel.batches.size() == 39u);
    CHECK(skyModel.colorAlphaTracks.size() == 14u);
    CHECK(skyModel.globalSequenceDurations.size() == 19u);

    // 3) Verify StratholmeFireSmokeEmberS.m2, StratholmeFireSmokeEmberM.m2, and WindowBeam02.m2
    const auto smallFireBytes = wowee::test::readTbcData(
        "world/lordaeron/stratholme/passivedoodads/fx/stratholmefiresmokeembers.m2");
    const wowee::pipeline::M2Model smallFire = wowee::pipeline::M2Loader::load(smallFireBytes);
    REQUIRE(smallFire.particleEmitters.size() == 2u);
    CHECK(smallFire.particleEmitters[0].emitterType == 1u);
    CHECK(smallFire.particleEmitters[1].emitterType == 1u);
    CHECK(smallFire.particleEmitters[1].blendingType == 4u);

    const auto largeFireBytes = wowee::test::readTbcData(
        "world/lordaeron/stratholme/passivedoodads/fx/stratholmefiresmokeemberm.m2");
    const wowee::pipeline::M2Model largeFire = wowee::pipeline::M2Loader::load(largeFireBytes);
    REQUIRE(largeFire.particleEmitters.size() == 3u);
    CHECK(largeFire.particleEmitters[0].blendingType == 2u);
    CHECK(largeFire.particleEmitters[0].particleScale.floatValues.at(2) == Catch::Approx(11.1111f).margin(0.01f));
    CHECK(largeFire.particleEmitters[2].blendingType == 4u);

    const auto beamBytes = wowee::test::readTbcData(
        "world/generic/passivedoodads/windowbeam/windowbeam02.m2");
    const wowee::pipeline::M2Model beamModel = wowee::pipeline::M2Loader::load(beamBytes);
    REQUIRE(beamModel.isValid());
    REQUIRE(beamModel.batches.size() == 1u);
    REQUIRE(beamModel.materials.size() == 1u);
    CHECK(beamModel.materials[0].blendMode == 4u);
    REQUIRE(beamModel.textureWeights.size() == 1u);
    CHECK(beamModel.textureWeights[0] == Catch::Approx(0.15f).margin(0.005f));
}

TEST_CASE("LightSkybox.dbc flags & 0x1 syncs NagrandSkyBox.mdx Sequence 0 to 24h game clock",
          "[nagrand][skybox][timesync][tbc]") {
    // 1) Pure clock mapping across 24 hours onto NagrandSkyBox.m2 Sequence 0 (320000 ms).
    constexpr float kNagrandSeqMs = 320000.0f;
    CHECK(computeSkyboxTimeOfDayAnimMs(0.0f, kNagrandSeqMs) == Catch::Approx(0.0f));
    CHECK(computeSkyboxTimeOfDayAnimMs(4.0f / 24.0f, kNagrandSeqMs) == Catch::Approx(53333.333f).margin(0.5f));
    CHECK(computeSkyboxTimeOfDayAnimMs(8.0f / 24.0f, kNagrandSeqMs) == Catch::Approx(106666.667f).margin(0.5f));
    CHECK(computeSkyboxTimeOfDayAnimMs(12.0f / 24.0f, kNagrandSeqMs) == Catch::Approx(160000.0f).margin(0.5f));
    CHECK(computeSkyboxTimeOfDayAnimMs(20.15f / 24.0f, kNagrandSeqMs) == Catch::Approx(268666.667f).margin(0.5f));
    CHECK(computeSkyboxTimeOfDayAnimMs(21.0f / 24.0f, kNagrandSeqMs) == Catch::Approx(280000.0f).margin(0.5f));
    // Wraps 1.0 -> 0.0 and negative fractions into [0, durationMs), staying strictly < durationMs.
    CHECK(computeSkyboxTimeOfDayAnimMs(1.0f, kNagrandSeqMs) == Catch::Approx(0.0f));
    CHECK(computeSkyboxTimeOfDayAnimMs(-0.25f, kNagrandSeqMs) == Catch::Approx(240000.0f).margin(0.5f));
    CHECK(computeSkyboxTimeOfDayAnimMs(0.999999f, kNagrandSeqMs) < kNagrandSeqMs);

    // 2) Shipped TBC LightSkybox.dbc sets flags=1 on row 12 (NagrandSkyBox.mdx) and flags=0 on other rows.
    const auto skyDbcBytes = wowee::test::readTbcData("dbfilesclient/lightskybox.dbc");
    wowee::pipeline::DBCFile skyDbc;
    REQUIRE(skyDbc.load(skyDbcBytes));
    REQUIRE(skyDbc.getFieldCount() >= 3u);
    uint32_t timeSyncedCount = 0;
    bool foundNagrandRow = false;
    for (uint32_t i = 0; i < skyDbc.getRecordCount(); ++i) {
        const uint32_t id = skyDbc.getUInt32(i, 0);
        const std::string path = skyDbc.getString(i, 1);
        const uint32_t flags = skyDbc.getUInt32(i, 2);
        if ((flags & kLightSkyboxFlagTimeOfDaySync) != 0u) {
            ++timeSyncedCount;
        }
        if (id == 12u) {
            CHECK(path == "Environments\\Stars\\NagrandSkyBox.mdx");
            CHECK(flags == kLightSkyboxFlagTimeOfDaySync);
            foundNagrandRow = true;
        } else {
            CHECK((flags & kLightSkyboxFlagTimeOfDaySync) == 0u);
        }
    }
    CHECK(foundNagrandRow);
    CHECK(timeSyncedCount == 1u);

    // 3) Shipped LightParams.dbc row 396 (Nagrand) references LightSkybox 12 (NagrandSkyBox.mdx).
    const auto lpBytes = wowee::test::readTbcData("dbfilesclient/lightparams.dbc");
    wowee::pipeline::DBCFile lpDbc;
    REQUIRE(lpDbc.load(lpBytes));
    bool foundLp396 = false;
    for (uint32_t i = 0; i < lpDbc.getRecordCount(); ++i) {
        if (lpDbc.getUInt32(i, 0) == 396u) {
            CHECK(lpDbc.getUInt32(i, 2) == 12u);
            foundLp396 = true;
            break;
        }
    }
    CHECK(foundLp396);

    // 4) NagrandSkyBox.m2 Sequence 0 (320000 ms) + 5 real-time GlobalSequences.
    const auto nagSkyBytes = wowee::test::readTbcData("environments/stars/nagrandskybox.m2");
    const wowee::pipeline::M2Model nagSky = wowee::pipeline::M2Loader::load(nagSkyBytes);
    REQUIRE(nagSky.isValid());
    REQUIRE(nagSky.sequences.size() == 1u);
    CHECK(nagSky.sequences[0].duration == 320000u);
    CHECK(nagSky.globalSequenceDurations.size() == 5u);
}

TEST_CASE("Pass-2 alpha-blended M2 doodad fade skips 4x4 Bayer screen-door discard",
          "[m2][doodad-fade][shader][tbc]") {
    const std::string m2Vert = readShaderFile("assets/shaders/m2.vert.glsl");
    const std::string m2Frag = readShaderFile("assets/shaders/m2.frag.glsl");
    CHECK(m2Vert.find("((lightFlags & 8u) != 0u ? 16 : 0)") != std::string::npos);
    CHECK(m2Frag.find("const bool isBlendedInstanceFade = (vSkyMode & 16) != 0;") != std::string::npos);
    CHECK(m2Frag.find("!isGroundDetailFringe && !isBlendedInstanceFade && blendMode <= 1") != std::string::npos);
}
