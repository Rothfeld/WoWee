#pragma once

#include "rendering/animation/animation_ids.hpp"

#include <algorithm>
#include <cstdint>

namespace wowee::rendering {

/// True for melee swing one-shot animations (main-hand, off-hand, 2H, unarmed,
/// and their flying variants). Used to avoid cutting an in-flight swing short
/// when the other hand, an extra-attack proc, a hit reaction, or a spell
/// precast arrives mid-swing.
inline constexpr bool isMeleeSwingAnimation(uint32_t animId) {
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
inline constexpr bool shouldQueueIncomingMeleeSwing(bool playingOneShot, uint32_t currentAnimId,
                                                    float currentTimeMs, float currentDurationMs) {
    return playingOneShot &&
           isMeleeSwingAnimation(currentAnimId) &&
           currentDurationMs > 0.0f &&
           currentTimeMs < currentDurationMs;
}

/// Per-swing duration cap (ms) when two melee swings overlap inside one
/// `attackTimeMs` cycle (dual-wield MH+OH or extra-attack procs).
///
/// Empirical Native TBC 2.4.3.8606 cadence:
///   - Brutallus (`24882`, `attackTimeMs = 1000`, authored `AttackUnarmed`
///     1634/1800 ms, `AttackOff` 1834/1634 ms): overlapping MH+OH each run
///     ~350-380 ms, leaving ~240 ms to blend (`150 ms`) back into `ReadyUnarmed`
///     before the next 1000 ms MH packet — zero cumulative phase drift and
///     zero clobbered swings across hundreds of cycles.
///   - Edwin VanCleef (`639`, `attackTimeMs = 1500`, `HumanMale.m2` 1000 ms):
///     un-overlapped swings (e.g. parry-hasted 750-880 ms apart) run at full
///     authored 1000 ms speed; closely spaced MH+OH pairs (~200 ms apart) run
///     ~570 ms each and settle into `Ready1H` before the next 1500 ms cycle.
inline constexpr float kDualWieldSwingCycleFraction = 0.38f;
inline constexpr float kMinDualWieldSwingMs = 180.0f;
inline constexpr float kMinRemainingSwingWallMs = 80.0f;

inline float dualWieldSwingMaxMs(float attackTimeMs) {
    if (attackTimeMs <= 0.0f) return 0.0f;
    return std::min(attackTimeMs * 0.45f,
                    std::max(kMinDualWieldSwingMs, attackTimeMs * kDualWieldSwingCycleFraction));
}

/// Updated `oneShotMaxDurationMs` for an already-playing melee swing when a
/// follow-up melee swing arrives mid-swing and queues behind it. Accelerates
/// only the remaining `(1 - progress)` fraction so the active swing's total
/// wall time lands on `pairSwingBudgetMs` (with an 80 ms floor on remaining
/// wall time and never slowing the current playback rate).
inline float activeSwingMaxMsOnQueue(float currentTimeMs,
                                     float authoredDurationMs,
                                     float currentMaxDurationMs,
                                     float pairSwingBudgetMs) {
    if (pairSwingBudgetMs <= 0.0f || authoredDurationMs <= 0.0f ||
        currentTimeMs < 0.0f || currentTimeMs >= authoredDurationMs) {
        return currentMaxDurationMs;
    }
    const float currentTotalWallMs =
        (currentMaxDurationMs > 0.0f && currentMaxDurationMs < authoredDurationMs)
            ? currentMaxDurationMs
            : authoredDurationMs;
    const float progress = currentTimeMs / authoredDurationMs;
    const float remainFraction = 1.0f - progress;
    const float elapsedWallMs = progress * currentTotalWallMs;
    const float currentRemainWallMs = remainFraction * currentTotalWallMs;
    const float targetRemainWallMs =
        std::max(kMinRemainingSwingWallMs, pairSwingBudgetMs - elapsedWallMs);
    if (targetRemainWallMs >= currentRemainWallMs || remainFraction <= 1e-4f) {
        return currentMaxDurationMs;
    }
    return targetRemainWallMs / remainFraction;
}

/// Units actively mid-swing on a melee attack one-shot do not cut their swing
/// short (or drop a queued off-hand swing) to play a hit-reaction flinch;
/// blood spurts and wound vocals still fire.
inline constexpr bool unitSuppressesHitReactionDuringSwing(bool playingOneShot,
                                                           uint32_t activeOneShotAnim) {
    return playingOneShot && isMeleeSwingAnimation(activeOneShotAnim);
}

} // namespace wowee::rendering
