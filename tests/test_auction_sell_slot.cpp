// The Auctions tab's sell slot: which items the client refuses, and what a
// click on the slot does with the cursor and with the item it holds.
//
// Every expectation is a measurement of the original 2.4.3 client at an
// auctioneer, recorded in _experiments/20261118T1935Z.auction_sell_slot in the
// workspace. The failure this is here to stop: the slot took a soulbound item,
// put the cursor down without unlocking the bag slot, and left the server to
// refuse the post - with the button greyed in the bag and the panel still
// showing the item.
#include <catch_amalgamated.hpp>

#include "game/auction_sell_slot.hpp"

#include <string>

using namespace wowee::game;

namespace {
// Template Bonding values (CMaNGOS ItemBondingType).
constexpr uint32_t kNoBind = 0;
constexpr uint32_t kBindWhenPickedUp = 1;
constexpr uint32_t kBindWhenEquipped = 2;

std::string refusal(bool soulbound, uint32_t bonding, uint32_t flags) {
    const char* name = auctionSellRefusal(soulbound, bonding, flags);
    return name ? name : "";
}
}  // namespace

TEST_CASE("a bound item is refused as soulbound", "[auction-sell]") {
    // Hearthstone, and Holy Spring Water (BoP, class Quest).
    CHECK(refusal(true, kBindWhenPickedUp, 0) == "ERR_AUCTION_BOUND_ITEM");
    // A bind-on-equip item once it has been equipped.
    CHECK(refusal(true, kBindWhenEquipped, 0) == "ERR_AUCTION_BOUND_ITEM");
}

TEST_CASE("a bound quest item is refused as a quest item", "[auction-sell]") {
    // Gnoll Paw (class Quest) and Wailing Essence (class Consumable): the
    // template's binding decides, not its class.
    CHECK(refusal(true, kBindQuestItem, 0) == "ERR_AUCTION_QUEST_ITEM");
}

TEST_CASE("binding in the template alone refuses nothing", "[auction-sell]") {
    // Unbound copies (a GM .additem) of Gnoll Paw and Wolfskin Bracers were
    // taken, and so was Okra, class Quest with no binding.
    CHECK(refusal(false, kBindQuestItem, 0).empty());
    CHECK(refusal(false, kBindWhenPickedUp, 0).empty());
    CHECK(refusal(false, kNoBind, 0).empty());
}

TEST_CASE("conjured and lootable items are refused by their template flags", "[auction-sell]") {
    // Conjured Water carries 0x2 among its flags, Small Barnacled Clam 0x4.
    CHECK(refusal(false, kNoBind, 0x00200002u) == "ERR_AUCTION_CONJURED_ITEM");
    CHECK(refusal(false, kNoBind, kItemFlagHasLoot) == "ERR_AUCTION_LOOT_ITEM");
}

namespace {
const BagSlotRef kNothing{};
const BagSlotRef kHearthstone{0, 2};
const BagSlotRef kConjuredWater{0, 6};
const BagSlotRef kGnollPaw{0, 7};
const BagSlotRef kOkra{0, 8};
}  // namespace

TEST_CASE("a refused item stays on the cursor and the slot keeps its item", "[auction-sell]") {
    // Conjured Water dropped on a slot holding Okra.
    const auto click = auctionSellClick(kOkra, kConjuredWater, "ERR_AUCTION_CONJURED_ITEM");
    CHECK(click.kind == AuctionSellClick::Kind::Refuse);
    CHECK(std::string(click.error) == "ERR_AUCTION_CONJURED_ITEM");
    CHECK(click.slot == kOkra);
    CHECK(click.toCursor == kConjuredWater);

    // The Hearthstone on an empty slot, which stays empty.
    const auto empty = auctionSellClick(kNothing, kHearthstone, "ERR_AUCTION_BOUND_ITEM");
    CHECK(empty.kind == AuctionSellClick::Kind::Refuse);
    CHECK(empty.slot.empty());
}

TEST_CASE("an item dropped on an empty slot goes in and the cursor empties", "[auction-sell]") {
    const auto click = auctionSellClick(kNothing, kGnollPaw, nullptr);
    CHECK(click.kind == AuctionSellClick::Kind::Place);
    CHECK(click.slot == kGnollPaw);
    CHECK(click.toCursor.empty());
}

TEST_CASE("an item dropped on an occupied slot sends the old one to the cursor", "[auction-sell]") {
    // Okra dropped on Gnoll Paw: Okra in the slot, Gnoll Paw carried.
    const auto click = auctionSellClick(kGnollPaw, kOkra, nullptr);
    CHECK(click.kind == AuctionSellClick::Kind::Place);
    CHECK(click.slot == kOkra);
    CHECK(click.toCursor == kGnollPaw);
}

TEST_CASE("an empty cursor takes the slot's item back", "[auction-sell]") {
    const auto click = auctionSellClick(kOkra, kNothing, nullptr);
    CHECK(click.kind == AuctionSellClick::Kind::TakeBack);
    CHECK(click.slot.empty());
    CHECK(click.toCursor == kOkra);
}

TEST_CASE("an empty cursor on an empty slot does nothing", "[auction-sell]") {
    const auto click = auctionSellClick(kNothing, kNothing, nullptr);
    CHECK(click.kind == AuctionSellClick::Kind::Nothing);
    CHECK(click.slot.empty());
    CHECK(click.toCursor.empty());
}
