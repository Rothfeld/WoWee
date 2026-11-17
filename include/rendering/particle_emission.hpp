#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {
inline constexpr uint32_t kParticleInModelSpace = 0x20;

// M2/WMO placements carry uniform scale in their transform. Matrix-created
// doodads have no separate scalar placement value; measure their basis.
inline float particlePlacementScale(const glm::mat4& model) {
    return glm::length(glm::vec3(model[0]));
}

// Attached particles are simulated before the animated emitter transform.
// Detached particles have already received that transform at birth.
inline glm::mat4 particleRenderTransform(uint32_t flags, const glm::mat4& model,
                                         const glm::mat4& bone, uint8_t emitterType = 1) {
    // Spline trails retain the emitter pose at birth. Applying today's bone
    // to every live particle collapses a traveling rune trail into one bulb.
    return (flags & kParticleInModelSpace)
        ? (emitterType == 3 ? model : model * bone) : glm::mat4(1.0f);
}

inline glm::mat4 particleBirthTransform(uint32_t flags, uint8_t emitterType,
                                       const glm::mat4& model, const glm::mat4& bone) {
    if (!(flags & kParticleInModelSpace)) return model * bone;
    return emitterType == 3 ? bone : glm::mat4(1.0f);
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
} // namespace wowee::rendering
