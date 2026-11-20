#include <catch_amalgamated.hpp>
#include "rendering/weather_mist_math.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::canUseWaterContinuePass;
using wowee::rendering::kMistNearEyeFadeEnd;
using wowee::rendering::kMistNearEyeFadeStart;
using wowee::rendering::kMistSoftFadeFactor;
using wowee::rendering::linearizeMistDepth;
using wowee::rendering::mistEffectiveSoftFadeRange;
using wowee::rendering::mistNearCameraFade;
using wowee::rendering::mistSoftDepthFade;
using wowee::rendering::mistSoftDepthFadeNdc;
using wowee::rendering::mistSoftFadeRange;

namespace {

/// Project a point at view-space distance `z` along the camera forward axis
/// using Vulkan [0, 1] depth perspective and return its NDC z in [0, 1].
float projectDepth(float viewDist, float nearPlane, float farPlane) {
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), 16.0f / 9.0f, nearPlane, farPlane);
    proj[1][1] *= -1.0f;
    const glm::vec4 clip = proj * glm::vec4(0.0f, 0.0f, -viewDist, 1.0f);
    return clip.z / clip.w;
}

} // namespace

TEST_CASE("mistSoftFadeRange scales proportionally with puff size", "[weather_mist]") {
    CHECK(mistSoftFadeRange(36.0f) == Catch::Approx(36.0f * kMistSoftFadeFactor));
    CHECK(mistSoftFadeRange(36.0f) > 5.0f);
    CHECK(mistSoftFadeRange(36.0f) < 36.0f);
}

TEST_CASE("linearizeMistDepth inverts Vulkan perspective projection", "[weather_mist]") {
    constexpr float nearPlane = 0.5f;
    constexpr float farPlane = 1000.0f;

    CHECK(linearizeMistDepth(0.0f, nearPlane, farPlane) == Catch::Approx(nearPlane));
    CHECK(linearizeMistDepth(1.0f, nearPlane, farPlane) == Catch::Approx(farPlane));

    for (float dist : {1.0f, 8.0f, 25.0f, 60.0f, 110.0f, 400.0f}) {
        const float ndc = projectDepth(dist, nearPlane, farPlane);
        CHECK(linearizeMistDepth(ndc, nearPlane, farPlane) == Catch::Approx(dist).epsilon(1e-3f));
    }
}

TEST_CASE("mistSoftDepthFade feathers terrain intersections smoothly", "[weather_mist]") {
    const float fadeRange = mistSoftFadeRange(36.0f);

    SECTION("at or behind opaque terrain fade is zero") {
        CHECK(mistSoftDepthFade(30.0f, 30.0f, fadeRange) == 0.0f);
        CHECK(mistSoftDepthFade(25.0f, 30.0f, fadeRange) == 0.0f);
    }

    SECTION("strictly monotonic across the fade band with smooth Hermite midpoint") {
        const float f1 = mistSoftDepthFade(30.0f + 0.1f * fadeRange, 30.0f, fadeRange);
        const float fMid = mistSoftDepthFade(30.0f + 0.5f * fadeRange, 30.0f, fadeRange);
        const float f9 = mistSoftDepthFade(30.0f + 0.9f * fadeRange, 30.0f, fadeRange);
        CHECK(f1 > 0.0f);
        CHECK(f1 < fMid);
        CHECK(fMid == Catch::Approx(0.5f));
        CHECK(fMid < f9);
        CHECK(f9 < 1.0f);
    }

    SECTION("beyond fade range and against sky fade saturates at 1.0") {
        CHECK(mistSoftDepthFade(30.0f + fadeRange, 30.0f, fadeRange) == 1.0f);
        CHECK(mistSoftDepthFade(1000.0f, 40.0f, fadeRange) == 1.0f);
    }

    SECTION("disabled fade range returns 1.0 regardless of depth") {
        CHECK(mistSoftDepthFade(30.0f, 30.0f, 0.0f) == 1.0f);
        CHECK(mistSoftDepthFade(10.0f, 30.0f, 0.0f) == 1.0f);
    }
}

TEST_CASE("mistSoftDepthFadeNdc eliminates hard seam on sloped terrain in NDC space", "[weather_mist]") {
    constexpr float nearPlane = 0.5f;
    constexpr float farPlane = 2000.0f;
    const float fadeRange = mistSoftFadeRange(36.0f);

    // Mist puff quad at 40 yards intersecting a hill whose depth varies across the quad
    const float quadNdc = projectDepth(40.0f, nearPlane, farPlane);
    const float hitTerrainNdc = projectDepth(40.0f, nearPlane, farPlane);
    const float nearTerrainNdc = projectDepth(40.0f + 0.25f * fadeRange, nearPlane, farPlane);
    const float farTerrainNdc = projectDepth(40.0f + 1.5f * fadeRange, nearPlane, farPlane);
    const float skyNdc = 1.0f;

    CHECK(mistSoftDepthFadeNdc(hitTerrainNdc, quadNdc, nearPlane, farPlane, fadeRange) == 0.0f);
    const float partial = mistSoftDepthFadeNdc(nearTerrainNdc, quadNdc, nearPlane, farPlane, fadeRange);
    CHECK(partial > 0.05f);
    CHECK(partial < 0.35f);
    CHECK(mistSoftDepthFadeNdc(farTerrainNdc, quadNdc, nearPlane, farPlane, fadeRange) == 1.0f);
    CHECK(mistSoftDepthFadeNdc(skyNdc, quadNdc, nearPlane, farPlane, fadeRange) == 1.0f);
}

TEST_CASE("canUseWaterContinuePass enables 1x continuation for MSAA off-screen post-processing", "[weather_mist]") {
    SECTION("non-MSAA relies on scene continuation pass regardless of off-screen target") {
        CHECK(canUseWaterContinuePass(false, false, false, false, true));
        CHECK(canUseWaterContinuePass(false, false, true, false, true));
        CHECK_FALSE(canUseWaterContinuePass(false, true, false, false, false));
    }

    SECTION("MSAA direct swapchain uses 1x water pass when available") {
        CHECK(canUseWaterContinuePass(true, true, false, false, false));
        CHECK_FALSE(canUseWaterContinuePass(true, false, false, false, true));
    }

    SECTION("MSAA with off-screen post-process (ffxGlow/FXAA/FSR/FSR2) uses 1x continuation when resolved attachments exist") {
        CHECK(canUseWaterContinuePass(true, true, true, true, false));
        CHECK_FALSE(canUseWaterContinuePass(true, true, true, false, false));
        CHECK_FALSE(canUseWaterContinuePass(true, false, true, true, false));
    }
}

TEST_CASE("mistSoftDepthFade preserves foreground mist in downward camera views", "[weather_mist]") {
    const float maxFadeRange = mistSoftFadeRange(36.0f);

    // Pitched down -42..-68 deg at zoom 14: ground is ~14.5 yd from camera eye,
    // and drifting mist puffs pass 9-11 yd from the eye (~3.5-5.5 yd above cobblestones).
    CHECK(mistEffectiveSoftFadeRange(10.0f, maxFadeRange) == Catch::Approx(3.0f));
    CHECK(mistSoftDepthFade(14.5f, 10.0f, maxFadeRange) == Catch::Approx(1.0f));
    // Right where the quad meets the cobblestones (depthDelta == 0..effRange), it still feathers smoothly.
    CHECK(mistSoftDepthFade(14.5f, 14.5f, maxFadeRange) == 0.0f);
    const float effAt13 = mistEffectiveSoftFadeRange(13.0f, maxFadeRange);
    CHECK(effAt13 == Catch::Approx(3.9f));
    CHECK(mistSoftDepthFade(13.0f + 0.5f * effAt13, 13.0f, maxFadeRange) == Catch::Approx(0.5f));
}

TEST_CASE("mistNearCameraFade fades near lens by 3D eye distance without killing downward views", "[weather_mist]") {
    CHECK(mistNearCameraFade(0.0f) == 0.0f);
    CHECK(mistNearCameraFade(kMistNearEyeFadeStart) == 0.0f);
    CHECK(mistNearCameraFade(0.5f * (kMistNearEyeFadeStart + kMistNearEyeFadeEnd)) == Catch::Approx(0.5f));
    CHECK(mistNearCameraFade(kMistNearEyeFadeEnd) == 1.0f);
    // Directly underneath a camera 14.5 yd above the player (horiz_dist = 0, dist3D = 14.5 yd): >60% mist.
    CHECK(mistNearCameraFade(14.5f) > 0.60f);
    CHECK(mistNearCameraFade(24.0f) == 1.0f);
}
