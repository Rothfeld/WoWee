#pragma once

#include <glm/glm.hpp>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <span>
#include <cstdint>

namespace wowee::rendering {

// Unnormalized local direction preserves the world-space ray parameter under
// instance scaling. Two-sided triangles match cursor occlusion on solid props.
inline float pickVisualTriangles(std::span<const glm::vec3> vertices,
                                 std::span<const uint16_t> indices,
                                 const glm::vec3& origin, const glm::vec3& direction,
                                 float limit) {
    assert(indices.size() % 3 == 0);
    for (size_t i = 0; i < indices.size(); i += 3) {
        assert(indices[i] < vertices.size() && indices[i + 1] < vertices.size() && indices[i + 2] < vertices.size());
        const auto& a = vertices[indices[i]];
        const glm::vec3 edge1 = vertices[indices[i + 1]] - a;
        const glm::vec3 edge2 = vertices[indices[i + 2]] - a;
        const glm::vec3 p = glm::cross(direction, edge2);
        const float determinant = glm::dot(edge1, p);
        if (std::abs(determinant) < 1e-8f) continue;
        const glm::vec3 offset = origin - a;
        const float u = glm::dot(offset, p) / determinant;
        if (u < 0.0f || u > 1.0f) continue;
        const glm::vec3 q = glm::cross(offset, edge1);
        const float v = glm::dot(direction, q) / determinant;
        if (v < 0.0f || u + v > 1.0f) continue;
        const float t = glm::dot(edge2, q) / determinant;
        if (t > 0.0f && t < limit) limit = t;
    }
    return limit;
}

} // namespace wowee::rendering
