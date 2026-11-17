#pragma once

#include "game/inventory.hpp"
#include <cstdint>

namespace wowee::core {

// M2 attachment ids (wowdev.wiki/M2#Attachment_IDs).
constexpr uint32_t kAttachShield = 0;
constexpr uint32_t kAttachRightHand = 1;
constexpr uint32_t kAttachLeftHand = 2;
constexpr uint32_t kAttachSheathMainHand = 26;
constexpr uint32_t kAttachSheathOffHand = 27;
constexpr uint32_t kAttachSheathShield = 28;
constexpr uint32_t kAttachLargeWeaponLeft = 30;
constexpr uint32_t kAttachLargeWeaponRight = 31;
constexpr uint32_t kAttachHipWeaponLeft = 32;
constexpr uint32_t kAttachHipWeaponRight = 33;

// Unit sheath state, UNIT_FIELD_BYTES_2 byte 0.
constexpr uint8_t kSheathStateUnarmed = 0;
constexpr uint8_t kSheathStateMelee = 1;
constexpr uint8_t kSheathStateRanged = 2;

// Where a weapon is held while its slot is drawn. A bow (INVTYPE_RANGED) is
// held in the left hand, a gun (INVTYPE_RANGEDRIGHT) in the right (build 8606,
// same experiment).
inline uint32_t heldAttachment(game::EquipSlot slot, uint8_t inventoryType) {
    if (inventoryType == game::InvType::SHIELD) return kAttachShield;
    if (inventoryType == game::InvType::RANGED_BOW) return kAttachLeftHand;
    return slot == game::EquipSlot::OFF_HAND ? kAttachLeftHand : kAttachRightHand;
}

// The unit sheath state in which a slot's item is in hand.
inline uint8_t heldSheathState(game::EquipSlot slot) {
    return slot == game::EquipSlot::RANGED ? kSheathStateRanged : kSheathStateMelee;
}

// Where a weapon hangs while its slot is sheathed, by the item's SheatheType
// (Item.dbc / item query / UNIT_VIRTUAL_ITEM_INFO byte 5). Build 8606, gnome
// and human models, every type on main and off hand:
// _experiments/20261117T190000Z.sheath_attachments. Types 0, 5, 6 and 7
// (rods, held-in-hand and fist items, monster props) are not drawn at all.
inline uint32_t sheathAttachment(uint8_t sheathType, game::EquipSlot slot) {
    const bool offHand = slot == game::EquipSlot::OFF_HAND;
    switch (sheathType) {
        case 1: return offHand ? kAttachSheathOffHand : kAttachSheathMainHand;
        case 2: return offHand ? kAttachLargeWeaponRight : kAttachLargeWeaponLeft;
        case 3: return offHand ? kAttachHipWeaponRight : kAttachHipWeaponLeft;
        case 4: return kAttachSheathShield;
        default: return UINT32_MAX;
    }
}

} // namespace wowee::core
