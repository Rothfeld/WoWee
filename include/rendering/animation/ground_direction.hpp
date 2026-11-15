#pragma once

#include <cmath>

namespace wowee::rendering {

struct GroundDirection {
    float modelYawOffset = 0.0f;
    float torsoYawOffset = 0.0f;
    bool backwards = false;
};

// Models run along local +X. Backpedal plays along local -X. Rotate the
// legs onto the travel axis while preserving the packet's upper-body facing.
inline GroundDirection groundDirection(bool forward, bool backward, bool left, bool right) {
    const float longitudinal = float(forward) - float(backward);
    const float lateral = float(left) - float(right);
    const bool backwards = longitudinal < 0.0f;
    if (longitudinal == 0.0f && lateral == 0.0f) return {};
    const float offset = backwards ? std::atan2(-lateral, -longitudinal)
                                   : std::atan2(lateral, longitudinal);
    return {offset, -offset, backwards};
}

} // namespace wowee::rendering
