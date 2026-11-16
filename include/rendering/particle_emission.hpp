#pragma once
#include <cmath>
#include <cstdint>
#include <glm/glm.hpp>

namespace wowee::rendering {
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
