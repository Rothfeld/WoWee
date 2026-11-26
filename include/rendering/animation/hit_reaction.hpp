#pragma once

#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/melee_swing_queue.hpp"
#include "rendering/spell_visual_kit_policy.hpp"

#include <cstdint>

namespace wowee::rendering {

/// CreatureTypeFlags (SMSG_CREATURE_QUERY_RESPONSE type flags) bit 0x8,
/// CMaNGOS CreatureTypeFlags::DO_NOT_PLAY_WOUND_ANIM.
inline constexpr uint32_t kCreatureTypeDoNotPlayWoundAnim = 0x8;

/// CreatureModelData.Flags bit 0x2: the model takes hits without flinching.
/// Set on Lich, Skeleton, Ghoul, Ghost, golems, elementals and totems; clear
/// on character models (0x4) and most beasts.
inline constexpr uint32_t kCreatureModelNoWound = 0x2;

/// True when `animationId` is one of the three wound hit-flinch sequences
/// (StandWound 8, CombatWound 9, CombatCritical 10).
inline constexpr bool isWoundAnimation(uint32_t animationId) {
    return animationId == anim::STAND_WOUND ||
           animationId == anim::COMBAT_WOUND ||
           animationId == anim::COMBAT_CRITICAL;
}

/// Whether a melee hit, spell hit, spell ImpactKit, or server wound emote
/// plays the victim's wound animation (StandWound 8, CombatWound 9, or
/// CombatCritical 10); dodge, parry, block, and non-wound emotes are not
/// affected. Native 2.4.3:
/// - a creature whose type flags carry 0x8 never flinches: Warchief Rend
///   Blackhand (10429, flags 0x8), Qiraji Gladiator (15324, flags 0x8),
///   Amani'shi Warbringer (23580, 0x848), Hex Lord Malacrass (24239, 0x4c);
///   Defias Thug (38, 0) and Blackhand Veteran (9819, 0) do
///   (_experiments/20261118T1540Z.zulaman_warbringer,
///    _experiments/20261126T204041Z.enemy_crit_hitflinch_parity);
/// - a unit whose display's model carries CreatureModelData flag 0x2 never
///   flinches, whatever its entry: Ras Frostwhisper (10508, Lich.mdx 0x3)
///   stood still through every hit and flinched once given HumanMale.mdx
///   (0x4); Defias Thug given Lich.mdx, Skeleton.mdx (0x2) or Ghoul.mdx (0x2)
///   stopped flinching, given Gnoll.mdx (0x0) kept it
///   (_experiments/20261119T1311Z.wound_rule).
inline constexpr bool unitPlaysWound(uint32_t reactionAnimation, bool isCreature, uint32_t creatureTypeFlags,
                                     uint32_t creatureModelFlags) {
    if (!isWoundAnimation(reactionAnimation)) return true;
    if (creatureModelFlags & kCreatureModelNoWound) return false;
    return !(isCreature && (creatureTypeFlags & kCreatureTypeDoNotPlayWoundAnim));
}

/// True when `idleAnimation` is a melee or ranged combat-ready stance.
/// Units in a combat-ready stance react to normal hits with CombatWound (9);
/// units standing upright out of combat stance (weapons sheathed, not attacking)
/// react with StandWound (8) when authored on the model.
inline constexpr bool isCombatReadyStance(uint32_t idleAnimation) {
    return idleAnimation == anim::READY_UNARMED ||
           idleAnimation == anim::READY_1H ||
           idleAnimation == anim::READY_2H ||
           idleAnimation == anim::READY_2H_LOOSE ||
           idleAnimation == anim::READY_BOW ||
           idleAnimation == anim::READY_RIFLE ||
           idleAnimation == anim::READY_THROWN ||
           idleAnimation == anim::READY_ABILITY ||
           idleAnimation == anim::HOLD_BOW ||
           idleAnimation == anim::HOLD_RIFLE ||
           idleAnimation == anim::HOLD_THROWN;
}

/// Select the wound hit-reaction animation for a non-critical incoming hit.
/// Observed on Native TBC 2.4.3 (BloodElfMale 15476 hit by Avian Ripper 21891):
/// standing idle (not auto-attacking) plays StandWound (8), whereas active
/// combat-ready stance plays CombatWound (9). Models lacking StandWound (8)
/// fall back to CombatWound (9).
inline constexpr uint32_t woundAnimationForStance(bool inCombatReadyStance, bool hasStandWound) {
    if (!inCombatReadyStance && hasStandWound) return anim::STAND_WOUND;
    return anim::COMBAT_WOUND;
}

/// Stunned units (UNIT_FLAG_STUNNED = 0x00040000) hold their Stun (14) pose
/// through incoming melee hits without playing wound/dodge/parry reactions.
/// Observed on Native TBC 2.4.3 (Defias Miner and Blackhand Veteran under
/// Hammer of Justice 10308).
inline constexpr bool unitSuppressesHitReactionWhileStunned(uint32_t unitFlags) {
    constexpr uint32_t kUnitFlagStunned = 0x00040000u;
    return (unitFlags & kUnitFlagStunned) != 0;
}

/// True when an active spell cast release one-shot (e.g. Cleave / Mortal
/// Strike Special2H 58, Whirlwind 126, SpellCastDirected 53) or an active
/// non-Stand, non-Stun aura StateKit animation (e.g. Rend Blackhand Whirlwind
/// 13736 kit 370 -> Whirlwind 126) suppresses a hit-reaction flinch.
/// Note: held precast/channel loops are NOT suppressed here for direct
/// melee/spell HitReaction callbacks, which play the wound once and resume
/// the precast/channel hold (20261120T230315Z.wrath_flinch_precast_parity).
inline constexpr bool unitSuppressesHitReactionDuringCastOrAura(
    bool hasActiveCastOneShot,
    uint32_t activeAuraStateAnim,
    uint32_t noAnimationSentinel) {
    if (hasActiveCastOneShot) return true;
    return auraStateSuppressesOneShotKitAnim(
        activeAuraStateAnim, noAnimationSentinel, anim::STAND, anim::STUN);
}

/// True when a server one-shot wound emote (SMSG_EMOTE EMOTE_ONESHOT_WOUND = 33
/// -> CombatWound 9, or EMOTE_ONESHOT_WOUNDCRITICAL = 34 -> CombatCritical 10,
/// which CMaNGOS Unit::DealMeleeDamage sends on every melee critical hit) must
/// be suppressed on a remote unit.
///
/// Empirical Native TBC 2.4.3 behavior (_experiments/20261126T204041Z.enemy_crit_hitflinch_parity):
/// - Non-wound emotes (e.g. ONESHOT_ROAR 15) and persistent STATE_ emotes are never suppressed here.
/// - CreatureTypeFlags 0x8 (DO_NOT_PLAY_WOUND_ANIM, e.g. Warchief Rend Blackhand 10429)
///   and CreatureModelData.Flags 0x2 (Ras Frostwhisper 10508, Skeleton, Ghoul, golems,
///   elementals, totems) suppress wound flinches on melee crits.
/// - Stunned units (UNIT_FLAG_STUNNED) keep their stun pose through SMSG_EMOTE 34.
/// - Units mid-melee-swing / Knockdown, holding an active spell precast/channel
///   (e.g. ReadySpellOmni 52), playing a spell cast release one-shot, or spinning
///   in an aura StateKit (e.g. Whirlwind 13736 -> 126) keep their active animation
///   without breaking into CombatCritical.
inline constexpr bool unitSuppressesWoundEmote(
    uint32_t emoteAnim,
    bool isState,
    bool isCreature,
    uint32_t creatureTypeFlags,
    uint32_t creatureModelFlags,
    uint32_t unitFlags,
    bool playingOneShot,
    uint32_t activeOneShotAnim,
    bool hasActiveCastHold,
    bool hasActiveCastOneShot,
    uint32_t activeAuraStateAnim,
    uint32_t noAnimationSentinel) {
    if (isState || !isWoundAnimation(emoteAnim)) return false;
    if (!unitPlaysWound(emoteAnim, isCreature, creatureTypeFlags, creatureModelFlags)) return true;
    if (unitSuppressesHitReactionWhileStunned(unitFlags)) return true;
    if (unitSuppressesHitReactionDuringSwing(playingOneShot, activeOneShotAnim)) return true;
    if (hasActiveCastHold) return true;
    return unitSuppressesHitReactionDuringCastOrAura(
        hasActiveCastOneShot, activeAuraStateAnim, noAnimationSentinel);
}

} // namespace wowee::rendering
