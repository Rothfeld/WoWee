#pragma once

#include <array>
#include <cstdint>

namespace wowee::rendering {

/// Background mode for off-screen CharacterPreview renders.
enum class PreviewBackgroundMode : uint8_t {
    /// Character select / creation: 3D racial glue scene + opaque studio clear.
    RacialScene,
    /// Full-body UI model frames (PaperDoll, Inspect, DressUp, Pet, Tabard):
    /// no racial scene, transparent alpha=0 clear so panel art shows behind.
    Transparent,
    /// Circular/micro unit portraits (SetPortraitTexture: Player, Target, Party,
    /// Focus, Pet, NPC): no racial scene, opaque black alpha=1 clear. Native
    /// TBC 2.4.3 renders portraits onto an opaque black disc so world sky/walls
    /// never bleed through and additive-only models (such as Night Elf Wisp
    /// Spirit, Creature\Wisp\Wisp.m2) fade smoothly without a hard black quad
    /// boundary against transparent pixels.
    OpaquePortrait,
};

/// Whether a preview background mode loads the 3D racial glue-screen backdrop.
[[nodiscard]] constexpr bool previewLoadsRacialBackdrop(PreviewBackgroundMode mode) {
    return mode == PreviewBackgroundMode::RacialScene;
}

/// RGBA clear color for a preview background mode.
[[nodiscard]] constexpr std::array<float, 4> previewClearColorRGBA(PreviewBackgroundMode mode) {
    switch (mode) {
        case PreviewBackgroundMode::RacialScene:    return {0.05f, 0.05f, 0.1f, 1.0f};
        case PreviewBackgroundMode::Transparent:    return {0.0f, 0.0f, 0.0f, 0.0f};
        case PreviewBackgroundMode::OpaquePortrait: return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    return {0.0f, 0.0f, 0.0f, 1.0f};
}

/// Select preview background mode for UnitPortrait framing: HUD circular/micro
/// face portraits use OpaquePortrait; dialog portraits backed by panel art
/// ("npc"/"questnpc" such as BankFrame/MerchantFrame/GossipFrame/QuestFrame)
/// and full-body UI model frames use Transparent so the panel's painted slate
/// portrait well shows behind the head.
[[nodiscard]] constexpr PreviewBackgroundMode portraitBackgroundModeForFraming(
        bool isFaceFraming, bool hasPanelBackdrop = false) {
    if (!isFaceFraming || hasPanelBackdrop) {
        return PreviewBackgroundMode::Transparent;
    }
    return PreviewBackgroundMode::OpaquePortrait;
}

/// Native TBC 2.4.3 never desaturates PlayerPortrait when the player is a ghost
/// (neither spectral ghosts nor Night Elf Wisp Spirit, where Rec.601 luminance
/// desaturation crushes pure blue Wisp.m2 glow by 0.114).
[[nodiscard]] constexpr bool shouldDesaturatePlayerPortrait(bool /*isPlayerGhost*/) {
    return false;
}

/// TBC 2.4.3 studio light parameters for off-screen CharacterPreview UBOs.
///
/// Measured on Native TBC 2.4.3 (build 8606) via DressUpModel:GetLight() and
/// CharacterModelFrame:GetLight() (plus characterselect.lua:34):
///   enabled=1, omni=0, dir=(0.00, 1.00, 0.00),
///   ambIntensity=1.00, ambRGB=(0.70, 0.70, 0.70),
///   dirIntensity=1.00, dirRGB=(0.80, 0.80, 0.64).
/// Setting lightDirW = 1.0 activates character.frag.glsl's TBC model2.bls
/// vertex-normal lighting (clamping litFactor to [0, 1] before multiplying
/// rawTexRgb and omitting synthetic Phong specular / Sobel normal maps so flat
/// weapon blades like Ethereum Nexus-Reaver do not blow out to solid white).
struct PreviewStudioLight {
    std::array<float, 3> ambientRGB;
    std::array<float, 3> diffuseRGB;
    std::array<float, 3> perFrameLightColorRGB;
    float lightDirW;
    float unitAmbGain;
};

[[nodiscard]] constexpr PreviewStudioLight previewStudioLightConstants() {
    constexpr float kAmbR = 0.70f;
    constexpr float kAmbG = 0.70f;
    constexpr float kAmbB = 0.70f;
    constexpr float kDiffR = 0.80f;
    constexpr float kDiffG = 0.80f;
    constexpr float kDiffB = 0.64f;
    constexpr float kTbcUnitDiffuseScale = 2.5f;
    return PreviewStudioLight{
        .ambientRGB = {kAmbR, kAmbG, kAmbB},
        .diffuseRGB = {kDiffR, kDiffG, kDiffB},
        .perFrameLightColorRGB = {
            kDiffR / kTbcUnitDiffuseScale,
            kDiffG / kTbcUnitDiffuseScale,
            kDiffB / kTbcUnitDiffuseScale,
        },
        .lightDirW = 1.0f,
        .unitAmbGain = 1.0f,
    };
}

} // namespace wowee::rendering
