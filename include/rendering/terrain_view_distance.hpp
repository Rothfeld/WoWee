#pragma once

#include <cassert>
#include <glm/glm.hpp>

namespace wowee::rendering {

// Terrain range is horizontal. Keep a chunk while any part of its bounding
// sphere overlaps the range; a center-only test clips nearby mountain faces.
inline bool terrainWithinViewDistance(const glm::vec3& center, float radius,
                                      const glm::vec3& camera, float viewDistance) {
    assert(radius >= 0.0f && viewDistance > 0.0f);
    const glm::vec2 delta = glm::vec2(center - camera);
    const float reach = viewDistance + radius;
    return glm::dot(delta, delta) <= reach * reach;
}

} // namespace wowee::rendering
