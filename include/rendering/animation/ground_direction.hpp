#pragma once

#include <cmath>
#include <optional>

namespace wowee::rendering {

struct GroundDirection {
    float modelYawOffset = 0.0f;
    float torsoYawOffset = 0.0f;
    bool backwards = false;
    bool moving = false;  // any direction key held
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
    return {offset, -offset, backwards, true};
}

// The lower body of a unit in the air keeps the direction it left the ground
// with until it lands (native 2.4.3: a strafing jumper's legs stay turned to
// the takeoff side when the keys switch sides, are released, or forward gains
// a strafe). A jump from standing takes the first direction pressed in the
// air, the one its drift follows. `held` carries that direction between
// frames; on the ground it is cleared and the keys apply.
inline GroundDirection airborneLegDirection(std::optional<GroundDirection>& held, bool airborne,
                                            const GroundDirection& keys) {
    if (!airborne) {
        held.reset();
        return keys;
    }
    if (!held || !held->moving) held = keys;
    return *held;
}

} // namespace wowee::rendering
