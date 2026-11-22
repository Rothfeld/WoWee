#pragma once

/// Weapon-loadout adaptation of SpellVisualKit.dbc AnimIDs, matching Native
/// TBC 2.4.3 (build 8606).
///
/// In TBC 2.4.3, SpellVisualKit.dbc authors a canonical weapon-attack animation
/// for melee abilities (for example, Mortal Strike, Heroic Strike, Overpower,
/// Rend, Cleave, Execute, Crusader Strike, and Raptor Strike all carry
/// Special1H = 57; Slam and Concussion Blow carry Special2H = 58; Sinister
/// Strike carries Attack1H = 17). The client adapts the two generic weapon-swing
/// families to the caster's equipped main-hand weapon:
///
///   1. Special weapon-attack family (Special1H, Special2H):
///      - Unarmed (no main-hand weapon) -> SpecialUnarmed (118)
///      - Two-handed weapon             -> Special2H (58)
///      - One-handed weapon (with or without shield / off-hand) -> Special1H (57)
///
///   2. Normal weapon-attack family (Attack1H, Attack2H, Attack2HL):
///      - Unarmed or fist weapon        -> AttackUnarmed (16)
///      - Two-handed polearm / staff    -> Attack2HL (19)
///      - Two-handed sword / axe / mace -> Attack2H (18)
///      - One-handed dagger             -> Attack1HPierce (85)
///      - One-handed sword / axe / mace -> Attack1H (17)
///
/// Every other SpellVisualKit animation (ShieldBash 59, Whirlwind 126, Kick 95,
/// Mutilate 212, SpecialUnarmed 118, Attack1HPierce 85, AttackOff 87,
/// AttackOffPierce 88, DragonStomp 181, DruidCat*/DruidBear*, SpellCast*,
/// BattleRoar 55, etc.) plays as authored in the DBC.

#include <cstdint>

#include "game/inventory.hpp"
#include "rendering/animation/animation_ids.hpp"
#include "rendering/animation/weapon_type.hpp"

namespace wowee::rendering {

inline uint32_t resolveWeaponSpellAnimation(uint32_t kitAnimId, const WeaponLoadout& loadout) {
    const bool unarmed = (loadout.inventoryType == game::InvType::NON_EQUIP);
    const bool twoHand = (loadout.inventoryType == game::InvType::TWO_HAND || loadout.is2HLoose);

    switch (kitAnimId) {
        case anim::SPECIAL_1H:
        case anim::SPECIAL_2H:
            if (unarmed) return anim::SPECIAL_UNARMED;
            if (twoHand) return anim::SPECIAL_2H;
            return anim::SPECIAL_1H;

        case anim::ATTACK_1H:
        case anim::ATTACK_2H:
        case anim::ATTACK_2H_LOOSE:
            if (unarmed || loadout.isFist) return anim::ATTACK_UNARMED;
            if (loadout.is2HLoose) return anim::ATTACK_2H_LOOSE;
            if (twoHand) return anim::ATTACK_2H;
            if (loadout.isDagger) return anim::ATTACK_1H_PIERCE;
            return anim::ATTACK_1H;

        default:
            return kitAnimId;
    }
}

/// True when a SpellVisualKit animation swings melee weapons (so the local
/// character unsheathes melee weapons and puts away any shown ranged weapon).
inline bool isMeleeWeaponSpellAnimation(uint32_t animId) {
    switch (animId) {
        case anim::ATTACK_UNARMED:
        case anim::ATTACK_1H:
        case anim::ATTACK_2H:
        case anim::ATTACK_2H_LOOSE:
        case anim::SPECIAL_1H:
        case anim::SPECIAL_2H:
        case anim::SHIELD_BASH:
        case anim::ATTACK_1H_PIERCE:
        case anim::ATTACK_2H_LOOSE_PIERCE:
        case anim::ATTACK_OFF:
        case anim::ATTACK_OFF_PIERCE:
        case anim::ATTACK_UNARMED_OFF:
        case anim::SPECIAL_UNARMED:
        case anim::WHIRLWIND:
        case anim::MUTILATE:
            return true;
        default:
            return false;
    }
}

/// Attached ranged weapon M2 animation driven by the wielder's ranged aim/fire
/// animation in TBC 2.4.3:
/// - Firearm_2H_Rifle_*.m2 (37 of 39 models) carry sequence 161 (BowRelease,
///   2000 ms) driving muzzle flash, spark, and barrel smoke particle emitters.
/// - Bow_1H_*.m2 (61 of 65 models) carry sequences 160 (BowPull) and 161
///   (BowRelease) for bowstring draw and release.
/// Returning animId == 0 releases any weapon held in BowPull back to Stand,
/// while leaving a one-shot BowRelease running so gun barrel smoke finishes
/// curling after the wielder's shorter AttackRifle animation completes.
struct RangedWeaponAnimAction {
    uint32_t animId = 0;
    bool holdFinalPose = false;
};

inline RangedWeaponAnimAction rangedWeaponAnimForWielder(uint32_t wielderAnimId) {
    switch (wielderAnimId) {
        case anim::ATTACK_RIFLE:
        case anim::FLY_ATTACK_RIFLE:
        case anim::ATTACK_BOW:
        case anim::FLY_ATTACK_BOW:
            return {anim::BOW_RELEASE, false};
        case anim::LOAD_BOW:
        case anim::HOLD_BOW:
        case anim::FLY_LOAD_BOW:
        case anim::FLY_HOLD_BOW:
            return {anim::BOW_PULL, true};
        default:
            return {0, false};
    }
}

}  // namespace wowee::rendering
