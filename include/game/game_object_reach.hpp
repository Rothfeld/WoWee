#pragma once

#include <glm/glm.hpp>

namespace wowee::game {

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
