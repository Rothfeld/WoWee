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

// Mount animation for a mounted creature (locomotion and neutral idle only).
// Rider-side overlays (spell cast holds such as ReadySpellDirected, melee ready
// stances, stun, emotes, aura poses, stealth) play on the rider's upper body,
// never on the mount model.
inline constexpr uint32_t creatureMountAnimation(bool moving, bool flying,
                                                 bool swimming, bool walking,
                                                 bool hoverIdle = false) {
    if (moving) return creatureMovingAnimation(flying, swimming, walking, /*stealthed=*/false);
    if (flying || hoverIdle) return anim::FLY_IDLE;
    if (swimming) return anim::SWIM_IDLE;
    return anim::STAND;
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
// Build 8606 decides from the spline's speed against the model's authored Walk
// movingSpeed (or the unit's groundWalkSpeed when the M2 authors Walk
// movingSpeed == 0, e.g. DoomGuard.m2, HumanFemale.m2) multiplied by
// OBJECT_FIELD_SCALE_X (not CreatureDisplayInfo.CreatureModelScale), not from
// the retained WALK_MODE flag or the spline's Runmode flag (CMaNGOS sets it on
// every move): it walks while speed < kSplineRunWalkRate * walkStride.
// Measured natively:
// - Unchained Doombringer 20900 (display 19945, DoomGuard.m2, Walk 0 ->
//   groundWalkSpeed 2.5, cdi_scale 2.5): at serverScale 1.0 (threshold 5.0),
//   2.5/3.6 walk and 6.0/10.0 run; at serverScale 2.0 (threshold 10.0), 6.0
//   walks (_experiments/20261122T223300Z.doombringer_gait_parity).
// - Phantom Guardsman (display 16457, HumanFemale.m2, Walk 0 -> 2.5, cdi 1.15):
//   at serverScale 1.0 (threshold 5.0), 2.5/4.0 walk and 5.6/8.0 run; at
//   serverScale 2.0 (threshold 10.0), 5.6 walks
//   (_experiments/20261118T104000Z.guardsman_gait).
// - Akama 23089 (display 21112 x2.6, Walk 2.5 -> threshold 5.0): 3.0, 4.2,
//   4.8 yd/s walk; 5.4, 6.0, 10.0, 10.2 yd/s run
//   (_experiments/20261122T201500Z.akama_walk_gait_parity).
// - TrollDire (x1.2, Walk 1.667 -> threshold 3.33): 2.4, 3.2 walk; 4.0+ run.
// - ElementalEarth (x1.0, Walk 3.889 -> threshold 7.78): 7.2 walk; 8.0 run.
inline constexpr float kSplineRunWalkRate = 2.0f;

inline std::optional<float> splineWalkStride(std::optional<float> modelWalkMoveSpeed,
                                             bool hasWalkAnimation,
                                             float unitGroundWalkSpeed,
                                             float serverScale) {
    const float scale = serverScale > 0.0f ? serverScale : 1.0f;
    if (modelWalkMoveSpeed && *modelWalkMoveSpeed > 0.0f) {
        return *modelWalkMoveSpeed * scale;
    }
    if (hasWalkAnimation && unitGroundWalkSpeed > 0.0f) {
        return unitGroundWalkSpeed * scale;
    }
    return std::nullopt;
}

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

// Minimum non-jump airborne duration (seconds) before local locomotion switches
// from ground gait to FALL, matching Application's walk-off FALLING threshold.
// Prevents brief bumpy-slope micro-ungrounding from flickering RUN -> FALL -> RUN
// (which resets Wisp.m2's Run orbit back to t=0 via Stand).
inline constexpr float kSlopeFallGraceSeconds = 0.15f;

inline constexpr bool effectiveAnimationGrounded(
    bool grounded,
    bool jumping,
    float airborneSeconds,
    bool transportRiding = false) {
    if (transportRiding || grounded) return true;
    return !jumping && airborneSeconds < kSlopeFallGraceSeconds;
}

// True when a new looping playAnimation() request resolves to the exact same
// M2 sequence index and playback direction as the sequence already playing
// under a different requested animation ID (for example, Wisp.m2 mapping
// Stand 0, ShuffleLeft 11, ShuffleRight 12, JumpStart 37, Fall 38, and
// JumpEnd 39 all onto sequence 0, or Sprint 143 onto Run 5). Preserving the
// running animation clock and skipping a self-crossfade avoids visible snaps
// and skips across locomotion/turn/jump state transitions.
inline constexpr bool preservesPlayingSequenceOnStandIn(
    uint32_t previousAnimId,
    uint32_t requestedAnimId,
    int previousSequenceIndex,
    int resolvedSequenceIndex,
    bool previousReversed,
    bool resolvedReversed,
    bool requestedLoop,
    bool frozenStandIn) {
    return requestedLoop && !frozenStandIn &&
           previousAnimId != requestedAnimId &&
           previousSequenceIndex >= 0 &&
           resolvedSequenceIndex == previousSequenceIndex &&
           previousReversed == resolvedReversed;
}

struct AnimationBlendTransitionInput {
    int previousSequenceIndex = -1;
    uint32_t previousAnimId = 0;
    float previousTime = 0.0f;
    float previousPlaybackRate = 1.0f;
    bool previousLoop = true;

    int previousBlendSequenceIndex = -1;
    uint32_t previousBlendAnimId = 0;
    float previousBlendTime = 0.0f;
    float previousBlendDuration = 0.0f;
    float previousBlendElapsed = 0.0f;
    float previousBlendPlaybackRate = 1.0f;
    bool previousBlendLoop = true;

    int newSequenceIndex = -1;
    uint32_t newAnimId = 0;
    float newBlendDuration = 0.0f;
    bool newLoop = true;
    bool frozenStandIn = false;
};

struct AnimationBlendTransition {
    bool resumePreviousBlendPose = false;
    int blendSequenceIndex = -1;
    float blendAnimationTime = 0.0f;
    float blendPlaybackRate = 1.0f;
    bool blendAnimationLoop = true;
    float blendDuration = 0.0f;
    float blendElapsed = 0.0f;
};

// Resolve the outgoing blend pose when CharacterRenderer::playAnimation()
// switches sequences. Handles three real-time blending invariants:
// 1. Same-frame round-trip (A -> B -> A before update() advances blendElapsed):
//    B was never rendered (0 ms elapsed), so cancel the unrendered transition
//    and continue A continuously from its previous timestamp (e.g. server
//    re-channeling sends MSG_CHANNEL_UPDATE 0 + MSG_CHANNEL_START in one tick).
// 2. Same-frame redirect (A -> B -> C with blendElapsed == 0): keep A as the
//    outgoing blend source instead of flashing the unrendered B.
// 3. Mid-blend reversal (A -> B for fraction p, then B -> A): preserve C0
//    continuity by reversing the blend at elapsed = newBlendDuration * (1 - p)
//    instead of snapping from (1 - p) of A back to 100% B.
inline AnimationBlendTransition resolveAnimationBlendTransition(
    const AnimationBlendTransitionInput& in) {
    AnimationBlendTransition out;
    if (in.frozenStandIn || in.previousSequenceIndex < 0 || in.newSequenceIndex < 0 ||
        (in.previousAnimId == in.newAnimId && in.newLoop)) {
        return out;
    }

    if (in.previousBlendSequenceIndex >= 0 && in.previousBlendElapsed <= 0.0f) {
        if (in.newLoop && in.previousBlendLoop && in.previousBlendAnimId == in.newAnimId) {
            out.resumePreviousBlendPose = true;
            return out;
        }
        if (in.newBlendDuration > 0.0f) {
            out.blendSequenceIndex = in.previousBlendSequenceIndex;
            out.blendAnimationTime = in.previousBlendTime;
            out.blendPlaybackRate = in.previousBlendPlaybackRate;
            out.blendAnimationLoop = in.previousBlendLoop;
            out.blendDuration = in.newBlendDuration;
            out.blendElapsed = 0.0f;
        }
        return out;
    }

    if (in.previousBlendSequenceIndex >= 0 &&
        in.previousBlendDuration > 0.0f &&
        in.previousBlendElapsed > 0.0f &&
        in.previousBlendElapsed < in.previousBlendDuration &&
        in.previousBlendAnimId == in.newAnimId) {
        if (in.newLoop && in.previousBlendLoop) {
            out.resumePreviousBlendPose = true;
        }
        if (in.newBlendDuration > 0.0f) {
            const float prevProgress = std::clamp(
                in.previousBlendElapsed / in.previousBlendDuration, 0.0f, 1.0f);
            out.blendSequenceIndex = in.previousSequenceIndex;
            out.blendAnimationTime = in.previousTime;
            out.blendPlaybackRate = in.previousPlaybackRate;
            out.blendAnimationLoop = in.previousLoop;
            out.blendDuration = in.newBlendDuration;
            out.blendElapsed = in.newBlendDuration * (1.0f - prevProgress);
        }
        return out;
    }

    if (in.newBlendDuration > 0.0f) {
        out.blendSequenceIndex = in.previousSequenceIndex;
        out.blendAnimationTime = in.previousTime;
        out.blendPlaybackRate = in.previousPlaybackRate;
        out.blendAnimationLoop = in.previousLoop;
        out.blendDuration = in.newBlendDuration;
        out.blendElapsed = 0.0f;
    }
    return out;
}

// Choose the looping animation a remote unit should resume after a one-shot
// SpellVisualKit animation completes. A hovering unit (e.g. Illidan playing
// SpellVisualKit 7668 anim 172 while in HOVER 193 before EMOTE_ONESHOT_LAND)
// must return to HOVER (193) rather than dropping to STAND (0) on the ground
// and then snapping back up to play LAND (200).
inline constexpr uint32_t resolveOneShotResumeAnimation(
    bool animationLoop,
    bool holdFinalPose,
    uint32_t currentAnimationId,
    uint32_t oneShotReturnAnim) {
    if (animationLoop) return currentAnimationId;
    if (!holdFinalPose) return oneShotReturnAnim;
    return 0;
}

} // namespace wowee::rendering
