// Quest progress through GameHandler's own dispatch. See game_replay.hpp.
#include <catch_amalgamated.hpp>

#include "game_replay.hpp"
#include "game/quest_handler.hpp"

#include <algorithm>
#include <string>
#include <vector>

using replay::Session;

namespace {

using wowee::game::Opcode;
using wowee::game::wireOpcode;
using wowee::network::Packet;

constexpr uint32_t kQuest = 33;
constexpr uint32_t kYoungWolf = 299;
constexpr uint32_t kAltar = 1234;

struct InfoMessages {
    std::vector<std::string> lines;
    explicit InfoMessages(Session& s) {
        s.handler().setAddonEventCallback(
            [this](const std::string& name, const std::vector<std::string>& args) {
                if (name == "UI_INFO_MESSAGE" && !args.empty()) lines.push_back(args[0]);
            });
    }
};

void acceptQuest(Session& s) {
    s.handler().getQuestHandler()->addQuestToLocalLogIfMissing(kQuest, "Wolves Across the Border", "");
}

// As AzerothCore's QueryHandler writes it.
Packet creatureName(uint32_t entry, const std::string& name) {
    Packet p(wireOpcode(Opcode::SMSG_CREATURE_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeString(name);
    for (int i = 0; i < 5; ++i) p.writeString("");
    for (int i = 0; i < 4 + 2 + 4; ++i) p.writeUInt32(0);  // type to rank, kill credits, models
    p.writeFloat(1.0f);
    p.writeFloat(1.0f);
    p.writeUInt8(0);
    for (int i = 0; i < 6 + 1; ++i) p.writeUInt32(0);  // quest items, movement id
    return p;
}

Packet gameObjectName(uint32_t entry, const std::string& name) {
    Packet p(wireOpcode(Opcode::SMSG_GAMEOBJECT_QUERY_RESPONSE));
    p.writeUInt32(entry);
    p.writeUInt32(10);  // GAMEOBJECT_TYPE_GOOBER
    p.writeUInt32(1);
    p.writeString(name);
    for (int i = 0; i < 6; ++i) p.writeString("");
    for (int i = 0; i < 24; ++i) p.writeUInt32(0);
    p.writeFloat(1.0f);
    for (int i = 0; i < 6; ++i) p.writeUInt32(0);
    return p;
}

// Player::SendQuestUpdateAddCreatureOrGo: a game object's entry has the high
// bit set.
Packet addKill(uint32_t wireEntry, uint32_t count, uint32_t required) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_KILL));
    p.writeUInt32(kQuest);
    p.writeUInt32(wireEntry);
    p.writeUInt32(count);
    p.writeUInt32(required);
    p.writeUInt64(0xF130000000000123ull);
    return p;
}

bool chatMentions(Session& s, const std::string& text) {
    const auto& history = s.handler().getChatHistory();
    return std::any_of(history.begin(), history.end(), [&](const auto& m) {
        return m.message.find(text) != std::string::npos;
    });
}

}  // namespace

TEST_CASE("a quest kill is shown as ERR_QUEST_ADD_KILL_SII", "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    s.receive(creatureName(kYoungWolf, "Young Wolf"));
    s.receive(addKill(kYoungWolf, 3, 10));
    CHECK(info.lines == std::vector<std::string>{"Young Wolf slain: 3/10"});
    CHECK_FALSE(chatMentions(s, "3/10"));
}

TEST_CASE("a quest object is named from the object cache, as ERR_QUEST_ADD_FOUND_SII",
          "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    s.receive(creatureName(kAltar, "Kobold Vermin"));
    s.receive(gameObjectName(kAltar, "Altar of Zul"));
    s.receive(addKill(kAltar | 0x80000000u, 1, 1));
    CHECK(info.lines == std::vector<std::string>{"Altar of Zul: 1/1"});
}

TEST_CASE("a quest player kill is shown as ERR_QUEST_ADD_PLAYER_KILL_SII", "[replay][quest]") {
    Session s;
    InfoMessages info(s);
    acceptQuest(s);
    Packet p(wireOpcode(Opcode::SMSG_QUESTUPDATE_ADD_PVP_KILL));
    p.writeUInt32(kQuest);
    p.writeUInt32(2);
    p.writeUInt32(5);
    s.receive(p);
    CHECK(info.lines == std::vector<std::string>{"Players slain: 2/5"});
    CHECK_FALSE(chatMentions(s, "2/5"));
}

namespace {

using replay::UpdateObject;
using wowee::game::QuestGiverStatus;
using wowee::game::UF;

constexpr uint64_t kGornek = 0xF130000C47000D73ull;
constexpr uint32_t kYourPlace = 4641;

size_t multipleQueries(const Session& s) {
    return s.sent().of(Opcode::CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY).size();
}

// Player::SendQuestGiverStatusMultiple: a count, then guid and one status byte
// per giver; 8 is TBC DIALOG_STATUS_REWARD.
Packet statusMultiple(uint64_t guid, uint8_t status) {
    Packet p(wireOpcode(Opcode::SMSG_QUESTGIVER_STATUS_MULTIPLE));
    p.writeUInt32(1);
    p.writeUInt64(guid);
    p.writeUInt8(status);
    return p;
}

// A quest log slot as Player::SetQuestSlot / SetQuestSlotCounter write it in
// TBC: id, state, packed counters, timer - four fields from
// PLAYER_QUEST_LOG_1_1.
std::map<uint16_t, uint32_t> slot1(uint32_t id, uint32_t counters) {
    const uint16_t start = wowee::game::fieldIndex(UF::PLAYER_QUEST_LOG_START);
    REQUIRE(start != 0xFFFF);
    return {{start, id}, {static_cast<uint16_t>(start + 2), counters}};
}

}  // namespace

// Build 8606 asks for every giver's status whenever its quest log fields
// change, and only then (pair 19: accepting Your Place In The World from
// Kaltunk, ".quest add/complete/remove", one Mottled Boar killed). Gornek,
// who takes the quest, kept the ! of Cutting Teeth when only Kaltunk was asked.
TEST_CASE("a quest log change asks every quest giver's status once", "[replay][quest]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 1}}, true));
    CHECK(multipleQueries(s) == 0);  // an empty log at login asks nothing

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(kYourPlace, 0)));
    CHECK(multipleQueries(s) == 1);
    s.receive(statusMultiple(kGornek, 8));
    CHECK(s.handler().getQuestGiverStatus(kGornek) == QuestGiverStatus::REWARD);

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(kYourPlace, 1)));
    CHECK(multipleQueries(s) == 2);  // a counter

    s.receive(UpdateObject().tbc().values(Session::kPlayerGuid, {{UF::UNIT_FIELD_LEVEL, 2}}));
    CHECK(multipleQueries(s) == 2);  // not the log

    s.receive(UpdateObject().tbc().valuesAt(Session::kPlayerGuid, slot1(0, 0)));
    CHECK(multipleQueries(s) == 3);  // removed or handed in
    CHECK(s.sent().of(Opcode::CMSG_QUESTGIVER_STATUS_QUERY).empty());
}

TEST_CASE("entering the world with quests in the log asks every status once", "[replay][quest]") {
    Session s("tbc");
    s.receive(UpdateObject().tbc().create(Session::kPlayerGuid, wowee::game::ObjectType::PLAYER,
                                          {{UF::UNIT_FIELD_LEVEL, 1},
                                           {UF::PLAYER_QUEST_LOG_START, kYourPlace}},
                                          true));
    CHECK(multipleQueries(s) == 1);
}
