#pragma once

#include <algorithm>
#include <cassert>
#include <glm/glm.hpp>

namespace wowee::rendering {

// Clip the orbit ray to the swimmer's side of the water surface. The pivot
// must already be on that side; neither yaw nor the requested zoom changes.
inline glm::vec3 constrainWaterCamera(const glm::vec3& pivot, const glm::vec3& eye,
                                     float boundary, bool submerged) {
    const float side = submerged ? -1.0f : 1.0f;
    assert(side * (pivot.z - boundary) >= 0.0f);
    if (side * (eye.z - boundary) >= 0.0f) return eye;
    const glm::vec3 offset = eye - pivot;
    assert(offset.z != 0.0f);
    return pivot + offset * std::clamp((boundary - pivot.z) / offset.z, 0.0f, 1.0f);
}

} // namespace wowee::rendering
