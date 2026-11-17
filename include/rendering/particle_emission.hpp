#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {
inline constexpr uint32_t kParticleNoTrail = 0x10;
inline constexpr uint32_t kParticleInModelSpace = 0x80;

// M2/WMO placements carry uniform scale in their transform. Matrix-created
// doodads have no separate scalar placement value; measure their basis.
inline float particlePlacementScale(const glm::mat4& model) {
    return glm::length(glm::vec3(model[0]));
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
} // namespace wowee::rendering
