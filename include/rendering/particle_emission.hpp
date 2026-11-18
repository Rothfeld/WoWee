#pragma once
#include <algorithm>
#include <functional>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "pipeline/m2_loader.hpp"

namespace wowee::rendering {
inline constexpr uint32_t kParticleNoTrail = 0x10;
inline constexpr uint32_t kParticleInModelSpace = 0x80;
// Sprites lie flat in the model's XY plane (ground rings: WaterElemental
// #16 WhiteRingNoiseWater, #21 Shockwave10, flags 0x1001).
inline constexpr uint32_t kParticleXYQuad = 0x1000;
inline constexpr uint32_t kParticlePinned = 0x400;
// Particle vertex: position 3, colour 4, half width 1, cell 1, tail or
// XYQuad axis 3, rotation 1 (radians).
inline constexpr size_t kParticleVertexFloats = 13;

// Pre-264 emitter +0x198 is a spin rate in radians per second: a sprite
// turns by spin * age. Christmas Tree (Medium), GO 178557 (spin 0.5, 3 s life,
// SPARKLE.BLP drawn as an X) shows the star mostly as a + in the original,
// most of a quarter turn by the sprite's brightest age; unrotated every sprite
// stood as the same X. 8191 of 16070 TBC emitters set it. Turning direction
// is counter-clockwise on screen for positive spin; not checked against
// the original.
inline float particleRotation(float spin, float ageSeconds) {
    return spin * ageSeconds;
}
// Emitters with flag 0x1 keep their authored colour inside a WMO interior;
// the others take the doodad's MODD placement colour. 689 of the 781 emitter
// kinds placed in TBC interiors carry it: torches, candles, braziers, glowing
// crystals. Water, smoke and steam vents mostly do not.
inline constexpr uint32_t kParticleOwnColor = 0x1;

// Twinkle size of a pre-264 emitter's particle (+0x180 speed, +0x188/+0x18C
// scale min/max): a random factor between min and max, drawn again every
// 1/speed seconds of the particle's age (once, for speed 0). Glowy (Red)
// 22518 (focusedcasting_state_chest, scale range 0.1-1.0): the light the aura
// adds around the unit fell from 6.2x native to 3.6x with it; Arcane Power
// 12042 (0.9-1.2) stays within 6% of native. The twinkle percent (+0x184) is
// not applied: Arcane Power's 0.6 changed nothing measurable.
inline float particleTwinkleScale(uint32_t seed, float age, float speed,
                                  float scaleMin, float scaleMax) {
    if (scaleMin == scaleMax) return scaleMin;
    const uint32_t period = speed > 0.0f ? static_cast<uint32_t>(age * speed) : 0u;
    uint32_t h = seed ^ (period * 0x9E3779B9u);
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return scaleMin + (scaleMax - scaleMin) * (static_cast<float>(h >> 8) / 16777215.0f);
}

// A tail quad runs from the particle back along -velocity * tailLength.
// Pinned tails (0x400) end at the particle's birth point at most. 2204 of
// the 3522 TBC tail emitters are pinned, many authoring tailLength 10 s:
// Bloodlust/Summon hand rings (0x479, 0.28 yd/s, 10 s) and the fire precast
// hand sparks (0x439, -1.67 yd/s inward, 5.45 s) otherwise draw 3-9 yd spokes
// the original client does not show. Acid breath drops (0x429, 4.44 yd/s,
// 0.1 s) keep their short velocity tail once past their birth point.
inline glm::vec3 particleTailVector(uint32_t flags, const glm::vec3& velocity, float tailLength,
                                    const glm::vec3& toBirth) {
    const glm::vec3 tail = -velocity * tailLength;
    if (!(flags & kParticlePinned)) return tail;
    return glm::dot(tail, tail) > glm::dot(toBirth, toBirth) ? toBirth : tail;
}

// M2/WMO placements carry uniform scale in their transform. Matrix-created
// doodads have no separate scalar placement value; measure their basis.
inline float particlePlacementScale(const glm::mat4& model) {
    return glm::length(glm::vec3(model[0]));
}

// Billboard half-width: the authored lifecycle scale at placement scale.
// No caps or floors; smoke plumes author scales well above one yard.
inline float particleSpriteHalfWidth(float authoredScale, float placementScale) {
    return authoredScale * placementScale;
}

// NoTrail particles remain attached to the animated emitter. Other particles
// retain its birth pose, either in model space or in world space. Bit 0x20 is
// UnLit; it does not select a simulation coordinate system.
inline glm::mat4 particleRenderTransform(uint32_t flags, const glm::mat4& model,
                                         const glm::mat4& bone) {
    if (flags & kParticleNoTrail) return model * bone;
    return flags & kParticleInModelSpace ? model : glm::mat4(1.0f);
}

inline glm::mat4 particleBirthTransform(uint32_t flags,
                                       const glm::mat4& model, const glm::mat4& bone) {
    if (flags & kParticleNoTrail) return glm::mat4(1.0f);
    return flags & kParticleInModelSpace ? bone : model * bone;
}

// Drag (emitter +0x194, pre-264 layout) scales speed by exp(-drag * t).
// Travel over dt at unit starting speed is the integral of that. Zero drag
// is straight-line motion. BFD_WallLight03 #0 authors 5.5: its sparks stop
// within 0.2 yd of the basin instead of rising for their 4 s life.
// 249 TBC emitters author negative drag (Blink_Impact_Chest #8 -8.95): they
// speed up by the same law.
inline float particleDragTravel(float drag, float dt) {
    if (drag == 0.0f) return dt;
    return (1.0f - std::exp(-drag * dt)) / drag;
}

// Emitter position of a particle born `age` seconds before the end of a step
// of length dt, with the emitter moving linearly from `previous` to `current`
// over that step.
inline glm::vec3 particleBirthPosition(const glm::mat4& previous, const glm::mat4& current,
                                       const glm::vec3& local, float age, float dt) {
    const float back = dt > 0.0f ? std::fmin(std::fmax(age / dt, 0.0f), 1.0f) : 0.0f;
    return glm::mix(glm::vec3(current * glm::vec4(local, 1.0f)),
                    glm::vec3(previous * glm::vec4(local, 1.0f)), back);
}

inline glm::vec3 particleGravityDirection(uint32_t flags, const glm::mat4& model) {
    // Gravity pulls along the model's -Z, never the emitter bone's: the
    // Core Hound's drool falls straight down on the original while its head
    // bone is pitched. The model placement keeps its scale and orientation.
    // NoTrail particles are simulated in the bone's own frame.
    if (flags & (kParticleNoTrail | kParticleInModelSpace)) return {0, 0, -1};
    return glm::normalize(glm::mat3(model) * glm::vec3(0, 0, -1));
}

// Particles detached into world space (neither NoTrail nor ModelSpace) move
// at their authored speed and gravity in yards, whatever the placement scale;
// only their birth point is placed with it. Attached particles live in the
// scaled bone or model frame. The Sunwell gate barrier (GO 188118,
// SUNWELLRAID_GATE_04.m2, scale 8.69, 0x409) otherwise throws its sparks 8.7x
// too fast, past the doorway, as radial streaks; Netherspite's Void Zone
// (PortalofKathune.m2, display scale 0.1, 0x229) lifts its smoke a tenth of
// the height. Brutallus' back flames (0x339, NoTrail, scale 6) do rise with
// the model's scale in the original.
inline glm::mat3 particleWorldVelocityBasis(uint32_t flags, const glm::mat4& birthTransform) {
    glm::mat3 basis(birthTransform);
    if (flags & (kParticleNoTrail | kParticleInModelSpace)) return basis;
    for (int c = 0; c < 3; ++c) basis[c] = glm::normalize(basis[c]);
    return basis;
}

struct ParticleEmission {
    glm::vec3 offset;
    glm::vec3 direction;
};

// Sphere emitter (type 2) birth point and travel direction.
// The area length / width tracks are the minimum / maximum radius; a birth
// takes a radius between them. The vertical range is an elevation above the
// model's horizontal plane, the horizontal range an azimuth about +Z.
// Particles travel away from the emitter centre (along the offset), or up +Z
// with flag 0x100; at zero radius without 0x100 they have no velocity.
// Native evidence (pair 23, _experiments/20261118T143000Z.sphere_emission):
// - Shadow Nova 30852 (shadow_nova_area.m2 #0, vertical 0, horizontal pi,
//   radius to 9.7, 0x2209, kit scale 1): a smoke band spreading flat over
//   the ground round the caster. Elevation taken as tilt from +Z (eb6f678,
//   reverted) made it one rising column.
// - Meeting Stone Summoning Portal 179944 (Summon_Ritual.m2, vertical pi,
//   horizontal 0): a ring standing in the model's YZ plane.
// - A negative radius is no radius. Etherial.m2 Death #20 (radius -1.11 to
//   -0.83, vertical 0, gravity 69) falls as thin lines under the rising
//   wrappings in the original; taken at its magnitude it spread a ring of
//   streaks 4 yd across that drew a white curtain at the Ethereal's waist.
//   11 TBC sphere emitters author negative radii (Etherial/EtherialRobe
//   Death, Illidan's glaive, shadow_nova_area's first 90 ms, IceNuke).
inline ParticleEmission sphericalParticleEmission(float length, float width,
    float elevation, float azimuth, float radius, uint32_t flags) {
    const glm::vec3 direction(std::cos(elevation) * std::sin(azimuth),
                              std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    const glm::vec3 offset = direction * std::fmax(glm::mix(length, width, radius), 0.0f);
    const float magnitude = glm::length(offset);
    return {offset, (flags & 0x100u) != 0 ? glm::vec3(0, 0, 1)
                    : magnitude > 0 ? offset / magnitude : glm::vec3(0)};
}

/// Whether a float track takes more than one value in this sequence.
inline bool particleTrackVaries(const pipeline::M2AnimationTrack& track, int sequence) {
    if (track.sequences.empty()) return false;
    const size_t index = track.globalSequence >= 0 ? 0u : static_cast<size_t>(sequence);
    if (index >= track.sequences.size()) return false;
    const auto& values = track.sequences[index].floatValues;
    return std::adjacent_find(values.begin(), values.end(), std::not_equal_to<>()) != values.end();
}

/// Path parameter of a spline birth, from the area length / width tracks.
/// Width held at one value: births spread uniformly between length and
/// width. Shadowmoon_Rune2 (constant 0 / 1; Witch's Sanctum, Ground Rune
/// 183036) draws its #0 streaks as a fence all round its ring in the
/// original; at the path end they stood in one purple column.
/// Width animated: births stand at its current value. FlameCircle.m2 (0 -> 1
/// over each 1 s Stand, length 0) is a flame front running round the Molten
/// Core rune once a second; spread over [0, width] the ring stayed brightest
/// at its start, which the original does not show.
inline float splineBirthParameter(float length, float width, bool widthVaries, float random01) {
    return widthVaries ? width : glm::mix(length, width, random01);
}

/// A point on a spline emitter's path at `t` in [0, 1], equal parameter per
/// cubic Bezier segment (knot, handle, handle, knot; 3k + 1 points).
inline glm::vec3 splineEmissionPoint(const std::vector<glm::vec3>& points, float t) {
    assert(points.size() >= 4 && (points.size() - 1) % 3 == 0);
    assert(t >= 0.0f && t <= 1.0f);
    const size_t segments = (points.size() - 1) / 3;
    const float scaled = t * static_cast<float>(segments);
    const size_t segment = std::min(static_cast<size_t>(scaled), segments - 1);
    const float u = scaled - static_cast<float>(segment);
    const float v = 1.0f - u;
    const glm::vec3* p = &points[segment * 3];
    return v * v * v * p[0] + 3.0f * v * v * u * p[1] + 3.0f * v * u * u * p[2] + u * u * u * p[3];
}

/// Which blend state an M2Blend particle mode draws with. The fragment shader
/// takes the mode itself for the per-mode alpha handling.
enum class ParticleBlendPipeline { Alpha, Additive, Mod, Mod2x };
inline ParticleBlendPipeline particleBlendPipeline(uint8_t m2Blend) {
    assert(m2Blend <= 6 && "M2Blend particle mode out of range");
    switch (m2Blend) {
        case 3: case 4: return ParticleBlendPipeline::Additive;
        case 5: return ParticleBlendPipeline::Mod;
        case 6: return ParticleBlendPipeline::Mod2x;
        default: return ParticleBlendPipeline::Alpha;  // 0 opaque, 1 alpha key, 2 alpha
    }
}

/// One authored cell range played over `phase` in [0, 1): `repeat` passes
/// from start to end inclusive, either direction (ShadowWordSilence_Breath
/// runs 31 -> 16). Wrapped into the texture's tile count, so an end equal to
/// the tile count (0..7 / 8..16 on 16 tiles, 680 TBC emitters) shows cell 0
/// for the final step.
inline uint32_t particleCellInRange(const pipeline::M2ParticleEmitter::CellRange& range,
                                    float phase, uint32_t tiles) {
    const int repeat = range.repeat > 0 ? range.repeat : 1;
    float f = phase * static_cast<float>(repeat);
    f -= std::floor(f);
    const int span = range.end - range.start;
    const int count = (span >= 0 ? span : -span) + 1;
    int step = static_cast<int>(f * static_cast<float>(count));
    if (step >= count) step = count - 1;
    const int cell = range.start + (span >= 0 ? step : -step);
    const int t = static_cast<int>(tiles);
    return static_cast<uint32_t>(((cell % t) + t) % t);
}

/// The texture cell a particle shows at `lifeRatio`, head or tail sprite.
inline uint32_t particleCell(const pipeline::M2ParticleEmitter& em, float lifeRatio,
                             bool tail, uint32_t tiles) {
    if (tiles <= 1) return 0;
    lifeRatio = std::fmin(std::fmax(lifeRatio, 0.0f), 0.99999f);
    if (!em.legacyCells) {
        const auto& fb = tail ? em.tailCellTrack : em.headCellTrack;
        if (fb.floatValues.empty()) return 0;
        float value = fb.floatValues.back();
        for (size_t i = 0; i + 1 < fb.timestamps.size() && i + 1 < fb.floatValues.size(); ++i) {
            if (lifeRatio <= fb.timestamps[i + 1]) {
                const float dt = fb.timestamps[i + 1] - fb.timestamps[i];
                const float k = dt > 0.0f ? (lifeRatio - fb.timestamps[i]) / dt : 0.0f;
                value = fb.floatValues[i] + (fb.floatValues[i + 1] - fb.floatValues[i]) * k;
                break;
            }
        }
        return static_cast<uint32_t>(std::floor(value)) % tiles;
    }
    const float mid = em.midPoint;
    const bool head = lifeRatio < mid;
    const float phase = head ? (mid > 0.0f ? lifeRatio / mid : 0.0f)
                             : (mid < 1.0f ? (lifeRatio - mid) / (1.0f - mid) : 0.0f);
    if (tail) return particleCellInRange(head ? em.tailCells : em.tailDecayCells, phase, tiles);
    return particleCellInRange(head ? em.headCells : em.decayCells, phase, tiles);
}
} // namespace wowee::rendering
