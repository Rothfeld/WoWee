// auction_sell_slot.hpp - the Auctions tab's sell slot, as build 8606 runs it.
//
// The slot does not take the item out of the bag. It names a bag slot, and the
// item there stays where it is, locked - drawn greyed - until it leaves the
// slot again: taken back onto the cursor, swapped out for another item, posted
// with StartAuction, or the auction house closes. The client refuses some items
// itself, before anything reaches the server, with an error of its own and
// nothing else changed: the item stays on the cursor and the slot keeps what it
// had.
//
// Measured on the original 2.4.3 client at an auctioneer
// (_experiments/20261118T1935Z.auction_sell_slot in the workspace):
//
//   Hearthstone (BoP, bound)               ERR_AUCTION_BOUND_ITEM
//   Holy Spring Water (BoP, bound)         ERR_AUCTION_BOUND_ITEM
//   Gnoll Paw (quest-bound, bound)         ERR_AUCTION_QUEST_ITEM
//   Wailing Essence (quest-bound, bound)   ERR_AUCTION_QUEST_ITEM   (class 0)
//   Gnoll Paw (quest-bound, not bound)     accepted
//   Wolfskin Bracers (BoP, not bound)      accepted
//   Okra (class Quest, no binding)         accepted
//   Conjured Water                         ERR_AUCTION_CONJURED_ITEM
//   Small Barnacled Clam (has loot)        ERR_AUCTION_LOOT_ITEM
//
// So "bound" is the item's own ITEM_FIELD_FLAGS bit, not its template's
// binding, and a bound item whose template binds as a quest item gets the quest
// message instead. Item class 12 (Quest) plays no part. Items the server binds
// at creation are always bound; a GM .additem leaves them unbound, which is how
// the unbound rows above were made.
//
// Not measured: which message an item meeting two conditions gets beyond the
// bound/quest pair, and the remaining 2.4.3 messages for this slot
// (ERR_AUCTION_BAG, ERR_AUCTION_REPAIR_ITEM, ERR_AUCTION_USED_CHARGES,
// ERR_AUCTION_WRAPPED_ITEM), so none of those is raised here.
#pragma once

#include <cstdint>

namespace wowee {
namespace game {

/// ItemPrototype Flags bits (CMaNGOS ItemPrototype.h, ItemPrototypeFlags).
inline constexpr uint32_t kItemFlagConjured = 0x00000002u;  // ITEM_FLAG_CONJURED
inline constexpr uint32_t kItemFlagHasLoot  = 0x00000004u;  // ITEM_FLAG_HAS_LOOT
/// ItemPrototype Bonding of a quest item (CMaNGOS ItemBondingType, BIND_QUEST_ITEM).
inline constexpr uint32_t kBindQuestItem = 4;

/// The GlobalStrings name of the error the client raises when this item is
/// put in the sell slot, or nullptr when it takes it.
///
/// `soulbound` is the item's ITEM_FIELD_FLAGS bit 0x1; `bonding` and
/// `templateFlags` are its template's Bonding and Flags.
inline const char* auctionSellRefusal(bool soulbound, uint32_t bonding,
                                      uint32_t templateFlags) {
    if (soulbound) {
        return bonding == kBindQuestItem ? "ERR_AUCTION_QUEST_ITEM"
                                         : "ERR_AUCTION_BOUND_ITEM";
    }
    if (templateFlags & kItemFlagConjured) return "ERR_AUCTION_CONJURED_ITEM";
    if (templateFlags & kItemFlagHasLoot)  return "ERR_AUCTION_LOOT_ITEM";
    return nullptr;
}

/// A bag slot in the interface's numbering: container (0 the backpack, 1-4 the
/// worn bags) and 1-based slot. `bag < 0` names nothing.
struct BagSlotRef {
    int bag = -1;
    int slot = 0;
    [[nodiscard]] bool empty() const { return bag < 0; }
    friend bool operator==(const BagSlotRef&, const BagSlotRef&) = default;
};

/// What one ClickAuctionSellItemButton does.
struct AuctionSellClick {
    enum class Kind {
        Nothing,    ///< empty cursor, empty slot
        Refuse,     ///< `error` is raised; cursor and slot are unchanged
        Place,      ///< the carried item goes in; `toCursor` is what it displaced
        TakeBack,   ///< empty cursor: the slot's item goes onto the cursor
    };
    Kind kind = Kind::Nothing;
    BagSlotRef slot;        ///< the sell slot afterwards
    BagSlotRef toCursor;    ///< what the cursor carries afterwards, if it changed
    const char* error = nullptr;
};

/// The sell slot's answer to a click, given what it holds (`current`), the bag
/// item the cursor is carrying (`carried`, empty for nothing) and
/// auctionSellRefusal's answer for that item.
///
/// Carrying an item that is refused changes nothing but the error line.
/// Carrying one that is taken puts it in the slot, and whatever the slot held
/// before comes back onto the cursor - still locked, since it is carried now.
/// An empty cursor takes the slot's item back the same way. Measured in the
/// experiment named at the top of this file.
inline AuctionSellClick auctionSellClick(BagSlotRef current, BagSlotRef carried,
                                         const char* refusal) {
    using Kind = AuctionSellClick::Kind;
    if (!carried.empty()) {
        if (refusal) return {Kind::Refuse, current, carried, refusal};
        return {Kind::Place, carried, current == carried ? BagSlotRef{} : current, nullptr};
    }
    if (!current.empty()) return {Kind::TakeBack, BagSlotRef{}, current, nullptr};
    return {};
}

}  // namespace game
}  // namespace wowee
