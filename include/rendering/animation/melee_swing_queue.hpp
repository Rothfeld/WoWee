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
        case anim::SPECIAL_1H:
        case anim::SPECIAL_2H:
        case anim::SPECIAL_UNARMED:
        case anim::FLY_ATTACK_UNARMED:
        case anim::FLY_ATTACK_1H:
        case anim::FLY_ATTACK_2H:
        case anim::FLY_ATTACK_2HL:
        case anim::FLY_ATTACK_1H_PIERCE:
        case anim::FLY_ATTACK_2H_LOOSE_PIERCE:
        case anim::FLY_ATTACK_OFF:
        case anim::FLY_ATTACK_OFF_PIERCE:
        case anim::FLY_ATTACK_UNARMED_OFF:
        case anim::FLY_SPECIAL_1H:
        case anim::FLY_SPECIAL_2H:
        case anim::FLY_SPECIAL_UNARMED:
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

/// True for spell-cast and dragon/creature ability one-shot animations that
/// must finish their authored clip before a queued auto-attack melee swing runs
/// (e.g. UBRS Gyth SpellCastDirected 53 left-to-right breath sweep when a 2000 ms
/// melee swing arrives 400 ms mid-breath).
inline constexpr bool isSpellCastOneShotAnimation(uint32_t animId) {
    switch (animId) {
        case anim::SPELL:
        case anim::SPELL_CAST:
        case anim::SPELL_CAST_AREA:
        case anim::SPELL_CAST_DIRECTED:
        case anim::SPELL_CAST_OMNI:
        case anim::BATTLE_ROAR:
        case anim::CREATURE_SPECIAL:
        case anim::DRAGON_TAIL:
        case anim::DRAGON_STOMP:
        case anim::DRAGON_SPIT:
        case anim::DRAGON_SPIT_HOVER:
        case anim::DRAGON_SPIT_FLY:
        case anim::FLY_SPELL:
        case anim::FLY_SPELL_CAST:
        case anim::FLY_SPELL_CAST_AREA:
        case anim::FLY_SPELL_CAST_DIRECTED:
        case anim::FLY_SPELL_CAST_OMNI:
        case anim::FLY_BATTLE_ROAR:
            return true;
        default:
            return false;
    }
}

/// Queue an incoming melee swing rather than interrupting when the unit is
/// already mid-swing on a melee one-shot or mid-cast on a spell-cast one-shot.
/// Non-swing poses (Ready1H, Stand, Run, CombatWound, etc.) or an already-finished
/// one-shot start immediately.
inline constexpr bool shouldQueueIncomingMeleeSwing(bool playingOneShot, uint32_t currentAnimId,
                                                    float currentTimeMs, float currentDurationMs) {
    return playingOneShot &&
           (isMeleeSwingAnimation(currentAnimId) || isSpellCastOneShotAnimation(currentAnimId)) &&
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

/// True for active defensive combat reaction one-shots (`Dodge`, weapon-grip
/// `Parry*`, `ShieldBlock`, `Block`, `Deflect`, and their flying variants).
/// Unlike passive wound flinches (`StandWound 8`, `CombatWound 9`,
/// `CombatCritical 10`), active defensive reactions interrupt an in-progress
/// melee swing one-shot on Native TBC 2.4.3.
inline constexpr bool isActiveDefenseReactionAnimation(uint32_t animId) {
    switch (animId) {
        case anim::PARRY_UNARMED:
        case anim::PARRY_1H:
        case anim::PARRY_2H:
        case anim::PARRY_2H_LOOSE:
        case anim::SHIELD_BLOCK:
        case anim::DODGE:
        case anim::BLOCK:
        case anim::DEFLECT:
        case anim::FLY_PARRY_UNARMED:
        case anim::FLY_PARRY_1H:
        case anim::FLY_PARRY_2H:
        case anim::FLY_PARRY_2HL:
        case anim::FLY_SHIELD_BLOCK:
        case anim::FLY_DODGE:
        case anim::FLY_BLOCK:
            return true;
        default:
            return false;
    }
}

/// True for non-looping melee attack or melee special one-shot animations
/// (auto-attack swings, crit/ability `Special*`, `ShieldBash`, `Kick`,
/// `Mutilate`, and feral swipe/bite/maul strikes).
inline constexpr bool isMeleeAttackOneShotAnimation(uint32_t animId) {
    if (isMeleeSwingAnimation(animId)) return true;
    switch (animId) {
        case anim::SPECIAL_1H:
        case anim::SPECIAL_2H:
        case anim::SHIELD_BASH:
        case anim::KICK:
        case anim::SPECIAL_UNARMED:
        case anim::DRUID_CAT_POUNCE:
        case anim::DRUID_CAT_RIP:
        case anim::DRUID_CAT_RAKE:
        case anim::DRUID_CAT_RAVAGE:
        case anim::DRUID_CAT_CLAW:
        case anim::DRUID_BEAR_SWIPE:
        case anim::DRUID_BEAR_BITE:
        case anim::DRUID_BEAR_MAUL:
        case anim::DRUID_BEAR_BASH:
        case anim::MUTILATE:
        case anim::FLY_SPECIAL_1H:
        case anim::FLY_SPECIAL_2H:
        case anim::FLY_SPECIAL_UNARMED:
            return true;
        default:
            return false;
    }
}

/// Units actively mid-knockdown suppress all hit reactions; units mid-swing on
/// a melee attack/special one-shot suppress wound flinches (`StandWound`,
/// `CombatWound`, `CombatCritical`) while allowing active defensive reactions
/// (`Dodge`, `Parry*`, `ShieldBlock`) to interrupt the swing.
inline constexpr bool unitSuppressesHitReactionDuringSwing(
    bool playingOneShot,
    uint32_t activeOneShotAnim,
    uint32_t reactionAnimId = anim::COMBAT_WOUND) {
    if (!playingOneShot) return false;
    if (activeOneShotAnim == anim::KNOCKDOWN) return true;
    if (isMeleeAttackOneShotAnimation(activeOneShotAnim)) {
        return !isActiveDefenseReactionAnimation(reactionAnimId);
    }
    return false;
}

/// Looping animation a remote unit resumes after a hit-reaction one-shot ends.
///
/// Empirical Native TBC 2.4.3 rule:
///   - Moving, hovering, cast-holding, aura-overridden, or out-of-combat units
///     (`!isCombatReadyIdle`) always resume `spawnerResumeAnim`.
///   - Stationary combat-ready units whose looping `Ready*` stance is active
///     between swings resume `Ready*` (`spawnerResumeAnim`) after `Dodge`,
///     `Parry*`, `ShieldBlock`, or `CombatWound`.
///   - Stationary combat-ready units whose non-looping melee swing (`Attack*` /
///     `Special*`) is interrupted mid-swing by an active defensive reaction
///     (`Dodge`, `Parry*`, `ShieldBlock`) transition to `Stand (0)` when that
///     reaction finishes, and stay in `Stand (0)` across any follow-up reaction
///     until their next melee swing restores `Attack* -> Ready*`.
inline constexpr uint32_t remoteHitReactionResumeAnimation(
    bool playingOneShot,
    uint32_t activeOneShotAnim,
    uint32_t reactionAnimId,
    uint32_t instanceResumeAnim,
    uint32_t spawnerResumeAnim,
    bool isCombatReadyIdle) {
    if (!isCombatReadyIdle) return spawnerResumeAnim;
    if (playingOneShot &&
        isMeleeAttackOneShotAnimation(activeOneShotAnim) &&
        isActiveDefenseReactionAnimation(reactionAnimId)) {
        return anim::STAND;
    }
    if (instanceResumeAnim == anim::STAND &&
        (!playingOneShot || !isMeleeAttackOneShotAnimation(activeOneShotAnim))) {
        return anim::STAND;
    }
    return spawnerResumeAnim;
}

} // namespace wowee::rendering
