#pragma once

#include "rendering/animation/animation_ids.hpp"
#include <algorithm>
#include <cassert>
#include <optional>

namespace wowee::rendering {

// WALKING selects ground speed, but does not turn a swimmer into a land animal.
inline constexpr uint32_t creatureMovingAnimation(bool flying, bool swimming, bool walking,
                                                  bool stealthed = false) {
    if (flying) return anim::FLY_FORWARD;
    if (swimming) return anim::SWIM;
    if (stealthed) return walking ? anim::STEALTH_WALK : anim::STEALTH_RUN;
    return walking ? anim::WALK : anim::RUN;
}

// Ground locomotion sequences whose playback rate scales with world speed /
// (abs(M2Sequence::movingSpeed) * instanceScale). Matches Native TBC 2.4.3
// where StealthWalk (119), StealthRun (223), and Sprint (143) scale identically
// to Walk (4), Run (5), and Walkbackwards (13).
inline constexpr bool isGroundLocomotionScaledAnimation(uint32_t animId) {
    switch (animId) {
        case anim::WALK:
        case anim::RUN:
        case anim::WALK_BACKWARDS:
        case anim::STEALTH_WALK:
        case anim::STEALTH_RUN:
        case anim::SPRINT:
            return true;
        default:
            return false;
    }
}

// All locomotion animations in TBC 2.4.3 AnimationData.dbc with BodyFlags & 0x40:
// the 6 ground locomotion sequences above plus Swim (42), SwimLeft (43),
// SwimRight (44), and SwimBackwards (45). Flight animations (Fly 135, Hover 193)
// do NOT carry BodyFlags 0x40 and play at fixed 1.0x rate regardless of speed.
inline constexpr bool isLocomotionSpeedScaledAnimation(uint32_t animId) {
    if (isGroundLocomotionScaledAnimation(animId)) return true;
    switch (animId) {
        case anim::SWIM:
        case anim::SWIM_LEFT:
        case anim::SWIM_RIGHT:
        case anim::SWIM_BACKWARDS:
            return true;
        default:
            return false;
    }
}

inline float computeLocomotionPlaybackRate(uint32_t animId, float locomotionSpeed,
                                           float seqMovingSpeed, float scale) {
    if (!isLocomotionSpeedScaledAnimation(animId) ||
        locomotionSpeed < 0.0f || seqMovingSpeed == 0.0f) {
        return 1.0f;
    }
    assert(scale > 0.0f);
    return locomotionSpeed / (std::abs(seqMovingSpeed) * scale);
}

// Nominal unscaled strides when an M2 model authors Walk/Run with movingSpeed == 0
// (e.g. HumanFemale.m2 Walk sequence has movingSpeed == 0).
inline constexpr float kDefaultWalkMoveSpeed = 2.5f;
inline constexpr float kDefaultRunMoveSpeed = 250.0f / 36.0f;

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

// Run -> Sprint (143) promotion rate for ground forward/strafe locomotion.
// Measured empirically on Native TBC 2.4.3 (_experiments/20261121T234730Z.movement_speed_anim_scaling_parity):
// HumanMale and NightElfMale stay in Run (5) at 1.55x (10.85 yd/s) and promote
// to Sprint (143) at >= 11.00 yd/s (including Walk mode at 4.4x = 11.00 yd/s,
// Rogue Sprint 1.7x = 11.90 yd/s, and .modify speed 2/5/10), scaling linearly
// with instance scale (TaurenMale 1.35x: 2.0x = 14.0 yd/s -> Run, 5.0x = 35.0 yd/s -> Sprint;
// RidingKodo 0.60x: 1.0x = 7.0 yd/s -> Sprint).
inline constexpr float kGroundSprintRunRate = 1.58f;

inline bool splineGroundGaitWalks(float speed, float walkStride) {
    assert(speed >= 0.0f && walkStride > 0.0f);
    return speed < kSplineRunWalkRate * walkStride;
}

// Select Walk (4), Run (5), or Sprint (143) for non-stealthed ground forward/strafe
// movement from world speed and the model's scaled Walk/Run strides.
inline constexpr uint32_t selectGroundForwardGaitAnimation(
    bool walkingMode,
    float speed,
    std::optional<float> walkStride,
    std::optional<float> runStride,
    bool hasSprint,
    bool sprintAura = false) {
    if (hasSprint && runStride && *runStride > 0.0f &&
        speed >= kGroundSprintRunRate * (*runStride)) {
        return anim::SPRINT;
    }
    if (walkingMode) {
        return anim::WALK;
    }
    if (sprintAura && hasSprint) {
        return anim::SPRINT;
    }
    if (walkStride && *walkStride > 0.0f && speed > 0.0f &&
        speed < kSplineRunWalkRate * (*walkStride)) {
        return anim::WALK;
    }
    return anim::RUN;
}

inline float creatureSwimmingZ(float serverZ, bool flying, bool swimming,
                               std::optional<float> surface, float authoredDepth) {
    if (flying || !swimming || !surface) return serverZ;
    assert(authoredDepth > 0.0f);
    return std::min(serverZ, *surface - authoredDepth);
}

} // namespace wowee::rendering
