#include <catch_amalgamated.hpp>

#include "audio/ambient_sound_manager.hpp"
#include "core/coordinates.hpp"
#include "pipeline/dbc_loader.hpp"
#include "rendering/night_sky_math.hpp"
#include "rendering/sky_params_from_lighting.hpp"
#include "rendering/sky_system.hpp"
#include "rendering/sun_direction.hpp"
#include "tbc_data_test_util.hpp"

#include <cmath>
#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering;
using wowee::audio::computeAmbiencePlaybackGain;
using wowee::audio::kAmbienceBusGain;

TEST_CASE("Deadwind Pass daytime sun stays visible under clear DBC overcast and attenuates in storms",
          "[sky][sun][sidedisc2]") {
    // LightParams 19 (Deadwind Pass / Karazhan exterior) authors FloatBand[2]
    // (CELESTIAL_GLOW_THROUGH) = 1.0 and FloatBand[3] (CLOUD_DENSITY) = 0.70.
    // Night stars/moon still suppress via overcastCloudCover == 1.0, while
    // daytime sun visibility follows active weatherIntensity * celestialGlowThrough.
    CHECK(overcastCloudCover(0.70f, 1.0f) == Catch::Approx(1.0f));
    CHECK(daytimeCelestialVisibility(0.0f, 1.0f) == Catch::Approx(1.0f));
    CHECK(daytimeCelestialVisibility(0.0f, 0.0f) == Catch::Approx(0.0f));
    CHECK(daytimeCelestialVisibility(0.93f, 1.0f) == Catch::Approx(0.0f));
    CHECK(daytimeCelestialVisibility(0.42f, 1.0f) > 0.25f);
    CHECK(daytimeCelestialVisibility(0.42f, 1.0f) < 0.75f);
}

TEST_CASE("Deadwind Pass 17:31 setting sun projects into lower-right NW sky beside Karazhan spire",
          "[sky][sun][sidedisc2]") {
    // In 20261126T173105Z/original.png, the camera at (-11143.2, -2102.1, 48.6)
    // is orbited to yaw ~78 deg, pitched up ~42 deg to frame Karazhan's 292z main
    // spire at pixel (811, 205), placing the 17:31 NW sun (az ~46 deg, elev ~28 deg)
    // in the lower-right sky above the battlements (u > 0.75, v > 0.60).
    // Conversely, facing due west at yaw = 1.65 rad (~94.54 deg, pitch +23.3 deg)
    // toward the lower rotunda dome at 18:17 puts the sun past the right viewport
    // edge (u > 1.15) behind the main spire rather than above the dome.
    const float yawRad = glm::radians(78.0f);
    const float pitchRad = glm::radians(42.0f);
    const glm::vec3 forward(std::cos(yawRad) * std::cos(pitchRad),
                            std::sin(yawRad) * std::cos(pitchRad),
                            std::sin(pitchRad));
    const glm::vec3 eye(-11145.5f, -2104.41f, 50.7f);
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(44.1f), 1280.0f / 720.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    const glm::vec3 sunDir = sunDirectionFromLightDir(
        sunLightDirectionForDayFraction((17.0f + 31.0f / 60.0f) / 24.0f));
    const SunOnScreen onScreen = sunScreenPosition(view, proj, sunDir);
    REQUIRE(onScreen.inFront);
    CHECK(onScreen.uv.x > 0.75f);
    CHECK(onScreen.uv.x < 1.02f);
    CHECK(onScreen.uv.y > 0.60f);
    CHECK(onScreen.uv.y < 0.95f);

    const float domeYaw = 1.65f;
    const float domePitch = glm::radians(23.3f);
    const glm::vec3 domeFwd(std::cos(domeYaw) * std::cos(domePitch),
                            std::sin(domeYaw) * std::cos(domePitch),
                            std::sin(domePitch));
    const glm::mat4 domeView = glm::lookAt(eye, eye + domeFwd, glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 sun1817 = sunDirectionFromLightDir(
        sunLightDirectionForDayFraction((18.0f + 17.0f / 60.0f) / 24.0f));
    const SunOnScreen domeSun = sunScreenPosition(domeView, proj, sun1817);
    REQUIRE(domeSun.inFront);
    CHECK(domeSun.uv.x > 1.15f);
}

TEST_CASE("underwaterSkyFogFactor obscures Stormwind Canal sky cutout while keeping Deadwind river misty ceiling",
          "[sky][water][underwater][sidedisc2]") {
    // Stormwind Canals underwater (Light 77 -> LightParams 24):
    // FogEnd = 1000 / 36 = 27.78 yd, FogStart = -27.78 yd.
    // Every sky elevation must fog to 0.0 transmittance (100% underwater fogColor)
    // so fogged buildings and lampposts above the surface never silhouette against
    // LP 24's static noon SkyMiddleColor (54, 112, 142).
    const float swFogEnd = 1000.0f / 36.0f;
    const float swFogStart = -swFogEnd;
    CHECK(underwaterSkyFogFactor(0.0f, swFogStart, swFogEnd) == Catch::Approx(0.0f));
    CHECK(underwaterSkyFogFactor(0.25f, swFogStart, swFogEnd) == Catch::Approx(0.0f));
    CHECK(underwaterSkyFogFactor(1.0f, swFogStart, swFogEnd) == Catch::Approx(0.0f));
    CHECK(daytimeCelestialVisibility(0.0f, 1.0f, true, 0.3f, swFogStart, swFogEnd) ==
          Catch::Approx(0.0f));

    // Deadwind Pass river underwater (Light 17 -> LightParams 20):
    // FogEnd = 3000 / 36 = 83.33 yd, FogStart = -41.67 yd.
    // Retains partial sky & sun transmittance (~0.32..0.44) for the misty blue-grey ceiling.
    const float dwFogEnd = 3000.0f / 36.0f;
    const float dwFogStart = -0.5f * dwFogEnd;
    const float dwHorizon = underwaterSkyFogFactor(0.10f, dwFogStart, dwFogEnd);
    const float dwUpper = underwaterSkyFogFactor(0.35f, dwFogStart, dwFogEnd);
    const float dwZenith = underwaterSkyFogFactor(1.0f, dwFogStart, dwFogEnd);
    CHECK(dwHorizon > 0.25f);
    CHECK(dwUpper > dwHorizon);
    CHECK(dwZenith > dwUpper);
    CHECK(dwZenith < 0.60f);

    // skyParamsFromLighting forwards fogStart and fogEnd from LightingParams.
    LightingParams lp;
    lp.fogStart = dwFogStart;
    lp.fogEnd = dwFogEnd;
    const SkyParams sp = skyParamsFromLighting(17.0f, 17.0f, 0.0f, &lp, false);
    CHECK(sp.fogStart == Catch::Approx(dwFogStart));
    CHECK(sp.fogEnd == Catch::Approx(dwFogEnd));
}

TEST_CASE("2D SoundAmbience playback gain matches Native TBC 2.4.3 across outdoor and indoor SoundEntries",
          "[audio][ambience][sidedisc2]") {
    CHECK(kAmbienceBusGain == Catch::Approx(0.61f));
    // DeadwindPassDay.wav (SE 4212, vol=0.69): Native TBC 2.4.3 measured 0.4197
    CHECK(computeAmbiencePlaybackGain(0.69f, 1.0f) == Catch::Approx(0.4197f).margin(0.005f));
    // Ironforge.wav (SE 4162, vol=0.55): Native TBC 2.4.3 measured 0.3354
    CHECK(computeAmbiencePlaybackGain(0.55f, 1.0f) == Catch::Approx(0.3354f).margin(0.005f));
    // Muted / clamped bounds
    CHECK(computeAmbiencePlaybackGain(0.69f, 0.0f) == Catch::Approx(0.0f));
    CHECK(computeAmbiencePlaybackGain(0.69f, 0.5f) == Catch::Approx(0.61f * 0.69f * 0.5f));

    const auto seBytes = wowee::test::readTbcData("dbfilesclient/soundentries.dbc");
    wowee::pipeline::DBCFile seDbc;
    REQUIRE(seDbc.load(seBytes));
    bool foundDeadwind = false;
    bool foundIronforge = false;
    for (uint32_t i = 0; i < seDbc.getRecordCount(); ++i) {
        const uint32_t id = seDbc.getUInt32(i, 0);
        if (id == 4212u) {
            const float vol = seDbc.getFloat(i, 24);
            CHECK(vol == Catch::Approx(0.69f).margin(0.01f));
            CHECK(computeAmbiencePlaybackGain(vol, 1.0f) == Catch::Approx(0.4209f).margin(0.005f));
            foundDeadwind = true;
        } else if (id == 4162u) {
            const float vol = seDbc.getFloat(i, 24);
            CHECK(vol == Catch::Approx(0.55f).margin(0.01f));
            CHECK(computeAmbiencePlaybackGain(vol, 1.0f) == Catch::Approx(0.3355f).margin(0.005f));
            foundIronforge = true;
        }
    }
    CHECK(foundDeadwind);
    CHECK(foundIronforge);
}
