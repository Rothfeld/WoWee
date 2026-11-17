// The "<Owner's ...>" line under summoned units (game/summon_title.hpp).
#include <catch_amalgamated.hpp>

#include "game/summon_title.hpp"

using namespace wowee::game;

TEST_CASE("a creature's summon is its minion", "[summon-title]") {
    // Stone Rumbler 0xF140000235000004, UNIT_FIELD_CREATEDBY = Razorfen
    // Geomancer 0xF1300011A8596BB5, elemental (type 4); 2026-11-17 15:28 log.
    const auto t = summonTitle(4, 0xF1300011A8596BB5ull);
    REQUIRE(t == SummonTitle::Minion);
    REQUIRE(std::string(summonTitleGlobal(t)) == "UNITNAME_TITLE_MINION");
    CHECK(formatSummonTitle("%s's Minion", "Razorfen Geomancer") == "Razorfen Geomancer's Minion");
}

TEST_CASE("a non-combat pet is its owner's companion", "[summon-title]") {
    CHECK(summonTitle(12, 0x0000000000000004ull) == SummonTitle::Companion);
    CHECK(std::string(summonTitleGlobal(SummonTitle::Companion)) == "UNITNAME_TITLE_COMPANION");
}

TEST_CASE("no title without a creator or for a player's other summons", "[summon-title]") {
    CHECK(summonTitle(4, 0) == SummonTitle::None);
    CHECK(summonTitle(1, 0x0000000000000004ull) == SummonTitle::None);  // undecided: no capture
    CHECK(summonTitleGlobal(SummonTitle::None) == nullptr);
    CHECK(formatSummonTitle("no placeholder", "x").empty());
}
