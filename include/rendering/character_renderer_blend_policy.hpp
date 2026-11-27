#pragma once

#include <algorithm>
#include <cstdint>

namespace wowee::rendering {

/// Resolve the blend mode sent to character.frag.glsl's CharMaterial UBO.
///
/// Attached weapon enchant visuals (`isEffectModel == true`, e.g. Soulfrost
/// `spells/shadow_frost_weapon_effect.m2` with authored `blendMode == 2` Alpha
/// over `GENERICGLOW_BLACK.BLP`, or `shadow_strikes_state_hand.m2`) are routed
/// to `additivePipeline_` (`VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE`) so
/// their black card backing adds zero light instead of compositing as a dark
/// quad. The fragment shader's `blendMode` uniform must agree with that
/// pipeline: leaving `blendMode` at `0`/`2` runs `applyFog` (`mix(fogColor,
/// result, fogFactor)`), which fills black texels of the additive quad with
/// zone/interior fog colour, and leaving `5`/`6` runs the `Mod`/`Mod2x` branch
/// (`mix(1.0/0.5, result, ...)`), adding solid white or grey across the quad.
[[nodiscard]] constexpr int32_t effectiveCharacterShaderBlendMode(
    uint16_t authoredBlendMode,
    bool isEffectModel) noexcept {
    if (isEffectModel) {
        return (authoredBlendMode == 3u) ? 3 : 4;
    }
    return static_cast<int32_t>(authoredBlendMode);
}

/// Compute output alpha for character.frag.glsl when drawing with the standard
/// character pipelines (`blendDisabled`, `blendAlpha`, or `blendAdditive` =
/// `SRC_ALPHA, ONE`).
///
/// - `blendMode == 0` (Opaque, without cutout/color-key) ignores texture alpha
///   so weapon/armor reflection masks stored in alpha do not punch holes in FBOs.
/// - `blendMode == 3` (`M2_BLEND_ADD` / `NoAlphaAdd`, D3D9 `ONE, ONE`, without
///   color-key) ignores texture alpha at full opacity (`outAlpha = opacity`) so
///   `SRC_ALPHA * result == opacity * result`, matching `ONE, ONE` when unfaded
///   while scaling linearly by `opacity` during camera, stealth, or spawn fades.
/// - `blendMode == 4` (`M2_BLEND_ADD_ALPHA`, e.g. Warglaive of Azzinoth's
///   `Glave_1H_DualBlade_D_02.m2` fel glow shell) scales by `texAlpha * opacity`.
[[nodiscard]] constexpr float characterBatchOutAlpha(
    int32_t blendMode,
    bool alphaTest,
    bool colorKeyBlack,
    float texAlpha,
    float opacity) noexcept {
    const float clampedOpacity = std::clamp(opacity, 0.0f, 1.0f);
    if (!colorKeyBlack && ((blendMode == 0 && !alphaTest) || blendMode == 3)) {
        return clampedOpacity;
    }
    return std::clamp(texAlpha, 0.0f, 1.0f) * clampedOpacity;
}

} // namespace wowee::rendering
