// Tests for indoor shadow disable logic (WMO interior groups)
//
// WMO interior groups (flag 0x2000) should NOT receive directional sun shadows
// because they rely on pre-baked vertex color lighting (MOCV) and the shadow map
// only makes them darker.  The fix is in the fragment shader: interior groups
// skip the shadow map sample entirely.
//
// These tests verify the data contract between the renderer and the shader:
//   - GPUPerFrameData.shadowParams.x controls global shadow enable
//   - WMOMaterial.isInterior controls per-group interior flag
//   - Interior groups ignore shadows regardless of global shadow state

#include <catch_amalgamated.hpp>
#include "pipeline/wmo_loader.hpp"
#include "rendering/vk_frame_data.hpp"

#include <glm/glm.hpp>

using wowee::rendering::GPUPerFrameData;

TEST_CASE("shared shadow push constants reach both shader stages", "[shadows]") {
    const auto stages = wowee::rendering::kSharedShadowPushStages;
    CHECK((stages & VK_SHADER_STAGE_VERTEX_BIT) != 0);
    CHECK((stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0);
    CHECK(sizeof(wowee::rendering::ShadowPush) <= 128);
}

// Replicates the shadow params logic from Renderer::updatePerFrameUBO()
// This should NOT be affected by indoor state - shadows remain globally enabled
static void applyShadowParams(GPUPerFrameData& fd,
                              bool shadowsEnabled,
                              float shadowDistance = 300.0f) {
    float shadowBias = glm::clamp(0.8f * (shadowDistance / 300.0f), 0.0f, 1.0f);
    fd.shadowParams = glm::vec4(shadowsEnabled ? 1.0f : 0.0f, shadowBias, 0.0f, 0.0f);
}

// Replicates the WMO interior shader logic:
// interior groups skip shadow sampling entirely (shadow factor = 1.0 = fully lit).
// This covers both lit and unlit interior materials - isInterior takes priority.
static float computeWmoShadowFactor(bool isInterior, float globalShadowEnabled, float rawShadow) {
    if (isInterior) {
        // Interior groups always get shadow factor 1.0 (no shadow darkening)
        // regardless of unlit flag - isInterior is checked first in shader
        return 1.0f;
    }
    if (globalShadowEnabled > 0.5f) {
        return rawShadow;  // exterior: use shadow map result
    }
    return 1.0f;  // shadows globally disabled
}

TEST_CASE("Global shadow params are not affected by indoor state", "[indoor_shadows]") {
    GPUPerFrameData fd{};

    // Shadows enabled - should stay 1.0 regardless of any indoor logic
    applyShadowParams(fd, /*shadowsEnabled=*/true);
    REQUIRE(fd.shadowParams.x == Catch::Approx(1.0f));

    // Shadows disabled - should be 0.0
    applyShadowParams(fd, /*shadowsEnabled=*/false);
    REQUIRE(fd.shadowParams.x == Catch::Approx(0.0f));
}

TEST_CASE("Interior WMO groups skip shadow sampling", "[indoor_shadows]") {
    // Even when shadows are globally on and the shadow map says 0.2 (dark shadow),
    // interior groups should get 1.0 (no shadow)
    float factor = computeWmoShadowFactor(/*isInterior=*/true, /*globalShadowEnabled=*/1.0f, /*rawShadow=*/0.2f);
    REQUIRE(factor == Catch::Approx(1.0f));
}

TEST_CASE("Exterior WMO groups receive shadows normally", "[indoor_shadows]") {
    float factor = computeWmoShadowFactor(/*isInterior=*/false, /*globalShadowEnabled=*/1.0f, /*rawShadow=*/0.3f);
    REQUIRE(factor == Catch::Approx(0.3f));
}

TEST_CASE("Exterior WMO groups skip shadows when globally disabled", "[indoor_shadows]") {
    float factor = computeWmoShadowFactor(/*isInterior=*/false, /*globalShadowEnabled=*/0.0f, /*rawShadow=*/0.3f);
    REQUIRE(factor == Catch::Approx(1.0f));
}

TEST_CASE("Interior WMO groups skip shadows even when globally disabled", "[indoor_shadows]") {
    float factor = computeWmoShadowFactor(/*isInterior=*/true, /*globalShadowEnabled=*/0.0f, /*rawShadow=*/0.5f);
    REQUIRE(factor == Catch::Approx(1.0f));
}

TEST_CASE("Unlit interior surfaces skip shadows (isInterior takes priority over unlit)", "[indoor_shadows]") {
    // Many interior walls use F_UNLIT material flag (0x01). The shader must check
    // isInterior BEFORE unlit so these surfaces don't receive shadow darkening.
    // Even though the surface is unlit, it's interior → shadow factor = 1.0
    float factor = computeWmoShadowFactor(/*isInterior=*/true, /*globalShadowEnabled=*/1.0f, /*rawShadow=*/0.1f);
    REQUIRE(factor == Catch::Approx(1.0f));
}

TEST_CASE("Outdoor unlit surfaces still receive shadows", "[indoor_shadows]") {
    // Exterior unlit surfaces (isInterior=false, unlit=true in shader) should
    // still receive shadow darkening from the shadow map
    float factor = computeWmoShadowFactor(/*isInterior=*/false, /*globalShadowEnabled=*/1.0f, /*rawShadow=*/0.25f);
    REQUIRE(factor == Catch::Approx(0.25f));
}

TEST_CASE("Shadow bias scales with shadow distance", "[indoor_shadows]") {
    GPUPerFrameData fd{};

    // At default 300.0f, bias = 0.8
    applyShadowParams(fd, true, 300.0f);
    REQUIRE(fd.shadowParams.y == Catch::Approx(0.8f));

    // At 150.0f, bias = 0.4
    applyShadowParams(fd, true, 150.0f);
    REQUIRE(fd.shadowParams.y == Catch::Approx(0.4f));

    // Bias is clamped to [0, 1]
    applyShadowParams(fd, true, 600.0f);
    REQUIRE(fd.shadowParams.y == Catch::Approx(1.0f));
}

TEST_CASE("Ambient color is NOT modified globally for indoor state", "[indoor_shadows]") {
    // The global UBO ambient color should never be modified based on indoor state.
    // Indoor lighting is handled per-group in the WMO shader via MOCV vertex colors
    // and MOHD ambient color.
    GPUPerFrameData fd{};
    fd.ambientColor = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f);

    applyShadowParams(fd, true);

    // Ambient should be untouched
    REQUIRE(fd.ambientColor.x == Catch::Approx(0.3f));
    REQUIRE(fd.ambientColor.y == Catch::Approx(0.3f));
    REQUIRE(fd.ambientColor.z == Catch::Approx(0.3f));
}

TEST_CASE("TBC mapobjoverbright MOCV alpha scales intBatch vertices by 1 + 4*alpha", "[indoor_shadows][wmo]") {
    auto tbcInteriorShade = [](glm::vec3 texRgb, glm::vec4 vertColor, glm::vec3 wmoAmbient, bool hasMocv) {
        const glm::vec3 mocv = hasMocv
            ? glm::vec3(vertColor) * (1.0f + 4.0f * vertColor.a)
            : wmoAmbient;
        return texRgb * mocv;
    };

    // Shadow Labyrinth Refectory brazier pool (MOCV = (121, 221, 255, 39) / 255):
    // 1 + 4 * (39/255) = 1.6118x overbright, matching Native 2.4.3 (~1.594x).
    const glm::vec3 floorTex(107.6f / 255.0f, 107.1f / 255.0f, 99.0f / 255.0f);
    const glm::vec4 slPool(121.0f / 255.0f, 221.0f / 255.0f, 1.0f, 39.0f / 255.0f);
    const glm::vec3 litPool = tbcInteriorShade(floorTex, slPool, glm::vec3(0.0f), true) * 255.0f;
    CHECK(litPool.b == Catch::Approx(159.56f).margin(0.5f));

    // Stormwind Bank lobby (MOCV.a == 0): exact 1.0x vertex color with zero overbright.
    const glm::vec4 swBank(0.75f, 0.72f, 0.68f, 0.0f);
    const glm::vec3 bankLit = tbcInteriorShade(glm::vec3(1.0f), swBank, glm::vec3(0.2f), true);
    CHECK(bankLit.r == Catch::Approx(0.75f).margin(1e-5f));
    CHECK(bankLit.g == Catch::Approx(0.72f).margin(1e-5f));
    CHECK(bankLit.b == Catch::Approx(0.68f).margin(1e-5f));

    // Group without MOCV falls back to MOHD ambient directly.
    const glm::vec3 noMocv = tbcInteriorShade(glm::vec3(0.8f), glm::vec4(1.0f, 1.0f, 1.0f, 0.0f),
                                              glm::vec3(0.25f, 0.30f, 0.35f), false);
    CHECK(noMocv.r == Catch::Approx(0.20f).margin(1e-5f));
    CHECK(noMocv.g == Catch::Approx(0.24f).margin(1e-5f));
    CHECK(noMocv.b == Catch::Approx(0.28f).margin(1e-5f));
}

TEST_CASE("WMO batch lighting classifies transBatch, intBatch, and extBatch", "[indoor_shadows][wmo]") {
    using wowee::pipeline::WmoBatchLighting;
    using wowee::pipeline::classifyWmoBatchLighting;

    // Scholomance entrance staircase group 07: flags=0x2805 (indoor + MOCV), batches=(7, 7, 0).
    CHECK(classifyWmoBatchLighting(0x2805u, 0, 7, 7) == WmoBatchLighting::Transition);
    CHECK(classifyWmoBatchLighting(0x2805u, 6, 7, 7) == WmoBatchLighting::Transition);
    CHECK(classifyWmoBatchLighting(0x2805u, 7, 7, 7) == WmoBatchLighting::Interior);
    CHECK(classifyWmoBatchLighting(0x2805u, 13, 7, 7) == WmoBatchLighting::Interior);
    // Trailing extBatch inside a locally-lit group uses exterior lighting.
    CHECK(classifyWmoBatchLighting(0x2805u, 14, 7, 7) == WmoBatchLighting::Exterior);

    // Outdoor / exterior-lit groups (MOGP 0x40 or non-0x2000) always classify as Exterior.
    CHECK(classifyWmoBatchLighting(0x2841u, 0, 3, 5) == WmoBatchLighting::Exterior);
    CHECK(classifyWmoBatchLighting(0x0809u, 0, 0, 0) == WmoBatchLighting::Exterior);
}

TEST_CASE("WMO transBatch blends interior MOCV and exterior lighting by MOCV alpha", "[indoor_shadows][wmo]") {
    auto tbcTransShade = [](glm::vec3 texRgb, glm::vec4 vertColor, glm::vec3 extLight) {
        const float extWeight = glm::clamp(vertColor.a, 0.0f, 1.0f);
        const glm::vec3 interiorPart = texRgb * glm::max(glm::vec3(vertColor) - glm::vec3(extWeight), glm::vec3(0.0f));
        const glm::vec3 exteriorLit = texRgb * extLight;
        return interiorPart + exteriorLit * extWeight;
    };

    const glm::vec3 tex(0.8f, 0.7f, 0.6f);
    const glm::vec3 scholoExterior(0.25f, 0.20f, 0.22f);

    // Scholomance grp 07 wood floor v[140]: MOCV=(254,254,254,254)/255 must take
    // exterior lighting (not fullbright white interior MOCV).
    const glm::vec4 floorVert(254.0f / 255.0f, 254.0f / 255.0f, 254.0f / 255.0f, 254.0f / 255.0f);
    const glm::vec3 floorLit = tbcTransShade(tex, floorVert, scholoExterior);
    CHECK(floorLit.r == Catch::Approx(tex.r * scholoExterior.r).margin(0.01f));
    CHECK(floorLit.g == Catch::Approx(tex.g * scholoExterior.g).margin(0.01f));
    CHECK(floorLit.b == Catch::Approx(tex.b * scholoExterior.b).margin(0.01f));

    // Pure interior side of transition (MOCV.a == 0) uses baked MOCV.rgb with no overbright.
    const glm::vec4 intVert(134.0f / 255.0f, 103.0f / 255.0f, 45.0f / 255.0f, 0.0f);
    const glm::vec3 intLit = tbcTransShade(tex, intVert, scholoExterior);
    CHECK(intLit.r == Catch::Approx(tex.r * intVert.r).margin(1e-5f));
    CHECK(intLit.g == Catch::Approx(tex.g * intVert.g).margin(1e-5f));
    CHECK(intLit.b == Catch::Approx(tex.b * intVert.b).margin(1e-5f));

    // Mid-transition Scholomance coffin wall v[0]: MOCV=(208,209,209,199)/255 replaces the
    // baked white sky component (199/255) with exterior lighting.
    const glm::vec4 wallVert(208.0f / 255.0f, 209.0f / 255.0f, 209.0f / 255.0f, 199.0f / 255.0f);
    const glm::vec3 wallLit = tbcTransShade(tex, wallVert, scholoExterior);
    const float w = 199.0f / 255.0f;
    CHECK(wallLit.r == Catch::Approx(tex.r * ((wallVert.r - w) + w * scholoExterior.r)).margin(1e-5f));

    // Amani Catacombs grp 06 doorway v[0]: MOCV = mix(ambient, white, 210/255) transitions
    // linearly from MOHD ambient to Ghostlands night outdoor lighting.
    const glm::vec3 amaniAmb(44.0f / 255.0f, 66.0f / 255.0f, 75.0f / 255.0f);
    const glm::vec3 nightOutdoor(0.18f, 0.22f, 0.20f);
    const float a = 210.0f / 255.0f;
    const glm::vec4 amaniVert(glm::mix(amaniAmb, glm::vec3(1.0f), a), a);
    const glm::vec3 amaniLit = tbcTransShade(glm::vec3(1.0f), amaniVert, nightOutdoor);
    const glm::vec3 amaniExpected = glm::mix(amaniAmb, nightOutdoor, a);
    CHECK(amaniLit.r == Catch::Approx(amaniExpected.r).margin(1e-4f));
    CHECK(amaniLit.g == Catch::Approx(amaniExpected.g).margin(1e-4f));
    CHECK(amaniLit.b == Catch::Approx(amaniExpected.b).margin(1e-4f));
}

TEST_CASE("wmoGroupBatchVertexRanges handles shared seam vertices between trans and int batches",
          "[indoor_shadows][wmo]") {
    using wowee::pipeline::WMOBatch;
    using wowee::pipeline::WMOGroup;
    using wowee::pipeline::wmoGroupBatchVertexRanges;

    // Matches woodtroll_temple_006.wmo: 7 transition batches spanning 0..1693,
    // followed by an interior batch whose startVertex is 28 due to a shared seam vertex.
    WMOGroup group{};
    group.vertices.resize(8308);
    group.transBatchCount = 2;
    group.intBatchCount = 1;
    group.extBatchCount = 0;

    WMOBatch t0{}; t0.startVertex = 0;    t0.lastVertex = 307;
    WMOBatch t1{}; t1.startVertex = 1416; t1.lastVertex = 1693;
    WMOBatch i0{}; i0.startVertex = 28;   i0.lastVertex = 2506;
    group.batches = {t0, t1, i0};

    const auto ranges = wmoGroupBatchVertexRanges(group);
    CHECK(ranges.transVertexEnd == 1694);
    CHECK(ranges.extVertexStart == 8308);

    // Exterior batch after interior batch keeps extVertexStart clamped >= transVertexEnd.
    group.extBatchCount = 1;
    WMOBatch e0{}; e0.startVertex = 8000; e0.lastVertex = 8307;
    group.batches.push_back(e0);
    const auto withExt = wmoGroupBatchVertexRanges(group);
    CHECK(withExt.transVertexEnd == 1694);
    CHECK(withExt.extVertexStart == 8000);
}
