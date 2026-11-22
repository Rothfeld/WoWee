#pragma once

#include <cstdint>

namespace wowee::rendering {

struct CharacterDepthBias {
    float constantFactor = 0.0f;
    float slopeFactor = 0.0f;
};

// Compute Vulkan dynamic depth bias for a CharacterRenderer batch under
// standard LESS depth (0 = near, 1 = far). Negative bias pulls fragments
// slightly toward the camera so coplanar layers do not Z-fight.
// - Fading instances (surfaceFade) use zero bias so pass 1 matches the
//   depth prepass (pass -1) under EQUAL / LESS_OR_EQUAL.
// - Higher materialLayer values pull forward by -0.5 per layer to separate
//   coplanar equipment/armor shells.
// - Non-depth-writing blended batches (blendMode >= 2 or M2 NO_ZWRITE 0x10,
//   such as Slime.m2 Sub[4] ground-hugging corpse puddle) receive a small
//   constant + slope bias (-2.0, -2.0) to prevent coplanar floor Z-fighting
//   against terrain and WMO triangles without visual floating.
inline CharacterDepthBias computeCharacterDepthBias(bool surfaceFade,
                                                    uint16_t materialLayer,
                                                    uint16_t blendMode,
                                                    uint16_t materialFlags) {
    if (surfaceFade) {
        return {};
    }
    const float layerBias = -static_cast<float>(materialLayer) * 0.5f;
    const bool noDepthWriteBlended = (blendMode >= 2u) || ((materialFlags & 0x10u) != 0u);
    if (noDepthWriteBlended) {
        return {layerBias - 2.0f, -2.0f};
    }
    return {layerBias, 0.0f};
}

} // namespace wowee::rendering
