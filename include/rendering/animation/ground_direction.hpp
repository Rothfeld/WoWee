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

// Native TBC 2.4.3 (build 8606) exponentially filters the lower-body yaw offset
// within [-pi/2, +pi/2] instead of snapping 1 frame or slewing at constant
// linear angular speed. Because the offset is a signed scalar in [-pi/2, +pi/2],
// reversing strafe direction (+pi/2 <-> -pi/2) always pivots through 0 (front),
// keeping torso counter-rotation within [-90 deg, +90 deg].
//
// Measured at 200 Hz sampling on Native 2.4.3 (DruidCat, HumanMale, TaurenMale;
// Stand->Strafe, Q<->E, W+Q<->W+E, S+Q<->S+E, W+E->W, Strafe->Stop):
//   - Remaining angular error multiplies by 0.75 per 60 Hz frame (alpha = 0.25):
//       theta(t + dt) = target + (current - target) * 0.75^(60 * dt)
//                     = target + (current - target) * exp(-kLegYawDecayPerSec * dt)
//     with kLegYawDecayPerSec = 60 * ln(4 / 3) = 17.2609243 s^-1.
//   - Q -> E (+90 -> -90 deg) at 60 Hz steps +90 -> +45 -> +11.25 -> -14.06 ->
//     -33.05 -> -47.29 -> -57.96 -> -65.97 -> -71.98 deg, crossing 0 deg (front)
//     at t_1/2 = ln(2) / k = 40.16 ms (~2.4 frames), 90% in 133 ms, 95% in 173 ms.
//   - Stopping (Strafe -> Stand) and diagonal release (W+E -> W) follow the
//     exact same exponential decay toward 0.
constexpr float kLegYawMaxOffsetRad = 0.5f * 3.14159265358979323846f;
constexpr float kLegYawDecayPerSec  = 17.260924347106847f; // 60 * ln(4 / 3)
constexpr float kLegYawSnapEpsRad   = 1e-4f;

inline float stepLegYawOffset(float currentRad, float targetRad, float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) return currentRad;
    const float clampedCurrent = std::clamp(currentRad, -kLegYawMaxOffsetRad, kLegYawMaxOffsetRad);
    const float clampedTarget  = std::clamp(targetRad,  -kLegYawMaxOffsetRad, kLegYawMaxOffsetRad);
    const float remaining = clampedCurrent - clampedTarget;
    if (std::abs(remaining) <= kLegYawSnapEpsRad) return clampedTarget;
    const float next = clampedTarget + remaining * std::exp(-kLegYawDecayPerSec * dt);
    if (std::abs(next - clampedTarget) <= kLegYawSnapEpsRad) return clampedTarget;
    return next;
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

// Target SpineLow upper-body counter-rotation relative to the lower-body yaw.
// On the ground, counter-rotating by -targetLegYawRad keeps the torso aligned
// with gameplay facing while the legs run sideways. While airborne (jump/fall)
// or mounted, Native TBC 2.4.3 does not counter-rotate SpineLow so the whole
// body faces the jump direction cleanly (avoiding spine-twist contortion on
// models whose JumpStart/Jump animations pitch/roll SpineLow, such as
// BloodElfMale, BloodElfFemale, and HumanMale).
inline float strafeTorsoYawTarget(float targetLegYawRad, bool airborne, bool mounted) {
    if (mounted || airborne) return 0.0f;
    return -std::clamp(targetLegYawRad, -kLegYawMaxOffsetRad, kLegYawMaxOffsetRad);
}

} // namespace wowee::rendering
