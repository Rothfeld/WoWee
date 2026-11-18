#pragma once

#include "rendering/animation/animation_ids.hpp"

#include <cstdint>

namespace wowee::rendering {

/// CreatureTypeFlags (SMSG_CREATURE_QUERY_RESPONSE type flags) bit 0x8,
/// CMaNGOS CreatureTypeFlags::DO_NOT_PLAY_WOUND_ANIM.
inline constexpr uint32_t kCreatureTypeDoNotPlayWoundAnim = 0x8;

/// Whether a melee hit plays the victim's wound animation (CombatWound 9 or
/// CombatCritical 10). A creature whose type flags carry 0x8 takes hits
/// without flinching; dodge, parry and block still play. Native 2.4.3: Qiraji
/// Gladiator (15324, flags 0x8), Amani'shi Warbringer (23580, 0x848) and Hex
/// Lord Malacrass (24239, 0x4c) never flinch; Defias Thug (38, 0) does
/// (_experiments/20261118T1540Z.zulaman_warbringer).
inline constexpr bool unitPlaysWound(uint32_t reactionAnimation, bool isCreature, uint32_t creatureTypeFlags) {
    const bool wound = reactionAnimation == anim::COMBAT_WOUND || reactionAnimation == anim::COMBAT_CRITICAL;
    return !(wound && isCreature && (creatureTypeFlags & kCreatureTypeDoNotPlayWoundAnim));
}

} // namespace wowee::rendering
