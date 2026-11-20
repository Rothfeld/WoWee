#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cassert>
#include <cmath>

namespace wowee::game {

/// `offset` (canonical: +X north, +Y west) in a game object's model frame:
/// +X its facing, +Y its left, given its canonical yaw c (server yaw c + pi/2),
/// which faces (cos c, -sin c). Left is that facing turned a quarter counter-
/// clockwise seen from above, (-sin c, -cos c). Original 2.4.3 (pair 17,
/// Shield Generator 185051, local Stand bounds y -1.464..1.477 x 2.099 + 5):
/// usable at exactly +8.100 yd to its left, which only the +1.477 side
/// reaches (8.1005); the -1.464 side ends at 8.073.
inline glm::vec3 gameObjectLocalOffset(const glm::vec3& offset, float canonicalYaw) {
    const float c = std::cos(canonicalYaw), s = std::sin(canonicalYaw);
    return {offset.x * c - offset.y * s, -offset.x * s - offset.y * c, offset.z};
}

/// Full 3D quaternion version of gameObjectLocalOffset.
/// `offset` is in canonical coordinates (+X north, +Y west, +Z up);
/// `worldRotation` maps model-local coordinates into server/render space
/// (+X west, +Y north, +Z up). For Z-only rotation by server yaw s = c + pi/2,
/// this matches gameObjectLocalOffset(offset, c) identically.
inline glm::vec3 gameObjectLocalOffset(const glm::vec3& offset, const glm::quat& worldRotation) {
    [[maybe_unused]] const float lenSq = glm::dot(worldRotation, worldRotation);
    assert(std::isfinite(lenSq) && lenSq > 1e-6f);
    const glm::vec3 serverOffset(offset.y, offset.x, offset.z);
    return glm::conjugate(glm::normalize(worldRotation)) * serverOffset;
}

/// Whether `local` (a point in the object's own frame, in yards) lies inside
/// the box [boundMin, boundMax] x scale grown by `reach` on every side.
inline bool withinGrownBox(const glm::vec3& local, const glm::vec3& boundMin,
                           const glm::vec3& boundMax, float scale, float reach) {
    for (int axis = 0; axis < 3; ++axis) {
        const float lo = boundMin[axis] * scale - reach;
        const float hi = boundMax[axis] * scale + reach;
        if (!(local[axis] > lo && local[axis] < hi)) return false;
    }
    return true;
}

} // namespace wowee::game
