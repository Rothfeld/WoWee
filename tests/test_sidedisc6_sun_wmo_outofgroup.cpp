#include <catch_amalgamated.hpp>

#include "rendering/sky_system.hpp"
#include "rendering/sun_direction.hpp"
#include "rendering/wmo_portal_visibility.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <glm/gtc/matrix_transform.hpp>

using namespace wowee::rendering;

TEST_CASE("Deadwind Pass 18:17 sun stays NW behind Karazhan spire and sets by 20:00",
          "[sky][sun][sidedisc6]") {
    // s3d_deadwind_sky camera: (-11145.5, -2104.4, 50.7), yaw = 1.65 rad (+94.54 deg),
    // pitch = +23.3 deg, 44.1 deg vFOV, 1280x720.
    // At 18:17 UTC, Native TBC 2.4.3 shows zero sun disc above Karazhan's curved
    // rotunda dome (x = 650..1020 px) because the NW sun (az ~ +45.7 deg) lies past
    // the right viewport edge (u > 1.15) behind Karazhan's main spire.
    const glm::vec3 eye(-11145.5f, -2104.4f, 50.7f);
    const float yaw = 1.65f;
    const float pitch = glm::radians(23.3f);
    const glm::vec3 fwd(std::cos(yaw) * std::cos(pitch),
                        std::sin(yaw) * std::cos(pitch),
                        std::sin(pitch));
    const glm::mat4 view = glm::lookAt(eye, eye + fwd, glm::vec3(0.0f, 0.0f, 1.0f));
    glm::mat4 proj = glm::perspective(glm::radians(44.1f), 1280.0f / 720.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    const float h1817 = 18.0f + 17.0f / 60.0f;
    const glm::vec3 sun1817 = sunDirectionFromLightDir(sunLightDirectionForDayFraction(h1817 / 24.0f));
    CHECK(glm::degrees(std::asin(sun1817.z)) == Catch::Approx(19.3f).margin(1.0f));
    CHECK(glm::degrees(std::atan2(sun1817.y, sun1817.x)) == Catch::Approx(45.7f).margin(1.5f));

    const SunOnScreen s3dScreen = sunScreenPosition(view, proj, sun1817);
    REQUIRE(s3dScreen.inFront);
    CHECK(s3dScreen.uv.x > 1.15f);

    // sunDiscTimeIntensity (backing Celestial::getSunIntensity) stays full at 17:31
    // and 18:17, fades across dusk, and reaches 0.0 by 20:05 so 20:12 night skies
    // never draw a daytime sun disc.
    CHECK(sunDiscTimeIntensity(17.52f) == Catch::Approx(1.0f));
    CHECK(sunDiscTimeIntensity(h1817) == Catch::Approx(1.0f));
    CHECK(sunDiscTimeIntensity(19.65f) == Catch::Approx(0.5f).margin(0.05f));
    CHECK(sunDiscTimeIntensity(20.12f) == Catch::Approx(0.0f));
    CHECK(sunDiscTimeIntensity(4.5f) == Catch::Approx(0.0f));
}

TEST_CASE("Skybox model cache evicts reaped and non-resident M2 model IDs before reuse",
          "[sky][skybox][sidedisc6]") {
    std::unordered_map<std::string, uint32_t> loadedSkyModels{
        {"Environments\\Stars\\StratholmeSkyBox.mdx", 101u},
        {"Environments\\Stars\\SkyBoxHFP.mdx", 202u},
        {"Environments\\Stars\\NagrandSkyBox.mdx", 303u},
    };
    std::unordered_set<uint32_t> residentModels{202u};

    // 1. Drain reaped IDs evicted by M2Renderer::cleanupUnusedModels(): 101u removed.
    CHECK(evictReapedSkyModels(loadedSkyModels, {101u, 999u}) == 1u);
    CHECK(loadedSkyModels.find("Environments\\Stars\\StratholmeSkyBox.mdx") == loadedSkyModels.end());
    CHECK(loadedSkyModels.size() == 2u);

    // 2. Resident model (202u) resolves immediately without eviction.
    const uint32_t hfpId = resolveCachedSkyModelId(
        loadedSkyModels, "Environments\\Stars\\SkyBoxHFP.mdx",
        [&](uint32_t id) { return residentModels.count(id) > 0; });
    CHECK(hfpId == 202u);
    CHECK(loadedSkyModels.count("Environments\\Stars\\SkyBoxHFP.mdx") == 1u);

    // 3. Stale model cleared via unloadModel/clearModels without reap entry (303u):
    //    hasModel(303u) == false evicts stale cache entry and returns 0 so loadSkyboxModel reloads.
    const uint32_t staleId = resolveCachedSkyModelId(
        loadedSkyModels, "Environments\\Stars\\NagrandSkyBox.mdx",
        [&](uint32_t id) { return residentModels.count(id) > 0; });
    CHECK(staleId == 0u);
    CHECK(loadedSkyModels.find("Environments\\Stars\\NagrandSkyBox.mdx") == loadedSkyModels.end());
}
