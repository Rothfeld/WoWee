// Deterministic TBC 2.4.3 & WotLK 3.3.5a wire-protocol and handler tests for
// Shard 3 Economy & Items:
//   - Auction: SMSG_AUCTION_OWNER_NOTIFICATION (expired vs sold vs active bid + randomProp),
//     SMSG_AUCTION_BIDDER_NOTIFICATION (won vs outbid + outbiddedAuctionIds -> CMSG_AUCTION_LIST_BIDDER_ITEMS),
//     SMSG_AUCTION_REMOVED_NOTIFICATION (ERR_AUCTION_REMOVED_S),
//     SMSG_AUCTION_COMMAND_RESULT (AUCTION_ERR_HIGHER_BID 16-byte update + bid success clear),
//     SMSG_AUCTION_LIST_RESULT (gem sockets 1..3 + spellCharges + GetAuctionItemLink).
//   - Trade: SMSG_TRADE_STATUS (CLOSE_WINDOW 9-byte InventoryResult, TBC 21/22),
//     CMSG_BUSY_TRADE & CMSG_IGNORE_TRADE,
//     SMSG_TRADE_STATUS_EXTENDED (TBC clientRevision staleness guard, 73-byte slot metadata,
//     slot-6 spell + GetTradeTargetItemInfo/Link + ClickTargetTradeButton TARGET_FLAG_TRADE_ITEM).
//   - Loot & Items: SMSG_LOOT_RESPONSE (LootError 11/15 + auto-loot lootSlotType 0/4 filter),
//     SMSG_LOOT_START_ROLL (randomSuffix/randomPropertyId + GetLootRollItemLink),
//     SMSG_READ_ITEM_OK / SMSG_READ_ITEM_FAILED (gated CMSG_PAGE_TEXT_QUERY),
//     SMSG_ITEM_TIME_UPDATE (timed item duration update),
//     CMSG_DESTROYITEM (DeleteCursorItem), CMSG_AUTOSTORE_BAG_ITEM (PutItemInBag/PutItemInBackpack),
//     CMSG_BUY_ITEM_IN_SLOT (TBC 22-byte vs WotLK 29-byte + merchant drag-drop to slot).

#include <catch_amalgamated.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

#include "addons/lua_engine.hpp"
#include "game/item_text.hpp"
#include "game_replay.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using replay::Session;
using wowee::game::AuctionEntry;
using wowee::game::EquipSlot;
using wowee::game::GameHandler;
using wowee::game::GameHandlerTestAccess;
using wowee::game::ItemDef;
using wowee::game::ItemQueryResponseData;
using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

namespace {

std::string evalLua(wowee::addons::LuaEngine& lua, const std::string& expr) {
    lua_State* L = lua.getState();
    const int top = lua_gettop(L);
    const std::string chunk = "return tostring(" + expr + ")";
    if (luaL_loadstring(L, chunk.c_str()) != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        std::string err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "lua error";
        lua_settop(L, top);
        FAIL(err);
    }
    std::string out = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_settop(L, top);
    return out;
}

void execLua(wowee::addons::LuaEngine& lua, const std::string& code) {
    REQUIRE(lua.executeString(code));
}

void cacheItem(GameHandler& gh, uint32_t itemId, const std::string& name,
               uint32_t quality = 3, uint32_t displayId = 20000,
               uint32_t pageTextId = 0) {
    ItemQueryResponseData info{};
    info.entry = itemId;
    info.name = name;
    info.quality = quality;
    info.displayInfoId = displayId;
    info.pageTextId = pageTextId;
    info.maxStack = 20;
    info.valid = true;
    gh.itemInfoCacheRef()[itemId] = info;
}

void installEconomyGlobalStrings(GameHandler& gh, std::vector<std::string>& seenKeys) {
    GameHandlerTestAccess::setNoticesInterfaceReady(gh, true);
    gh.setGlobalStringLookup([&seenKeys](const char* key) -> std::string {
        if (!key) return {};
        const std::string k(key);
        seenKeys.push_back(k);
        if (k == "ERR_AUCTION_EXPIRED_S") return "Your auction of %s has expired.";
        if (k == "ERR_AUCTION_SOLD_S") return "A buyer has been found for your auction of %s.";
        if (k == "ERR_AUCTION_OUTBID_S") return "You have been outbid on %s.";
        if (k == "ERR_AUCTION_WON_S") return "You won an auction for %s";
        if (k == "ERR_AUCTION_REMOVED_S") return "Your auction of %s has been cancelled by the seller.";
        if (k == "ERR_AUCTION_BID_PLACED") return "Bid accepted.";
        if (k == "ERR_AUCTION_HIGHER_BID") return "There is already a higher bid on that item.";
        if (k == "ERR_TRADE_BAG_FULL") return "Trade failed, you don't have enough space.";
        if (k == "ERR_TRADE_TARGET_BAG_FULL") return "Trade failed, target doesn't have enough space.";
        if (k == "ERR_TRADE_MAX_COUNT_EXCEEDED") return "You have too many of a unique item.";
        if (k == "ERR_TRADE_TARGET_MAX_COUNT_EXCEEDED") return "Your trade partner has too many of a unique item.";
        if (k == "ERR_RESTRICTED_ACCOUNT") return "Trial accounts cannot perform that action";
        if (k == "ERR_TRADE_WRONG_REALM") return "You may only trade conjured items to players from other realms";
        if (k == "ERR_PLAY_TIME_EXCEEDED") return "Maximum play time exceeded";
        if (k == "ERR_ALREADY_PICKPOCKETED") return "Your target has already had its pockets picked";
        return k;
    });
}

void writeTbcTradeSlot(Packet& p, uint8_t slotNum, uint32_t itemId, uint32_t displayId,
                       uint32_t stackCnt, uint32_t permEnchant,
                       std::array<uint32_t, 3> gems, int32_t charges,
                       uint32_t suffixFactor, int32_t randomProp,
                       uint32_t lockId, uint32_t maxDur, uint32_t curDur,
                       bool wrapped = false) {
    p.writeUInt8(slotNum);
    p.writeUInt32(itemId);
    p.writeUInt32(displayId);
    p.writeUInt32(stackCnt);
    p.writeUInt32(wrapped ? 1u : 0u);
    p.writeUInt64(0); // giftCreator
    p.writeUInt32(permEnchant);
    p.writeUInt32(gems[0]);
    p.writeUInt32(gems[1]);
    p.writeUInt32(gems[2]);
    p.writeUInt64(0xCAFEULL); // creator
    p.writeUInt32(static_cast<uint32_t>(charges));
    p.writeUInt32(suffixFactor);
    p.writeUInt32(static_cast<uint32_t>(randomProp));
    p.writeUInt32(lockId);
    p.writeUInt32(maxDur);
    p.writeUInt32(curDur);
}

} // namespace

TEST_CASE("SMSG_AUCTION_OWNER_NOTIFICATION distinguishes expired, sold, and active bid updates",
          "[replay][tbc][auction]") {
    Session s("tbc");
    auto& gh = s.handler();
    cacheItem(gh, 22446, "Greater Planar Essence", 2);

    std::vector<std::string> seenKeys;
    installEconomyGlobalStrings(gh, seenKeys);
    std::vector<std::string> events;
    gh.setAddonEventCallback([&](const std::string& e, const std::vector<std::string>&) {
        events.push_back(e);
    });

    // Seed two owned auctions.
    {
        Packet list(wireOpcode(Opcode::SMSG_AUCTION_OWNER_LIST_RESULT));
        list.writeUInt32(2);
        for (uint32_t id : {1001u, 1002u}) {
            list.writeUInt32(id);
            list.writeUInt32(22446);
            for (int slot = 0; slot < 6; ++slot) {
                list.writeUInt32(0);
                list.writeUInt32(0);
                list.writeUInt32(0);
            }
            list.writeUInt32(0);
            list.writeUInt32(0);
            list.writeUInt32(1);
            list.writeUInt32(0);
            list.writeUInt32(0);
            list.writeUInt64(Session::kPlayerGuid);
            list.writeUInt32(1000);
            list.writeUInt32(50);
            list.writeUInt32(2000);
            list.writeUInt32(3600000);
            list.writeUInt64(0);
            list.writeUInt32(0);
        }
        list.writeUInt32(2);
        list.writeUInt32(300);
        s.receive(list);
    }
    REQUIRE(gh.getAuctionOwnerResults().auctions.size() == 2);
    seenKeys.clear();
    events.clear();

    // 1. Active auction receives a bid (bidderGuid != 0): updates row in-place, no sold/expired text.
    {
        Packet p(wireOpcode(Opcode::SMSG_AUCTION_OWNER_NOTIFICATION));
        p.writeUInt32(1001);
        p.writeUInt32(1200);          // new bid
        p.writeUInt32(60);            // next min increment
        p.writeUInt64(0x998877ULL);   // bidderGuid != 0
        p.writeUInt32(22446);
        p.writeUInt32(static_cast<uint32_t>(-45));
        s.receive(p);
    }
    CHECK(seenKeys.empty());
    REQUIRE(gh.getAuctionOwnerResults().auctions.size() == 2);
    CHECK(gh.getAuctionOwnerResults().auctions[0].currentBid == 1200);
    CHECK(gh.getAuctionOwnerResults().auctions[0].minBidIncrement == 60);
    CHECK(gh.getAuctionOwnerResults().auctions[0].bidderGuid == 0x998877ULL);
    CHECK(std::find(events.begin(), events.end(), "AUCTION_OWNED_LIST_UPDATE") != events.end());

    // 2. Auction expires (bidderGuid == 0, bid == 0): emits ERR_AUCTION_EXPIRED_S with randomProp link & removes row.
    events.clear();
    {
        Packet p(wireOpcode(Opcode::SMSG_AUCTION_OWNER_NOTIFICATION));
        p.writeUInt32(1002);
        p.writeUInt32(0);   // bid == 0 -> expired
        p.writeUInt32(0);
        p.writeUInt64(0);   // bidderGuid == 0
        p.writeUInt32(22446);
        p.writeUInt32(static_cast<uint32_t>(-45));
        s.receive(p);
    }
    REQUIRE(seenKeys == std::vector<std::string>{"ERR_AUCTION_EXPIRED_S"});
    REQUIRE(gh.getAuctionOwnerResults().auctions.size() == 1);
    CHECK(gh.getAuctionOwnerResults().auctions[0].auctionId == 1001);
    REQUIRE_FALSE(gh.getChatHistory().empty());
    CHECK(gh.getChatHistory().back().message.find(":-45:0|h[") != std::string::npos);

    // 3. Auction sells (bidderGuid == 0, bid != 0): emits ERR_AUCTION_SOLD_S & removes row.
    seenKeys.clear();
    {
        Packet p(wireOpcode(Opcode::SMSG_AUCTION_OWNER_NOTIFICATION));
        p.writeUInt32(1001);
        p.writeUInt32(2000); // bid != 0 -> sold
        p.writeUInt32(0);
        p.writeUInt64(0);    // bidderGuid == 0
        p.writeUInt32(22446);
        p.writeUInt32(0);
        s.receive(p);
    }
    REQUIRE(seenKeys == std::vector<std::string>{"ERR_AUCTION_SOLD_S"});
    CHECK(gh.getAuctionOwnerResults().auctions.empty());
}

TEST_CASE("Auction bidder/removed notifications, outbidded ID list, command result higher-bid, and gem links",
          "[replay][tbc][auction]") {
    Session s("tbc");
    auto& gh = s.handler();
    wowee::game::setItemLinkShape(wowee::game::ItemLinkShape::Tbc);
    cacheItem(gh, 28430, "Lionheart Champion", 4);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    std::vector<std::string> seenKeys;
    installEconomyGlobalStrings(gh, seenKeys);
    std::vector<std::string> errors;
    gh.setUIErrorCallback([&](const std::string& msg) { errors.push_back(msg); });

    // Open auction house via MSG_AUCTION_HELLO.
    constexpr uint64_t kAuctioneer = 0xF130002158000001ULL;
    {
        Packet hello(wireOpcode(Opcode::MSG_AUCTION_HELLO));
        hello.writeUInt64(kAuctioneer);
        hello.writeUInt32(1); // Alliance AH
        s.receive(hello);
    }
    REQUIRE(gh.isAuctionHouseOpen());

    // Browse result with permanent enchant + 3 gem sockets + spellCharges.
    {
        Packet browse(wireOpcode(Opcode::SMSG_AUCTION_LIST_RESULT));
        browse.writeUInt32(1);
        browse.writeUInt32(555);
        browse.writeUInt32(28430);
        const uint32_t ench[6] = {2673, 0, 2725, 2734, 2740, 2877};
        for (int i = 0; i < 6; ++i) {
            browse.writeUInt32(ench[i]);
            browse.writeUInt32(0);
            browse.writeUInt32(0);
        }
        browse.writeUInt32(static_cast<uint32_t>(-12)); // randomPropertyId
        browse.writeUInt32(99);                         // suffixFactor
        browse.writeUInt32(1);
        browse.writeUInt32(static_cast<uint32_t>(-5));  // spellCharges
        browse.writeUInt32(0);
        browse.writeUInt64(0x7777ULL);
        browse.writeUInt32(50000);
        browse.writeUInt32(2500);
        browse.writeUInt32(100000);
        browse.writeUInt32(7200000);
        browse.writeUInt64(Session::kPlayerGuid);
        browse.writeUInt32(52500);
        browse.writeUInt32(1);
        browse.writeUInt32(300);
        s.receive(browse);
    }
    const std::string link = evalLua(lua, "GetAuctionItemLink('list', 1)");
    CHECK(link.find("|Hitem:28430:2673:2725:2734:2740:0:-12:0|h[") != std::string::npos);

    // Outbid notification (bidAmount != 0): ERR_AUCTION_OUTBID_S + adds 555 and 556 to outbiddedIds.
    for (uint32_t id : {555u, 556u}) {
        Packet outbid(wireOpcode(Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION));
        outbid.writeUInt32(1);        // houseId
        outbid.writeUInt32(id);
        outbid.writeUInt64(0xBEEFULL);
        outbid.writeUInt32(60000);    // bidAmount != 0 -> outbid
        outbid.writeUInt32(3000);
        outbid.writeUInt32(28430);
        outbid.writeUInt32(static_cast<uint32_t>(-12));
        s.receive(outbid);
    }
    CHECK(std::count(seenKeys.begin(), seenKeys.end(), "ERR_AUCTION_OUTBID_S") == 2);

    // Query bidder items -> CMSG_AUCTION_LIST_BIDDER_ITEMS carries [555, 556].
    gh.auctionListBidderItems(0);
    const auto bidderReqs = s.sent().of(Opcode::CMSG_AUCTION_LIST_BIDDER_ITEMS);
    REQUIRE(bidderReqs.size() == 1);
    {
        Packet req = bidderReqs[0];
        CHECK(req.readUInt64() == kAuctioneer);
        CHECK(req.readUInt32() == 0);
        REQUIRE(req.readUInt32() == 2);
        CHECK(req.readUInt32() == 555);
        CHECK(req.readUInt32() == 556);
    }

    // AUCTION_ERR_HIGHER_BID (error == 5) with 16-byte tail updates browse row 555 in-place!
    seenKeys.clear();
    errors.clear();
    {
        Packet cmd(wireOpcode(Opcode::SMSG_AUCTION_COMMAND_RESULT));
        cmd.writeUInt32(555);
        cmd.writeUInt32(2); // bid
        cmd.writeUInt32(5); // AUCTION_ERR_HIGHER_BID
        cmd.writeUInt64(0xCAFEBABEULL);
        cmd.writeUInt32(75000);
        cmd.writeUInt32(3750);
        s.receive(cmd);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_AUCTION_HIGHER_BID"});
    REQUIRE_FALSE(errors.empty());
    CHECK(gh.getAuctionBrowseResults().auctions[0].bidderGuid == 0xCAFEBABEULL);
    CHECK(gh.getAuctionBrowseResults().auctions[0].currentBid == 75000);
    CHECK(gh.getAuctionBrowseResults().auctions[0].minBidIncrement == 3750);

    // Won notification on 555 (bidAmount == 0): emits ERR_AUCTION_WON_S and drops 555 from outbidded list.
    seenKeys.clear();
    {
        Packet won(wireOpcode(Opcode::SMSG_AUCTION_BIDDER_NOTIFICATION));
        won.writeUInt32(1);
        won.writeUInt32(555);
        won.writeUInt64(Session::kPlayerGuid);
        won.writeUInt32(0); // bidAmount == 0 -> WON
        won.writeUInt32(0);
        won.writeUInt32(28430);
        won.writeUInt32(0);
        s.receive(won);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_AUCTION_WON_S"});

    // Removed notification on 556: emits ERR_AUCTION_REMOVED_S (not EXPIRED) and drops 556 from outbidded list.
    seenKeys.clear();
    {
        Packet rem(wireOpcode(Opcode::SMSG_AUCTION_REMOVED_NOTIFICATION));
        rem.writeUInt32(556);
        rem.writeUInt32(28430);
        rem.writeUInt32(0);
        s.receive(rem);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_AUCTION_REMOVED_S"});

    gh.auctionListBidderItems(0);
    const auto bidderReqsAfter = s.sent().of(Opcode::CMSG_AUCTION_LIST_BIDDER_ITEMS);
    REQUIRE(bidderReqsAfter.size() == 2);
    {
        Packet req = bidderReqsAfter[1];
        (void)req.readUInt64();
        (void)req.readUInt32();
        CHECK(req.readUInt32() == 0); // outbiddedIds cleared
    }
}

TEST_CASE("TBC trade status errors, busy/ignore replies, extended metadata, and ClickTargetTradeButton",
          "[replay][tbc][trade]") {
    Session s("tbc");
    auto& gh = s.handler();
    wowee::game::setItemLinkShape(wowee::game::ItemLinkShape::Tbc);
    cacheItem(gh, 29348, "The Bladefist", 4);
    gh.spellNameCacheRef()[27984].name = "Enchant Weapon - Mongoose";
    gh.spellNameCacheLoadedRef() = true;

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    std::vector<std::string> seenKeys;
    installEconomyGlobalStrings(gh, seenKeys);

    // 1. Ignored player initiates trade -> client replies CMSG_IGNORE_TRADE and stays closed.
    constexpr uint64_t kIgnoredPeer = 0x0000000000000999ULL;
    {
        Packet contactList(wireOpcode(Opcode::SMSG_CONTACT_LIST));
        contactList.writeUInt32(0x02); // SOCIAL_FLAG_IGNORED mask
        contactList.writeUInt32(1);
        contactList.writeUInt64(kIgnoredPeer);
        contactList.writeUInt32(0x02); // ignored flag
        contactList.writeString("");
        s.receive(contactList);
    }
    {
        Packet begin(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        begin.writeUInt32(1); // BEGIN_TRADE
        begin.writeUInt64(kIgnoredPeer);
        s.receive(begin);
    }
    CHECK(s.sent().of(Opcode::CMSG_IGNORE_TRADE).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_BEGIN_TRADE).empty());
    CHECK_FALSE(gh.isTradeOpen());

    // 2. Open trade window, then receive second BEGIN_TRADE -> replies CMSG_BUSY_TRADE without closing.
    {
        Packet openPkt(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        openPkt.writeUInt32(2);  // OPEN_WINDOW
        openPkt.writeUInt32(42); // tradeId
        s.receive(openPkt);
    }
    REQUIRE(gh.isTradeOpen());
    {
        Packet busyAsk(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        busyAsk.writeUInt32(1);
        busyAsk.writeUInt64(0x12345ULL);
        s.receive(busyAsk);
    }
    CHECK(s.sent().of(Opcode::CMSG_BUSY_TRADE).size() == 1);
    CHECK(gh.isTradeOpen());

    // 3. TBC clientRevision staleness check on own-side SMSG_TRADE_STATUS_EXTENDED.
    gh.setTradeGold(5000); // increments myTradeClientStateIndex_ to 1
    {
        // Stale clientRevision = 0 must be ignored.
        Packet extStale(wireOpcode(Opcode::SMSG_TRADE_STATUS_EXTENDED));
        extStale.writeUInt8(0);  // own side
        extStale.writeUInt32(42);
        extStale.writeUInt32(0); // stale clientRevision < 1
        extStale.writeUInt32(1);
        extStale.writeUInt32(99999);
        extStale.writeUInt32(0);
        s.receive(extStale);
    }
    CHECK(gh.getMyTradeGold() == 0);
    {
        // Current clientRevision = 1 applies gold + slot 0.
        Packet extCur(wireOpcode(Opcode::SMSG_TRADE_STATUS_EXTENDED));
        extCur.writeUInt8(0);
        extCur.writeUInt32(42);
        extCur.writeUInt32(1); // matches local index 1
        extCur.writeUInt32(2);
        extCur.writeUInt32(5000);
        extCur.writeUInt32(0);
        writeTbcTradeSlot(extCur, 0, 29348, 20000, 1, 0, {0, 0, 0}, 0, 0, 0, 0, 105, 105);
        s.receive(extCur);
    }
    CHECK(gh.getMyTradeGold() == 5000);
    CHECK(gh.getMyTradeSlots()[0].occupied);

    // 4. TBC status 22 (WRONG_REALM) clears offending own slot and raises ERR_TRADE_WRONG_REALM.
    seenKeys.clear();
    {
        Packet wr(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        wr.writeUInt32(22); // TBC TRADE_STATUS_WRONG_REALM
        wr.writeUInt8(0);   // slot 0
        s.receive(wr);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_TRADE_WRONG_REALM"});
    CHECK_FALSE(gh.getMyTradeSlots()[0].occupied);

    // 5. Partner's SMSG_TRADE_STATUS_EXTENDED on slot 6 (TRADE_SLOT_NONTRADED) with spell + gems.
    {
        Packet peerExt(wireOpcode(Opcode::SMSG_TRADE_STATUS_EXTENDED));
        peerExt.writeUInt8(1); // trader side
        peerExt.writeUInt32(42);
        peerExt.writeUInt32(1);
        peerExt.writeUInt32(7);
        peerExt.writeUInt32(0);
        peerExt.writeUInt32(27984); // Enchant Weapon - Mongoose on slot 6
        writeTbcTradeSlot(peerExt, 6, 29348, 20000, 1, 2673, {2725, 2734, 0}, -1, 0, -20, 0, 105, 90);
        s.receive(peerExt);
    }
    CHECK(evalLua(lua, "select(6, GetTradeTargetItemInfo(7))") == "Enchant Weapon - Mongoose");
    CHECK(evalLua(lua, "GetTradeTargetItemLink(7)").find("|Hitem:29348:2673:2725:2734:0:0:-20:0|h[") != std::string::npos);

    // 6. ClickTargetTradeButton(7) casts armed item-targeting spell with TARGET_FLAG_TRADE_ITEM (0x1000) & PackedGuid(6).
    gh.beginSpellItemTargeting(27984, "Enchant Weapon - Mongoose");
    REQUIRE(gh.isAwaitingItemTarget());
    execLua(lua, "ClickTargetTradeButton(7)");
    CHECK_FALSE(gh.isAwaitingItemTarget());
    const auto casts = s.sent().of(Opcode::CMSG_CAST_SPELL);
    REQUIRE(casts.size() == 1);
    {
        Packet c = casts[0];
        CHECK(c.readUInt32() == 27984); // TBC: spellId first
        CHECK(c.readUInt8() == 0);      // castCount
        CHECK(c.readUInt32() == 0x1000u); // TARGET_FLAG_TRADE_ITEM
        CHECK(c.readPackedGuid() == 6ULL); // TRADE_SLOT_NONTRADED
        CHECK(c.getRemainingSize() == 0);
    }

    // 7. CLOSE_WINDOW (status 12) 9-byte InventoryResult handling + TBC status 21 (TRIAL_ACCOUNT).
    seenKeys.clear();
    {
        Packet closeFull(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        closeFull.writeUInt32(12); // CLOSE_WINDOW
        closeFull.writeUInt32(4);  // EQUIP_ERR_BAG_FULL
        closeFull.writeUInt8(0);   // CMaNGOS inverted flag: 0 means partner's bag is full
        closeFull.writeUInt32(0);
        s.receive(closeFull);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_TRADE_TARGET_BAG_FULL"});
    CHECK_FALSE(gh.isTradeOpen());

    seenKeys.clear();
    {
        Packet trial(wireOpcode(Opcode::SMSG_TRADE_STATUS));
        trial.writeUInt32(21); // TBC TRADE_STATUS_TRIAL_ACCOUNT
        s.receive(trial);
    }
    CHECK(seenKeys == std::vector<std::string>{"ERR_RESTRICTED_ACCOUNT"});
}

TEST_CASE("LootError 11/15, auto-loot slotType filtering, and loot roll randomPropertyId links",
          "[replay][tbc][loot]") {
    Session s("tbc");
    auto& gh = s.handler();
    wowee::game::setItemLinkShape(wowee::game::ItemLinkShape::Tbc);
    cacheItem(gh, 25978, "Sethikk Feather", 2);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    std::vector<std::string> seenKeys;
    installEconomyGlobalStrings(gh, seenKeys);

    // 1. LootError 11 (ERR_PLAY_TIME_EXCEEDED) & 15 (ERR_ALREADY_PICKPOCKETED).
    for (auto [errByte, expectedKey] : {std::pair<uint8_t, const char*>{11, "ERR_PLAY_TIME_EXCEEDED"},
                                        std::pair<uint8_t, const char*>{15, "ERR_ALREADY_PICKPOCKETED"}}) {
        seenKeys.clear();
        Packet p(wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        p.writeUInt64(0xF130001111000001ULL);
        p.writeUInt8(0); // LOOT_NONE refusal
        p.writeUInt8(errByte);
        s.receive(p);
        REQUIRE(seenKeys == std::vector<std::string>{expectedKey});
    }

    // 2. Auto-loot skips VIEW (1), MASTER (2), and REQS (3), looting only NORMAL (0) and OWNER (4).
    gh.setAutoLoot(true);
    {
        Packet loot(wireOpcode(Opcode::SMSG_LOOT_RESPONSE));
        loot.writeUInt64(0xF130001111000002ULL);
        loot.writeUInt8(1); //LOOT_CORPSE
        loot.writeUInt32(0); // gold
        loot.writeUInt8(5);  // 5 items
        const std::pair<uint8_t, uint8_t> slotsAndTypes[5] = {
            {0, 0}, // NORMAL -> loot
            {1, 1}, // VIEW -> skip
            {2, 2}, // MASTER -> skip
            {3, 3}, // REQS -> skip
            {4, 4}, // OWNER -> loot
        };
        for (auto [slotIdx, slotType] : slotsAndTypes) {
            loot.writeUInt8(slotIdx);
            loot.writeUInt32(25978);
            loot.writeUInt32(1);
            loot.writeUInt32(20000);
            loot.writeUInt32(0);
            loot.writeUInt32(0);
            loot.writeUInt8(slotType);
        }
        s.receive(loot);
    }
    const auto autostores = s.sent().of(Opcode::CMSG_AUTOSTORE_LOOT_ITEM);
    REQUIRE(autostores.size() == 2);
    {
        Packet a0 = autostores[0];
        Packet a1 = autostores[1];
        CHECK(a0.readUInt8() == 0);
        CHECK(a1.readUInt8() == 4);
    }

    // 3. SMSG_LOOT_START_ROLL stores randomSuffix & randomPropertyId and surfaces them in GetLootRollItemLink.
    {
        Packet roll(wireOpcode(Opcode::SMSG_LOOT_START_ROLL));
        roll.writeUInt64(0xF130001111000003ULL);
        roll.writeUInt32(2);
        roll.writeUInt32(25978);
        roll.writeUInt32(77);                          // randomSuffix
        roll.writeUInt32(static_cast<uint32_t>(-34));  // randomPropertyId
        roll.writeUInt32(60000);
        roll.writeUInt8(0x03);
        s.receive(roll);
    }
    const auto* r = gh.getLootRoll(0);
    REQUIRE(r != nullptr);
    CHECK(r->randomSuffix == 77);
    CHECK(r->randomPropertyId == -34);
    CHECK(evalLua(lua, "GetLootRollItemLink(0)").find("|Hitem:25978:0:0:0:0:0:-34:0|h[") != std::string::npos);
}

TEST_CASE("SMSG_READ_ITEM_OK gating, SMSG_ITEM_TIME_UPDATE, DeleteCursorItem, PutItemInBag, and BuyItemInSlot",
          "[replay][tbc][items]") {
    Session s("tbc");
    auto& gh = s.handler();
    cacheItem(gh, 23799, "Faded Journal", 1, 20001, /*pageTextId=*/888);
    cacheItem(gh, 2589, "Linen Cloth", 1, 20002);

    wowee::addons::LuaEngine lua;
    REQUIRE(lua.initialize());
    lua.setGameHandler(&gh);

    // Place Faded Journal in backpack slot 0 (wire slot 23) with GUID 0xAA01.
    constexpr uint64_t kBookGuid = 0x000000000000AA01ULL;
    {
        ItemDef book{};
        book.itemId = 23799;
        book.name = "Faded Journal";
        book.pageTextId = 888;
        book.stackCount = 1;
        book.guid = kBookGuid;
        gh.inventoryRef().setBackpackSlot(0, book);
        gh.backpackSlotGuidsRef()[0] = kBookGuid;
    }

    // 1. readItemBySlot sends CMSG_READ_ITEM first; SMSG_READ_ITEM_FAILED prevents CMSG_PAGE_TEXT_QUERY;
    //    SMSG_READ_ITEM_OK dispatches CMSG_PAGE_TEXT_QUERY(888, kBookGuid).
    gh.readItemBySlot(0);
    CHECK(s.sent().of(Opcode::CMSG_READ_ITEM).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_PAGE_TEXT_QUERY).empty());

    {
        Packet fail(wireOpcode(Opcode::SMSG_READ_ITEM_FAILED));
        fail.writeUInt64(kBookGuid);
        s.receive(fail);
    }
    CHECK(s.sent().of(Opcode::CMSG_PAGE_TEXT_QUERY).empty());

    gh.readItemBySlot(0);
    {
        Packet ok(wireOpcode(Opcode::SMSG_READ_ITEM_OK));
        ok.writeUInt64(kBookGuid);
        s.receive(ok);
    }
    const auto pageReqs = s.sent().of(Opcode::CMSG_PAGE_TEXT_QUERY);
    REQUIRE(pageReqs.size() == 1);
    {
        Packet pq = pageReqs[0];
        CHECK(pq.readUInt32() == 888);
        CHECK(pq.readUInt64() == kBookGuid);
    }

    // 2. SMSG_ITEM_TIME_UPDATE updates ItemDef::durationSec and getItemDuration(guid).
    {
        Packet timeUpd(wireOpcode(Opcode::SMSG_ITEM_TIME_UPDATE));
        timeUpd.writeUInt64(kBookGuid);
        timeUpd.writeUInt32(3600);
        s.receive(timeUpd);
    }
    CHECK(gh.getItemDuration(kBookGuid) == 3600);
    CHECK(gh.getInventory().getBackpackSlot(0).item.durationSec == 3600);

    // 3. Equip a 6-slot bag in worn bag 0 (inventory slot 20, wire container 19, GUID 0xBB01),
    //    pick up backpack slot 1 (1-based) and test PutItemInBag(20), PutItemInBackpack(), DeleteCursorItem().
    constexpr uint64_t kBagGuid = 0x000000000000BB01ULL;
    {
        ItemDef bagItem{};
        bagItem.itemId = 4496;
        bagItem.name = "Small Brown Pouch";
        bagItem.bagSlots = 6;
        bagItem.guid = kBagGuid;
        gh.inventoryRef().setEquipSlot(EquipSlot::BAG1, bagItem);
        gh.inventoryRef().setBagSize(0, 6);
        gh.equipSlotGuidsRef()[wowee::game::Inventory::FIRST_BAG_EQUIP_SLOT] = kBagGuid;
    }

    execLua(lua, "PickupContainerItem(0, 1); PutItemInBag(20)");
    execLua(lua, "PickupContainerItem(0, 1); PutItemInBackpack()");
    const auto autoStores = s.sent().of(Opcode::CMSG_AUTOSTORE_BAG_ITEM);
    REQUIRE(autoStores.size() == 2);
    {
        Packet p0 = autoStores[0];
        CHECK(p0.readUInt8() == 0xFF);
        CHECK(p0.readUInt8() == 23);
        CHECK(p0.readUInt8() == 19); // worn bag 0

        Packet p1 = autoStores[1];
        CHECK(p1.readUInt8() == 0xFF);
        CHECK(p1.readUInt8() == 23);
        CHECK(p1.readUInt8() == 0xFF); // backpack
    }

    execLua(lua, "PickupContainerItem(0, 1); DeleteCursorItem()");
    const auto destroys = s.sent().of(Opcode::CMSG_DESTROYITEM);
    REQUIRE(destroys.size() == 1);
    {
        Packet d = destroys[0];
        REQUIRE(d.getData().size() == 6);
        CHECK(d.readUInt8() == 0xFF);
        CHECK(d.readUInt8() == 23);
        CHECK(d.readUInt8() == 0); // whole stack
        CHECK(d.readUInt8() == 0);
        CHECK(d.readUInt8() == 0);
        CHECK(d.readUInt8() == 0);
    }

    // 4. Open vendor via SMSG_LIST_INVENTORY, drag merchant item onto backpack slot 3 and bag 1 slot 2 ->
    //    sends 22-byte TBC CMSG_BUY_ITEM_IN_SLOT!
    constexpr uint64_t kVendorGuid = 0xF130000099990001ULL;
    {
        Packet listInv(wireOpcode(Opcode::SMSG_LIST_INVENTORY));
        listInv.writeUInt64(kVendorGuid);
        listInv.writeUInt8(1);
        listInv.writeUInt32(1);     // vendorSlot
        listInv.writeUInt32(2589);  // itemId
        listInv.writeUInt32(20002); // displayId
        listInv.writeUInt32(0xFFFFFFFFu); // unlimited
        listInv.writeUInt32(100);   // price
        listInv.writeUInt32(0);     // durability
        listInv.writeUInt32(1);     // stackCount
        listInv.writeUInt32(0);     // extendedCost
        s.receive(listInv);
    }
    execLua(lua, "PickupMerchantItem(1); PickupContainerItem(0, 3)");
    execLua(lua, "PickupMerchantItem(1); PickupContainerItem(1, 2)");
    const auto buySlots = s.sent().of(Opcode::CMSG_BUY_ITEM_IN_SLOT);
    REQUIRE(buySlots.size() == 2);
    {
        Packet b0 = buySlots[0];
        REQUIRE(b0.getData().size() == 22);
        CHECK(b0.readUInt64() == kVendorGuid);
        CHECK(b0.readUInt32() == 2589);
        CHECK(b0.readUInt64() == Session::kPlayerGuid);
        CHECK(b0.readUInt8() == 25); // 23 + (3 - 1)
        CHECK(b0.readUInt8() == 1);

        Packet b1 = buySlots[1];
        REQUIRE(b1.getData().size() == 22);
        CHECK(b1.readUInt64() == kVendorGuid);
        CHECK(b1.readUInt32() == 2589);
        CHECK(b1.readUInt64() == kBagGuid);
        CHECK(b1.readUInt8() == 1); // 0-based slot inside bag 1
        CHECK(b1.readUInt8() == 1);
    }

    // 5. WotLK expansion gating check on CMSG_BUY_ITEM_IN_SLOT (29 bytes with vendorSlot + uint32 count).
    {
        Session wotlk("wotlk");
        wotlk.handler().buyItemInSlot(kVendorGuid, 2589, 7, Session::kPlayerGuid, 24, 5);
        const auto wotlkBuys = wotlk.sent().of(Opcode::CMSG_BUY_ITEM_IN_SLOT);
        REQUIRE(wotlkBuys.size() == 1);
        Packet wb = wotlkBuys[0];
        REQUIRE(wb.getData().size() == 29);
        CHECK(wb.readUInt64() == kVendorGuid);
        CHECK(wb.readUInt32() == 2589);
        CHECK(wb.readUInt32() == 7); // vendorSlot
        CHECK(wb.readUInt64() == Session::kPlayerGuid);
        CHECK(wb.readUInt8() == 24);
        CHECK(wb.readUInt32() == 5);
    }
}
