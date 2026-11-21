// Native TBC 2.4.3 plays client-side EMOTE_TALK (anim 60) on the local player
// whenever initiating an interaction with an in-range, interactable NPC unit
// (QuestGiver, Gossip, Vendor, Trainer, Banker, FlightMaster, Innkeeper,
// Auctioneer, GuildMaster, Battlemaster), without broadcasting SMSG_EMOTE to
// remote observers. It does not play for GameObject questgivers (Wanted
// Posters), out-of-range NPCs, non-interactable creatures, or follow-up panel
// option clicks.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"

#include <vector>

using replay::Session;
using replay::UpdateObject;
using wowee::game::ObjectType;
using wowee::game::Opcode;
using UF = wowee::game::UF;

namespace {

void spawnPlayer(Session& s, uint32_t hp = 100) {
    s.receive(UpdateObject().tbc().create(
        Session::kPlayerGuid, ObjectType::PLAYER,
        {{UF::UNIT_FIELD_HEALTH, hp}, {UF::UNIT_FIELD_MAXHEALTH, 100}}, true));
}

void spawnNpc(Session& s, uint64_t guid, uint32_t npcFlags) {
    s.receive(UpdateObject().tbc().create(
        guid, ObjectType::UNIT,
        {{UF::UNIT_FIELD_HEALTH, 100}, {UF::UNIT_FIELD_MAXHEALTH, 100},
         {UF::UNIT_NPC_FLAGS, npcFlags}}));
}

} // namespace

TEST_CASE("local player plays talk emote when interacting with any service NPC in range",
          "[replay][npc-talk]") {
    Session s("tbc");
    spawnPlayer(s);
    auto& gh = s.handler();
    gh.setPosition(2.0f, 0.0f, 0.0f);

    std::vector<uint64_t> talkedTo;
    gh.setLocalNpcInteractTalkCallback([&](uint64_t guid) { talkedTo.push_back(guid); });

    struct Case {
        uint64_t guid;
        uint32_t npcFlags;
    };
    const Case npcs[] = {
        {0xF1300000F0000001ull, 1u},        // Gossip (Stormwind Guard)
        {0xF1300000F1000002ull, 2u},        // QuestGiver (Remy "Two Times")
        {0xF1300000F0000003ull, 3u},        // Gossip + QuestGiver (Marshal Dughan)
        {0xF130000042000004ull, 128u},      // Vendor (Tharynn Bouden)
        {0xF130000202000005ull, 16u},       // Trainer (Smith Argus)
        {0xF130000997000006ull, 131072u},   // Banker (Olivia Burnside)
        {0xF130000160000007ull, 8192u},     // FlightMaster (Dungar Longdrink)
        {0xF130000127000008ull, 65536u},    // Innkeeper (Innkeeper Farley)
        {0xF130003D2B000009ull, 2097152u},  // Auctioneer (Auctioneer Jaxon)
        {0xF13000050E00000Aull, 4096u},     // GuildMaster (Aldric Moore)
        {0xF130001CF200000Bull, 1048576u},  // Battlemaster (Thelman Slatefist)
    };

    for (const auto& c : npcs) {
        spawnNpc(s, c.guid, c.npcFlags);
        gh.interactWithNpc(c.guid);
    }

    REQUIRE(talkedTo.size() == 11);
    for (size_t i = 0; i < talkedTo.size(); ++i) {
        CHECK(talkedTo[i] == npcs[i].guid);
    }
    // 10 CMSG_GOSSIP_HELLO + 1 CMSG_BATTLEMASTER_HELLO; no CMSG_EMOTE sent.
    CHECK(s.sent().of(Opcode::CMSG_GOSSIP_HELLO).size() == 10);
    CHECK(s.sent().of(Opcode::CMSG_BATTLEMASTER_HELLO).size() == 1);
    CHECK(s.sent().of(Opcode::CMSG_EMOTE).empty());
    CHECK(s.sent().of(Opcode::CMSG_TEXT_EMOTE).empty());
}

TEST_CASE("local player does not play talk emote for non-interactable, out-of-range, or GameObject questgivers",
          "[replay][npc-talk]") {
    Session s("tbc");
    spawnPlayer(s);
    auto& gh = s.handler();

    std::vector<uint64_t> talkedTo;
    gh.setLocalNpcInteractTalkCallback([&](uint64_t guid) { talkedTo.push_back(guid); });

    constexpr uint64_t kCritter = 0xF13000151E000001ull;
    constexpr uint64_t kDughan = 0xF1300000F0000002ull;
    constexpr uint64_t kWantedPoster = 0xF11000002F000003ull;

    spawnNpc(s, kCritter, 0u);  // NpcFlags == 0 (Palomino / Rabbit)
    spawnNpc(s, kDughan, 3u);   // QuestGiver + Gossip
    s.receive(UpdateObject().tbc().create(
        kWantedPoster, ObjectType::GAMEOBJECT,
        {{UF::GAMEOBJECT_DISPLAYID, 129}}));

    // Non-interactable creature in range: no talk.
    gh.setPosition(2.0f, 0.0f, 0.0f);
    gh.interactWithNpc(kCritter);
    CHECK(talkedTo.empty());

    // Interactable NPC out of interaction reach (> 5.55 yd): no talk.
    gh.setPosition(10.0f, 0.0f, 0.0f);
    gh.interactWithNpc(kDughan);
    CHECK(talkedTo.empty());

    // GameObject questgiver (Wanted Poster) in range: no talk.
    gh.setPosition(2.0f, 0.0f, 0.0f);
    gh.interactWithGameObject(kWantedPoster);
    gh.interactWithNpc(kWantedPoster);
    CHECK(talkedTo.empty());

    // Now interact with Dughan in range: talks once.
    gh.interactWithNpc(kDughan);
    REQUIRE(talkedTo.size() == 1);
    CHECK(talkedTo[0] == kDughan);

    // Follow-up panel clicks (selecting a quest/option) do not re-trigger talk.
    gh.selectGossipQuest(62);
    gh.selectGossipOption(0);
    CHECK(talkedTo.size() == 1);
}

TEST_CASE("corpse does not talk to NPCs while ghost talking to spirit healer does",
          "[replay][npc-talk]") {
    Session s("tbc");
    spawnPlayer(s, 0);
    auto& gh = s.handler();
    gh.setPosition(2.0f, 0.0f, 0.0f);

    std::vector<uint64_t> talkedTo;
    gh.setLocalNpcInteractTalkCallback([&](uint64_t guid) { talkedTo.push_back(guid); });

    constexpr uint64_t kSpiritHealer = 0xF13000195B000001ull;
    spawnNpc(s, kSpiritHealer, 0x00004000u);  // NPC_FLAG_SPIRIT_HEALER

    // Corpse (dead, not ghost): no talk animation.
    gh.interactWithNpc(kSpiritHealer);
    CHECK(talkedTo.empty());

    // Ghost (dead + PLAYER_FLAGS_GHOST): can talk to Spirit Healer.
    s.receive(UpdateObject().tbc().values(
        Session::kPlayerGuid, {{UF::PLAYER_FLAGS, 0x00000010u}}));
    gh.interactWithNpc(kSpiritHealer);
    REQUIRE(talkedTo.size() == 1);
    CHECK(talkedTo[0] == kSpiritHealer);
}
