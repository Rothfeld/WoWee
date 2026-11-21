#pragma once

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/stand_pose.hpp"

#include <cstdint>

namespace wowee::rendering {

// AnimationData.dbc BodyFlags (column 3) bits observed in TBC 2.4.3.8606:
//   0x0008: upper-body-capable actions (AttackUnarmed/1H/2H/2HL/Off, CombatWound,
//           CombatCritical, Parry*, ShieldBlock, Dodge, SpellCastDirected/Omni/Area,
//           AttackBow/Rifle/Thrown, BattleRoar, upper-body emotes).
//   0x0040: moving locomotion (Walk, Run, Walkbackwards, StealthWalk/Run, Sprint,
//           Swim, SwimLeft/Right/Backwards).
//
// Models with M2 keyBoneId SpineLow (4) split an active upper-body one-shot
// above the spine when locomotion starts (e.g., a Felguard killing a mob or
// being recalled mid-swing and immediately running back: legs run while torso
// and arms finish the swing), and play new upper-body one-shots above the spine
// when already running. Models without SpineLow (quadrupeds) play full-body.
constexpr uint32_t kAnimationBodyFlagUpperBody = 0x08u;
constexpr uint32_t kAnimationBodyFlagLocomotion = 0x40u;

inline constexpr bool isUpperBodySplitAnimation(uint32_t animationId, uint32_t bodyFlags) {
    (void)animationId;
    return (bodyFlags & kAnimationBodyFlagUpperBody) != 0u;
}

inline constexpr bool isLocomotionBaseAnimation(uint32_t animationId, uint32_t bodyFlags) {
    if ((bodyFlags & kAnimationBodyFlagLocomotion) != 0u) return true;
    switch (animationId) {
        case anim::WALK:
        case anim::RUN:
        case anim::WALK_BACKWARDS:
        case anim::RUN_LEFT:
        case anim::RUN_RIGHT:
        case anim::SWIM:
        case anim::SWIM_LEFT:
        case anim::SWIM_RIGHT:
        case anim::SWIM_BACKWARDS:
        case anim::STEALTH_WALK:
        case anim::STEALTH_RUN:
        case anim::SPRINT:
        case anim::FLY_FORWARD:
            return true;
        default:
            return false;
    }
}

// Base animations over which an active non-looping upper-body action may
// continue until completion: seated/mounted poses, locomotion, and stationary
// idle/ready stances (so a swing promoted during Run finishes cleanly after
// the unit arrives and transitions Run -> Stand). Death, Stun, Knockdown,
// Sleep, Whirlwind, and full-body actions return false and clear the layer.
inline constexpr bool upperBodyAllowsBase(uint32_t baseAnimId, uint32_t baseBodyFlags, bool riding) {
    if (isSeatedBaseAnimation(baseAnimId)) return true;
    if (riding && baseAnimId == anim::MOUNT) return true;
    if (isLocomotionBaseAnimation(baseAnimId, baseBodyFlags)) return true;
    switch (baseAnimId) {
        case anim::STAND:
        case anim::READY_UNARMED:
        case anim::READY_1H:
        case anim::READY_2H:
        case anim::READY_2H_LOOSE:
        case anim::READY_BOW:
        case anim::READY_RIFLE:
        case anim::SWIM_IDLE:
        case anim::FLY_IDLE:
        case anim::STEALTH_STAND:
            return true;
        default:
            return false;
    }
}

struct UpperBodyPromoteInput {
    bool hasSpineLow = false;
    bool riding = false;
    bool previousSequenceValid = false;
    bool previousLoop = true;
    uint32_t previousAnimId = 0;
    uint32_t previousBodyFlags = 0;
    float previousTimeMs = 0.0f;
    float previousDurationMs = 0.0f;
    uint32_t incomingAnimId = 0;
    uint32_t incomingBodyFlags = 0;
    bool incomingLoop = true;
};

// True when switching the base track to a looping locomotion animation should
// preserve the in-progress full-body one-shot on the upper-body layer instead
// of cutting it short.
inline constexpr bool shouldPromoteToUpperBodyOnBaseChange(const UpperBodyPromoteInput& in) {
    if (!in.hasSpineLow || in.riding) return false;
    if (!in.previousSequenceValid || in.previousLoop) return false;
    if (in.previousDurationMs <= 0.0f || in.previousTimeMs >= in.previousDurationMs) return false;
    if (!isUpperBodySplitAnimation(in.previousAnimId, in.previousBodyFlags)) return false;
    return in.incomingLoop && isLocomotionBaseAnimation(in.incomingAnimId, in.incomingBodyFlags);
}

struct UpperBodyPlayInput {
    bool hasSpineLow = false;
    bool riding = false;
    bool currentSequenceValid = false;
    bool currentLoop = false;
    uint32_t currentAnimId = 0;
    uint32_t currentBodyFlags = 0;
    uint32_t incomingAnimId = 0;
    uint32_t incomingBodyFlags = 0;
    bool incomingLoop = false;
    bool incomingHoldFinalPose = false;
};

// True when a one-shot action arriving while the unit is already playing a
// locomotion loop should play on the upper-body layer so the legs keep moving.
inline constexpr bool shouldPlayOnUpperBodyOverLocomotion(const UpperBodyPlayInput& in) {
    if (!in.hasSpineLow || in.riding) return false;
    if (!in.currentSequenceValid || !in.currentLoop) return false;
    if (in.incomingLoop || in.incomingHoldFinalPose) return false;
    if (!isLocomotionBaseAnimation(in.currentAnimId, in.currentBodyFlags)) return false;
    return isUpperBodySplitAnimation(in.incomingAnimId, in.incomingBodyFlags);
}

} // namespace wowee::rendering
