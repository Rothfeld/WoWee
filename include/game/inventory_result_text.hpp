#pragma once

#include <array>
#include <cstdint>

namespace wowee::game {

// Which GlobalStrings line an InventoryResult (SMSG_INVENTORY_CHANGE_FAILURE,
// and SMSG_SEND_MAIL_RESULT's MAIL_ERR_EQUIP_ERROR) shows on the error line.
// Index: the result; names from CMaNGOS TBC Item.h, whose comment beside each
// value is the client's line. The loaded expansion's GlobalStrings.lua
// supplies the words. Original 2.4.3: taking a second Hearthstone out of mail
// reads "You can't carry any more of those items." (17, ERR_ITEM_MAX_COUNT),
// on the error line only, no chat line
// (_experiments/20261117T182446Z.pair7_cast_errors/original-mail_take.png).
//
// nullptr: no line - OK, 59 NONE (its comment names ERR_CANT_BE_DISENCHANTED,
// which GlobalStrings does not have), 81 AUTOEQUIP_BIND_CONFIRM (a question,
// not a failure). Values above 80 are not in the TBC enum.
inline constexpr std::array<const char*, 82> kInventoryResultGlobalNames = {
    nullptr,                                // 0 OK
    "ERR_CANT_EQUIP_LEVEL_I",               // 1, %d required level
    "ERR_CANT_EQUIP_SKILL",
    "ERR_WRONG_SLOT",
    "ERR_BAG_FULL",
    "ERR_BAG_IN_BAG",                       // 5
    "ERR_TRADE_EQUIPPED_BAG",
    "ERR_AMMO_ONLY",
    "ERR_PROFICIENCY_NEEDED",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_CANT_EQUIP_EVER",                  // 10
    "ERR_CANT_EQUIP_EVER",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_2HANDED_EQUIPPED",
    "ERR_2HSKILLNOTFOUND",
    "ERR_WRONG_BAG_TYPE",                   // 15
    "ERR_WRONG_BAG_TYPE",
    "ERR_ITEM_MAX_COUNT",
    "ERR_NO_SLOT_AVAILABLE",
    "ERR_CANT_STACK",
    "ERR_NOT_EQUIPPABLE",                   // 20
    "ERR_CANT_SWAP",
    "ERR_SLOT_EMPTY",
    "ERR_ITEM_NOT_FOUND",
    "ERR_DROP_BOUND_ITEM",
    "ERR_OUT_OF_RANGE",                     // 25
    "ERR_TOO_FEW_TO_SPLIT",
    "ERR_SPLIT_FAILED",
    "ERR_SPELL_FAILED_REAGENTS_GENERIC",
    "ERR_NOT_ENOUGH_MONEY",
    "ERR_NOT_A_BAG",                        // 30
    "ERR_DESTROY_NONEMPTY_BAG",
    "ERR_NOT_OWNER",
    "ERR_ONLY_ONE_QUIVER",
    "ERR_NO_BANK_SLOT",
    "ERR_NO_BANK_HERE",                     // 35
    "ERR_ITEM_LOCKED",
    "ERR_GENERIC_STUNNED",
    "ERR_PLAYER_DEAD",
    "ERR_CLIENT_LOCKED_OUT",
    "ERR_INTERNAL_BAG_ERROR",               // 40
    "ERR_ONLY_ONE_BOLT",
    "ERR_ONLY_ONE_AMMO",
    "ERR_CANT_WRAP_STACKABLE",
    "ERR_CANT_WRAP_EQUIPPED",
    "ERR_CANT_WRAP_WRAPPED",                // 45
    "ERR_CANT_WRAP_BOUND",
    "ERR_CANT_WRAP_UNIQUE",
    "ERR_CANT_WRAP_BAGS",
    "ERR_LOOT_GONE",
    "ERR_INV_FULL",                         // 50
    "ERR_BAG_FULL",
    "ERR_VENDOR_SOLD_OUT",
    "ERR_BAG_FULL",
    "ERR_ITEM_NOT_FOUND",
    "ERR_CANT_STACK",                       // 55
    "ERR_BAG_FULL",
    "ERR_VENDOR_SOLD_OUT",
    "ERR_OBJECT_IS_BUSY",
    nullptr,                                // 59 NONE
    "ERR_NOT_IN_COMBAT",                    // 60
    "ERR_NOT_WHILE_DISARMED",
    "ERR_BAG_FULL",
    "ERR_CANT_EQUIP_RANK",
    "ERR_CANT_EQUIP_REPUTATION",
    "ERR_TOO_MANY_SPECIAL_BAGS",            // 65
    "ERR_LOOT_CANT_LOOT_THAT_NOW",
    "ERR_ITEM_UNIQUE_EQUIPPABLE",
    "ERR_VENDOR_MISSING_TURNINS",
    "ERR_NOT_ENOUGH_HONOR_POINTS",
    "ERR_NOT_ENOUGH_ARENA_POINTS",          // 70
    "ERR_ITEM_MAX_COUNT_SOCKETED",
    "ERR_MAIL_BOUND_ITEM",
    "ERR_INTERNAL_BAG_ERROR",
    "ERR_BAG_FULL",
    "ERR_ITEM_MAX_COUNT_EQUIPPED_SOCKETED", // 75
    "ERR_ITEM_UNIQUE_EQUIPPABLE_SOCKETED",
    "ERR_TOO_MUCH_GOLD",
    "ERR_NOT_DURING_ARENA_MATCH",
    "ERR_TRADE_BOUND_ITEM",
    "ERR_CANT_EQUIP_RATING",                // 80
    nullptr,                                // 81 AUTOEQUIP_BIND_CONFIRM
};

inline const char* inventoryResultGlobalName(uint32_t result) {
    return result < kInventoryResultGlobalNames.size() ? kInventoryResultGlobalNames[result] : nullptr;
}

// SMSG_SEND_MAIL_RESULT MailResponseResult (CMaNGOS TBC SharedDefines.h) ->
// GlobalStrings line. MAIL_ERR_EQUIP_ERROR (1) carries an InventoryResult
// instead (above). Original 2.4.3: sending to yourself reads
// ERR_MAIL_TO_SELF, to a missing name ERR_MAIL_TARGET_NOT_FOUND, both on the
// error line. The rest are matched by name to the only GlobalStrings line for
// that refusal and not yet observed. nullptr: GlobalStrings has no line that
// names the refusal (5 NOT_YOUR_TEAM, 14 trial account, 17 suspended).
inline const char* mailResultGlobalName(uint32_t result) {
    switch (result) {
        case 2:  return "ERR_MAIL_TO_SELF";
        case 3:  return "ERR_NOT_ENOUGH_MONEY";
        case 4:  return "ERR_MAIL_TARGET_NOT_FOUND";
        case 6:  return "ERR_MAIL_DATABASE_ERROR";
        case 15: return "ERR_MAIL_REACHED_CAP";
        case 16: return "ERR_MAIL_WRAPPED_COD";
        case 18: return "ERR_MAIL_TOO_MANY_ATTACHMENTS";
        case 19: return "ERR_MAIL_INVALID_ATTACHMENT";
        default: return nullptr;
    }
}

// How activating an item by ID (action bar UseAction, UseItemByName, /use)
// dispatches across worn slots, bags, and keyring.
// Equippable non-quest items are used (CMSG_USE_ITEM) when already worn and
// auto-equipped (CMSG_AUTOEQUIP_ITEM) when only carried in a bag; sending
// CMSG_USE_ITEM for an unequipped equippable item causes the server to reply
// EQUIP_ERR_ITEM_NOT_FOUND (23) instead of equipping or reporting equip errors
// such as EQUIP_ERR_CANT_EQUIP_SKILL (2).
enum class ItemByIdAction : uint8_t {
    NONE,
    USE_EQUIPPED,
    AUTO_EQUIP_BAG,
    USE_BAG,
    USE_KEYRING,
};

inline bool isEquippableForUse(uint32_t inventoryType, uint32_t itemClass) {
    constexpr uint32_t kItemClassQuest = 12;
    return inventoryType != 0 && itemClass != kItemClassQuest;
}

inline ItemByIdAction resolveItemByIdAction(bool equippable,
                                            bool inEquipped,
                                            bool inBag,
                                            bool inKeyring) {
    if (equippable) {
        if (inEquipped) return ItemByIdAction::USE_EQUIPPED;
        if (inBag) return ItemByIdAction::AUTO_EQUIP_BAG;
        return ItemByIdAction::NONE;
    }
    if (inBag) return ItemByIdAction::USE_BAG;
    if (inKeyring) return ItemByIdAction::USE_KEYRING;
    if (inEquipped) return ItemByIdAction::USE_EQUIPPED;
    return ItemByIdAction::NONE;
}

} // namespace wowee::game
