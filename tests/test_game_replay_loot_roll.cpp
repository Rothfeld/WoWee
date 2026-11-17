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

wowee::network::Packet startRoll() {
    wowee::network::Packet p(wireOpcode(Opcode::SMSG_LOOT_START_ROLL));
    p.writeUInt64(kCorpse);
    p.writeUInt32(0);           // loot slot
    p.writeUInt32(kThornspike);
    p.writeUInt32(0);           // random suffix
    p.writeUInt32(0);           // random property
    p.writeUInt32(60000);       // countdown
    p.writeUInt8(0x03);         // need | greed
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
