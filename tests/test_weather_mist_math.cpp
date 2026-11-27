#include <catch_amalgamated.hpp>
#include "rendering/renderer_bloom_math.hpp"
#include "rendering/weather_mist_math.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

using wowee::rendering::canUseWaterContinuePass;
using wowee::rendering::kMistNearEyeFadeEnd;
using wowee::rendering::kMistNearEyeFadeStart;
using wowee::rendering::kMistOuterFadeMargin;
using wowee::rendering::kMistSoftFadeFactor;
using wowee::rendering::kMistViewDepthFadeEnd;
using wowee::rendering::kMistViewDepthFadeStart;
using wowee::rendering::kRainMistGradeFull;
using wowee::rendering::kRainMistGradeStart;
using wowee::rendering::linearizeMistDepth;
using wowee::rendering::mistCylinderRadius;
using wowee::rendering::mistEffectiveSoftFadeRange;
using wowee::rendering::mistNearCameraFade;
using wowee::rendering::mistOuterRadiusFade;
using wowee::rendering::mistSoftDepthFade;
using wowee::rendering::mistSoftDepthFadeNdc;
using wowee::rendering::mistSoftFadeRange;
using wowee::rendering::mistViewDepthFade;
using wowee::rendering::rainMistAmount;
using wowee::rendering::mod2xNeutralize128;
using wowee::rendering::rainDropMipLodBias;
using wowee::rendering::resolveWeatherLandingHeight;
using wowee::rendering::weatherSplashLiftZ;
using wowee::rendering::weatherSplashMinCornerZ;

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

TEST_CASE("mistCylinderRadius preserves horizontal slab footprint area", "[weather_mist]") {
    constexpr float kPi = 3.14159265358979323846f;
    const float r = mistCylinderRadius(65.0f, -28.0f, 110.0f);
    CHECK(kPi * r * r == Catch::Approx(130.0f * 138.0f).epsilon(1e-4f));
    CHECK(r > 70.0f);
    CHECK(r < 85.0f);
}

TEST_CASE("mistOuterRadiusFade smoothly reaches zero at cylinder rim", "[weather_mist]") {
    const float r = mistCylinderRadius(65.0f, -28.0f, 110.0f);
    CHECK(mistOuterRadiusFade(0.0f, r) == 1.0f);
    CHECK(mistOuterRadiusFade(r - kMistOuterFadeMargin, r) == 1.0f);
    CHECK(mistOuterRadiusFade(r - 0.5f * kMistOuterFadeMargin, r) == Catch::Approx(0.5f));
    CHECK(mistOuterRadiusFade(r, r) == 0.0f);
    CHECK(mistOuterRadiusFade(r + 10.0f, r) == 0.0f);
}

TEST_CASE("mistViewDepthFade eliminates hard camera-plane pop on camera turns", "[weather_mist]") {
    CHECK(mistViewDepthFade(-5.0f) == 0.0f);
    CHECK(mistViewDepthFade(kMistViewDepthFadeStart) == 0.0f);
    const float mid = 0.5f * (kMistViewDepthFadeStart + kMistViewDepthFadeEnd);
    CHECK(mistViewDepthFade(mid) == Catch::Approx(0.5f));
    CHECK(mistViewDepthFade(kMistViewDepthFadeEnd) == 1.0f);
    // Steep downward view (-68 deg, 10 yd below eye => viewDepth ~9.3 yd) stays at full opacity.
    CHECK(mistViewDepthFade(9.3f) == 1.0f);
}

TEST_CASE("rainMistAmount matches Native TBC 2.4.3 rain mist grade onset", "[weather_mist]") {
    // Light/moderate rain (grade <= 0.50, including Karazhan 0.396) produces zero mist puffs.
    CHECK(rainMistAmount(0.0f) == 0.0f);
    CHECK(rainMistAmount(0.396f) == 0.0f);
    CHECK(rainMistAmount(kRainMistGradeStart) == 0.0f);

    // Heavier rain ramps smoothly to full mist at grade 1.00.
    const float at06 = rainMistAmount(0.60f);
    const float at07 = rainMistAmount(0.70f);
    const float at085 = rainMistAmount(0.85f);
    CHECK(at06 > 0.05f);
    CHECK(at06 < 0.20f);
    CHECK(at07 > at06);
    CHECK(at085 > at07);
    CHECK(rainMistAmount(kRainMistGradeFull) == 1.0f);
    CHECK(rainMistAmount(1.5f) == 1.0f);
}

TEST_CASE("resolveWeatherLandingHeight lands rain and snow on rivers, bridges, and ocean", "[weather_mist][weather]") {
    // Deadwind Pass river canyon: riverbed at z=18.9, river surface at z=23.8, spawn top z=74.0 -> lands on river (23.8).
    const auto riverHit = resolveWeatherLandingHeight(18.9f, std::nullopt, 23.8f, 74.0f);
    REQUIRE(riverHit.has_value());
    CHECK(*riverHit == Catch::Approx(23.8f));

    // WMO bridge deck over the river at z=31.0 -> lands on bridge deck (31.0).
    const auto bridgeHit = resolveWeatherLandingHeight(18.9f, 31.0f, 23.8f, 74.0f);
    REQUIRE(bridgeHit.has_value());
    CHECK(*bridgeHit == Catch::Approx(31.0f));

    // Elevated aqueduct/pool at z=80 above spawn ceiling maxZ=60 is ignored.
    const auto belowAqueduct = resolveWeatherLandingHeight(14.0f, std::nullopt, 80.0f, 60.0f);
    REQUIRE(belowAqueduct.has_value());
    CHECK(*belowAqueduct == Catch::Approx(14.0f));

    // Deep ocean where terrain is -300 and sea level is 0.0 -> returns 0.0 so kRainCeiling (74 yd) stays active.
    const auto oceanHit = resolveWeatherLandingHeight(-300.0f, std::nullopt, 0.0f, 55.0f);
    REQUIRE(oceanHit.has_value());
    CHECK(*oceanHit == Catch::Approx(0.0f));
}

TEST_CASE("rainDropMipLodBias aligns 16x128 RainDrop01.blp with 0.10x2.0 yd drop aspect", "[weather_mist][weather]") {
    const float bias = rainDropMipLodBias(16.0f, 128.0f, 0.10f, 2.0f);
    CHECK(bias == Catch::Approx(std::log2(0.4f)).margin(1e-4f));
    CHECK(bias < -1.30f);
    CHECK(bias > -1.35f);
}

TEST_CASE("weatherSplashMinCornerZ keeps splash rings above flat ground and water depth at all pitches", "[weather_mist][weather]") {
    constexpr float kSplashCell = 0.23f;
    constexpr float kWaterZ = 23.81f;
    CHECK(weatherSplashLiftZ(kSplashCell) == Catch::Approx(0.125f));
    for (float lookdown : {0.0f, 0.15f, 0.30f, 0.60f, 0.85f, 1.0f}) {
        for (float upZ : {0.0f, 0.5f, 0.866f, 0.966f, 1.0f}) {
            CHECK(weatherSplashMinCornerZ(kWaterZ, lookdown, upZ, kSplashCell) >= kWaterZ);
        }
    }
}

TEST_CASE("mod2xNeutralize128 maps BLP neutral grey byte 128 to exact 0.5", "[weather_mist][weather]") {
    CHECK(mod2xNeutralize128(128.0f / 255.0f) == Catch::Approx(0.5f).margin(1e-7f));
    CHECK(2.0f * mod2xNeutralize128(128.0f / 255.0f) == Catch::Approx(1.0f).margin(1e-7f));
    CHECK(2.0f * mod2xNeutralize128(1.0f) == Catch::Approx(510.0f / 256.0f).margin(1e-6f));
}

TEST_CASE("shipped TBC RainDrop01.blp stores 8 authored DXT3 mip levels keeping distance contrast", "[weather_mist][weather][tbc]") {
    const char* envDir = std::getenv("WOWEE_TBC_DATA_DIR");
    const std::string base = envDir ? envDir : "/workspace/azerothcore/client/wowee-data/expansions/tbc";
    FILE* f = std::fopen((base + "/textures/weather/raindrop01.blp").c_str(), "rb");
    if (!f) {
        SKIP("TBC RainDrop01.blp not present");
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    REQUIRE(sz >= 1172);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    REQUIRE(std::fread(raw.data(), 1, raw.size(), f) == raw.size());
    std::fclose(f);

    CHECK(std::memcmp(raw.data(), "BLP2", 4) == 0);
    CHECK(raw[8] == 2u);  // DXTC
    CHECK(raw[9] == 8u);  // alphaDepth 8
    CHECK(raw[10] == 1u); // DXT3 / BC2
    uint32_t w = 0, h = 0;
    std::memcpy(&w, raw.data() + 12, 4);
    std::memcpy(&h, raw.data() + 16, 4);
    CHECK(w == 16u);
    CHECK(h == 128u);

    uint32_t mipOffsets[16]{};
    uint32_t mipSizes[16]{};
    std::memcpy(mipOffsets, raw.data() + 20, sizeof(mipOffsets));
    std::memcpy(mipSizes, raw.data() + 84, sizeof(mipSizes));
    int mipCount = 0;
    for (int i = 0; i < 16 && mipOffsets[i] != 0 && mipSizes[i] != 0; ++i) {
        ++mipCount;
    }
    CHECK(mipCount == 8);
    // Authored mip 7 (1x1 BC2 block, color endpoint c0 at offset +8) stays > 140 (> 0.55),
    // whereas box-blitting mip 0 collapses mip 7 to 128.5 (invisible under Mod2x).
    uint16_t c0 = 0;
    std::memcpy(&c0, raw.data() + mipOffsets[7] + 8, 2);
    const float g8 = static_cast<float>((c0 >> 5) & 63u) * (255.0f / 63.0f);
    CHECK(g8 > 140.0f);
}

TEST_CASE("TBC 2.4.3 ffxGlow target extents, resolution-scaled Gaussian kernel, and composite transfer curve",
          "[ffxglow][bloom][tbc]") {
    using namespace wowee::rendering;

    SECTION("Two-stage 2x2 downsample produces exact half and quarter extents with non-zero floor") {
        const auto e720 = computeBloomTargetExtents(1280u, 720u);
        CHECK(e720.halfWidth == 640u);
        CHECK(e720.halfHeight == 360u);
        CHECK(e720.quarterWidth == 320u);
        CHECK(e720.quarterHeight == 180u);

        const auto e1080 = computeBloomTargetExtents(1920u, 1080u);
        CHECK(e1080.quarterWidth == 480u);
        CHECK(e1080.quarterHeight == 270u);

        const auto tiny = computeBloomTargetExtents(2u, 3u);
        CHECK(tiny.quarterWidth == 1u);
        CHECK(tiny.quarterHeight == 1u);
    }

    SECTION("Quarter-res Gaussian sigma scales proportionally with screen height and maintains +-3 sigma support") {
        const float s720 = computeQuarterBlurSigmaTexels(180u);
        const float s1080 = computeQuarterBlurSigmaTexels(270u);
        const float s2160 = computeQuarterBlurSigmaTexels(540u);
        CHECK(s720 == Catch::Approx(kTbcFfxGlowQuarterSigmaAt720p));
        CHECK(s1080 == Catch::Approx(1.5f * s720));
        CHECK(s2160 == Catch::Approx(3.0f * s720));

        const float step720 = computeQuarterBlurTapStepTexels(s720);
        const float step2160 = computeQuarterBlurTapStepTexels(s2160);
        CHECK(step720 == Catch::Approx(1.0f));
        CHECK(step2160 == Catch::Approx(3.0f));

        // Normalized 9-tap weights stay symmetric, positive, peaked at center, and sum to 1.0.
        const auto w = computeGaussianTapWeights9(s2160, step2160);
        float sum = 0.0f;
        for (size_t i = 0; i < 9; ++i) {
            CHECK(w[i] > 0.0f);
            CHECK(w[i] == Catch::Approx(w[8 - i]));
            sum += w[i];
        }
        CHECK(sum == Catch::Approx(1.0f));
        CHECK(w[4] > w[3]);
        CHECK(w[3] > w[2]);
        CHECK(w[2] > w[1]);
        CHECK(w[1] > w[0]);
    }

    SECTION("effectiveTbcFfxGlowStrength honors expansion, world frame, CVars, Nether World, and LightParams.dbc glow") {
        // Active TBC world frame with ffx=1, ffxGlow=1, no Nether World -> exact clamped DBC glow.
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, true, true, false, 0.50f) == Catch::Approx(0.50f));
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, true, true, false, 1.25f) == Catch::Approx(1.00f));
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, true, true, false, 0.00f) == 0.0f);

        // Disabled on WotLK, login/loading screen, renderer off, ffx=0, ffxGlow=0, or Nether World invisibility.
        CHECK(effectiveTbcFfxGlowStrength(false, true, true, true, true, false, 0.65f) == 0.0f);
        CHECK(effectiveTbcFfxGlowStrength(true, false, true, true, true, false, 0.65f) == 0.0f);
        CHECK(effectiveTbcFfxGlowStrength(true, true, false, true, true, false, 0.65f) == 0.0f);
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, false, true, false, 0.65f) == 0.0f);
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, true, false, false, 0.65f) == 0.0f);
        CHECK(effectiveTbcFfxGlowStrength(true, true, true, true, true, true, 0.65f) == 0.0f);
    }

    SECTION("evaluateTbcFfxGlowComposite matches shipped ffxglow.bls squared-blur additive curve") {
        // Dark stone (30, 18, 12)/255 next to glowing window / flame blur (0.50, 0.25, 0.05) at LP 336 glow = 0.50:
        const glm::vec3 darkStone(30.0f / 255.0f, 18.0f / 255.0f, 12.0f / 255.0f);
        const glm::vec3 warmBlur(0.50f, 0.25f, 0.05f);
        const glm::vec3 lit = evaluateTbcFfxGlowComposite(darkStone, warmBlur, 0.50f);
        CHECK(lit.r == Catch::Approx(darkStone.r + 0.50f * 0.25f));
        CHECK(lit.g == Catch::Approx(darkStone.g + 0.50f * 0.0625f));
        CHECK(lit.b == Catch::Approx(darkStone.b + 0.50f * 0.0025f));

        // Saturated highlights clamp cleanly to 1.0, and glow = 0 is an exact identity.
        CHECK(evaluateTbcFfxGlowComposite(glm::vec3(0.95f, 0.50f, 0.10f), glm::vec3(0.80f), 1.0f).r == Catch::Approx(1.0f));
        CHECK(evaluateTbcFfxGlowComposite(darkStone, warmBlur, 0.0f) == darkStone);
    }
}

TEST_CASE("Shipped TBC LightParams.dbc col 4 authors zone glow values used by ffxGlow", "[ffxglow][dbc][tbc]") {
    const char* envDir = std::getenv("WOWEE_TBC_DATA_DIR");
    const std::string base = envDir ? envDir : "/workspace/azerothcore/client/wowee-data/expansions/tbc";
    FILE* f = std::fopen((base + "/dbfilesclient/lightparams.dbc").c_str(), "rb");
    if (!f) {
        SKIP("TBC LightParams.dbc not present");
    }
    std::fseek(f, 0, SEEK_END);
    const long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    REQUIRE(sz > 20);
    std::vector<uint8_t> raw(static_cast<size_t>(sz));
    REQUIRE(std::fread(raw.data(), 1, raw.size(), f) == raw.size());
    std::fclose(f);

    CHECK(std::memcmp(raw.data(), "WDBC", 4) == 0);
    uint32_t recordCount = 0, fieldCount = 0, recordSize = 0;
    std::memcpy(&recordCount, raw.data() + 4, 4);
    std::memcpy(&fieldCount, raw.data() + 8, 4);
    std::memcpy(&recordSize, raw.data() + 12, 4);
    REQUIRE(recordCount == 582u);
    REQUIRE(fieldCount == 9u);
    REQUIRE(recordSize == 36u);

    auto findGlow = [&](uint32_t targetId) -> float {
        for (uint32_t i = 0; i < recordCount; ++i) {
            const uint8_t* rec = raw.data() + 20 + static_cast<size_t>(i) * recordSize;
            uint32_t id = 0;
            float glow = -1.0f;
            std::memcpy(&id, rec, 4);
            if (id == targetId) {
                std::memcpy(&glow, rec + 16, 4);
                return glow;
            }
        }
        return -1.0f;
    };

    // Verified against Native TBC 2.4.3 captures across all 4 zones:
    CHECK(findGlow(12u) == Catch::Approx(0.65f));   // Map 0 default / Molten Core fallback
    CHECK(findGlow(208u) == Catch::Approx(1.00f));  // Teldrassil
    CHECK(findGlow(336u) == Catch::Approx(0.50f));  // Stratholme Alonsus Chapel
    CHECK(findGlow(552u) == Catch::Approx(0.30f));  // Netherstorm Eco-Domes / The Stormspire
}
