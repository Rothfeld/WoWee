#pragma once

#include <algorithm>
#include <cmath>
#include <optional>

namespace wowee::rendering {

struct GroundDirection {
    float modelYawOffset = 0.0f;
    float torsoYawOffset = 0.0f;
    bool backwards = false;
    bool moving = false;  // any direction key held
};

// Native TBC 2.4.3 (build 8606) slews the lower-body yaw offset smoothly
// within [-pi/2, +pi/2] instead of snapping 1 frame to the new strafe/diagonal
// angle. Because the offset is a signed scalar in [-pi/2, +pi/2], reversing
// strafe direction (+pi/2 <-> -pi/2) always pivots through 0 (front), keeping
// torso counter-rotation within [-90 deg, +90 deg].
// Measured from 30 fps Native 2.4.3 captures across Human Male, Tauren Male,
// and Druid Cat (Run, Walk, diagonal, back+strafe, stand->strafe, strafe->stop):
//   - Moving transitions (L<->R 180 deg in ~250ms; 90 deg in ~125ms): 4*pi rad/s (720 deg/s)
//   - Stopping return to idle (90 deg -> 0 in ~250ms):                2*pi rad/s (360 deg/s)
constexpr float kLegYawMaxOffsetRad      = 0.5f * 3.14159265358979323846f;
constexpr float kLegYawMoveRateRadPerSec = 4.0f * 3.14159265358979323846f;
constexpr float kLegYawStopRateRadPerSec = 2.0f * 3.14159265358979323846f;

inline float stepLegYawOffset(float currentRad, float targetRad, bool moving, float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return currentRad;
    const float clampedCurrent = std::clamp(currentRad, -kLegYawMaxOffsetRad, kLegYawMaxOffsetRad);
    const float clampedTarget  = std::clamp(targetRad,  -kLegYawMaxOffsetRad, kLegYawMaxOffsetRad);
    const float diff = clampedTarget - clampedCurrent;
    const float maxStep = (moving ? kLegYawMoveRateRadPerSec : kLegYawStopRateRadPerSec) * dt;
    if (std::abs(diff) <= maxStep) return clampedTarget;
    return clampedCurrent + (diff > 0.0f ? maxStep : -maxStep);
}

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
