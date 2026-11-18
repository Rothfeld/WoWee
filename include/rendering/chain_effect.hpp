#pragma once

/// Rules for drawing a SpellChainEffects.dbc beam (channel beams such as
/// Hydross' Blue Beam 38015, chain 17) that do not need a renderer.
///
/// Measured against build 8606 with paired captures of the same server beam
/// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).

#include <cassert>
#include <cstdint>
#include <string_view>
#include <vector>

namespace wowee::rendering {

/// Texture coordinate along a beam, s yards from the caster on a beam
/// `length` yards long, t seconds after the beam appeared.
///
/// The texture repeats TextureLength times over the whole beam, whatever its
/// length, and its pattern travels |TexCoordScale| beam lengths a second,
/// toward the target when TexCoordScale is positive; a negative scale also
/// mirrors the texture. Native, ManaBeam (-2, TextureLength 2) at 8 and 28 yd
/// and at Hydross: 2 repeats, 4.0 Hz, toward the caster; WhiteBeam (1.66,
/// TextureLength 2) at 28 yd: 2.1 repeats, 3.34 Hz, toward the target. A
/// repeat count of |TexCoordScale| drew WhiteBeam with 1.66 repeats at 2.76 Hz.
inline float chainTexCoord(float s, float length, float texCoordScale, float textureLength, float t) {
    assert(length > 0.0f);
    const float mirror = texCoordScale < 0.0f ? -1.0f : 1.0f;
    return mirror * textureLength * (s / length - texCoordScale * t);
}

/// The extra strands a row's Combo string names: 4-byte entries whose first
/// byte is another SpellChainEffects ID (the other three bytes are not
/// interpreted). Sunshatter 45215 / the Black Temple door channel 41269
/// (chain 215, combo "S..." "Q..." x4 "Y..." x3) natively draws thin red
/// strands (81, WhiteBeam 255,0,0, width 0.15) and a blue lightning strand
/// (83, DrainManaLightning 0,0,255) beside its green-yellow bolt; 89 has no row.
inline std::vector<uint32_t> chainComboStrands(std::string_view combo) {
    assert(combo.size() % 4 == 0);
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < combo.size(); i += 4) ids.push_back(static_cast<uint8_t>(combo[i]));
    return ids;
}

/// How far the beam's interior vertices stray from the straight line:
/// NoiseScale of the beam's length. Native ManaBeam (0.01) at 28 yd kinks by
/// about 0.15 yd on screen; Sunshatter's bolt (0.019) at 24 yd by about 0.5 yd.
inline float chainNoiseRadius(float noiseScale, float length) { return noiseScale * length; }

}  // namespace wowee::rendering
