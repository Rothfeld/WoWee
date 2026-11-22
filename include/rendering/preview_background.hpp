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

/// Select preview background mode for UnitPortrait framing: circular/micro face
/// portraits use OpaquePortrait; full-body UI model frames use Transparent.
[[nodiscard]] constexpr PreviewBackgroundMode portraitBackgroundModeForFraming(bool isFaceFraming) {
    return isFaceFraming ? PreviewBackgroundMode::OpaquePortrait
                         : PreviewBackgroundMode::Transparent;
}

/// Native TBC 2.4.3 never desaturates PlayerPortrait when the player is a ghost
/// (neither spectral ghosts nor Night Elf Wisp Spirit, where Rec.601 luminance
/// desaturation crushes pure blue Wisp.m2 glow by 0.114).
[[nodiscard]] constexpr bool shouldDesaturatePlayerPortrait(bool /*isPlayerGhost*/) {
    return false;
}

} // namespace wowee::rendering
