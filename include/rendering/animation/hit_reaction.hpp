#pragma once

#include "rendering/animation/animation_ids.hpp"

#include <cstdint>

namespace wowee::rendering {

/// CreatureTypeFlags (SMSG_CREATURE_QUERY_RESPONSE type flags) bit 0x8,
/// CMaNGOS CreatureTypeFlags::DO_NOT_PLAY_WOUND_ANIM.
inline constexpr uint32_t kCreatureTypeDoNotPlayWoundAnim = 0x8;

/// CreatureModelData.Flags bit 0x2: the model takes hits without flinching.
/// Set on Lich, Skeleton, Ghoul, Ghost, golems, elementals and totems; clear
/// on character models (0x4) and most beasts.
inline constexpr uint32_t kCreatureModelNoWound = 0x2;

/// Whether a melee hit plays the victim's wound animation (CombatWound 9 or
/// CombatCritical 10); dodge, parry and block are not affected. Native 2.4.3:
/// - a creature whose type flags carry 0x8 never flinches: Qiraji Gladiator
///   (15324, flags 0x8), Amani'shi Warbringer (23580, 0x848), Hex Lord
///   Malacrass (24239, 0x4c); Defias Thug (38, 0) does
///   (_experiments/20261118T1540Z.zulaman_warbringer);
/// - a unit whose display's model carries CreatureModelData flag 0x2 never
///   flinches, whatever its entry: Ras Frostwhisper (10508, Lich.mdx 0x3)
///   stood still through every hit and flinched once given HumanMale.mdx
///   (0x4); Defias Thug given Lich.mdx, Skeleton.mdx (0x2) or Ghoul.mdx (0x2)
///   stopped flinching, given Gnoll.mdx (0x0) kept it
///   (_experiments/20261119T1311Z.wound_rule).
inline constexpr bool unitPlaysWound(uint32_t reactionAnimation, bool isCreature, uint32_t creatureTypeFlags,
                                     uint32_t creatureModelFlags) {
    const bool wound = reactionAnimation == anim::STAND_WOUND ||
                       reactionAnimation == anim::COMBAT_WOUND ||
                       reactionAnimation == anim::COMBAT_CRITICAL;
    if (!wound) return true;
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
/// Observed on Native TBC 2.4.3 (Defias Miner under Hammer of Justice 10308).
inline constexpr bool unitSuppressesHitReactionWhileStunned(uint32_t unitFlags) {
    constexpr uint32_t kUnitFlagStunned = 0x00040000u;
    return (unitFlags & kUnitFlagStunned) != 0;
}

} // namespace wowee::rendering
