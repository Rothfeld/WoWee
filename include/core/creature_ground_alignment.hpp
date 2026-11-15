#pragma once

#include "pipeline/m2_loader.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::core {

// M2 TiltX/TiltY describe the slope along model axes. Model +X is forward;
// a forward rise therefore rotates around -Y, not around X.
// A missing support sample means the footprint is not on a known floor.
template<class SampleFloor>
glm::vec3 creatureGroundRotation(const pipeline::M2Model& model, float scale,
                                 const glm::vec3& position, float yaw,
                                 SampleFloor&& sampleFloor) {
    assert(std::isfinite(scale) && scale > 0.0f);
    glm::vec3 rotation(0.0f, 0.0f, yaw);
    const glm::vec3 forward(std::cos(yaw), std::sin(yaw), 0.0f);
    const glm::vec3 left(-forward.y, forward.x, 0.0f);
    // Sample inside the collision footprint, excluding snout/tail extremities.
    const glm::vec3 halfSpan = (model.boundMax - model.boundMin) * (0.25f * scale);
    if ((model.globalFlags & 0x1u) && halfSpan.x > 0.0f) {
        const auto front = sampleFloor(position + forward * halfSpan.x);
        const auto back = sampleFloor(position - forward * halfSpan.x);
        if (front && back) rotation.y = -std::atan2(*front - *back, 2.0f * halfSpan.x);
    }
    if ((model.globalFlags & 0x2u) && halfSpan.y > 0.0f) {
        const auto leftHeight = sampleFloor(position + left * halfSpan.y);
        const auto rightHeight = sampleFloor(position - left * halfSpan.y);
        if (leftHeight && rightHeight) rotation.x = std::atan2(
            *leftHeight - *rightHeight, 2.0f * halfSpan.y);
    }
    return rotation;
}

} // namespace wowee::core
