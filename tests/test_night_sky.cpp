#include <catch_amalgamated.hpp>

#include "pipeline/dbc_loader.hpp"
#include "rendering/light_coords.hpp"
#include "rendering/light_volume_order.hpp"
#include "rendering/night_sky_math.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "rendering/sky_system.hpp"
#include "tbc_data_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <vector>

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

    // Dusk zones whose DBC SkyTop stays bright past 19:00 (e.g. Orgrimmar LightParams 247
    // at 19:45: SkyTop = (116, 78, 114), sunset keyframe at 21:00, night at 22:00)
    // keep full DBC dusk clouds until SkyTop darkens into night.
    const glm::vec3 orgDuskTop(116.0f / 255.0f, 78.0f / 255.0f, 114.0f / 255.0f);
    const glm::vec3 tanarisNightTop(26.0f / 255.0f, 27.0f / 255.0f, 28.0f / 255.0f);
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 19.75f, 0.0f, orgDuskTop) == Catch::Approx(0.5f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 22.0f, 0.0f, glm::vec3(0.0f)) == Catch::Approx(0.0f));
    CHECK(effectiveSkyCloudDensity(false, 0.5f, 22.25f, 0.0f, tanarisNightTop) == Catch::Approx(0.0f));

    // Permanent-overcast zones (Karazhan LightParams 386 / Deadwind Pass 19 / Duskwood 57:
    // FloatBand[3] = 0.70, SkyTop = (27, 34, 37)) keep their full overcast cloud deck at night
    // and suppress stars/moon through overcastCloudCover.
    const glm::vec3 karaNightTop(27.0f / 255.0f, 34.0f / 255.0f, 37.0f / 255.0f);
    CHECK(effectiveSkyCloudDensity(false, 0.70f, 22.2f, 0.0f, karaNightTop) == Catch::Approx(0.70f));
    CHECK(effectiveSkyCloudDensity(false, 0.68f, 0.05f, 0.0f, glm::vec3(0.0f)) == Catch::Approx(0.68f));
    CHECK(overcastCloudCover(0.50f) == Catch::Approx(0.0f));
    CHECK(overcastCloudCover(0.70f) == Catch::Approx(1.0f));
    CHECK(starFieldEffectiveIntensity(22.2f, overcastCloudCover(0.70f), 1.0f, karaNightTop) == Catch::Approx(0.0f));

    // Zones with FloatBand[3] = 0.50 AND an authored IntBand[12] secondary cloud
    // layer (e.g. Hinterlands LP 51 / Jintha'Alor LP 72: cldDrk = (38, 41, 37),
    // cloudLayer2Weight = 1.0) keep their night cloud deck and hide stars/moon,
    // while clear-night 0.50 zones with black IntBand[12] (Mirage Raceway LP 239)
    // remain cloudless starry skies.
    CHECK(effectiveSkyCloudDensity(false, 0.50f, 22.22f, 0.0f, glm::vec3(0.0f), 1.0f) == Catch::Approx(0.50f));
    CHECK(effectiveSkyCloudDensity(false, 0.50f, 22.22f, 0.0f, glm::vec3(0.0f), 0.0f) == Catch::Approx(0.0f));
    CHECK(overcastCloudCover(0.50f, 1.0f) == Catch::Approx(1.0f));
    CHECK(overcastCloudCover(0.50f, 0.0f) == Catch::Approx(0.0f));
    CHECK(starFieldEffectiveIntensity(22.22f, overcastCloudCover(0.50f, 1.0f), 1.0f, glm::vec3(0.0f)) == Catch::Approx(0.0f));
    CHECK(moonNightVisibility(22.22f, glm::vec3(0.0f), overcastCloudCover(0.50f, 1.0f), 1.0f) == Catch::Approx(0.0f));

    // Custom M2 skyboxes (Outland) never receive procedural dome clouds.
    CHECK(effectiveSkyCloudDensity(true, 0.5f, 12.0f, 0.8f) == Catch::Approx(0.0f));
    CHECK(effectiveSkyCloudDensity(true, 0.5f, 19.75f, 0.0f, orgDuskTop) == Catch::Approx(0.0f));
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

TEST_CASE("skyParamsFromLighting forwards cloudEmissiveColor and LightIntBand[9] sunColor from LightingParams", "[sky][night][clouds][celestial]") {
    LightingParams lp;
    lp.diffuseColor = glm::vec3(0.40f, 0.35f, 0.30f);
    lp.specularColor = glm::vec3(1.0f, 142.0f / 255.0f, 0.0f);  // Blackwing Lair LP 85 LightIntBand[9]
    lp.cloudColor = glm::vec3(50.0f / 255.0f, 97.0f / 255.0f, 132.0f / 255.0f);
    lp.cloudEmissiveColor = glm::vec3(18.0f / 255.0f, 56.0f / 255.0f, 81.0f / 255.0f);
    const SkyParams sp = skyParamsFromLighting(22.5f, 22.5f, 0.0f, &lp, false);
    CHECK(sp.sunColor.r == Catch::Approx(1.0f));
    CHECK(sp.sunColor.g == Catch::Approx(142.0f / 255.0f));
    CHECK(sp.sunColor.b == Catch::Approx(0.0f));
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

    const uint32_t lp192SunColorBlock = (192u - 1u) * 18u + 9u + 1u;
    const uint32_t lp85SunColorBlock = (85u - 1u) * 18u + 9u + 1u;
    const uint32_t lp192CloudSunBlock = (192u - 1u) * 18u + 10u + 1u;
    const uint32_t lp192CloudEmissiveBlock = (192u - 1u) * 18u + 11u + 1u;
    bool foundSun = false, foundEmissive = false;
    bool foundLp192Celestial = false, foundLp85Celestial = false;
    for (uint32_t i = 0; i < intBandDbc.getRecordCount(); ++i) {
        const uint32_t blk = intBandDbc.getUInt32(i, 0);
        if (blk == lp192SunColorBlock) {
            const uint32_t rgb = intBandDbc.getUInt32(i, 18); // midnight keyframe
            const uint32_t b = rgb & 0xFFu;
            const uint32_t g = (rgb >> 8) & 0xFFu;
            const uint32_t r = (rgb >> 16) & 0xFFu;
            CHECK(r > 200u);
            CHECK(g > 200u);
            CHECK(b > 200u); // Azeroth default night celestial tint is pale blue-white (232, 241, 255)
            foundLp192Celestial = true;
        } else if (blk == lp85SunColorBlock) {
            const uint32_t rgb = intBandDbc.getUInt32(i, 18); // Blackwing Lair LP 85
            const uint32_t b = rgb & 0xFFu;
            const uint32_t g = (rgb >> 8) & 0xFFu;
            const uint32_t r = (rgb >> 16) & 0xFFu;
            CHECK(r == 255u);
            CHECK(g == 142u);
            CHECK(b == 0u); // Blackwing Lair celestial tint is fiery orange (255, 142, 0)
            foundLp85Celestial = true;
        } else if (blk == lp192CloudSunBlock) {
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
    CHECK(foundLp192Celestial);
    CHECK(foundLp85Celestial);
    CHECK(foundSun);
    CHECK(foundEmissive);
}

TEST_CASE("Outland nested Light.dbc sub-volumes inherit enclosing zone LightSkybox in TBC 2.4.3",
          "[sky][skybox][tbc]") {
    using wowee::rendering::isMapDefaultLight;
    using wowee::rendering::layerLightSkyboxWeights;
    using wowee::rendering::layerLightWeights;
    using wowee::rendering::LIGHT_COORD_UNITS_PER_YARD;
    using wowee::rendering::lightPositionToWorld;
    using wowee::rendering::lightVolumeFalloff;
    using wowee::rendering::SkyVolumeCandidate;

    const auto lightBytes = wowee::test::readTbcData("dbfilesclient/light.dbc");
    const auto paramsBytes = wowee::test::readTbcData("dbfilesclient/lightparams.dbc");
    const auto skyboxBytes = wowee::test::readTbcData("dbfilesclient/lightskybox.dbc");

    wowee::pipeline::DBCFile lightDbc, paramsDbc, skyboxDbc;
    REQUIRE(lightDbc.load(lightBytes));
    REQUIRE(paramsDbc.load(paramsBytes));
    REQUIRE(skyboxDbc.load(skyboxBytes));

    std::map<uint32_t, uint32_t> skyboxByParams;
    for (uint32_t i = 0; i < paramsDbc.getRecordCount(); ++i) {
        skyboxByParams[paramsDbc.getUInt32(i, 0)] = paramsDbc.getUInt32(i, 2);
    }
    std::map<uint32_t, std::string> skyboxPaths;
    for (uint32_t i = 0; i < skyboxDbc.getRecordCount(); ++i) {
        skyboxPaths[skyboxDbc.getUInt32(i, 0)] = skyboxDbc.getString(i, 1);
    }

    struct Vol {
        uint32_t id;
        glm::vec3 pos;
        float innerR;
        float outerR;
        uint32_t clearLp;
        uint32_t stormLp;
    };
    std::vector<Vol> map530;
    uint32_t defClearSky = 0, defStormSky = 0;
    bool hasDefault = false;
    for (uint32_t i = 0; i < lightDbc.getRecordCount(); ++i) {
        if (lightDbc.getUInt32(i, 1) != 530u) continue;
        const float dbcX = lightDbc.getFloat(i, 2);
        const float dbcZ = lightDbc.getFloat(i, 3);
        const float dbcY = lightDbc.getFloat(i, 4);
        const uint32_t clearLp = lightDbc.getUInt32(i, 7);
        const uint32_t stormLp = lightDbc.getUInt32(i, 9);
        if (isMapDefaultLight(dbcX, dbcY, dbcZ)) {
            hasDefault = true;
            defClearSky = skyboxByParams[clearLp];
            defStormSky = skyboxByParams[stormLp];
            continue;
        }
        map530.push_back({
            .id = lightDbc.getUInt32(i, 0),
            .pos = lightPositionToWorld(dbcX, dbcY, dbcZ),
            .innerR = lightDbc.getFloat(i, 5) / LIGHT_COORD_UNITS_PER_YARD,
            .outerR = lightDbc.getFloat(i, 6) / LIGHT_COORD_UNITS_PER_YARD,
            .clearLp = clearLp,
            .stormLp = stormLp,
        });
    }
    REQUIRE(hasDefault);

    auto evalAt = [&](const glm::vec3& pt, float stormWeight = 0.0f) {
        struct Hit {
            const Vol* v;
            float falloff;
        };
        std::vector<Hit> hits;
        for (const auto& v : map530) {
            const float f = lightVolumeFalloff(glm::length(pt - v.pos), v.innerR, v.outerR);
            if (f > 0.0f) hits.push_back({&v, f});
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
            if (a.v->outerR != b.v->outerR) return a.v->outerR < b.v->outerR;
            return a.v->id < b.v->id;
        });
        std::vector<float> colorShares;
        std::vector<SkyVolumeCandidate> skyCands;
        for (const auto& h : hits) {
            colorShares.push_back(h.falloff);
            skyCands.push_back({
                .falloff = h.falloff,
                .clearSkyboxId = skyboxByParams[h.v->clearLp],
                .stormSkyboxId = skyboxByParams[h.v->stormLp],
            });
        }
        layerLightWeights(colorShares, hasDefault);
        uint32_t topColorLightId = hits.empty() ? 0u : hits[0].v->id;
        float topColorShare = colorShares.empty() ? 0.0f : colorShares[0];
        const auto skyShares = layerLightSkyboxWeights(
            skyCands, stormWeight, defClearSky, defStormSky, hasDefault);
        return std::make_tuple(topColorLightId, topColorShare, skyShares);
    };

    // 1. The Stormspire live user report spot & volume center: Light 669 (LP 552)
    //    owns 100% color share while NetherstormSkyBox.mdx (sb=10) stays at 1.0.
    for (const glm::vec3 pt : {glm::vec3(4192.57f, 3089.15f, 335.82f),
                               glm::vec3(4135.20f, 3007.60f, 344.20f)}) {
        const auto [colorId, colorShare, sky] = evalAt(pt);
        CHECK(colorId == 669u);
        CHECK(colorShare == Catch::Approx(1.0f));
        REQUIRE(sky.size() == 1);
        CHECK(skyboxPaths[sky[0].skyboxId] == "Environments\\Stars\\NetherstormSkyBox.mdx");
        CHECK(sky[0].weight == Catch::Approx(1.0f));
    }

    // 2. Eco-Dome Midrealm (Light 673, LP 552) & Manaforge B'naar / Scrap Field (Light 674, LP 520).
    {
        const auto [midId, midShare, midSky] = evalAt(glm::vec3(3449.08f, 2948.37f, 144.60f));
        CHECK(midId == 673u);
        CHECK(midShare == Catch::Approx(1.0f));
        REQUIRE(midSky.size() == 1);
        CHECK(skyboxPaths[midSky[0].skyboxId] == "Environments\\Stars\\NetherstormSkyBox.mdx");
        CHECK(midSky[0].weight == Catch::Approx(1.0f));

        const auto [mfId, mfShare, mfSky] = evalAt(glm::vec3(3254.70f, 2691.20f, 148.50f));
        CHECK(mfId == 674u);
        CHECK(mfShare == Catch::Approx(1.0f));
        REQUIRE(mfSky.size() == 1);
        CHECK(skyboxPaths[mfSky[0].skyboxId] == "Environments\\Stars\\NetherstormSkyBox.mdx");
        CHECK(mfSky[0].weight == Catch::Approx(1.0f));
    }

    // 3. Netherweb Ridge (Light 602: clear LP 565 sb=0, rain LP 564 sb=15) inside Bone Wastes (Light 742 sb=8).
    {
        const auto [nwId, nwShare, nwClear] = evalAt(glm::vec3(-3892.30f, 4327.70f, 6.50f), 0.0f);
        CHECK(nwId == 602u);
        CHECK(nwShare == Catch::Approx(1.0f));
        REQUIRE(nwClear.size() == 1);
        CHECK(skyboxPaths[nwClear[0].skyboxId] == "Environments\\Stars\\BoneWastesSkyBox.mdx");
        CHECK(nwClear[0].weight == Catch::Approx(1.0f));

        const auto [nwIdS, nwShareS, nwStorm] = evalAt(glm::vec3(-3892.30f, 4327.70f, 6.50f), 1.0f);
        REQUIRE(nwStorm.size() == 1);
        CHECK(skyboxPaths[nwStorm[0].skyboxId] == "Environments\\Stars\\BladesedgeNightElfSkyBox.mdx");
        CHECK(nwStorm[0].weight == Catch::Approx(1.0f));
    }

    // 4. Blade's Edge grove (Light 659, LP 554 sb=0) inside Blade's Edge (Light 455 sb=11).
    {
        const auto [beId, beShare, beSky] = evalAt(glm::vec3(2168.80f, 5450.20f, 149.10f));
        CHECK(beId == 659u);
        CHECK(beShare == Catch::Approx(1.0f));
        REQUIRE(beSky.size() == 1);
        CHECK(skyboxPaths[beSky[0].skyboxId] == "Environments\\Stars\\BladesEdgeSkyBox.mdx");
        CHECK(beSky[0].weight == Catch::Approx(1.0f));
    }
}
