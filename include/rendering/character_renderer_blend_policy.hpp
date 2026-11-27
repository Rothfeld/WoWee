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

/// Whether a per-instance M2 texture slot override (`inst.textureSlotOverrides`)
/// applies to a batch referencing a slot of `texType`.
///
/// In M2 character/creature models (`Character\*\*.m2`), cloak batches (`1502+`)
/// reference `texType == 2` (`TEX_COMPONENT_OBJECT_SKIN`), hair-card batches
/// reference `texType == 6` (`TEX_COMPONENT_CHAR_HAIR`), and skin-extra batches
/// reference `texType == 8`. Every batch whose texture lookup selects a
/// `texType == 1` (`TEX_COMPONENT_SKIN`) slot -- including facial hair / horns
/// (`1xx`, `2xx`, `3xx`), ears (`7xx`), trouser cuffs (`11xx`), bare back
/// (`1501`), and body/limb submeshes -- is UV-mapped to the composited skin
/// atlas, matching `AppearanceComposer::setModelTexture` on the local player.
[[nodiscard]] constexpr bool characterTextureSlotOverrideApplies(
    uint32_t texType,
    uint16_t /*submeshId*/) noexcept {
    return texType != 0u;
}

/// Whether an un-bound (`whiteTexture_`) character batch should fall back to
/// the instance's `type 1` / `type 11` skin composite texture.
///
/// Covers all character skin-atlas geoset groups (`0..5`, `7..13`, `15` for
/// `1501` no-cape torso patch and `ScourgeMale`/`FelOrcMale` `type 1` exposed
/// spine passes on `1502..1506`, `18`, `20`) while never bleeding skin onto
/// scene backdrops, `texType == 2` object-skin cloaks (`1502+`), or
/// `texType == 6` hair cards.
[[nodiscard]] constexpr bool characterBatchUsesSkinFallback(
    bool isSceneModel,
    uint16_t submeshId,
    uint32_t batchTexType) noexcept {
    if (isSceneModel || batchTexType == 2u || batchTexType == 6u) {
        return false;
    }
    const uint16_t group = submeshId / 100u;
    switch (group) {
        case 0u:
        case 1u:
        case 2u:
        case 3u:
        case 4u:
        case 5u:
        case 7u:
        case 8u:
        case 9u:
        case 10u:
        case 11u:
        case 12u:
        case 13u:
        case 15u:
        case 18u:
        case 20u:
            return true;
        default:
            return false;
    }
}

} // namespace wowee::rendering
