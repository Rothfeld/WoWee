#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>
#include "pipeline/m2_loader.hpp"

namespace wowee::rendering {
inline constexpr uint32_t kParticleNoTrail = 0x10;
inline constexpr uint32_t kParticleInModelSpace = 0x80;

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

inline glm::vec3 particleGravityDirection(const glm::mat4& birthTransform) {
    // Preserve authored acceleration scale and orientation with position and
    // velocity when a particle is detached into model/world space.
    return glm::mat3(birthTransform) * glm::vec3(0, 0, -1);
}

struct ParticleEmission {
    glm::vec3 offset;
    glm::vec3 direction;
};

inline ParticleEmission sphericalParticleEmission(float length, float width,
    float elevation, float azimuth, float radius, uint32_t flags) {
    // Sphere tracks are minimum/maximum radius, not planar box dimensions.
    // Zero horizontal spread leaves a ring in the model YZ plane (+X normal).
    const glm::vec3 direction(std::cos(elevation) * std::sin(azimuth),
                              std::cos(elevation) * std::cos(azimuth), std::sin(elevation));
    const glm::vec3 offset = direction * glm::mix(length, width, radius);
    const float magnitude = glm::length(offset);
    return {offset, (flags & 0x100u) != 0 ? glm::vec3(0, 0, 1)
                    : magnitude > 0 ? offset / magnitude : glm::vec3(0)};
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
