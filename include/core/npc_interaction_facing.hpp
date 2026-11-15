#pragma once

#include "core/coordinates.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace wowee::core {

// Local conversation facing turns along the shortest arc without touching
// server movement state. A full turn takes one second.
inline float advanceNpcInteractionFacing(float current, float target, float dt) {
    assert(std::isfinite(current) && std::isfinite(target) && std::isfinite(dt) && dt >= 0.0f);
    const float delta = coords::normalizeAngleRad(target - current);
    const float step = 6.28318530718f * dt;
    return coords::normalizeAngleRad(current + std::clamp(delta, -step, step));
}

} // namespace wowee::core
