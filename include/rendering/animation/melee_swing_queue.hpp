#pragma once

#include "rendering/animation/animation_ids.hpp"

#include <algorithm>
#include <cstdint>

namespace wowee::rendering {

/// True for melee swing one-shot animations (main-hand, off-hand, 2H, unarmed,
/// and their flying variants). Used to avoid cutting an in-flight swing short
/// when the other hand or an extra-attack proc arrives mid-swing.
inline bool isMeleeSwingAnimation(uint32_t animId) {
    switch (animId) {
        case anim::ATTACK_UNARMED:
        case anim::ATTACK_1H:
        case anim::ATTACK_2H:
        case anim::ATTACK_2H_LOOSE:
        case anim::ATTACK_1H_PIERCE:
        case anim::ATTACK_2H_LOOSE_PIERCE:
        case anim::ATTACK_OFF:
        case anim::ATTACK_OFF_PIERCE:
        case anim::ATTACK_UNARMED_OFF:
        case anim::FLY_ATTACK_UNARMED:
        case anim::FLY_ATTACK_1H:
        case anim::FLY_ATTACK_2H:
        case anim::FLY_ATTACK_2HL:
        case anim::FLY_ATTACK_1H_PIERCE:
        case anim::FLY_ATTACK_2H_LOOSE_PIERCE:
        case anim::FLY_ATTACK_OFF:
        case anim::FLY_ATTACK_OFF_PIERCE:
        case anim::FLY_ATTACK_UNARMED_OFF:
            return true;
        default:
            return false;
    }
}

/// One-slot pending melee swing queued behind an in-flight melee swing so
/// closely spaced dual-wield or Thrash packets transition smoothly on
/// completion instead of cutting the active swing short or building a backlog.
struct QueuedMeleeSwing {
    uint32_t animId = 0;
    uint32_t returnAnim = 0;
    float maxDurationMs = 0.0f;

    [[nodiscard]] bool valid() const { return animId != 0; }
    void clear() {
        animId = 0;
        returnAnim = 0;
        maxDurationMs = 0.0f;
    }
};

/// Queue an incoming melee swing rather than interrupting when the unit is
/// already mid-swing on a melee one-shot. Non-swing poses (Ready1H, Stand,
/// Run, CombatWound, etc.) or an already-finished swing start immediately.
inline bool shouldQueueIncomingMeleeSwing(bool playingOneShot, uint32_t currentAnimId,
                                          float currentTimeMs, float currentDurationMs) {
    return playingOneShot &&
           isMeleeSwingAnimation(currentAnimId) &&
           currentDurationMs > 0.0f &&
           currentTimeMs < currentDurationMs;
}

/// Effective one-shot duration cap (ms) when a dual-wield swing arrives while
/// the other hand is still swinging. Two hands sharing a cadence of
/// `attackTimeMs` each have `attackTimeMs * 0.5` per swing so back-to-back
/// main/off pairs finish inside one cycle without drifting behind; slow
/// dual-wielders (`attackTimeMs * 0.5 >= authoredDuration`) still run at
/// authored speed because `fitOneShotDuration` only speeds up, never slows.
inline float dualWieldEffectiveSwingMaxMs(uint32_t activeAnimId, uint32_t incomingAnimId,
                                          float attackTimeMs) {
    if (attackTimeMs <= 0.0f) return 0.0f;
    if (activeAnimId != incomingAnimId) {
        return std::max(250.0f, attackTimeMs * 0.5f);
    }
    return attackTimeMs;
}

} // namespace wowee::rendering
