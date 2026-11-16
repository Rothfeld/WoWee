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
    const glm::vec3 direction(std::cos(elevation) * std::cos(azimuth),
                              std::cos(elevation) * std::sin(azimuth), std::sin(elevation));
    const glm::vec3 offset = direction * glm::vec3(length, width, length) * (0.5f * radius);
    const float magnitude = glm::length(offset);
    return {offset, (flags & 0x100u) != 0 ? glm::vec3(0, 0, 1)
                    : magnitude > 0 ? offset / magnitude : glm::vec3(0)};
}
} // namespace wowee::rendering
