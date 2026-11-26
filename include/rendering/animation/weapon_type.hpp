#pragma once

#include <cstdint>

#include "game/inventory.hpp"
#include "rendering/animation/animation_ids.hpp"

namespace wowee {
namespace rendering {

/// Ranged weapon type for animation selection.
///
/// A wand is separate from a gun: INVTYPE_RANGEDRIGHT carries guns, crossbows
/// and wands alike, so the inventory type alone gives a wand the rifle pose.
enum class RangedWeaponType : uint8_t { NONE = 0, BOW, GUN, CROSSBOW, THROWN, WAND };

/// Where a hand's item is put away, which decides that arm's sheathe reach.
enum class SheathSpot : uint8_t { NONE = 0, HIP, BACK };

// ============================================================================
// WeaponLoadout - extracted from AnimationController
//
// Consolidates the weapon grip flags + inventory type + ranged type
// into a single value type shared across local and remote units.
// ============================================================================
struct WeaponLoadout {
    uint32_t inventoryType = 0;
    bool is2HLoose  = false;  // Polearm, staff, spear, or fishing pole (ItemSubClass.dbc f6 == 1)
    bool isFist     = false;  // Fist weapon
    bool isDagger   = false;  // Dagger (uses pierce variants)
    bool hasOffHand = false;  // Has off-hand weapon (dual wield)
    // The off-hand weapon's own kind. isFist and isDagger above are the main
    // hand's, and the off-hand swing was picked from those - so a sword and a
    // dagger swung the dagger's pierce animation with the sword, and the
    // sword's with the dagger, whichever way round they were equipped.
    bool offHandIsFist   = false;
    bool offHandIsDagger = false;
    bool hasShield  = false;  // Has shield equipped (for SHIELD_BASH)
    RangedWeaponType rangedType = RangedWeaponType::NONE;
};

/// Populate the main-hand melee fields of `loadout` from an item's class,
/// subclass, and inventory type per TBC 2.4.3 ItemSubClass.dbc (Class == 2):
///   - 2H Loose (f6 == 1, f9 == 2): Polearm (6), Staff (10), Spear (17),
///     Fishing Pole (20) -> Ready2HL (28), Attack2HL (19), Parry2HL (23).
///   - 2H Tight (f9 == 2, f6 != 1): 2H Axe (1), 2H Mace (5), 2H Sword (8),
///     2H Exotic (12) -> Ready2H (27), Attack2H (18), Parry2H (22), including
///     monster virtual items authored with INVTYPE_WEAPON (13).
///   - Fist (13): ReadyUnarmed (25), AttackUnarmed (16), ParryUnarmed (20).
///   - Dagger (15): Ready1H (26), Attack1HPierce (85), Parry1H (21).
///   - 1H / Miscellaneous (0, 4, 7, 9, 11, 14): Ready1H (26), Attack1H (17),
///     Parry1H (21), even when Miscellaneous (14) carries INVTYPE_2HWEAPON (17)
///     (for example Mallet of Zul'Farrak 9239).
inline constexpr void applyMainHandWeaponSubclass(WeaponLoadout& loadout,
                                                  uint32_t itemClass,
                                                  uint32_t subClass,
                                                  uint8_t inventoryType) {
    loadout.inventoryType = game::InvType::NON_EQUIP;
    loadout.is2HLoose = false;
    loadout.isFist = false;
    loadout.isDagger = false;
    if (itemClass != game::ITEM_CLASS_WEAPON) {
        return;
    }
    switch (subClass) {
        case game::WEAPON_SUBCLASS_POLEARM:
        case game::WEAPON_SUBCLASS_STAFF:
        case game::WEAPON_SUBCLASS_SPEAR:
        case game::WEAPON_SUBCLASS_FISHING_POLE:
            loadout.inventoryType = game::InvType::TWO_HAND;
            loadout.is2HLoose = true;
            return;
        case game::WEAPON_SUBCLASS_AXE_2H:
        case game::WEAPON_SUBCLASS_MACE_2H:
        case game::WEAPON_SUBCLASS_SWORD_2H:
        case game::WEAPON_SUBCLASS_EXOTIC_2H:
            loadout.inventoryType = game::InvType::TWO_HAND;
            return;
        case game::WEAPON_SUBCLASS_FIST:
            loadout.inventoryType = game::InvType::ONE_HAND;
            loadout.isFist = true;
            return;
        case game::WEAPON_SUBCLASS_DAGGER:
            loadout.inventoryType = game::InvType::ONE_HAND;
            loadout.isDagger = true;
            return;
        default:
            if (subClass <= game::WEAPON_SUBCLASS_FISHING_POLE) {
                loadout.inventoryType = game::InvType::ONE_HAND;
            } else {
                loadout.inventoryType = (inventoryType == game::InvType::TWO_HAND)
                    ? game::InvType::TWO_HAND
                    : game::InvType::ONE_HAND;
            }
            return;
    }
}

/// Populate the off-hand melee and shield fields of `loadout` from an item's
/// class, subclass, and inventory type.
inline constexpr void applyOffHandWeaponSubclass(WeaponLoadout& loadout,
                                                 uint32_t itemClass,
                                                 uint32_t subClass,
                                                 uint8_t inventoryType) {
    loadout.hasShield = (inventoryType == game::InvType::SHIELD);
    loadout.hasOffHand = game::isOffHandWeaponInventoryType(inventoryType);
    loadout.offHandIsFist = loadout.hasOffHand &&
        game::isWeaponSubclass(itemClass, subClass, game::WEAPON_SUBCLASS_FIST);
    loadout.offHandIsDagger = loadout.hasOffHand &&
        game::isWeaponSubclass(itemClass, subClass, game::WEAPON_SUBCLASS_DAGGER);
}

/// Canonical main-hand normal melee swing animation for `loadout`.
inline constexpr uint32_t meleeNormalAttackAnimation(const WeaponLoadout& loadout) {
    if (loadout.inventoryType == game::InvType::NON_EQUIP || loadout.isFist) {
        return anim::ATTACK_UNARMED;
    }
    if (loadout.is2HLoose) return anim::ATTACK_2H_LOOSE;
    if (loadout.inventoryType == game::InvType::TWO_HAND) return anim::ATTACK_2H;
    if (loadout.isDagger) return anim::ATTACK_1H_PIERCE;
    return anim::ATTACK_1H;
}

/// Canonical main-hand critical auto-attack animation (`HITINFO_CRITICALHIT`
/// on `SMSG_ATTACKERSTATEUPDATE`) in TBC 2.4.3:
///   - Daggers keep Attack1HPierce (85).
///   - Unarmed and Fist weapons play SpecialUnarmed (118).
///   - Two-handed weapons (both 2H and 2HL, including Fishing Poles, Spears,
///     Polearms, and Staves) play Special2H (58).
///   - One-handed non-dagger weapons (including Miscellaneous) play Special1H (57).
inline constexpr uint32_t meleeCriticalAttackAnimation(const WeaponLoadout& loadout) {
    if (loadout.isDagger) return anim::ATTACK_1H_PIERCE;
    if (loadout.inventoryType == game::InvType::NON_EQUIP || loadout.isFist) {
        return anim::SPECIAL_UNARMED;
    }
    if (loadout.is2HLoose || loadout.inventoryType == game::InvType::TWO_HAND) {
        return anim::SPECIAL_2H;
    }
    return anim::SPECIAL_1H;
}

} // namespace rendering
} // namespace wowee
