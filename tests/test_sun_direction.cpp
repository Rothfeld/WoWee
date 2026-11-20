// Where the sun is, and in particular where it is not.
//
// SkySystem::getSunPosition mirrored a sun below the horizon back up into the
// sky - `sunDir = dir` instead of `-dir` - and the lens flare is the only
// thing that ever asks it. So the mirror did nothing but invent a sun for the
// flare to draw around, at a position no sun was at, and the flare's own
// height attenuation read the mirrored height as a sun climbing the sky and
// let it through at full strength.
//
// Reported as a lens flare with no sun in sight.
#include <catch_amalgamated.hpp>

#include "rendering/sun_direction.hpp"

#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::celestialBillboardWorldPos;
using wowee::rendering::sunDirectionFromLightDir;
using wowee::rendering::sunLightDirectionForDayFraction;
using wowee::rendering::sunScreenPosition;

TEST_CASE("the sun is opposite the direction its light travels", "[sky][sun]") {
    // Light going down means a sun overhead.
    const glm::vec3 up = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, -1.0f));
    CHECK(up.z == Catch::Approx(1.0f));

    // And the answer is a unit vector whatever length it is handed.
    const glm::vec3 scaled = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, -57.0f));
    CHECK(glm::length(scaled) == Catch::Approx(1.0f));
    CHECK(scaled.z == Catch::Approx(1.0f));
}

TEST_CASE("a sun below the horizon stays below it", "[sky][sun]") {
    // Light travelling upward is light from under the ground. The sun is down
    // there with it, and this is the assertion the mirror would fail.
    const glm::vec3 night = sunDirectionFromLightDir(glm::vec3(0.0f, 0.0f, 1.0f));
    CHECK(night.z == Catch::Approx(-1.0f));
    CHECK(night.z < 0.0f);

    // Just under the horizon, which is where the mirror did the most damage:
    // a flare at full strength opposite a sun that had already set.
    const glm::vec3 dusk = sunDirectionFromLightDir(glm::vec3(0.6f, 0.0f, 0.08f));
    CHECK(dusk.z < 0.0f);
}

TEST_CASE("no sun given reads as straight up", "[sky][sun]") {
    // Everything downstream gates on height, and straight up is the value that
    // leaves those gates to decide rather than producing a sideways sun.
    const glm::vec3 none = sunDirectionFromLightDir(glm::vec3(0.0f));
    CHECK(none.z == Catch::Approx(1.0f));
    CHECK(glm::length(none) == Catch::Approx(1.0f));
}

// The sun shafts stream from where this puts the sun, and every full-screen pass
// samples with (0,0) at the top left. A sun that came out mirrored top to bottom
// would pour its rays up out of the ground.
TEST_CASE("the sun lands on screen where it is in the sky", "[sky][sun]") {
    const glm::vec3 eye(-8913.0f, 554.0f, 94.0f);
    const glm::vec3 forward(1.0f, 0.0f, 0.0f);
    const glm::mat4 view = glm::lookAt(eye, eye + forward, glm::vec3(0.0f, 0.0f, 1.0f));
    // As the camera builds it: Y flipped for Vulkan.
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 30000.0f);
    proj[1][1] *= -1.0f;

    // Dead ahead is the middle of the screen, wherever the camera stands.
    const auto ahead = sunScreenPosition(view, proj, forward);
    REQUIRE(ahead.inFront);
    CHECK(ahead.uv.x == Catch::Approx(0.5f).margin(1e-5f));
    CHECK(ahead.uv.y == Catch::Approx(0.5f).margin(1e-5f));

    // Ahead and a little up is above the middle - smaller v, toward the top.
    const auto raised = sunScreenPosition(view, proj, glm::normalize(glm::vec3(1.0f, 0.0f, 0.2f)));
    REQUIRE(raised.inFront);
    CHECK(raised.uv.y < 0.5f);
    CHECK(raised.uv.x == Catch::Approx(0.5f).margin(1e-5f));

    // Behind the camera it has no place on screen at all.
    CHECK_FALSE(sunScreenPosition(view, proj, -forward).inFront);
}

TEST_CASE("sun arc matches TBC 2.4.3 elevation and azimuth across the day", "[sky][sun]") {
    // Noon: 60 deg elevation toward +X, unit length
    const glm::vec3 noonSun = sunDirectionFromLightDir(sunLightDirectionForDayFraction(0.5f));
    CHECK(glm::length(noonSun) == Catch::Approx(1.0f).margin(1e-5f));
    CHECK(noonSun.z == Catch::Approx(0.8660254f).margin(1e-4f));
    CHECK(noonSun.y == Catch::Approx(0.0f).margin(1e-4f));
    CHECK(noonSun.x > 0.0f);

    // 13:57 (13.95h): measured Native TBC 2.4.3 in Elwynn Forest had elevation 49.2 deg
    // and direction ~(0.407, 0.511, 0.757).
    const glm::vec3 afternoonSun = sunDirectionFromLightDir(
        sunLightDirectionForDayFraction((13.0f + 57.0f / 60.0f) / 24.0f));
    CHECK(glm::degrees(std::asin(afternoonSun.z)) == Catch::Approx(49.1f).margin(1.0f));
    CHECK(afternoonSun.x > 0.35f);
    CHECK(afternoonSun.y > 0.45f);

    // Sunrise (06:00) at east horizon (-Y), sunset (18:00) at west horizon (+Y),
    // midnight (00:00) below horizon.
    const glm::vec3 sunrise = sunDirectionFromLightDir(sunLightDirectionForDayFraction(0.25f));
    const glm::vec3 sunset  = sunDirectionFromLightDir(sunLightDirectionForDayFraction(0.75f));
    const glm::vec3 midnight = sunDirectionFromLightDir(sunLightDirectionForDayFraction(0.0f));
    CHECK(sunrise.z == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(sunrise.y == Catch::Approx(-1.0f).margin(1e-5f));
    CHECK(sunset.z == Catch::Approx(0.0f).margin(1e-5f));
    CHECK(sunset.y == Catch::Approx(1.0f).margin(1e-5f));
    CHECK(midnight.z < -0.8f);
}

TEST_CASE("celestial billboard projects as a circle at all elevations and aspect ratios", "[sky][sun]") {
    const float elevationsDeg[] = {15.0f, 49.0f, 85.0f};
    const float aspects[] = {4.0f / 3.0f, 16.0f / 9.0f, 21.0f / 9.0f};

    for (float elevDeg : elevationsDeg) {
        const float elev = glm::radians(elevDeg);
        const glm::vec3 dir(std::cos(elev), 0.0f, std::sin(elev));
        const glm::vec3 center = dir * 800.0f;
        const glm::vec2 size(150.0f, 150.0f);

        // Quad vertices must lie strictly perpendicular to the radial vector
        const glm::vec3 pRight = celestialBillboardWorldPos(center, size, {0.5f, 0.0f});
        const glm::vec3 pLeft  = celestialBillboardWorldPos(center, size, {-0.5f, 0.0f});
        const glm::vec3 pTop   = celestialBillboardWorldPos(center, size, {0.0f, 0.5f});
        const glm::vec3 pBot   = celestialBillboardWorldPos(center, size, {0.0f, -0.5f});
        CHECK(glm::dot(pRight - center, dir) == Catch::Approx(0.0f).margin(1e-3f));
        CHECK(glm::dot(pTop - center, dir) == Catch::Approx(0.0f).margin(1e-3f));
        CHECK(glm::length(pRight - pLeft) == Catch::Approx(150.0f).margin(1e-3f));
        CHECK(glm::length(pTop - pBot) == Catch::Approx(150.0f).margin(1e-3f));

        // Project onto viewport looking directly at the celestial body:
        // screen pixel width must equal screen pixel height (1:1 circle, never a 2:1 oval).
        const glm::mat4 rotView = glm::lookAt(glm::vec3(0.0f), dir, glm::vec3(0.0f, 0.0f, 1.0f));
        for (float aspect : aspects) {
            glm::mat4 proj = glm::perspective(glm::radians(50.0f), aspect, 0.5f, 30000.0f);
            proj[1][1] *= -1.0f;
            auto toPixels = [&](const glm::vec3& p) {
                glm::vec4 clip = proj * rotView * glm::vec4(p, 1.0f);
                glm::vec2 ndc = glm::vec2(clip) / clip.w;
                return glm::vec2((ndc.x * 0.5f + 0.5f) * (720.0f * aspect),
                                 (ndc.y * 0.5f + 0.5f) * 720.0f);
            };
            const float pxWidth  = glm::length(toPixels(pRight) - toPixels(pLeft));
            const float pxHeight = glm::length(toPixels(pTop)   - toPixels(pBot));
            CHECK(pxWidth / pxHeight == Catch::Approx(1.0f).margin(1e-3f));
        }
    }
}
