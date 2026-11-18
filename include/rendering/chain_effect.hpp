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

/// The further rows a row's Combo string names, drawn whole between the
/// same ends. Each entry is 4 bytes: ID bits 0-6 in the first, ID bit 7 as
/// 0x02 and ID bit 8 as 0x20 of the fourth; bytes 2-3 are always 0x01 and
/// the fourth's 0x10 is not interpreted. Read so, 214 of the 245 TBC entries
/// name rows that exist, nearly all just after their parent (48 -> 49,
/// 215 -> 209/211/217, 237 -> 239, 302 -> 301); the rest name IDs the
/// file lacks (52 -> 53). Native Tear of Azzinoth Summon Channel 39857 (237:
/// "o..\x03", "o..\x13", 239 twice) draws three green waves; Sunshatter
/// 45215 (215) a blue bolt (209), a broad purple-white wave (211) and thin
/// purple bolts (217) beside its green one.
inline std::vector<uint32_t> chainComboStrands(std::string_view combo) {
    assert(combo.size() % 4 == 0);
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < combo.size(); i += 4) {
        const auto low = static_cast<uint8_t>(combo[i]);
        const auto high = static_cast<uint8_t>(combo[i + 3]);
        ids.push_back(low | ((high & 0x02u) << 6) | ((high & 0x20u) << 3));
    }
    return ids;
}

/// How far the beam's interior vertices stray from the straight line:
/// NoiseScale of the beam's length. Native ManaBeam (0.01) at 28 yd kinks by
/// about 0.15 yd on screen; Sunshatter's bolt (0.019) at 24 yd by about 0.5 yd.
inline float chainNoiseRadius(float noiseScale, float length) { return noiseScale * length; }

}  // namespace wowee::rendering
