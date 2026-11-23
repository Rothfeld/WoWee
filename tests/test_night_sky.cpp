#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "rendering/night_sky_math.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "rendering/sky_system.hpp"
#include "tbc_data_test_util.hpp"

#include <cmath>

using namespace wowee::rendering;

TEST_CASE("starfield fades out completely during daytime and peaks at night", "[sky][night][stars]") {
    // Daytime: zero stars
    CHECK(starFieldTimeVisibility(6.0f) == Catch::Approx(0.0f));
    CHECK(starFieldTimeVisibility(9.0f) == Catch::Approx(0.0f));
    CHECK(starFieldTimeVisibility(12.0f) == Catch::Approx(0.0f));
    CHECK(starFieldTimeVisibility(15.0f) == Catch::Approx(0.0f));
    CHECK(starFieldTimeVisibility(18.0f) == Catch::Approx(0.0f));

    // Dusk fade-in (18:00..20:00)
    const float dusk1830 = starFieldTimeVisibility(18.5f);
    const float dusk1900 = starFieldTimeVisibility(19.0f);
    const float dusk1930 = starFieldTimeVisibility(19.5f);
    CHECK(dusk1830 > 0.0f);
    CHECK(dusk1900 > dusk1830);
    CHECK(dusk1930 > dusk1900);
    CHECK(starFieldTimeVisibility(20.0f) == Catch::Approx(1.0f));

    // Full night (20:00..04:00)
    CHECK(starFieldTimeVisibility(22.25f) == Catch::Approx(1.0f));
    CHECK(starFieldTimeVisibility(0.03f) == Catch::Approx(1.0f));
    CHECK(starFieldTimeVisibility(3.5f) == Catch::Approx(1.0f));

    // Dawn fade-out (04:00..06:00)
    const float dawn0430 = starFieldTimeVisibility(4.5f);
    const float dawn0500 = starFieldTimeVisibility(5.0f);
    const float dawn0530 = starFieldTimeVisibility(5.5f);
    CHECK(dawn0430 < 1.0f);
    CHECK(dawn0500 < dawn0430);
    CHECK(dawn0530 < dawn0500);
    CHECK(dawn0530 > 0.0f);
}

TEST_CASE("starfield stays bright on clear Azeroth nights and suppresses under overcast or daylight", "[sky][night][stars]") {
    // Barrens / Thousand Needles / Westfall midnight: skyTop = (0, 0, 0)
    CHECK(starFieldEffectiveIntensity(0.03f, 0.0f, 1.0f, glm::vec3(0.0f)) == Catch::Approx(1.0f));

    // Tanaris midnight: skyTop = (26/255, 27/255, 28/255) -> luma ~0.105, still clear night
    const glm::vec3 tanarisNightTop(26.0f / 255.0f, 27.0f / 255.0f, 28.0f / 255.0f);
    CHECK(starFieldEffectiveIntensity(22.25f, 0.0f, 1.0f, tanarisNightTop) == Catch::Approx(1.0f));

    // Noon sky: zero stars regardless of sky color
    CHECK(starFieldEffectiveIntensity(12.0f, 0.0f, 1.0f, glm::vec3(0.0f)) == Catch::Approx(0.0f));

    // Heavy weather / storm suppresses stars
    CHECK(starFieldEffectiveIntensity(0.0f, 0.85f, 1.0f, glm::vec3(0.0f)) == Catch::Approx(0.0f));

    // DBC celestialGlowThrough == 0 suppresses stars
    CHECK(starFieldEffectiveIntensity(0.0f, 0.0f, 0.0f, glm::vec3(0.0f)) == Catch::Approx(0.0f));

    // Bright zenith sky suppresses stars even at night hour
    CHECK(starFieldEffectiveIntensity(0.0f, 0.0f, 1.0f, glm::vec3(0.5f, 0.6f, 0.8f)) == Catch::Approx(0.0f));
}

TEST_CASE("sky cloud layer enables on Azeroth DBC clouds and skips Outland skybox M2s and clear deserts", "[sky][night][clouds]") {
    // Azeroth outdoor zones with FloatBand[3] = 0.5 (Barrens, 1kNeedles, Westfall)
    CHECK(shouldRenderSkyClouds(false, 0.5f, 0.0f));

    // Clear desert zone with FloatBand[3] = 0.0 (Tanaris) stays cloud-free
    CHECK_FALSE(shouldRenderSkyClouds(false, 0.0f, 0.0f));
    // ...unless active storm weather arrives
    CHECK(shouldRenderSkyClouds(false, 0.0f, 0.5f));

    // Outland custom skybox M2s (Hellfire, Netherstorm, Nagrand) are never overlaid
    CHECK_FALSE(shouldRenderSkyClouds(true, 0.5f, 0.0f));
    CHECK_FALSE(shouldRenderSkyClouds(true, 1.0f, 1.0f));
}

TEST_CASE("effectiveSkyCloudDensity suppresses clear-night procedural clouds and preserves daytime and storm clouds", "[sky][night][clouds]") {
    // Clear night at Mirage Raceway (23:12 = 23.2h, FloatBand[3] = 0.5) and midnight (00:03):
    // Native TBC 2.4.3 CSkyScene renders only vertex sky bands + Stars.m2 (zero procedural clouds).
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 23.2f, 0.0f) == Catch::Approx(0.0f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 0.05f, 0.0f) == Catch::Approx(0.0f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 3.5f, 0.0f) == Catch::Approx(0.0f));

    // Daytime (06:00..18:00) preserves full authored DBC cloud density (FloatBand[3] = 0.5).
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 6.0f, 0.0f) == Catch::Approx(0.5f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 12.0f, 0.0f) == Catch::Approx(0.5f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 18.0f, 0.0f) == Catch::Approx(0.5f));

    // Smooth dusk fade-out (18:00..20:00) and dawn fade-in (04:00..06:00).
    const float duskMid = effectiveSkyCloudDensity(false, 0.5f, 19.0f, 0.0f);
    const float dawnMid = effectiveSkyCloudDensity(false, 0.5f, 5.0f, 0.0f);
    CHECK(duskMid > 0.05f);
    CHECK(duskMid < 0.45f);
    CHECK(dawnMid > 0.05f);
    CHECK(dawnMid < 0.45f);

    // Storm weather at night still brings in overcast storm clouds.
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 23.2f, 0.5f) == Catch::Approx(0.2f));

    // Custom M2 skyboxes (Outland) never receive procedural dome clouds.
    CHECK(effectiveSkyCloudDensity(true, 0.5f, 12.0f, 0.8f) == Catch::Approx(0.0f));
}

TEST_CASE("moon trajectory, angular scale, and visibility match Native TBC 2.4.3 night sky", "[sky][night][moon]") {
    // Tanaris 22:15 (22.25h): moon rising right on NNW horizon (~0.665 rad azimuth, low positive elevation)
    const glm::vec3 dir2215 = moonDirectionForTimeOfDay(22.25f);
    CHECK(glm::length(dir2215) == Catch::Approx(1.0f).margin(1e-5f));
    const float elev2215 = glm::degrees(std::asin(dir2215.z));
    CHECK(elev2215 > 0.5f);
    CHECK(elev2215 < 10.0f);
    const float az2215 = std::atan2(dir2215.y, dir2215.x);
    CHECK(az2215 == Catch::Approx(0.66522f).margin(0.25f));

    // Culmination around 02:00: ~45 deg elevation
    const glm::vec3 dir0200 = moonDirectionForTimeOfDay(2.0f);
    CHECK(glm::degrees(std::asin(dir0200.z)) == Catch::Approx(45.0f).margin(1.0f));

    // Daytime 12:00: moon below horizon and zero visibility
    const glm::vec3 dirNoon = moonDirectionForTimeOfDay(12.0f);
    CHECK(dirNoon.z < 0.0f);
    CHECK(moonNightVisibility(12.0f, glm::vec3(0.0f, 0.12f, 0.29f), 0.0f, 1.0f) == Catch::Approx(0.0f));

    // Visible on clear Tanaris night at 22:15, suppressed in heavy storm
    const glm::vec3 tanarisNightTop(26.0f / 255.0f, 27.0f / 255.0f, 28.0f / 255.0f);
    CHECK(moonNightVisibility(22.25f, tanarisNightTop, 0.0f, 1.0f) > 0.8f);
    CHECK(moonNightVisibility(22.25f, tanarisNightTop, 0.85f, 1.0f) == Catch::Approx(0.0f));

    // Angular scale at 800 units gives ~10.4 deg disc (UV radius 0.35 -> 70% of quad)
    const float scale = moonBillboardScaleAtDistance(800.0f);
    const float discDiameter = scale * 0.70f;
    const float angularDeg = glm::degrees(2.0f * std::atan((discDiameter * 0.5f) / 800.0f));
    CHECK(angularDeg == Catch::Approx(10.4f).margin(0.3f));
}

TEST_CASE("skyParamsFromLighting forwards cloudEmissiveColor from LightingParams", "[sky][night][clouds]") {
    LightingParams lp;
    lp.cloudColor = glm::vec3(50.0f / 255.0f, 97.0f / 255.0f, 132.0f / 255.0f);
    lp.cloudEmissiveColor = glm::vec3(18.0f / 255.0f, 56.0f / 255.0f, 81.0f / 255.0f);
    const SkyParams sp = skyParamsFromLighting(0.03f, 0.03f, 0.0f, &lp, false);
    CHECK(sp.cloudColor.b == Catch::Approx(132.0f / 255.0f));
    CHECK(sp.cloudEmissiveColor.b == Catch::Approx(81.0f / 255.0f));
}

TEST_CASE("TBC 2.4.3 DBC skybox and cloud bands match Azeroth night sky expectations", "[sky][night][tbc]") {
    const auto skyboxBytes = wowee::test::readTbcData("dbfilesclient/lightskybox.dbc");
    wowee::pipeline::DBCFile skyboxDbc;
    REQUIRE(skyboxDbc.load(skyboxBytes));

    bool hasStars = false;
    bool hasHellfire = false;
    bool hasNetherstorm = false;
    bool hasShadowmoon = false;
    bool hasNagrand = false;
    bool hasBladesEdge = false;
    for (uint32_t i = 0; i < skyboxDbc.getRecordCount(); ++i) {
        const std::string path = skyboxDbc.getString(i, 1);
        if (path == "Environments\\Stars\\Stars.mdx") hasStars = true;
        if (path == "Environments\\Stars\\HellfireSkyBox.mdx") hasHellfire = true;
        if (path == "Environments\\Stars\\NetherstormSkyBox.mdx") hasNetherstorm = true;
        if (path == "Environments\\Stars\\ShadowMoonSkyBox.mdx") hasShadowmoon = true;
        if (path == "Environments\\Stars\\NagrandSkyBox.mdx") hasNagrand = true;
        if (path == "Environments\\Stars\\BladesEdgeSkyBox.mdx") hasBladesEdge = true;
    }
    CHECK(hasStars);
    CHECK(hasHellfire);
    CHECK(hasNetherstorm);
    CHECK(hasShadowmoon);
    CHECK(hasNagrand);
    CHECK(hasBladesEdge);

    // Check Kalimdor default LightParams 192 cloud channels (IntBand 10 & 11) at midnight (t=0)
    const auto intBandBytes = wowee::test::readTbcData("dbfilesclient/lightintband.dbc");
    wowee::pipeline::DBCFile intBandDbc;
    REQUIRE(intBandDbc.load(intBandBytes));

    const uint32_t lp192CloudSunBlock = (192u - 1u) * 18u + 10u + 1u;
    const uint32_t lp192CloudEmissiveBlock = (192u - 1u) * 18u + 11u + 1u;
    bool foundSun = false, foundEmissive = false;
    for (uint32_t i = 0; i < intBandDbc.getRecordCount(); ++i) {
        const uint32_t blk = intBandDbc.getUInt32(i, 0);
        if (blk == lp192CloudSunBlock) {
            const uint32_t rgb = intBandDbc.getUInt32(i, 18); // first keyframe (t=0, midnight)
            const uint32_t b = rgb & 0xFFu;
            const uint32_t r = (rgb >> 16) & 0xFFu;
            CHECK(b > r); // Midnight cloud sun color is luminous blue (50, 97, 132)
            foundSun = true;
        } else if (blk == lp192CloudEmissiveBlock) {
            const uint32_t rgb = intBandDbc.getUInt32(i, 18);
            const uint32_t b = rgb & 0xFFu;
            const uint32_t r = (rgb >> 16) & 0xFFu;
            CHECK(b > r); // Midnight cloud emissive color is deep blue (18, 56, 81)
            foundEmissive = true;
        }
    }
    CHECK(foundSun);
    CHECK(foundEmissive);
}
