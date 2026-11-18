// What the server says an NPC has for you, and the mark that goes over it.
//
// The numbers below are DIALOG_STATUS_* from the server's own QuestDef.h, read
// off an AzerothCore checkout rather than inferred from what the client did
// with them. This client named six of the eleven and mislabelled two: 7 was
// "available, low level" where the server means available-with-reputation, and
// the real low-level-available is 2.
//
// A status with no name matched no branch in any of the four places that draw
// the mark, so nothing was drawn - and 2, 3 and 4 are exactly what an NPC
// reports once its quests are below your level.
#include <catch_amalgamated.hpp>

#include <string>

#include "game/quest_giver_status.hpp"
#include "game/gossip_quest_status.hpp"
#include "game/level_difficulty.hpp"
#include <utility>
#include <array>

using wowee::game::QuestGiverStatus;
using wowee::game::questGiverMarker;

namespace {
QuestGiverStatus wire(uint8_t v) { return static_cast<QuestGiverStatus>(v); }
std::string symbolOf(uint8_t v) {
    const auto m = questGiverMarker(wire(v));
    return m.symbol ? m.symbol : "";
}
}  // namespace

TEST_CASE("the values are the server's DIALOG_STATUS numbers", "[questgiver]") {
    CHECK(static_cast<uint8_t>(QuestGiverStatus::NONE) == 0);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::UNAVAILABLE) == 1);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_AVAILABLE) == 2);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_REWARD_REP) == 3);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::LOW_LEVEL_AVAILABLE_REP) == 4);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::INCOMPLETE) == 5);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD_REP) == 6);
    // 7 is available-with-reputation. It was named AVAILABLE_LOW here, which
    // is what 2 is, so a rep-gated quest was drawn grey and an out-levelled
    // one was drawn not at all.
    CHECK(static_cast<uint8_t>(QuestGiverStatus::AVAILABLE_REP) == 7);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::AVAILABLE) == 8);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD2) == 9);
    CHECK(static_cast<uint8_t>(QuestGiverStatus::REWARD) == 10);
}

TEST_CASE("every status the server can send draws something or nothing on purpose",
          "[questgiver]") {
    // The bug was silence: a value with no branch drew no mark, and the player
    // saw an NPC with nothing over its head. Only 0 means nothing.
    for (uint8_t v = 0; v <= 10; ++v) {
        INFO("status " << static_cast<int>(v));
        const auto m = questGiverMarker(wire(v));
        if (v == 0) {
            CHECK(m.symbol == nullptr);
        } else {
            REQUIRE(m.symbol != nullptr);
            CHECK(m.tooltip != nullptr);
        }
    }
}

TEST_CASE("an exclamation mark means there is something to take", "[questgiver]") {
    CHECK(symbolOf(8) == "!");   // available
    CHECK(symbolOf(7) == "!");   // available, reputation gated
    CHECK(symbolOf(2) == "!");   // available, below your level
    CHECK(symbolOf(4) == "!");   // available, below your level, rep gated

    SECTION("and the low-level ones are grey") {
        CHECK_FALSE(questGiverMarker(wire(8)).dim);
        CHECK_FALSE(questGiverMarker(wire(7)).dim);
        CHECK(questGiverMarker(wire(2)).dim);
        CHECK(questGiverMarker(wire(4)).dim);
    }
}

TEST_CASE("a question mark means something is in progress or ready", "[questgiver]") {
    CHECK(symbolOf(10) == "?");  // ready to turn in
    CHECK(symbolOf(9) == "?");   // ready, no minimap dot
    CHECK(symbolOf(6) == "?");   // ready, reputation
    CHECK(symbolOf(3) == "?");   // ready, below your level
    CHECK(symbolOf(5) == "?");   // started, not finished

    SECTION("in progress is grey, ready is gold") {
        CHECK(questGiverMarker(wire(5)).dim);
        CHECK_FALSE(questGiverMarker(wire(10)).dim);
        CHECK_FALSE(questGiverMarker(wire(9)).dim);
        CHECK(questGiverMarker(wire(3)).dim);
    }
}

TEST_CASE("status 9 is the one that keeps off the minimap", "[questgiver]") {
    // It is the only thing separating 9 from 10, and drawing it as a dot would
    // put a marker on the map for a turn-in the server is hiding there.
    CHECK_FALSE(questGiverMarker(wire(9)).onMinimap);
    CHECK(questGiverMarker(wire(10)).onMinimap);
    CHECK(std::string(questGiverMarker(wire(9)).symbol) ==
          std::string(questGiverMarker(wire(10)).symbol));
}

TEST_CASE("a status the server never sends draws nothing", "[questgiver]") {
    // DIALOG_STATUS_SCRIPTED_NO_STATUS is 0x1000 and does not fit a uint8, so
    // the wire cannot carry it; anything above 11 (CHAT, this client's own
    // value for TBC's status 2) is a value it does not know and must not
    // guess at.
    CHECK(questGiverMarker(wire(12)).symbol == nullptr);
    CHECK(wowee::game::questGiverCursor(wire(12)) == nullptr);
    CHECK(questGiverMarker(wire(255)).symbol == nullptr);
}

TEST_CASE("TBC gossip uses the server's TBC dialog statuses", "[questgiver][gossip]") {
    using S = wowee::game::GossipQuestState;
    // CMaNGOS QuestDef.h and Player::PrepareQuestMenu: offered quests use 6,
    // incomplete quests use 3, and completed quests use 4.
    constexpr std::array expected{S::None, S::None, S::Available, S::Incomplete,
        S::Completable, S::Available, S::Available, S::Completable, S::Completable};
    for (uint32_t icon = 0; icon < expected.size(); ++icon) {
        INFO("TBC dialog status " << icon);
        CHECK(wowee::game::gossipQuestState(icon, true) == expected[icon]);
    }
    CHECK(wowee::game::gossipQuestState(255, true) == S::None);
}

TEST_CASE("gossip status classification keeps non-TBC semantics", "[questgiver][gossip]") {
    using S = wowee::game::GossipQuestState;
    constexpr std::array expected{S::None, S::None, S::Available, S::Incomplete,
        S::Incomplete, S::Completable, S::Completable, S::Available,
        S::Available, S::None, S::Completable};
    for (uint32_t icon = 0; icon < expected.size(); ++icon) {
        INFO("non-TBC dialog status " << icon);
        CHECK(wowee::game::gossipQuestState(icon, false) == expected[icon]);
    }
}

TEST_CASE("Incomplete quests retain the NPC mark without a minimap marker", "[quest][minimap]") {
    const auto marker = questGiverMarker(QuestGiverStatus::INCOMPLETE);
    CHECK(std::string(marker.symbol) == "?");
    CHECK(marker.dim);
    CHECK_FALSE(marker.onMinimap);
    CHECK(questGiverMarker(QuestGiverStatus::AVAILABLE).onMinimap);
    CHECK(questGiverMarker(QuestGiverStatus::REWARD).onMinimap);
}

TEST_CASE("unavailable quests show a grey world exclamation without a minimap dot", "[questgiver][tbc]") {
    const auto marker = questGiverMarker(QuestGiverStatus::UNAVAILABLE);
    REQUIRE(marker.symbol);
    CHECK(std::string(marker.symbol) == "!");
    CHECK(marker.dim);
    CHECK_FALSE(marker.onMinimap);
}

TEST_CASE("an out-levelled TBC quest giver has no mark but the Quest cursor", "[questgiver][tbc]") {
    // Haughty Modiste, Yeh'kinya and Wanted Poster 142122 to a level 70
    // character in build 8606 (pair 16): their level 44-46 quests come as
    // DIALOG_STATUS_CHAT.
    CHECK(questGiverMarker(QuestGiverStatus::CHAT).symbol == nullptr);
    CHECK(std::string(wowee::game::questGiverCursor(QuestGiverStatus::CHAT)) == "Quest");
}

TEST_CASE("the quest cursor follows the status, not the mark", "[questgiver][cursor]") {
    using wowee::game::questGiverCursor;
    CHECK(questGiverCursor(QuestGiverStatus::NONE) == nullptr);
    CHECK(questGiverCursor(QuestGiverStatus::UNAVAILABLE) == nullptr);  // grey !
    CHECK(std::string(questGiverCursor(QuestGiverStatus::AVAILABLE)) == "Quest");
    // A grey ? over the giver, and still the Quest cursor.
    CHECK(std::string(questGiverCursor(QuestGiverStatus::INCOMPLETE)) == "Quest");
    CHECK(std::string(questGiverCursor(QuestGiverStatus::REWARD)) == "QuestTurnIn");
}

// GetQuestGreenRange as build 8606 answers it (native /script, pair 16), and
// the "(low level)" the gossip and greeting lists show, which is not drawn
// from it.
TEST_CASE("TBC quest green range and trivial quests", "[questgiver][tbc]") {
    using wowee::game::questGreenRange;
    using wowee::game::questIsTrivial;
    for (const auto& [level, range] : {std::pair{5, 4}, {6, 4}, {9, 4}, {10, 5}, {19, 5}, {20, 6},
                                       {29, 6}, {30, 7}, {39, 7}, {40, 8}, {44, 8}, {45, 8},
                                       {49, 8}, {50, 8}, {54, 8}, {55, 8}, {59, 8}, {60, 8}, {70, 8}}) {
        INFO("level " << level);
        CHECK(questGreenRange(true, level) == range);
    }
    // Gossip "(low level)": more than 5 levels below, whatever the green range.
    CHECK_FALSE(questIsTrivial(true, 17, 12));  // Thundris Windweaver
    CHECK(questIsTrivial(true, 18, 12));
    CHECK_FALSE(questIsTrivial(true, 45, 40));  // Rigglefuzz
    CHECK(questIsTrivial(true, 46, 40));
    CHECK_FALSE(questIsTrivial(true, 63, 58));  // Smokey LaRue
    CHECK(questIsTrivial(true, 64, 58));
    CHECK(questIsTrivial(true, 55, 44));        // Yeh'kinya
    CHECK_FALSE(questIsTrivial(true, 55, 0));
}
