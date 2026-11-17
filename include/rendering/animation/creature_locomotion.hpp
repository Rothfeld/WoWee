#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <algorithm>
#include <cassert>
#include <optional>

namespace wowee::rendering {

// WALKING selects ground speed, but does not turn a swimmer into a land animal.
inline constexpr uint32_t creatureMovingAnimation(bool flying, bool swimming, bool walking) {
    if (flying) return anim::FLY_FORWARD;
    if (swimming) return anim::SWIM;
    return walking ? anim::WALK : anim::RUN;
}

// Walk or Run for a server-driven (SMSG_MONSTER_MOVE spline) ground unit.
// Build 8606 decides from the spline's speed against the model's own Walk
// stride (Walk sequence moveSpeed x model scale), not from the retained
// WALK_MODE flag, the spline's Runmode flag (CMaNGOS sets it on every move)
// or the unit's walk/run speeds: it walks until Walk would play at
// kSplineRunWalkRate times its authored rate. Measured natively, see
// _experiments/20261117T215856Z.berserker_gait: TrollDire x1.2 (stride 2.0)
// walks at 3.2 and runs at 4.0 yd/s; ElementalEarth (3.89) walks at 7.2 and
// runs at 8.0; Chicken x10 (25.0) walks at 8.0. Walk rates up to 1.85
// walked, 2.00 and above ran.
inline constexpr float kSplineRunWalkRate = 2.0f;

inline bool splineGroundGaitWalks(float speed, float walkStride) {
    assert(speed >= 0.0f && walkStride > 0.0f);
    return speed < kSplineRunWalkRate * walkStride;
}

inline float creatureSwimmingZ(float serverZ, bool flying, bool swimming,
                               std::optional<float> surface, float authoredDepth) {
    if (flying || !swimming || !surface) return serverZ;
    assert(authoredDepth > 0.0f);
    return std::min(serverZ, *surface - authoredDepth);
}

} // namespace wowee::rendering
