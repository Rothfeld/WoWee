// SMSG_GUILD_BANK_LIST, whose tab-name block is optional.
//
// Guild::SendBankList writes the tab names under one flag - "with tab info" -
// and the items after them. The reader gated the names on that flag *and* on
// the list describing tab zero, which is Cataclysm's rule
// (GuildBankQueryResults::Write tests `!Tab && FullUpdate`) and not 3.3.5's. On
// a server that answers a tab query with the names attached, the block is on
// the wire and was skipped: the item count came out of the tab count and the
// items out of the names, so the panel drew nothing.
//
// The three shapes are pinned here because nothing about a mis-parse raises -
// the bytes are all valid, they are just read a block early.
#include <catch_amalgamated.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/application.hpp"
#include "game/world_packets.hpp"

namespace wowee {
namespace core {
Application* Application::instance = nullptr;
}
}

using namespace wowee::game;

namespace {

struct Writer {
    std::vector<uint8_t> bytes;
    void u8(uint8_t v) { bytes.push_back(v); }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(v >> (i * 8)));
    }
    void cstr(const std::string& s) {
        bytes.insert(bytes.end(), s.begin(), s.end());
        bytes.push_back(0);
    }
    /// One occupied slot, in the order the reader expects it.
    void item(uint8_t slotId, uint32_t entry, uint32_t stack) {
        u8(slotId);
        u32(entry);
        u32(0);        // flags
        u32(0);        // randomPropertyId - zero, so no seed follows
        u32(stack);
        u32(0);        // permanent enchantment
        u8(0);         // charges
        u8(0);         // sockets
    }
};

/// money, tabId, remaining withdrawals, then the optional tab block.
Writer header(uint8_t tabId, bool withTabInfo,
              const std::vector<std::string>& tabNames) {
    Writer w;
    w.u64(123456);
    w.u8(tabId);
    w.u32(7);                    // withdrawals remaining
    w.u8(withTabInfo ? 1 : 0);
    if (withTabInfo) {
        w.u8(static_cast<uint8_t>(tabNames.size()));
        for (const std::string& name : tabNames) {
            w.cstr(name);
            w.cstr("Interface\\Icons\\INV_Misc_Bag_08");
        }
    }
    return w;
}

GuildBankData parse(Writer& w) {
    wowee::network::Packet packet(0, std::move(w.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(packet, data));
    return data;
}

}  // namespace

TEST_CASE("the opening list carries the tabs and the first tab's items") {
    Writer w = header(0, true, {"Tab One", "Tab Two"});
    w.u8(2);
    w.item(0, 6948, 1);
    w.item(3, 4306, 20);

    const GuildBankData data = parse(w);
    CHECK(data.money == 123456u);
    CHECK(data.tabId == 0);
    CHECK(data.withdrawAmount == 7);
    CHECK(data.tabsIncluded);
    REQUIRE(data.tabs.size() == 2);
    CHECK(data.tabs[0].tabName == "Tab One");
    CHECK(data.tabs[1].tabName == "Tab Two");
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].slotId == 0);
    CHECK(data.tabItems[0].itemEntry == 6948u);
    CHECK(data.tabItems[1].slotId == 3);
    CHECK(data.tabItems[1].itemEntry == 4306u);
    CHECK(data.tabItems[1].stackCount == 20u);
}

TEST_CASE("a later tab may carry the names too") {
    // The regression. Reading this as though the names were absent takes the
    // tab count for the item count and the first name for a slot.
    Writer w = header(2, true, {"Tab One", "Tab Two", "Tab Three"});
    w.u8(1);
    w.item(5, 2589, 12);

    const GuildBankData data = parse(w);
    CHECK(data.tabId == 2);
    CHECK(data.tabsIncluded);
    REQUIRE(data.tabs.size() == 3);
    CHECK(data.tabs[2].tabName == "Tab Three");
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].slotId == 5);
    CHECK(data.tabItems[0].itemEntry == 2589u);
    CHECK(data.tabItems[0].stackCount == 12u);
}

TEST_CASE("a slots-only update leaves the tabs it found") {
    Writer first = header(0, true, {"Tab One", "Tab Two"});
    first.u8(0);
    wowee::network::Packet firstPacket(0, std::move(first.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(firstPacket, data));
    REQUIRE(data.tabs.size() == 2);

    Writer second = header(1, false, {});
    second.u8(1);
    second.item(9, 1234, 3);
    wowee::network::Packet secondPacket(0, std::move(second.bytes));
    REQUIRE(GuildBankListParser::parse(secondPacket, data));

    CHECK_FALSE(data.tabsIncluded);
    CHECK(data.tabs.size() == 2);          // kept, not cleared
    CHECK(data.tabId == 1);
    REQUIRE(data.tabItems.size() == 1);
    CHECK(data.tabItems[0].itemEntry == 1234u);
}

TEST_CASE("an empty slot carries nothing after its entry") {
    Writer w = header(0, true, {"Only Tab"});
    w.u8(2);
    w.u8(4);
    w.u32(0);          // empty: no flags, no stack, no enchant
    w.item(6, 858, 5);

    const GuildBankData data = parse(w);
    REQUIRE(data.tabItems.size() == 2);
    CHECK(data.tabItems[0].itemEntry == 0u);
    CHECK(data.tabItems[1].slotId == 6);
    CHECK(data.tabItems[1].itemEntry == 858u);
    CHECK(data.tabItems[1].stackCount == 5u);
}

TEST_CASE("TBC 2.4.3 SMSG_GUILD_BANK_LIST parses without flags and with uint8 stackCount, sockets, and suffix") {
    Writer w = header(0, true, {"Materials", "Consumables"});
    w.u8(2);
    // Slot 0: enchanted + 2 gems + randomPropertyId + suffixFactor
    w.u8(0);
    w.u32(28477);          // itemEntry
    w.u32(123);            // randomPropertyId (non-zero -> suffixFactor follows)
    w.u32(456);            // suffixFactor
    w.u8(1);               // TBC uint8 stackCount
    w.u32(2673);           // permEnchantId
    w.u8(0);               // charges
    w.u8(2);               // socketCount
    w.u8(0); w.u32(2725);  // socket 0 gem enchant
    w.u8(2); w.u32(2730);  // socket 2 gem enchant
    // Slot 5: plain stack of 20
    w.u8(5);
    w.u32(21877);          // Netherweave Cloth
    w.u32(0);              // randomPropertyId = 0 (no suffixFactor)
    w.u8(20);              // TBC uint8 stackCount
    w.u32(0);              // permEnchantId
    w.u8(0);               // charges
    w.u8(0);               // socketCount

    wowee::network::Packet pkt(0, std::move(w.bytes));
    GuildBankData data;
    REQUIRE(GuildBankListParser::parse(pkt, data, /*isTbc=*/true));
    CHECK(data.itemsIncluded);
    REQUIRE(data.tabs.size() == 2);
    REQUIRE(data.tabs[0].items.size() == 2);
    CHECK(data.tabs[0].items[0].itemEntry == 28477u);
    CHECK(data.tabs[0].items[0].randomPropertyId == 123u);
    CHECK(data.tabs[0].items[0].suffixFactor == 456u);
    CHECK(data.tabs[0].items[0].stackCount == 1u);
    CHECK(data.tabs[0].items[0].enchantId == 2673u);
    CHECK(data.tabs[0].items[0].socketEnchantIds[0] == 2725u);
    CHECK(data.tabs[0].items[0].socketEnchantIds[1] == 0u);
    CHECK(data.tabs[0].items[0].socketEnchantIds[2] == 2730u);
    CHECK(data.tabs[0].items[1].slotId == 5);
    CHECK(data.tabs[0].items[1].itemEntry == 21877u);
    CHECK(data.tabs[0].items[1].stackCount == 20u);

    // Incremental 1-slot update removes slot 0 without wiping slot 5
    Writer upd = header(0, false, {});
    upd.u8(1);
    upd.u8(0);
    upd.u32(0); // slot 0 emptied
    wowee::network::Packet updPkt(0, std::move(upd.bytes));
    REQUIRE(GuildBankListParser::parse(updPkt, data, /*isTbc=*/true));
    REQUIRE(data.tabs[0].items.size() == 1);
    CHECK(data.tabs[0].items[0].slotId == 5);
    CHECK(data.tabs[0].items[0].itemEntry == 21877u);

    // Money-only update (numSlots=0) preserves existing tab items
    Writer moneyOnly = header(0, false, {});
    moneyOnly.u8(0);
    wowee::network::Packet moneyPkt(0, std::move(moneyOnly.bytes));
    REQUIRE(GuildBankListParser::parse(moneyPkt, data, /*isTbc=*/true));
    CHECK_FALSE(data.itemsIncluded);
    REQUIRE(data.tabs[0].items.size() == 1);
    CHECK(data.tabs[0].items[0].slotId == 5);
}

TEST_CASE("TBC 2.4.3 CMSG_GUILD_BANK_SWAP_ITEMS wire layouts") {
    const uint64_t guid = 0xF11002DB9B000001ULL;

    SECTION("bank to inventory AutoStore (0xFF, 0xFF)") {
        auto p = GuildBankSwapItemsPacket::buildBankToInventory(guid, 1, 4, 0xFF, 0xFF, 0, /*isTbc=*/true);
        CHECK(p.readUInt64() == guid);
        CHECK(p.readUInt8() == 0);   // bankToBank = 0
        CHECK(p.readUInt8() == 1);   // tab
        CHECK(p.readUInt8() == 4);   // slot
        CHECK(p.readUInt32() == 0u); // itemEntry
        CHECK(p.readUInt8() == 1);   // autoStore = 1
        CHECK(p.readUInt8() == 0);   // TBC uint8 autoStoreCount
        CHECK(p.readUInt8() == 1);   // toChar = 1
        CHECK(p.readUInt8() == 0);   // TBC uint8 unk
        CHECK(p.getRemainingSize() == 0u);
    }

    SECTION("bank to manual backpack slot (0xFF, 23) with split") {
        auto p = GuildBankSwapItemsPacket::buildBankToInventory(guid, 0, 2, 0xFF, 23, 5, /*isTbc=*/true);
        CHECK(p.readUInt64() == guid);
        CHECK(p.readUInt8() == 0);    // bankToBank = 0
        CHECK(p.readUInt8() == 0);    // tab
        CHECK(p.readUInt8() == 2);    // slot
        CHECK(p.readUInt32() == 0u);  // itemEntry
        CHECK(p.readUInt8() == 0);    // autoStore = 0 (manual backpack slot!)
        CHECK(p.readUInt8() == 0xFF); // backpack wire bag
        CHECK(p.readUInt8() == 23);   // backpack slot 0
        CHECK(p.readUInt8() == 1);    // toChar = 1
        CHECK(p.readUInt8() == 5);    // TBC uint8 splitCount
        CHECK(p.getRemainingSize() == 0u);
    }

    SECTION("inventory to bank deposit with split") {
        auto p = GuildBankSwapItemsPacket::buildInventoryToBank(guid, 2, 7, 19, 3, 10, /*isTbc=*/true);
        CHECK(p.readUInt64() == guid);
        CHECK(p.readUInt8() == 0);   // bankToBank = 0
        CHECK(p.readUInt8() == 2);   // tab
        CHECK(p.readUInt8() == 7);   // bankSlot
        CHECK(p.readUInt32() == 0u); // itemEntry
        CHECK(p.readUInt8() == 0);   // autoStore = 0
        CHECK(p.readUInt8() == 19);  // srcBag
        CHECK(p.readUInt8() == 3);   // srcSlot
        CHECK(p.readUInt8() == 0);   // toChar = 0 (deposit)
        CHECK(p.readUInt8() == 10);  // TBC uint8 splitCount
        CHECK(p.getRemainingSize() == 0u);
    }

    SECTION("bank to bank cross-tab split") {
        auto p = GuildBankSwapItemsPacket::buildBankToBank(guid, 0, 1, 1, 9, 21877, 4, /*isTbc=*/true);
        CHECK(p.readUInt64() == guid);
        CHECK(p.readUInt8() == 1);      // bankToBank = 1
        CHECK(p.readUInt8() == 1);      // dstTab
        CHECK(p.readUInt8() == 9);      // dstSlot
        CHECK(p.readUInt32() == 0u);    // always 0
        CHECK(p.readUInt8() == 0);      // srcTab
        CHECK(p.readUInt8() == 1);      // srcSlot
        CHECK(p.readUInt32() == 21877u);// itemEntry
        CHECK(p.readUInt8() == 0);      // always 0
        CHECK(p.readUInt8() == 4);      // TBC uint8 splitCount
        CHECK(p.getRemainingSize() == 0u);
    }
}

TEST_CASE("TBC 2.4.3 MSG_GUILD_BANK_LOG_QUERY parses uint8 item stack counts and money log") {
    SECTION("item log with deposit and cross-tab move") {
        Writer w;
        w.u8(0); // tab 0
        w.u8(2); // 2 entries
        // Entry 1: deposit 15x item 21877
        w.u8(1);                   // GUILD_BANK_LOG_DEPOSIT_ITEM
        w.u64(0x0000000000000042); // playerGuid
        w.u32(21877);              // itemEntry
        w.u8(15);                  // TBC uint8 ItemStackCount
        w.u32(120);                // secondsAgo
        // Entry 2: move 5x item 21877 to tab 1
        w.u8(3);                   // GUILD_BANK_LOG_MOVE_ITEM
        w.u64(0x0000000000000042);
        w.u32(21877);
        w.u8(5);                   // TBC uint8 ItemStackCount
        w.u8(1);                   // DestTabId
        w.u32(60);                 // secondsAgo

        wowee::network::Packet pkt(0, std::move(w.bytes));
        uint8_t tab = 0xFF;
        std::vector<GuildBankLogEntry> entries;
        REQUIRE(GuildBankLogParser::parse(pkt, tab, entries, /*isTbc=*/true));
        CHECK(tab == 0);
        REQUIRE(entries.size() == 2);
        CHECK(entries[0].type == 1);
        CHECK(entries[0].itemId == 21877u);
        CHECK(entries[0].count == 15u);
        CHECK(entries[0].secondsAgo == 120u);
        CHECK(entries[1].type == 3);
        CHECK(entries[1].itemId == 21877u);
        CHECK(entries[1].count == 5u);
        CHECK(entries[1].otherTab == 1);
        CHECK(entries[1].secondsAgo == 60u);
    }

    SECTION("money log (tab 6)") {
        Writer w;
        w.u8(6); // GUILD_BANK_MAX_TABS = money log
        w.u8(1);
        w.u8(4);                   // GUILD_BANK_LOG_DEPOSIT_MONEY
        w.u64(0x0000000000000042);
        w.u32(500000);             // 50g in copper
        w.u32(30);

        wowee::network::Packet pkt(0, std::move(w.bytes));
        uint8_t tab = 0;
        std::vector<GuildBankLogEntry> entries;
        REQUIRE(GuildBankLogParser::parse(pkt, tab, entries, /*isTbc=*/true));
        CHECK(tab == 6);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0].type == 4);
        CHECK(entries[0].count == 500000u);
        CHECK(entries[0].secondsAgo == 30u);
    }
}

TEST_CASE("MSG_QUERY_GUILD_BANK_TEXT and CMSG_SET_GUILD_BANK_TEXT wire layout") {
    Writer w;
    w.u8(1);
    w.cstr("Raid consumables only");
    wowee::network::Packet pkt(0, std::move(w.bytes));
    uint8_t tab = 0xFF;
    std::string text;
    REQUIRE(GuildBankTextParser::parse(pkt, tab, text));
    CHECK(tab == 1);
    CHECK(text == "Raid consumables only");

    auto setPkt = GuildBankSetTabTextPacket::build(1, "Raid consumables only");
    CHECK(setPkt.readUInt8() == 1);
    CHECK(setPkt.readString() == "Raid consumables only");
    CHECK(setPkt.getRemainingSize() == 0u);
}
