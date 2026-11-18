// SMSG_LOOT_START_ROLL through GameHandler's own dispatch, built as cmangos-tbc
// Group::SendLootStartRoll writes it: guid, slot, item, random suffix, random
// property, countdown, vote mask. See game_replay.hpp.
//
// GroupLootFrame_OnShow (TBC lootframe.lua) reads the item's name, icon and
// quality once. START_LOOT_ROLL is raised only once the item is cached, so a
// roll on an unfamiliar drop opens complete rather than as "Item #N".
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <string>
#include <utility>
#include <vector>

using replay::Session;
using wowee::game::Opcode;
using wowee::game::wireOpcode;

namespace {

constexpr uint64_t kCorpse = 0xF130001148470001ull;
constexpr uint32_t kThornspike = 6681;

wowee::network::Packet startRoll(uint32_t slot = 0) {
    wowee::network::Packet p(wireOpcode(Opcode::SMSG_LOOT_START_ROLL));
    p.writeUInt64(kCorpse);
    p.writeUInt32(slot);        // loot slot
    p.writeUInt32(kThornspike);
    p.writeUInt32(0);           // random suffix
    p.writeUInt32(0);           // random property
    p.writeUInt32(60000);       // countdown
    p.writeUInt8(0x03);         // need | greed
    return p;
}

/// Group::SendLootAllPassed: guid, slot, item, random suffix, random property.
wowee::network::Packet allPassed(uint32_t slot) {
    wowee::network::Packet p(wireOpcode(Opcode::SMSG_LOOT_ALL_PASSED));
    p.writeUInt64(kCorpse);
    p.writeUInt32(slot);
    p.writeUInt32(kThornspike);
    p.writeUInt32(0);
    p.writeUInt32(0);
    return p;
}

/// TBC SMSG_ITEM_QUERY_SINGLE_RESPONSE for Thornspike, field order as
/// TbcPacketParsers::parseItemQueryResponse reads it.
wowee::network::Packet thornspikeQueryResponse() {
    wowee::network::Packet p(wireOpcode(Opcode::SMSG_ITEM_QUERY_SINGLE_RESPONSE));
    p.writeUInt32(kThornspike);
    p.writeUInt32(2);           // weapon
    p.writeUInt32(15);          // dagger
    p.writeUInt32(0xFFFFFFFFu); // sound override subclass
    p.writeString("Thornspike");
    p.writeString("");
    p.writeString("");
    p.writeString("");
    p.writeUInt32(20593);       // displayInfoId
    p.writeUInt32(3);           // rare
    p.writeUInt32(0);           // flags
    p.writeUInt32(0);           // buy
    p.writeUInt32(0);           // sell
    p.writeUInt32(13);          // one-hand
    for (int i = 0; i < 14; ++i) p.writeUInt32(i < 2 ? 0xFFFFFFFFu : 0);
    for (int i = 0; i < 20; ++i) p.writeUInt32(0);   // stats
    for (int i = 0; i < 5; ++i) { p.writeFloat(0.0f); p.writeFloat(0.0f); p.writeUInt32(0); }
    for (int i = 0; i < 7; ++i) p.writeUInt32(0);    // armor, resistances
    p.writeUInt32(1800);        // delay
    p.writeUInt32(0);           // ammo
    p.writeFloat(0.0f);         // ranged range
    for (int i = 0; i < 30; ++i) p.writeUInt32(0);   // spells
    p.writeUInt32(2);           // bind on equip
    return p;
}

}  // namespace

TEST_CASE("a loot roll opens once its item is known", "[replay][lootroll]") {
    Session s("tbc");
    std::vector<std::pair<std::string, std::vector<std::string>>> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>& a) { events.emplace_back(e, a); });
    auto rollEvents = [&] {
        std::vector<std::vector<std::string>> out;
        for (const auto& [e, a] : events)
            if (e == "START_LOOT_ROLL") out.push_back(a);
        return out;
    };

    s.receive(startRoll());
    CHECK(rollEvents().empty());
    // The client asked for the item.
    CHECK(s.sent().of(Opcode::CMSG_ITEM_QUERY_SINGLE).size() == 1);

    s.receive(thornspikeQueryResponse());
    const auto opened = rollEvents();
    REQUIRE(opened.size() == 1);
    REQUIRE(opened[0].size() == 2);
    CHECK(opened[0][1] == "60000");
    const auto* roll = s.handler().getLootRoll(std::stoi(opened[0][0]));
    REQUIRE(roll != nullptr);
    CHECK(roll->announced);
    const auto* info = s.handler().getItemInfo(kThornspike);
    REQUIRE(info != nullptr);
    CHECK(info->name == "Thornspike");
    CHECK(info->quality == 3u);
}

TEST_CASE("a loot roll on a cached item opens at once", "[replay][lootroll]") {
    Session s("tbc");
    s.receive(thornspikeQueryResponse());
    int opened = 0;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>&) { opened += e == "START_LOOT_ROLL"; });
    s.receive(startRoll());
    CHECK(opened == 1);
}

// Measured on the original client (build 8606) with an event logger addon on
// eleven-item Lord Kazzak drops: four rolls are announced, the rest wait; each
// roll that ends - the player's choice or the server's result, announced or
// waiting - raises CANCEL_LOOT_ROLL and then announces the oldest waiting one
// with the full countdown. The count drops for waiting rolls too and wraps, after
// which nothing more is announced. Evidence:
// _experiments/20261118T002549Z.loot_roll_queue_pair7 in the workspace root.
TEST_CASE("four loot rolls are announced at a time", "[replay][lootroll]") {
    Session s("tbc");
    s.receive(thornspikeQueryResponse());
    std::vector<std::pair<std::string, std::vector<std::string>>> events;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>& a) {
            if (e == "START_LOOT_ROLL" || e == "CANCEL_LOOT_ROLL") events.emplace_back(e, a);
        });
    using Events = std::vector<std::pair<std::string, std::vector<std::string>>>;

    for (uint32_t slot = 0; slot < 6; ++slot) s.receive(startRoll(slot));
    CHECK(events == Events{{"START_LOOT_ROLL", {"0", "60000"}},
                           {"START_LOOT_ROLL", {"1", "60000"}},
                           {"START_LOOT_ROLL", {"2", "60000"}},
                           {"START_LOOT_ROLL", {"3", "60000"}}});
    // A waiting roll is known to the interface by id before it is announced.
    REQUIRE(s.handler().getLootRoll(5) != nullptr);
    CHECK_FALSE(s.handler().getLootRoll(5)->announced);

    // The server ends a waiting roll: it is cancelled and the next one opens.
    // The roll is still there while CANCEL_LOOT_ROLL is raised.
    events.clear();
    bool presentDuringCancel = false;
    s.handler().setAddonEventCallback(
        [&](const std::string& e, const std::vector<std::string>& a) {
            if (e == "CANCEL_LOOT_ROLL" && a == std::vector<std::string>{"5"})
                presentDuringCancel = s.handler().getLootRoll(5) != nullptr;
            if (e == "START_LOOT_ROLL" || e == "CANCEL_LOOT_ROLL") events.emplace_back(e, a);
        });
    s.receive(allPassed(5));
    CHECK(presentDuringCancel);
    CHECK(s.handler().getLootRoll(5) == nullptr);
    CHECK(events == Events{{"CANCEL_LOOT_ROLL", {"5"}}, {"START_LOOT_ROLL", {"4", "60000"}}});

    // The player passes on an announced roll; nothing is left waiting.
    events.clear();
    s.handler().sendLootRoll(kCorpse, 0, 0);
    CHECK(events == Events{{"CANCEL_LOOT_ROLL", {"0"}}});
    CHECK(s.sent().of(Opcode::CMSG_LOOT_ROLL).size() == 1);

    // Rolls 1-4 end: the count, three after the pass, wraps below zero and the
    // next drop is not announced at all.
    for (uint32_t slot = 1; slot < 5; ++slot) s.receive(allPassed(slot));
    events.clear();
    s.receive(startRoll(6));
    CHECK(events.empty());
    REQUIRE(s.handler().getLootRoll(6) != nullptr);
    CHECK_FALSE(s.handler().getLootRoll(6)->announced);
}
