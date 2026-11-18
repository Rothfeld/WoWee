#pragma once

/// Rules for drawing a SpellChainEffects.dbc beam (channel beams such as
/// Hydross' Blue Beam 38015, chain 17) that do not need a renderer.
///
/// Measured against build 8606 with paired captures of the same server beam
/// (_experiments/20261118T1328Z.hydross_beam_scroll in the workspace root).

#include <algorithm>
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

/// How far joint i (1-based, of JointCount spread evenly between the ends)
/// may stray from the line, in yards: JointOffsetRadius, times
/// MinorJointScale on every JointsPerMinorJoint-th joint and MajorJointScale
/// on every (JointsPerMinorJoint x MinorJointsPerMajorJoint)-th. Native
/// Open Portal from Shattrath 46801 (chain 411: 27 joints, 0.07, 3, 4, 3.7,
/// 8.45) over 24 yd: strands kinked about every yard, bent over several,
/// spread about a yard across; WoWee drew them straight (NoiseScale 0).
struct ChainJoints {
    uint32_t count = 0, perMinor = 0, minorPerMajor = 0;
    float radius = 0, minorScale = 0, majorScale = 0;
};
inline float chainJointRadius(const ChainJoints& joints, uint32_t i) {
    assert(i >= 1 && i <= joints.count);
    if (joints.perMinor && joints.minorPerMajor && i % (joints.perMinor * joints.minorPerMajor) == 0)
        return joints.radius * joints.majorScale;
    if (joints.perMinor && i % joints.perMinor == 0) return joints.radius * joints.minorScale;
    return joints.radius;
}

/// The joints' offset at fraction f (0..1) along the beam: straight between
/// neighbouring joints, zero at the ends. `offsets` holds one per joint.
template <typename T>
T chainJointOffsetAt(const std::vector<T>& offsets, float f) {
    const size_t n = offsets.size();
    if (n == 0) return T(0);
    const float x = std::clamp(f, 0.0f, 1.0f) * static_cast<float>(n + 1);
    const size_t k = static_cast<size_t>(x);
    const float a = x - static_cast<float>(k);
    const T before = (k == 0 || k > n) ? T(0) : offsets[k - 1];
    const T after = k >= n ? T(0) : offsets[k];
    return before * (1.0f - a) + after * a;
}

}  // namespace wowee::rendering
