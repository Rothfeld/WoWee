#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee {
namespace rendering {

// ADT MCLY (Map Chunk Layer) flags bitfield (TBC 2.4.3 / WoW ADT spec).
inline constexpr uint32_t kMclyAnimRotationMask = 0x07u;   // bits 0..2: 0..7 in 45-degree steps
inline constexpr uint32_t kMclyAnimSpeedMask    = 0x38u;   // bits 3..5: 0..7 scroll speed tier
inline constexpr uint32_t kMclyAnimSpeedShift   = 3u;
inline constexpr uint32_t kMclyAnimEnabled      = 0x40u;   // bit 6: linear UV scroll enabled
inline constexpr uint32_t kMclyOverbright       = 0x80u;   // bit 7: emissive/unlit overbright layer
inline constexpr uint32_t kMclyUseAlpha         = 0x100u;  // bit 8: MCAL alpha map present
inline constexpr uint32_t kMclyCompressedAlpha  = 0x200u;  // bit 9: RLE compressed MCAL alpha map

// Native TBC 2.4.3 UV scroll rate per MCLY animation_speed step (UV tiles/sec).
// Measured in Burning Steppes (azeroth_34_44.adt, rot=6 spd=5 -> ~0.156 tiles/sec,
// rot=0 spd=4 -> ~0.125 tiles/sec).
inline constexpr float kMclyAnimSpeedStep = 1.0f / 32.0f;

struct TerrainLayerMclyParams {
    glm::vec2 uvVelocity{0.0f, 0.0f};
    bool overbright = false;
};

/// Decode ADT MCLY layer flags into per-layer UV scroll velocity (UV tiles/sec)
/// and overbright (emissive) state.
///
/// Terrain vertices assign TexCoord = (-renderY, -renderX) * (8 / CHUNK_SIZE)
/// where +u runs South (-wowX) and +v runs East (-wowY). Negating (cos, sin)
/// shifts the sampled texture pattern along the authored compass direction:
///   rot=0 (0 deg):   duv=(-1, 0) -> pattern scrolls South (-wowX)
///   rot=2 (90 deg):  duv=(0, -1) -> pattern scrolls East  (-wowY)
///   rot=4 (180 deg): duv=(+1, 0) -> pattern scrolls North (+wowX)
///   rot=6 (270 deg): duv=(0, +1) -> pattern scrolls West  (+wowY)
inline TerrainLayerMclyParams decodeMclyLayer(uint32_t flags) {
    TerrainLayerMclyParams out{};
    out.overbright = (flags & kMclyOverbright) != 0u;
    if ((flags & kMclyAnimEnabled) != 0u) {
        const uint32_t rot = flags & kMclyAnimRotationMask;
        const uint32_t spd = (flags & kMclyAnimSpeedMask) >> kMclyAnimSpeedShift;
        if (spd > 0u) {
            constexpr float kQuarterPi = 0.78539816339744830962f;
            const float angle = static_cast<float>(rot) * kQuarterPi;
            const float rate = static_cast<float>(spd) * kMclyAnimSpeedStep;
            out.uvVelocity = glm::vec2(-std::cos(angle) * rate, -std::sin(angle) * rate);
        }
    }
    return out;
}

/// Compute blended per-fragment overbright weight (0..1) matching terrain.frag.glsl.
inline float blendTerrainOverbright(bool bigAlpha,
                                    float a1, float a2, float a3,
                                    float ob0, float ob1, float ob2, float ob3) {
    if (bigAlpha) {
        const float w0 = std::max(1.0f - a1 - a2 - a3, 0.0f);
        return std::clamp(ob0 * w0 + ob1 * a1 + ob2 * a2 + ob3 * a3, 0.0f, 1.0f);
    }
    const float b1 = ob0 + (ob1 - ob0) * a1;
    const float b2 = b1 + (ob2 - b1) * a2;
    const float b3 = b2 + (ob3 - b2) * a3;
    return std::clamp(b3, 0.0f, 1.0f);
}

} // namespace rendering
} // namespace wowee
