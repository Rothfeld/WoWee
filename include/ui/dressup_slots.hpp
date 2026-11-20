#pragma once

// Dressing room (DressUpModel:TryOn) equipment overlay state machine.
//
// Measured against native TBC 2.4.3 (build 8606, pair 19,
// _experiments/20261119T1923Z.dressup_hand_cursor/):
//
// - Non-dual-wield character (no Dual Wield passive spell 674 / 30798):
//   - One-Hand (13) or Main-Hand (21): goes to Main Hand; clears Off Hand if
//     Off Hand holds a two-handed/ranged weapon or an Off-Hand weapon (22),
//     keeping Shield (14) or Holdable (23).
//   - Off-Hand weapon (22): goes to Off Hand and clears Main Hand.
// - Dual-wield character (knows Dual Wield spell 674 or 30798):
//   - One-Hand (13): if Main Hand already holds a 1H (13) or Main-Hand (21)
//     weapon (and neither hand holds a both-hands weapon), goes to Off Hand
//     (replacing whatever was in Off Hand, including a shield); otherwise goes
//     to Main Hand (clearing any both-hands weapon in Off Hand).
//   - Main-Hand (21): goes to Main Hand; clears any both-hands weapon in Off
//     Hand, keeping Off-Hand weapons/shields/holdables.
//   - Off-Hand weapon (22): goes to Off Hand; clears any both-hands weapon in
//     Main Hand, keeping 1H/MH weapons in Main Hand.
// - For both:
//   - Two-Hand (17), Thrown (25), Gun/Wand/Crossbow (26): Main Hand, clears OH.
//   - Bow (15): Off Hand (left hand), clears MH.
//   - Shield (14), Holdable (23): Off Hand, clears any both-hands weapon in MH.
//   - Armor slots overwrite their corresponding EquipSlot (chest & robe share
//     CHEST). Non-visual slots (neck, finger, trinket, bag, ammo) are ignored.

#include "game/inventory.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wowee::ui {

inline constexpr bool dressUpIsBothHands(uint8_t invType) {
    namespace IT = game::InvType;
    return invType == IT::TWO_HAND || invType == IT::RANGED_BOW ||
           invType == IT::THROWN || invType == IT::RANGED_GUN;
}

inline void applyDressUpTryOn(std::array<uint32_t, 19>& displayIds,
                              std::array<uint8_t, 19>& invTypes,
                              uint32_t displayInfoId,
                              uint8_t invType,
                              bool canDualWield) {
    if (displayInfoId == 0 || invType == 0) return;

    namespace IT = game::InvType;
    const auto at = [](game::EquipSlot slot) { return static_cast<size_t>(slot); };
    const size_t right = at(game::EquipSlot::MAIN_HAND);
    const size_t left = at(game::EquipSlot::OFF_HAND);

    const auto clear = [&](size_t slot) {
        displayIds[slot] = 0;
        invTypes[slot] = 0;
    };

    size_t slot = 0;
    switch (invType) {
        case IT::ONE_HAND:
            if (canDualWield && displayIds[right] != 0 &&
                !dressUpIsBothHands(invTypes[right]) &&
                !dressUpIsBothHands(invTypes[left])) {
                slot = left;
            } else {
                slot = right;
                if (dressUpIsBothHands(invTypes[left]) ||
                    (!canDualWield && invTypes[left] == IT::OFF_HAND)) {
                    clear(left);
                }
            }
            break;
        case IT::MAIN_HAND:
            slot = right;
            if (dressUpIsBothHands(invTypes[left]) ||
                (!canDualWield && invTypes[left] == IT::OFF_HAND)) {
                clear(left);
            }
            break;
        case IT::TWO_HAND:
        case IT::THROWN:
        case IT::RANGED_GUN:
            slot = right;
            clear(left);
            break;
        case IT::RANGED_BOW:
            slot = left;
            clear(right);
            break;
        case IT::OFF_HAND:
            slot = left;
            if (!canDualWield || dressUpIsBothHands(invTypes[right])) {
                clear(right);
            }
            break;
        case IT::SHIELD:
        case IT::HOLDABLE:
            slot = left;
            if (dressUpIsBothHands(invTypes[right])) clear(right);
            break;
        case IT::HEAD:      slot = at(game::EquipSlot::HEAD); break;
        case IT::SHOULDERS: slot = at(game::EquipSlot::SHOULDERS); break;
        case IT::SHIRT:     slot = at(game::EquipSlot::SHIRT); break;
        case IT::CHEST:
        case IT::ROBE:      slot = at(game::EquipSlot::CHEST); break;
        case IT::WAIST:     slot = at(game::EquipSlot::WAIST); break;
        case IT::LEGS:      slot = at(game::EquipSlot::LEGS); break;
        case IT::FEET:      slot = at(game::EquipSlot::FEET); break;
        case IT::WRISTS:    slot = at(game::EquipSlot::WRISTS); break;
        case IT::HANDS:     slot = at(game::EquipSlot::HANDS); break;
        case IT::BACK:      slot = at(game::EquipSlot::BACK); break;
        case IT::TABARD:    slot = at(game::EquipSlot::TABARD); break;
        default:            return;
    }

    displayIds[slot] = displayInfoId;
    invTypes[slot] = invType;
}

} // namespace wowee::ui
