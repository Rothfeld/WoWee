#include <algorithm>
#include <array>
#include <catch_amalgamated.hpp>
#include "rendering/preview_background.hpp"
#include "rendering/vk_pipeline.hpp"
#include "ui/unit_portrait.hpp"

using wowee::rendering::PipelineBuilder;
using wowee::rendering::PreviewBackgroundMode;
using wowee::rendering::portraitBackgroundModeForFraming;
using wowee::rendering::previewClearColorRGBA;
using wowee::rendering::previewLoadsRacialBackdrop;
using wowee::rendering::shouldDesaturatePlayerPortrait;
using wowee::ui::creatureDisplayPortraitPath;

namespace {

using RGBA = std::array<float, 4>;

float evalBlendFactor(VkBlendFactor factor, float srcChan, float srcA, float dstChan, float dstA) {
    switch (factor) {
        case VK_BLEND_FACTOR_ZERO:                return 0.0f;
        case VK_BLEND_FACTOR_ONE:                 return 1.0f;
        case VK_BLEND_FACTOR_SRC_ALPHA:           return srcA;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 1.0f - srcA;
        case VK_BLEND_FACTOR_DST_ALPHA:           return dstA;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return 1.0f - dstA;
        default:
            FAIL("Unexpected VkBlendFactor in preview blend simulation");
            return 0.0f;
    }
    (void)srcChan;
    (void)dstChan;
}

RGBA applyBlendState(const VkPipelineColorBlendAttachmentState& state,
                     const RGBA& src,
                     const RGBA& dst,
                     bool clampUnorm8 = true) {
    if (!state.blendEnable) {
        return src;
    }
    RGBA out{};
    for (size_t c = 0; c < 3; ++c) {
        const float sf = evalBlendFactor(state.srcColorBlendFactor, src[c], src[3], dst[c], dst[3]);
        const float df = evalBlendFactor(state.dstColorBlendFactor, src[c], src[3], dst[c], dst[3]);
        out[c] = src[c] * sf + dst[c] * df;
    }
    const float sa = evalBlendFactor(state.srcAlphaBlendFactor, src[3], src[3], dst[3], dst[3]);
    const float da = evalBlendFactor(state.dstAlphaBlendFactor, src[3], src[3], dst[3], dst[3]);
    out[3] = src[3] * sa + dst[3] * da;
    if (clampUnorm8) {
        for (float& v : out) {
            v = std::clamp(v, 0.0f, 1.0f);
        }
    }
    return out;
}

/// Exact fragment expression of assets/shaders/ui_premultiplied.frag.glsl.
RGBA evalPremultipliedUiFrag(const RGBA& sampled, const RGBA& vertexColor) {
    return {
        sampled[0] * vertexColor[0] * vertexColor[3],
        sampled[1] * vertexColor[1] * vertexColor[3],
        sampled[2] * vertexColor[2] * vertexColor[3],
        sampled[3] * vertexColor[3],
    };
}

} // namespace

TEST_CASE("Unit portrait framing selects opaque black portrait disc vs transparent full-body", "[portrait]") {
    const auto faceMode = portraitBackgroundModeForFraming(/*isFaceFraming=*/true);
    CHECK(faceMode == PreviewBackgroundMode::OpaquePortrait);
    CHECK_FALSE(previewLoadsRacialBackdrop(faceMode));
    const auto faceClear = previewClearColorRGBA(faceMode);
    CHECK(faceClear[0] == 0.0f);
    CHECK(faceClear[1] == 0.0f);
    CHECK(faceClear[2] == 0.0f);
    CHECK(faceClear[3] == 1.0f);

    const auto fullBodyMode = portraitBackgroundModeForFraming(/*isFaceFraming=*/false);
    CHECK(fullBodyMode == PreviewBackgroundMode::Transparent);
    CHECK_FALSE(previewLoadsRacialBackdrop(fullBodyMode));
    const auto fullBodyClear = previewClearColorRGBA(fullBodyMode);
    CHECK(fullBodyClear[3] == 0.0f);

    const auto npcDialogFace = portraitBackgroundModeForFraming(
        /*isFaceFraming=*/true, /*hasPanelBackdrop=*/true);
    CHECK(npcDialogFace == PreviewBackgroundMode::Transparent);
    CHECK(previewClearColorRGBA(npcDialogFace)[3] == 0.0f);

    CHECK(previewLoadsRacialBackdrop(PreviewBackgroundMode::RacialScene));
    const auto sceneClear = previewClearColorRGBA(PreviewBackgroundMode::RacialScene);
    CHECK(sceneClear[3] == 1.0f);
}

TEST_CASE("Unit portrait 2D fallbacks distinguish players, creatures, pets, and 0-vertex stalkers", "[portrait]") {
    using wowee::ui::resolveUnitPortraitFallback;
    using wowee::ui::temporaryPortraitPath;

    CHECK(temporaryPortraitPath(1, 0, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Male-Human");
    CHECK(temporaryPortraitPath(4, 1, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Female-NightElf");
    CHECK(temporaryPortraitPath(0, 0xFF, true, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Pet");
    CHECK(temporaryPortraitPath(4, 0, false, true) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Monster");

    CHECK(resolveUnitPortraitFallback(0, "", 0xF1300047FF000001ULL, 4, 0, false, true, true) ==
          "Interface\\Buttons\\WHITE8X8");
    CHECK(resolveUnitPortraitFallback(0, "", 0xF1300047FF000001ULL, 4, 0, false, true, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Monster");
    CHECK(resolveUnitPortraitFallback(0, "", 0x1AULL, 1, 0, false, false, false) ==
          "Interface\\CharacterFrame\\TemporaryPortrait-Male-Human");
}

TEST_CASE("PlayerPortrait is never desaturated in ghost or Night Elf Wisp Spirit form", "[portrait]") {
    CHECK_FALSE(shouldDesaturatePlayerPortrait(/*isPlayerGhost=*/false));
    CHECK_FALSE(shouldDesaturatePlayerPortrait(/*isPlayerGhost=*/true));
}

TEST_CASE("CreatureDisplayInfo PortraitTextureName resolves to Interface\\Icons path", "[portrait][dbc]") {
    CHECK(creatureDisplayPortraitPath("").empty());
    CHECK(creatureDisplayPortraitPath("Spell_Shadow_GatherShadows") ==
          "Interface\\Icons\\Spell_Shadow_GatherShadows");
    CHECK(creatureDisplayPortraitPath("Spell_Fire_Fireball02") ==
          "Interface\\Icons\\Spell_Fire_Fireball02");
    CHECK(creatureDisplayPortraitPath("INV_Misc_Head_Undead_01") ==
          "Interface\\Icons\\INV_Misc_Head_Undead_01");
    CHECK(creatureDisplayPortraitPath("Interface\\Icons\\INV_Cask_04") ==
          "Interface\\Icons\\INV_Cask_04");
}

TEST_CASE("Preview FBO additive and premultiplied UI blend states preserve backdrop under additive glows",
          "[portrait][preview][blend]") {
    const auto addState = PipelineBuilder::blendAdditive();
    CHECK(addState.blendEnable == VK_TRUE);
    CHECK(addState.srcColorBlendFactor == VK_BLEND_FACTOR_SRC_ALPHA);
    CHECK(addState.dstColorBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(addState.colorBlendOp == VK_BLEND_OP_ADD);
    CHECK(addState.srcAlphaBlendFactor == VK_BLEND_FACTOR_ZERO);
    CHECK(addState.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(addState.alphaBlendOp == VK_BLEND_OP_ADD);

    const auto premulState = PipelineBuilder::blendPremultiplied();
    CHECK(premulState.blendEnable == VK_TRUE);
    CHECK(premulState.srcColorBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(premulState.dstColorBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK(premulState.colorBlendOp == VK_BLEND_OP_ADD);
    CHECK(premulState.srcAlphaBlendFactor == VK_BLEND_FACTOR_ONE);
    CHECK(premulState.dstAlphaBlendFactor == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK(premulState.alphaBlendOp == VK_BLEND_OP_ADD);

    const RGBA clearTransparent = previewClearColorRGBA(PreviewBackgroundMode::Transparent);
    const RGBA dressUpBackdrop = {0.95f, 0.70f, 0.75f, 1.0f};
    const RGBA fullOpacityVertex = {1.0f, 1.0f, 1.0f, 1.0f};

    // Ethereum Nexus-Reaver (item 30722, Axe_2H_OutlandRaid_D_01.m2 Sub[2]):
    // Stacked spherical billboards using BLUE_GLOW2.BLP (DXT1 1-bit alpha where
    // texAlpha == 1.0 across the disc and RGB fades to (0,0,0) near the rim)
    // with M2_BLEND_ADD_ALPHA and Trans[1] == 0.66.
    SECTION("Black DXT1 rim texel of additive weapon glow leaves UI backdrop untouched") {
        const RGBA blackRimFrag = {0.0f, 0.0f, 0.0f, 0.66f};
        const RGBA fbo = applyBlendState(addState, blackRimFrag, clearTransparent);
        CHECK(fbo[0] == Catch::Approx(0.0f));
        CHECK(fbo[1] == Catch::Approx(0.0f));
        CHECK(fbo[2] == Catch::Approx(0.0f));
        CHECK(fbo[3] == Catch::Approx(0.0f));

        const RGBA uiOut = applyBlendState(
            premulState, evalPremultipliedUiFrag(fbo, fullOpacityVertex), dressUpBackdrop);
        CHECK(uiOut[0] == Catch::Approx(dressUpBackdrop[0]));
        CHECK(uiOut[1] == Catch::Approx(dressUpBackdrop[1]));
        CHECK(uiOut[2] == Catch::Approx(dressUpBackdrop[2]));
        CHECK(uiOut[3] == Catch::Approx(1.0f));
    }

    SECTION("Stacked blue additive core billboards brighten UI backdrop instead of occluding it") {
        const RGBA glowCoreFrag = {0.25f, 0.45f, 1.0f, 0.66f};
        RGBA fbo = applyBlendState(addState, glowCoreFrag, clearTransparent);
        fbo = applyBlendState(addState, glowCoreFrag, fbo);
        CHECK(fbo[0] == Catch::Approx(0.33f));
        CHECK(fbo[1] == Catch::Approx(0.594f));
        CHECK(fbo[2] == Catch::Approx(1.0f));
        CHECK(fbo[3] == Catch::Approx(0.0f));

        const RGBA uiOut = applyBlendState(
            premulState, evalPremultipliedUiFrag(fbo, fullOpacityVertex), dressUpBackdrop);
        CHECK(uiOut[0] == Catch::Approx(1.0f));
        CHECK(uiOut[1] == Catch::Approx(1.0f));
        CHECK(uiOut[2] == Catch::Approx(1.0f));
        CHECK(uiOut[3] == Catch::Approx(1.0f));
    }

    SECTION("Straight-alpha FBO draws do not suffer double-multiplied alpha darkening") {
        const auto alphaState = PipelineBuilder::blendAlpha();
        const RGBA semiTransparentFrag = {0.40f, 0.80f, 1.00f, 0.50f};
        const RGBA fbo = applyBlendState(alphaState, semiTransparentFrag, clearTransparent);
        CHECK(fbo[0] == Catch::Approx(0.20f));
        CHECK(fbo[1] == Catch::Approx(0.40f));
        CHECK(fbo[2] == Catch::Approx(0.50f));
        CHECK(fbo[3] == Catch::Approx(0.50f));

        const RGBA uiOut = applyBlendState(
            premulState, evalPremultipliedUiFrag(fbo, fullOpacityVertex), dressUpBackdrop);
        CHECK(uiOut[0] == Catch::Approx(0.20f + 0.50f * dressUpBackdrop[0]));
        CHECK(uiOut[1] == Catch::Approx(0.40f + 0.50f * dressUpBackdrop[1]));
        CHECK(uiOut[2] == Catch::Approx(0.50f + 0.50f * dressUpBackdrop[2]));
        CHECK(uiOut[3] == Catch::Approx(1.0f));
    }

    SECTION("Widget fadeout scales both opaque coverage and alpha=0 additive glow to zero") {
        const RGBA glowFrag = {0.40f, 0.60f, 1.00f, 0.80f};
        const RGBA fboGlow = applyBlendState(addState, glowFrag, clearTransparent);
        const RGBA fadedOutVertex = {1.0f, 1.0f, 1.0f, 0.0f};
        const RGBA uiZero = applyBlendState(
            premulState, evalPremultipliedUiFrag(fboGlow, fadedOutVertex), dressUpBackdrop);
        CHECK(uiZero[0] == Catch::Approx(dressUpBackdrop[0]));
        CHECK(uiZero[1] == Catch::Approx(dressUpBackdrop[1]));
        CHECK(uiZero[2] == Catch::Approx(dressUpBackdrop[2]));

        const RGBA halfFadeVertex = {1.0f, 1.0f, 1.0f, 0.5f};
        const RGBA uiHalf = applyBlendState(
            premulState, evalPremultipliedUiFrag(fboGlow, halfFadeVertex), dressUpBackdrop);
        CHECK(uiHalf[0] == Catch::Approx(std::min(1.0f, dressUpBackdrop[0] + 0.5f * fboGlow[0])));
        CHECK(uiHalf[1] == Catch::Approx(std::min(1.0f, dressUpBackdrop[1] + 0.5f * fboGlow[1])));
        CHECK(uiHalf[2] == Catch::Approx(std::min(1.0f, dressUpBackdrop[2] + 0.5f * fboGlow[2])));
    }
}

TEST_CASE("Preview studio light uses TBC 2.4.3 model2.bls rig without overexposing albedo",
          "[portrait][preview][lighting]") {
    const auto studio = wowee::rendering::previewStudioLightConstants();
    CHECK(studio.lightDirW == Catch::Approx(1.0f));
    CHECK(studio.unitAmbGain == Catch::Approx(1.0f));
    CHECK(studio.ambientRGB[0] == Catch::Approx(0.70f));
    CHECK(studio.ambientRGB[1] == Catch::Approx(0.70f));
    CHECK(studio.ambientRGB[2] == Catch::Approx(0.70f));
    CHECK(studio.perFrameLightColorRGB[0] * 2.5f == Catch::Approx(0.80f));
    CHECK(studio.perFrameLightColorRGB[1] * 2.5f == Catch::Approx(0.80f));
    CHECK(studio.perFrameLightColorRGB[2] * 2.5f == Catch::Approx(0.64f));

    // Under TBC model2.bls (lightDirW == 1.0), litFactor = clamp(amb + NdotL * diff, 0, 1)
    // stays at 0.70 on the unlit hemisphere and caps at 1.00 on the directly lit hemisphere,
    // preventing flat weapon blades (Ethereum Nexus-Reaver) from blowing out to solid white.
    for (size_t c = 0; c < 3; ++c) {
        const float effAmb = studio.ambientRGB[c] * studio.unitAmbGain;
        const float effDiff = studio.perFrameLightColorRGB[c] * 2.5f;
        const float unlitSide = std::clamp(effAmb, 0.0f, 1.0f);
        const float fullLitSide = std::clamp(effAmb + 1.0f * effDiff, 0.0f, 1.0f);
        CHECK(unlitSide == Catch::Approx(0.70f));
        CHECK(fullLitSide == Catch::Approx(1.0f));
    }
}
